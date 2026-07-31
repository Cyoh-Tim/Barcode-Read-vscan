#!/usr/bin/env bash
# ci_gate.sh — 회귀 게이트 두 개를 한 번에 돌린다.
#
#   1) 고정 40종      : 통과/실패 게이트. 세 경로 모두 기준치 미만이면 실패.
#   2) 심볼로지 각도 스윕: 14종 x 0~90도. 검출률 100% 미만이거나 오디코딩이
#                        하나라도 나오면 실패.
#   3) 고정 시드 코퍼스: 검출률/평균/p95/중복을 기록하고, 직전 기준선 대비
#                        허용치를 넘게 나빠지면 실패.
#
# 왜 셋 다 필요한가.
#  - 40종은 "읽히느냐"만 본다. 실제로 있었던 일: 회전 구제를 고쳤더니
#    검출은 그대로인데 평균이 3~5ms 늘었다(§3.11). 코퍼스가 그걸 잡는다.
#  - 코퍼스는 난수라 **특정 심볼로지의 특정 각도**가 통째로 죽어도 전체
#    평균에 묻힌다. 실제로 있었던 일: ITF가 각도 5~80도에서 정답과 함께
#    "345670" 같은 부분 디코딩을 같이 뱉고 있었는데(오디코딩 11건),
#    코퍼스 지표로는 안 보였고 심볼로지별 각도 스윕을 돌리고서야 나왔다.
#    산업 현장에서 품번을 잘못 읽는 것이라 미검출보다 나쁘다 — 그래서
#    이건 기준선 비교가 아니라 **0/100% 하드 게이트**로 건다.
#
# 코퍼스는 **스트리밍**이라 디스크에 이미지를 한 장도 안 남긴다(§3.8).
#
# 사용:
#   tools/ci_gate.sh                 # 측정 + 기준선과 비교
#   tools/ci_gate.sh --update-baseline   # 지금 값을 새 기준선으로 저장
#
# 환경변수:
#   BUILD_DIR   기본 build
#   CORPUS_N    코퍼스 장수 (기본 300)
#   BASELINE    기준선 파일 (기본 tools/ci_baseline.txt)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CORPUS_N="${CORPUS_N:-300}"
BASELINE="${BASELINE:-$ROOT/tools/ci_baseline.txt}"
UPDATE=0
[ "${1:-}" = "--update-baseline" ] && UPDATE=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/third_party/zxing-cpp/core:$BUILD_DIR/zbar_install/lib:${LD_LIBRARY_PATH:-}"
VERIFY="$WORK/verify_accuracy"

echo ">> [1/5] verify_accuracy 빌드"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$VERIFY"

echo ">> [2/5] 고정 40종 게이트"
python3 "$ROOT/tools/generate_stress_images.py" --outdir "$WORK/stress" >/dev/null
"$VERIFY" "$WORK/stress" > "$WORK/stress.txt" 2>/dev/null || true
LINE="$(grep "검출 성공" "$WORK/stress.txt" || true)"
echo "   $LINE"
# 세 경로(full / 2stage / 2stage-fast) 각각의 "N/40"을 뽑아 전부 기준치 이상인지 본다.
# 고정 문자열로 grep하던 것을 숫자 비교로 바꾼 이유: 검출이 **좋아져도**
# 실패했다(37/40 패턴이 38/40과 안 맞는다). 게이트는 회귀만 잡아야 한다.
STRESS_MIN="${STRESS_MIN:-38}"
for N in $(echo "$LINE" | grep -oE '[0-9]+/40' | cut -d/ -f1); do
  if [ "$N" -lt "$STRESS_MIN" ]; then
    echo "!! 40종 기준선(${STRESS_MIN}/40) 미달 — 검출 회귀 (${N}/40)"
    sed -n '1,50p' "$WORK/stress.txt"
    exit 1
  fi
done

echo ">> [3/5] 심볼로지 각도 스윕 (14종 x 0~90도 5도 간격, 디스크 0)"
SWEEP_FAIL=0
for S in $(python3 -c "import sys;sys.path.insert(0,'$ROOT/tools');import generate_corpus as g;print(' '.join(sorted(g._SWEEP_PAYLOAD)))"); do
  OUT=$(python3 "$ROOT/tools/generate_corpus.py" --sweep angle:0:90:5 \
          --base sym=$S,module=8,count=1 --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
        | "$VERIFY" --stdin --paths full --reps 1 2>/dev/null | grep -E "^full " | tr -s ' ')
  RATE_S=$(echo "$OUT" | cut -d' ' -f4)
  MD_S=$(echo "$OUT" | cut -d' ' -f5)
  if [ "$RATE_S" != "100.0%" ] || [ "${MD_S:-1}" != "0" ]; then
    echo "   !! $S 검출 $RATE_S / 오디코딩 $MD_S"
    SWEEP_FAIL=1
  fi
done
if [ "$SWEEP_FAIL" = 1 ]; then
  echo "!! 심볼로지 각도 스윕 실패 — 검출 100% 또는 오디코딩 0을 못 지켰다"
  exit 1
fi
echo "   14종 x 19각도 전부 100% / 오디코딩 0"

echo ">> [4/5] 고정 시드 코퍼스 ($CORPUS_N장, 디스크 0)"
python3 "$ROOT/tools/generate_corpus.py" -n "$CORPUS_N" --difficulty mixed \
        --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
  | "$VERIFY" --stdin --paths 2stage --reps 2 > "$WORK/corpus.txt" 2>/dev/null

# 8번째 줄: path | img_pass% | codes | text_ok% | misdec | dup | mean | p50 | p95
# 게이트 지표는 text_ok(코드 단위 텍스트 일치율) — 개수만 세는 것보다 엄격하다
read -r _ _ _ RATE MISDEC DUP MEAN _ P95 <<<"$(sed -n '8p' "$WORK/corpus.txt" | tr -s ' ')"
RATE="${RATE%\%}"
echo "   검출 ${RATE}% / 평균 ${MEAN}ms / p95 ${P95}ms / 오디코딩 ${MISDEC} / 중복 ${DUP}"

echo ">> [5/5] 기준선 비교"
if [ "$UPDATE" = 1 ] || [ ! -f "$BASELINE" ]; then
  printf 'rate=%s\nmean=%s\np95=%s\nmisdec=%s\ndup=%s\n' \
         "$RATE" "$MEAN" "$P95" "$MISDEC" "$DUP" > "$BASELINE"
  echo "   기준선 저장: $BASELINE"
  exit 0
fi

# shellcheck disable=SC1090
BASE_RATE=$(grep '^rate=' "$BASELINE" | cut -d= -f2)
BASE_MEAN=$(grep '^mean=' "$BASELINE" | cut -d= -f2)
BASE_P95=$(grep '^p95='  "$BASELINE" | cut -d= -f2)
BASE_DUP=$(grep '^dup='  "$BASELINE" | cut -d= -f2)

# 허용치: 검출률 -1.0%p, 평균/p95 +20%(측정 잡음이 10% 안팎이라 그 두 배),
#         중복은 0에서 늘어나면 무조건 실패(정확성 문제라 잡음 여지가 없다)
fail=0
awk -v a="$RATE" -v b="$BASE_RATE" 'BEGIN{exit !(a < b - 1.0)}' && {
  echo "!! 검출률 회귀: ${BASE_RATE}% -> ${RATE}%"; fail=1; }
awk -v a="$MEAN" -v b="$BASE_MEAN" 'BEGIN{exit !(a > b * 1.20)}' && {
  echo "!! 평균 시간 회귀: ${BASE_MEAN}ms -> ${MEAN}ms"; fail=1; }
awk -v a="$P95" -v b="$BASE_P95" 'BEGIN{exit !(a > b * 1.20)}' && {
  echo "!! p95 회귀: ${BASE_P95}ms -> ${P95}ms"; fail=1; }
[ "$DUP" -gt "$BASE_DUP" ] && { echo "!! 중복 반환 증가: ${BASE_DUP} -> ${DUP}"; fail=1; }

if [ "$fail" = 1 ]; then
  echo ">> ❌ 코퍼스 게이트 실패"
  exit 1
fi
echo ">> ✅ 통과 (40종 ${STRESS_MIN}/40 이상, 심볼로지 스윕 100%/오디코딩 0, 코퍼스 기준선 이내)"
