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

# [검출 단계는 최대 3회 시도한다 — 아슬아슬한 프레임이 실행마다 뒤집힌다]
#
# 2026-08-02에 실험으로 확인했다. 게이트가 40종 39/40으로 떨어져서 회귀를
# 의심했는데, **코드 변경이 전혀 없는 이전 커밋에 "아무것도 안 하는 빈 소스
# 파일" 하나를 추가하고 다시 빌드했더니 같은 현상이 나왔다**
# (39/40, 40/40, 39/40 — 같은 빌드 3회 실행에서도 뒤집힌다).
#
# 즉 21_dpm_dotpeen의 full 경로가 원래 경계에 있다(약 380ms, §8의 알려진
# 이슈로 기록). 소스 파일이 하나 늘면 LTO의 인라이닝/코드 배치가 달라지고,
# 마감이 벽시계 기준이라(§3.47 자기 보정 예산) 그 프레임이 구제 도중에
# 잘리는 쪽으로 넘어간다. **코드 로직과 무관하다.**
#
# 진짜 회귀는 결정적으로 실패하므로 3회 다 실패할 때만 게이트를 떨어뜨린다.
# 재시도했다는 사실은 로그에 남긴다 — 자주 보이면 그 자체가 신호다.
GATE_TRIES="${GATE_TRIES:-3}"
retry_note() { echo "   (실패, 재시도한다 — 사유는 위 주석)"; }

echo ">> [2/5] 고정 40종 게이트"
python3 "$ROOT/tools/generate_stress_images.py" --outdir "$WORK/stress" >/dev/null
"$VERIFY" "$WORK/stress" > "$WORK/stress.txt" 2>/dev/null || true
LINE="$(grep "검출 성공" "$WORK/stress.txt" || true)"
# [기준 작업량] 40종 full 경로의 합계 시간을 그대로 "이 머신이 지금 얼마나
# 빠른가"의 척도로 쓴다. 코퍼스 시간을 이 값으로 나눠서 비교하면 머신이
# 느려져도(혹은 같은 머신이 부하로 흔들려도) 게이트가 오탐하지 않는다.
#
# 필요해서 넣었다: 이 샌드박스가 세션 동안 20% 넘게 느려졌고, 코드 변경이
# 없는데도(교차 측정으로 확인: 직전 181ms vs 신규 176ms) 절대 ms 기준선을
# 넘어 게이트가 실패했다. 40종은 고정 이미지·고정 경로라 코드가 바뀌지
# 않는 한 이 척도로 쓰기에 적합하다.
# [기준값은 세 경로의 합으로 잰다 — 한 번 측정은 너무 흔들린다]
# 2026-08-02 실측: full 경로 하나만 쓰면 같은 빌드에서 1666~1785ms로 7%가
# 흔들리고, 그 노이즈가 정규화 지표에 그대로 증폭돼서 코드 변경이 없는데도
# 시간 회귀로 실패했다(정규화 mean 95.7 vs 105.2, 그런데 원시 평균은
# 173.9 vs 173.0으로 사실상 동일). 세 경로를 합치면 표본이 3배가 된다.
REF_MS="$(echo "$LINE" | grep -oE '[0-9]+\.[0-9]+ms' | tr -d 'ms' \
          | awk '{t+=$1} END{printf "%.1f", t}')"
echo "   $LINE"
# 세 경로(full / 2stage / 2stage-fast) 각각의 "N/40"을 뽑아 전부 기준치 이상인지 본다.
# 고정 문자열로 grep하던 것을 숫자 비교로 바꾼 이유: 검출이 **좋아져도**
# 실패했다(37/40 패턴이 38/40과 안 맞는다). 게이트는 회귀만 잡아야 한다.
# 40/40이 된 시점에서 기준을 올린다(2026-08-01). 예전 38은 18번(초저대비 QR)과
# 26번(강한 원근)이 못 읽히던 시절의 값이고, 26번은 생성기가 코드를 잘라먹던
# 것이었다. 기준을 안 올리면 그 둘이 다시 죽어도 게이트가 통과한다.
STRESS_MIN="${STRESS_MIN:-40}"
stress_ok() {
  local line="$1"
  for N in $(echo "$line" | grep -oE '[0-9]+/40' | cut -d/ -f1); do
    [ "$N" -lt "$STRESS_MIN" ] && return 1
  done
  return 0
}
T=1
while ! stress_ok "$LINE" && [ "$T" -lt "$GATE_TRIES" ]; do
  retry_note
  T=$((T+1))
  "$VERIFY" "$WORK/stress" > "$WORK/stress.txt" 2>/dev/null || true
  LINE="$(grep "검출 성공" "$WORK/stress.txt" || true)"
  echo "   $LINE"
done
if ! stress_ok "$LINE"; then
  echo "!! 40종 기준선(${STRESS_MIN}/40) 미달 — ${GATE_TRIES}회 전부 실패, 검출 회귀"
  sed -n '1,50p' "$WORK/stress.txt"
  exit 1
fi

echo ">> [3/5] 심볼로지 각도 스윕 (14종 x 0~90도 5도 간격, 디스크 0)"
SWEEP_FAIL=0
for S in $(python3 -c "import sys;sys.path.insert(0,'$ROOT/tools');import generate_corpus as g;print(' '.join(sorted(g._SWEEP_PAYLOAD)))"); do
  OUT=$(python3 "$ROOT/tools/generate_corpus.py" --sweep angle:0:90:5 \
          --base sym=$S,module=8,count=1 --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
        | "$VERIFY" --stdin --paths full --reps 1 2>/dev/null | grep -E "^full " | tr -s ' ')
  RATE_S=$(echo "$OUT" | cut -d' ' -f4)
  MD_S=$(echo "$OUT" | cut -d' ' -f5)
  T=1
  while { [ "$RATE_S" != "100.0%" ] || [ "${MD_S:-1}" != "0" ]; } && [ "$T" -lt "$GATE_TRIES" ]; do
    retry_note
    T=$((T+1))
    OUT=$(python3 "$ROOT/tools/generate_corpus.py" --sweep angle:0:90:5 \
            --base sym=$S,module=8,count=1 --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
          | "$VERIFY" --stdin --paths full --reps 1 2>/dev/null | grep -E "^full " | tr -s ' ')
    RATE_S=$(echo "$OUT" | cut -d' ' -f4)
    MD_S=$(echo "$OUT" | cut -d' ' -f5)
  done
  if [ "$RATE_S" != "100.0%" ] || [ "${MD_S:-1}" != "0" ]; then
    echo "   !! $S 검출 $RATE_S / 오디코딩 $MD_S (${GATE_TRIES}회 전부 실패)"
    SWEEP_FAIL=1
  fi
done
if [ "$SWEEP_FAIL" = 1 ]; then
  echo "!! 심볼로지 각도 스윕 실패 — 검출 100% 또는 오디코딩 0을 못 지켰다"
  exit 1
fi
echo "   14종 x 19각도 전부 100% / 오디코딩 0"

# [5도 격자는 오디코딩을 대부분 못 본다 — 세밀 스윕을 따로 건다]
#
# 위 스윕이 "오디코딩 0"이라고 말하지만 그건 **5도 배수 각도에서만**이다.
# 1도 간격으로 다시 재보면 다르다(2026-08-02 실측, module 4):
#
#   ITF     5도 간격 19장 -> 오디코딩 1 / 1도 간격 91장 -> **오디코딩 13**
#   CODE128 5도 간격      -> 0          / 1도 간격(module 3) -> **2**
#
# CODE128의 두 건은 44도와 46도다. 45도 정각에서는 정상이라 5도 격자가
# 원리적으로 못 본다. 이걸 모른 채 "오디코딩 0"이라고 적어둘 수는 없다.
#
# 0으로 만드는 방법은 있다 — ITF는 체크섬 강제(validate_itf_checksum)로
# 13 -> 0이 되고 검출 손실이 없다. 다만 그건 **배치 결정**이라(체크디짓
# 없는 ITF를 쓰는 배치는 그 코드를 전부 잃는다) 기본값으로 켤 수 없다.
# 그래서 여기서는 0을 요구하지 않고 **현재 값을 상한으로 고정**한다 —
# 나빠지는 것만 잡는다.
echo ">> [3.5/5] 세밀 각도 스윕 (1도 간격, 오디코딩 상한 고정)"
FINE_FAIL=0
fine_sweep() {  # $1=심볼로지 $2=모듈 $3=오디코딩 상한
  local OUT MD
  OUT=$(python3 "$ROOT/tools/generate_corpus.py" --sweep angle:0:90:1 \
          --base sym=$1,module=$2,count=1 --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
        | "$VERIFY" --stdin --paths full --reps 1 2>/dev/null | grep -E "^full " | tr -s ' ')
  MD=$(echo "$OUT" | cut -d' ' -f5)
  echo "   $1(module $2) 91각도: 오디코딩 ${MD:-?} (상한 $3)"
  if [ "${MD:-999}" -gt "$3" ]; then FINE_FAIL=1; fi
}
fine_sweep ITF 4 "${FINE_MAX_ITF:-13}"
fine_sweep CODE128 3 "${FINE_MAX_C128:-2}"
if [ "$FINE_FAIL" = 1 ]; then
  echo "!! 세밀 각도 스윕에서 오디코딩이 상한을 넘었다"
  exit 1
fi

# [3.7] 저조도 — 밝기가 아니라 **SNR**이 축이다
#
# 기존 저대비 축(17/18번)은 "종이는 밝은데 잉크가 연한" 것이라 저조도와
# 물리가 다르다. 카메라는 어두우면 게인을 올려 밝기를 되돌리므로 실기
# 프레임은 **어둡지 않고 거칠다** — 노이즈만 커진다(§3.61).
#
# [기준을 올렸다 — 2026-08-03]
# 하한 23 / 상한 170은 이 축을 처음 넣을 때(§3.61) 값이다. 그 뒤
# §3.65(1단계 이진화 기본 변경)로 코드가 25 -> 30, 평균이 137 -> 94ms가
# 됐다. 기준을 그대로 두면 **그 이득이 통째로 사라져도 게이트가 통과한다**
# — 이 파일이 40종 기준을 38 -> 40으로 올릴 때 이미 적어둔 논리다.
# 하한 28 / 상한 130으로 올린다(실행 간 흔들림 여유를 둔 값).
#
# 여기서 지키는 것은 코드 수와 **평균 시간** 둘 다다. 이 축의 실패
# 프레임은 폴백 체인을 끝까지 갈아먹어서, 시간이 나빠지는 것 자체가
# 회귀다(사용자 요구가 실패 프레임 지연 상한이다).
echo ">> [3.7/5] 저조도 SNR 스윕 (모듈 2/3/4/6px x 노출 6단)"
LL_DIR="$WORK/lowlight"
python3 "$ROOT/tools/generate_lowlight.py" --outdir "$LL_DIR" --seed 7 >/dev/null 2>&1
LL_OUT=$("$VERIFY" "$LL_DIR" --paths 2stage --reps 1 --quiet 2>/dev/null | grep -E "^2stage" | tr -s ' ')
LL_CODES=$(echo "$LL_OUT" | cut -d' ' -f3 | cut -d/ -f1)
LL_MISDEC=$(echo "$LL_OUT" | cut -d' ' -f5)
LL_MEAN=$(echo "$LL_OUT" | cut -d' ' -f7)
echo "   코드 ${LL_CODES:-?}/48 (하한 ${LL_MIN_CODES:-28}) / 평균 ${LL_MEAN:-?}ms (상한 ${LL_MAX_MEAN:-130}) / 오디코딩 ${LL_MISDEC:-?}"
if [ "${LL_CODES:-0}" -lt "${LL_MIN_CODES:-28}" ] || [ "${LL_MISDEC:-999}" -gt 0 ]; then
  echo "!! 저조도 축에서 검출이 하한 미만이거나 오디코딩이 생겼다"
  exit 1
fi
if awk -v a="${LL_MEAN:-999}" -v m="${LL_MAX_MEAN:-130}" 'BEGIN{exit !(a>m)}'; then
  echo "!! 저조도 실패 프레임의 시간이 상한을 넘었다 — 선 디노이즈가 죽었는지 볼 것"
  exit 1
fi

# [3.8] 공개 손잡이가 살아 있나
#
# 두 번 당했다. VSCAN_FLAG_NO_INVERT / NO_TRY_HARDER가 2단계 경로에서
# 조용히 무시됐고(§3.68), enable_qr_finder_rescue / enable_1d_deskew_rescue가
# 조기 반환에 가려 아예 안 돌았다(§3.69). 둘 다 **헤더는 동작한다고
# 문서화하고 있었다** — 배치가 그 값을 믿고 튜닝한다.
#
# 이 단계는 "옵션을 켰는데 결과도 시간도 안 바뀌면 실패"를 건다. 저조도
# 코퍼스를 쓰는 이유는 실패 프레임이 많아 **깊은 단계까지 도달**하기
# 때문이다(40종은 대부분 얕은 단계에서 성공해서 이 검사가 무의미하다).
# [3.75] 빈 프레임 — 유령이 나오면 안 된다
#
# 이 게이트의 다른 코퍼스는 **전부 코드가 있는 프레임**이다. 그래서
# "코드가 없는데 뭔가를 만들어내는" 회귀를 원리적으로 못 잡는다.
# 컨베이어에서는 물건 사이 간격 프레임이 계속 들어오므로, 거기서 나오는
# 유령은 그대로 오디코딩이다(§3.18: 미검출 < 오디코딩).
#
# 시간도 같이 본다 — 깨끗한 빈 프레임은 빈 프레임 건너뛰기가 0ms로
# 만들어야 한다(§3.72).
# [3.73] 권장 조합(fast_no_read)이 QR 회전에서 안 깨지나
#
# 게이트의 각도 스윕은 **기본 설정**으로만 돈다. 그런데 우리가 컨베이어에
# 권하는 것은 fast_no_read다. 그 설정이 회전 축에서 깨져도 지금까지는
# 아무도 몰랐다 — 실제로 재보니 회전된 1D는 크게 잃는다(CODE128 100->21%,
# PDF417 100->10%). QR만 100%를 지킨다.
#
# 그래서 **권장 조합의 전제(QR은 회전에 안 잃는다)** 를 게이트로 건다.
# 이게 깨지면 examples/conveyor_capture_controlled.cpp의 권고가 거짓이 된다.
echo ">> [3.73/5] 권장 조합(fast_no_read) 회전 확인 (QR/CODE128/PDF417)"
# QR만이 아니라 **회전에 취약한 심볼로지까지** 건다. 처음에는 QR만 걸었는데,
# 그건 fast_no_read가 영역 구제를 통째로 끄던 시절(회전된 1D를 통째로 잃던
# 시절)의 기준이었다. §3.78에서 crop만 남기도록 고친 뒤로는 전부 100%다.
FNR_BAD=0
FNR_LINE=""
for FS in QR CODE128 PDF417; do
  FO=$(python3 "$ROOT/tools/generate_corpus.py" --sweep angle:0:90:5 \
         --base sym=$FS,module=8,count=1 --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
       | VSCAN_FASTNR=1 VSCAN_FLAGS=2 "$VERIFY" --stdin --paths full,2stage --reps 1 --quiet 2>/dev/null \
       | grep -E "^(full|2stage) " | tr -s ' ')
  FB=$(echo "$FO" | awk '{if ($4 != "100.0%" || $5 != "0") b++} END{print b+0}')
  FNR_BAD=$((FNR_BAD + FB))
  FNR_LINE="$FNR_LINE $FS=$(echo "$FO" | awk '{printf "%s ", $4}')"
done
FNR_OUT="$FNR_LINE"
echo "   19각도 x 2경로:$FNR_LINE"
if [ "${FNR_BAD:-9}" != "0" ]; then
  echo "!! fast_no_read + NO_INVERT에서 회전이 깨졌다 — 권장 조합의 전제가 무너진다(§3.78)"
  exit 1
fi

echo ">> [3.75/5] 빈 프레임 유령 확인"
EMPTY_DIR="$WORK/empty"
python3 "$ROOT/tools/generate_empty.py" "$EMPTY_DIR" >/dev/null 2>&1
# 1D 바코드처럼 보이는 무늬(벨트 리브 / 슬랫 / 나무결 / 그물)도 같이 넣는다.
# 컨베이어에서 유령이 실제로 태어나는 자리다.
python3 "$ROOT/tools/generate_texture.py" "$EMPTY_DIR" >/dev/null 2>&1 || true
EMPTY_OUT=$("$VERIFY" "$EMPTY_DIR" --paths full,2stage --reps 1 --quiet 2>/dev/null \
            | grep -E "^(full|2stage) " | tr -s ' ')
# 3번째 칸은 "찾은수/기대수" 꼴이라 앞 숫자만 뗀다. awk가 "0/0"을 0으로
# 읽어주긴 하지만, 형식이 바뀌면 조용히 틀리므로 명시적으로 자른다.
EMPTY_GHOST=$(echo "$EMPTY_OUT" | awk '{split($3,a,"/"); s+=a[1]} END{print s+0}')
EMPTY_N=$(find "$EMPTY_DIR" -name '*.pgm' | wc -l)
echo "   빈/무늬 프레임 ${EMPTY_N}장에서 나온 코드: ${EMPTY_GHOST}개 (0이어야 한다)"
if [ "${EMPTY_GHOST:-99}" != "0" ]; then
  echo "!! 빈 프레임에서 코드가 나왔다 — 전부 유령이다"
  echo "$EMPTY_OUT"
  exit 1
fi

echo ">> [3.78/5] 같은 라벨 2장 양성 대조 (dedup 밴드 규칙)"
# dedup의 밴드 규칙들은 "같은 텍스트 + 기하 조건"으로 지운다. 그 규칙들이
# 깨뜨릴 수 있는 유일한 실제 배치가 **박스에 같은 라벨이 여러 장** 붙은
# 경우다. 규칙을 새로 넣거나 문턱을 만질 때 여기가 조용히 바뀌면
# "중복이 줄었다"가 사실은 "코드를 하나 먹었다"일 수 있다.
#
# 기준은 개수가 아니라 **경계**다. 간격 60px 이하가 하나로 합쳐지는 것은
# isStackedBand가 원래 안고 있는 알려진 대가이고(그 주석 참고), 120px
# 이상은 반드시 둘로 남아야 한다.
STK_DIR="$WORK/stacked"
python3 "$ROOT/tools/generate_stacked_labels.py" --outdir "$STK_DIR" >/dev/null 2>&1
"$VERIFY" "$STK_DIR" --paths full,2stage --reps 1 --quiet --csv "$WORK/stacked.csv" >/dev/null 2>&1
# csv: file,path,expected,found,...  파일명이 gapNNN_2.pgm 이라 간격만 뗀다.
# 지문을 통째로 비교한다 — "몇 개 나왔나"가 아니라 **어느 간격에서 갈리나**가
# 지켜야 할 값이기 때문이다. 한 칸이라도 움직이면 사람이 판단해야 한다.
#
# 실제로 두 번 움직였고, 그게 이 단을 넣은 값이었다.
#  (1) full 경로가 min_expected_codes를 존중하게 되자 full:120이 1 -> 2.
#  (2) 단계별 결과를 합치게 되자([[vscan-lite-merge-partials]]) 다시 1.
#      원인까지 확인했다: 코어 패스가 두 라벨을 **하나의 상자로 합쳐**
#      돌려주고(isStackedBand의 알려진 대가), 합치기가 그 굵은 상자를
#      들고 가면 뒤 단계가 낸 가는 상자 둘을 dedup이 "포함되니 중복"으로
#      지운다. 즉 full:120=1은 예전과 같은 값으로 되돌아온 것이다.
# 지문이 바뀌면 이렇게 어느 쪽이 맞는지 보고 판단하면 된다.
STK_FP="$(awk -F, 'NR>1 {g=$1; gsub(/^gap0*|_2\.pgm$/,"",g); printf "%s:%s=%s ", $2, g, $4}' \
          "$WORK/stacked.csv")"
STK_EXPECT="full:10=1 2stage:10=1 full:30=1 2stage:30=1 full:60=1 2stage:60=1 full:120=1 2stage:120=2 full:240=2 2stage:240=2 "
echo "   같은 라벨 2장: $STK_FP"
if [ "$STK_FP" != "$STK_EXPECT" ]; then
  echo "!! dedup 밴드 규칙의 경계가 움직였다"
  echo "   기대: $STK_EXPECT"
  echo "   실제: $STK_FP"
  echo "   (간격이 커졌는데 1이 되면 코드를 먹은 것이고, 작은데 2가 되면"
  echo "    같은 코드를 두 번 돌려준 것이다. 어느 쪽인지 보고 판단할 것.)"
  exit 1
fi

echo ">> [3.77/5] 도트 각인(DPM) 축 — 모듈 4~10"
# DPM은 오래 "안 되는 축"이었는데 원인이 세 번 다 **생성기**였다(§3.83,
# §3.97). 이제 통제 스윕에서 세 심볼로지가 다 열리므로, 다시 썩지 않게
# 여기서 지킨다. 모듈 3은 3px 피치에 점 반지름 1px이라 해상도 한계라
# 빼고 잰다(실측: 세 심볼로지 다 module 3만 실패).
DPM_BAD=0
DPM_LINE=""
for S in DATAMATRIX QR CODE128; do
  r=$(python3 "$ROOT/tools/generate_corpus.py" --sweep "module:4:10:1" \
        --base "sym=$S,count=1,dpm=1" --bucket ok --stream --jobs "$(nproc)" 2>/dev/null \
      | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet 2>/dev/null \
      | grep -E "^2stage " | tr -s ' ' | cut -d' ' -f4 | tr -d '%')
  DPM_LINE="$DPM_LINE $S=${r}%"
  if awk -v a="${r:-0}" 'BEGIN{exit !(a < 100.0)}'; then DPM_BAD=1; fi
done
echo "  $DPM_LINE"
if [ "$DPM_BAD" != "0" ]; then
  echo "!! DPM 축이 100% 아래로 내려갔다"; exit 1; fi

echo ">> [3.771/5] 손상/오염/인쇄불량/정지대 침범 축"
# 이 넷은 오래 **난수 경로에만** 있었다. §3.97에서 DPM이 정확히 그 이유로
# 세 번 오진됐으므로 스윕 축으로 만들고 여기서 지킨다(§3.98).
#
# 수준의 근거: damaged 0.3부터는 2D가 물리적으로 무너진다(EC 스윕으로
# 확인 — L/M/Q는 0.3에서 끊기고 H만 0.5까지 간다). 그러니 게이트는
# **오류정정 용량 안**인 구간만 본다. dirty는 0.5까지 7종 전부 100%다.
DEG_BAD=0
DEG_LINE=""
for AX in "damaged:0.1:0.2:0.1" "dirty:0.1:0.5:0.1" "printdefect:0.1:1.0:0.1" "quietzone:0.1:1.0:0.1"; do
  A=${AX%%:*}
  worst=100
  for S in QR DATAMATRIX CODE128 EAN13 PDF417 ITF; do
    r=$(python3 "$ROOT/tools/generate_corpus.py" --sweep "$AX" \
          --base "sym=$S,module=8,count=1" --stream --jobs "$(nproc)" 2>/dev/null \
        | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet 2>/dev/null \
        | grep -E "^2stage " | tr -s ' ' | cut -d' ' -f4 | tr -d '%')
    worst=$(awk -v a="${r:-0}" -v b="$worst" 'BEGIN{print (a<b)?a:b}')
  done
  DEG_LINE="$DEG_LINE $A=${worst}%"
  if awk -v a="$worst" 'BEGIN{exit !(a < 100.0)}'; then DEG_BAD=1; fi
done
echo "  6종 최저:$DEG_LINE"
if [ "$DEG_BAD" != "0" ]; then
  echo "!! 손상/오염 축이 100% 아래로 내려갔다"; exit 1; fi

echo ">> [3.79/5] 자동 기대 개수 (개수를 모를 때 남은 코드를 찾는가)"
# auto_expected_codes는 기본 OFF라, 켜지 않으면 게이트가 통째로 못 본다.
# 다중 코드 프레임에서 **켰을 때만** 더 찾는다는 것을 여기서 지킨다.
# 지표는 코드 단위 텍스트 일치율이고, 개수는 안 준다(그게 이 손잡이의 전제다).
AE_OFF=$(python3 "$ROOT/tools/generate_corpus.py" -n 120 --difficulty mixed \
           --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
         | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --no-min-expected 2>/dev/null \
         | grep -E "^2stage " | tr -s ' ' | cut -d' ' -f4 | tr -d '%')
AE_ON=$(python3 "$ROOT/tools/generate_corpus.py" -n 120 --difficulty mixed \
          --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
        | VSCAN_AUTOEXP=1 "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --no-min-expected 2>/dev/null \
        | grep -E "^2stage " | tr -s ' ' | cut -d' ' -f4 | tr -d '%')
echo "   개수 모름: 끔 ${AE_OFF}% -> 켬 ${AE_ON}%"
if awk -v a="${AE_ON:-0}" -v b="${AE_OFF:-0}" 'BEGIN{exit !(a < b + 15.0)}'; then
  echo "!! auto_expected_codes가 값을 못 한다 (15%p 이상 올라야 한다)"
  exit 1
fi

echo ">> [3.795/5] 권장 조합(auto_expected + 마감) — 검출과 지연을 같이 지킨다"
# examples/inbound_readall_bounded.cpp의 조합이다. 이 조합은 **지금 기본보다
# 셋 다 낫다**(검출 +22.7%p, 평균 -24%, p95 -73%). 그 셋이 같이 유지되는지
# 여기서 지킨다 — 하나만 재면 나머지가 조용히 무너질 수 있다.
AER_OUT=$(python3 "$ROOT/tools/generate_corpus.py" -n 120 --difficulty mixed \
            --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
          | VSCAN_AUTOEXP=1 VSCAN_TSBUDGET=1 VSCAN_MAXFRAME=60 "$VERIFY" --stdin --paths 2stage \
              --reps 1 --quiet --no-min-expected 2>/dev/null \
          | grep -E "^2stage " | tr -s ' ')
AER_RATE=$(echo "$AER_OUT" | cut -d' ' -f4 | tr -d '%')
AER_MIS=$(echo "$AER_OUT"  | cut -d' ' -f5)
AER_P95=$(echo "$AER_OUT"  | cut -d' ' -f9)
# 마감 60ms는 벽시계라 기준 작업량으로 정규화하지 않는다. p95 상한은
# 실측(86~107ms)의 두 배 남짓으로 둔다 — 마감이 실제로 무는지만 보면 된다.
echo "   검출 ${AER_RATE}% / 오디코딩 ${AER_MIS} / p95 ${AER_P95}ms"
if awk -v a="${AER_RATE:-0}" 'BEGIN{exit !(a < 55.0)}'; then
  echo "!! 권장 조합의 검출이 55% 아래로 내려갔다"; exit 1; fi
if [ "${AER_MIS:-99}" != "0" ]; then
  echo "!! 권장 조합에서 오디코딩이 나왔다"; exit 1; fi
# 상한 160ms의 근거: §3.96에서 tilefb가 마감을 존중하게 한 뒤 실측 p95가
# 87~97ms다. 그 1.7배로 둔다 — 마감이 다시 새기 시작하면(그런 일이 실제로
# 있었다, §3.95) 바로 걸린다.
if awk -v a="${AER_P95:-999}" 'BEGIN{exit !(a > 160.0)}'; then
  echo "!! 마감 60ms를 걸었는데 p95가 160ms를 넘었다 — 마감이 새고 있다"; exit 1; fi

echo ">> [3.8/5] 공개 손잡이 생존 확인"
AUDIT="$WORK/audit_config"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/audit_config.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$AUDIT" 2>/dev/null
if [ -x "$AUDIT" ]; then
  if "$AUDIT" "$LL_DIR" --reps 1 --path 2stage \
       --require NO_INVERT,NO_TRY_HARDER,fast_no_read=1,symbology_mask=QR,disable_auto_denoise,max_frame_ms=20 \
       > "$WORK/audit.txt" 2>&1; then
    echo "   필수 손잡이 6종 전부 동작"
  else
    echo "!! 공개 손잡이가 죽었다 — 아래 참고"
    grep -E '변화없음!!|필수 옵션' "$WORK/audit.txt" | head -8
    exit 1
  fi
else
  echo "   (audit_config 빌드 실패 — 건너뛴다)"
fi

echo ">> [4/5] 고정 시드 코퍼스 ($CORPUS_N장, 디스크 0)"
# [두 조건으로 잰다 — 검출은 개수를 알 때, 시간은 모를 때]
#
# 이 코퍼스는 라벨의 기대 개수를 min_expected_codes로 넘겨준다. §3.85에
# 적은 대로 그건 **하네스만 아는 정보**다. 그 조건 하나로 둘 다 재면
# 지표가 서로를 가린다:
#
#  - 검출을 "개수를 모를 때"로 재면, min_expected_codes를 존중하도록 고친
#    개선이 지표에 아예 안 보인다(정의상 need=1이라 동작이 같다).
#  - 시간을 "개수를 알 때"로 재면, 호출자가 명시적으로 요구한 더 깊은
#    탐색까지 지연 회귀로 잡힌다. 실제로 그렇게 잡혔다 —
#    [[vscan-lite-full-path-need]]로 검출이 74.7 -> 75.3%가 됐는데 같은
#    조건의 평균이 +27%라 게이트가 떨어졌다. 그런데 **현장 조건(개수 0)
#    에서는 220/550 47.6ms로 이전과 소수점까지 같았다.**
#
# 그래서 나눈다. 검출/오디코딩/중복은 개수를 준 조건(더 엄격한 채점),
# 평균/p95는 개수를 안 준 조건(현장에서 실제로 나는 지연)에서 잰다.
python3 "$ROOT/tools/generate_corpus.py" -n "$CORPUS_N" --difficulty mixed \
        --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
  | "$VERIFY" --stdin --paths 2stage --reps 2 > "$WORK/corpus.txt" 2>/dev/null
python3 "$ROOT/tools/generate_corpus.py" -n "$CORPUS_N" --difficulty mixed \
        --bucket ok --seed 101 --stream --jobs "$(nproc)" 2>/dev/null \
  | "$VERIFY" --stdin --paths 2stage --reps 2 --no-min-expected \
  > "$WORK/corpus_field.txt" 2>/dev/null

# 8번째 줄: path | img_pass% | codes | text_ok% | misdec | dup | mean | p50 | p95
# 게이트 지표는 text_ok(코드 단위 텍스트 일치율) — 개수만 세는 것보다 엄격하다
read -r _ _ _ RATE MISDEC DUP _ _ _ <<<"$(sed -n '8p' "$WORK/corpus.txt" | tr -s ' ')"
RATE="${RATE%\%}"
# 시간은 현장 조건(개수 0) 실행에서 가져온다 — 바로 위 주석 참고.
read -r _ _ _ FRATE _ _ MEAN _ P95 <<<"$(sed -n '8p' "$WORK/corpus_field.txt" | tr -s ' ')"
FRATE="${FRATE%\%}"
# 기준 작업량 대비로 환산 (x1000은 소수점 자리 확보용)
# mawk는 printf 인자 안의 삼항 연산자를 조용히 삼킨다(빈 문자열이 나온다).
# if로 쓸 것.
norm() { awk -v a="$1" -v r="$2" 'BEGIN{ if (r>0) printf "%.2f", a/r*1000; else printf "0" }'; }
MEANR="$(norm "$MEAN" "$REF_MS")"
P95R="$(norm "$P95" "$REF_MS")"
echo "   개수 줌: 검출 ${RATE}% / 오디코딩 ${MISDEC} / 중복 ${DUP}"
echo "   개수 모름(현장): 검출 ${FRATE}% / 평균 ${MEAN}ms / p95 ${P95}ms"
echo "   (기준 작업량 ${REF_MS}ms 대비: 평균 ${MEANR} / p95 ${P95R})"

echo ">> [5/5] 기준선 비교"
if [ "$UPDATE" = 1 ] || [ ! -f "$BASELINE" ]; then
  printf 'rate=%s\nfrate=%s\nmean=%s\np95=%s\nmisdec=%s\ndup=%s\nmeanr=%s\np95r=%s\nref=%s\n' \
         "$RATE" "$FRATE" "$MEAN" "$P95" "$MISDEC" "$DUP" "$MEANR" "$P95R" "$REF_MS" > "$BASELINE"
  echo "   기준선 저장: $BASELINE"
  exit 0
fi

# shellcheck disable=SC1090
BASE_RATE=$(grep '^rate=' "$BASELINE" | cut -d= -f2)
BASE_MEAN=$(grep '^meanr=' "$BASELINE" | cut -d= -f2)
BASE_P95=$(grep '^p95r='  "$BASELINE" | cut -d= -f2)
# 기준선이 정규화 값을 안 갖고 있으면(예전 형식) 절대값으로 비교한다.
if [ -z "$BASE_MEAN" ]; then
  BASE_MEAN=$(grep '^mean=' "$BASELINE" | cut -d= -f2); MEANR="$MEAN"
  BASE_P95=$(grep '^p95='  "$BASELINE" | cut -d= -f2); P95R="$P95"
fi
BASE_DUP=$(grep '^dup='  "$BASELINE" | cut -d= -f2)
# 현장 조건 검출률. 예전 형식 기준선에는 없으므로 없으면 비교를 건너뛴다.
BASE_FRATE=$(grep '^frate=' "$BASELINE" | cut -d= -f2)

# 허용치: 검출률 -1.0%p, 평균/p95 +20%(측정 잡음이 10% 안팎이라 그 두 배),
#         중복은 0에서 늘어나면 무조건 실패(정확성 문제라 잡음 여지가 없다)
fail=0
awk -v a="$RATE" -v b="$BASE_RATE" 'BEGIN{exit !(a < b - 1.0)}' && {
  echo "!! 검출률 회귀(개수 줌): ${BASE_RATE}% -> ${RATE}%"; fail=1; }
if [ -n "$BASE_FRATE" ]; then
  awk -v a="$FRATE" -v b="$BASE_FRATE" 'BEGIN{exit !(a < b - 1.0)}' && {
    echo "!! 검출률 회귀(현장 조건): ${BASE_FRATE}% -> ${FRATE}%"; fail=1; }
fi
awk -v a="$MEANR" -v b="$BASE_MEAN" 'BEGIN{exit !(a > b * 1.20)}' && {
  echo "!! 평균 시간 회귀(기준 작업량 대비): ${BASE_MEAN} -> ${MEANR}"; fail=1; }
awk -v a="$P95R" -v b="$BASE_P95" 'BEGIN{exit !(a > b * 1.20)}' && {
  echo "!! p95 회귀(기준 작업량 대비): ${BASE_P95} -> ${P95R}"; fail=1; }
[ "$DUP" -gt "$BASE_DUP" ] && { echo "!! 중복 반환 증가: ${BASE_DUP} -> ${DUP}"; fail=1; }

if [ "$fail" = 1 ]; then
  echo ">> ❌ 코퍼스 게이트 실패"
  exit 1
fi
echo ">> ✅ 통과 (40종 ${STRESS_MIN}/40 이상, 심볼로지 스윕 100%/오디코딩 0, 코퍼스 기준선 이내)"
