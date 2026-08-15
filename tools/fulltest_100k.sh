#!/usr/bin/env bash
# fulltest_100k.sh — **전 심볼로지 x 전 조건 격자**를 10만 장 규모로 돌린다.
#
# 이 저장소의 평소 측정은 300~600장짜리 표본이다. 그 크기에서는 심볼로지
# 하나가 특정 조건 구간에서 통째로 죽어도 전체 평균에 묻힌다 — 실제로
# 그런 일이 여러 번 있었다(§3.29 원근, §3.41 반전, §3.97 DPM, §3.98 정지대).
# 이 스크립트는 **묻힐 자리를 없애려고** 축을 조밀하게 훑는다.
#
# 디스크는 0이다. 생성기가 stdout으로 프레임을 흘리고 verify_accuracy가
# stdin으로 받는다(§3.8). 10만 장 x 3MB = 300GB를 안 쓴다.
#
# 결과는 `$OUT`에 심볼로지·격자별 CSV와 요약 한 줄씩 쌓인다. 중간에 죽어도
# 그때까지 것은 유효하다.
#
#   tools/fulltest_100k.sh [출력디렉터리]
#
# 환경변수:
#   FT_JOBS     생성 병렬도 (기본 nproc)
#   FT_MODULES  격자를 돌릴 기준 모듈 크기 (기본 "4 8")
#   FT_RANDOM_N 난수 혼합 코퍼스 장수 (기본 40000)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/fulltest-out}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${FT_JOBS:-$(nproc)}"
MODULES="${FT_MODULES:-4 8}"
RANDOM_N="${FT_RANDOM_N:-46000}"

export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/third_party/zxing-cpp/core:$BUILD_DIR/zbar_install/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "$OUT"
VERIFY="$OUT/verify_accuracy"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$VERIFY" || exit 1

SYMS="$(python3 -c "import sys;sys.path.insert(0,'$ROOT/tools');import generate_corpus as g;print(' '.join(sorted(g._SWEEP_PAYLOAD)))")"

SUMMARY="$OUT/summary.tsv"
[ -f "$SUMMARY" ] || printf 'grid\tsym\tmodule\tframes\tcodes_found\tcodes_total\trate\tmisdec\tdup\tmean_ms\tp95_ms\n' > "$SUMMARY"

# 한 격자를 돌리고 요약 한 줄을 남긴다. 축은 여러 개를 곱한다(데카르트 곱).
run_grid() {  # $1=격자이름 $2=심볼로지 $3=모듈 $4.. = --sweep 인자들
  local name="$1" sym="$2" mod="$3"; shift 3
  local tag="${name}_${sym}_m${mod}"
  local csv="$OUT/${tag}.csv"
  [ -s "$csv" ] && return 0                       # 이미 돈 격자는 건너뛴다(재개 가능)
  local sweeps=()
  for a in "$@"; do sweeps+=(--sweep "$a"); done
  local line
  line="$(python3 "$ROOT/tools/generate_corpus.py" "${sweeps[@]}" \
            --base "sym=$sym,module=$mod,count=1" --stream --jobs "$JOBS" 2>/dev/null \
          | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --csv "$csv" 2>/dev/null \
          | grep -E '^2stage ' | tr -s ' ')"
  [ -z "$line" ] && { echo "  !! $tag 실패"; return 1; }
  local codes rate mis dup mean p95 frames
  codes="$(echo "$line" | cut -d' ' -f3)"
  rate="$(echo "$line" | cut -d' ' -f4 | tr -d '%')"
  mis="$(echo "$line" | cut -d' ' -f5)"
  dup="$(echo "$line" | cut -d' ' -f6)"
  mean="$(echo "$line" | cut -d' ' -f7)"
  p95="$(echo "$line" | cut -d' ' -f9)"
  frames="$(( $(wc -l < "$csv") - 1 ))"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$sym" "$mod" "$frames" "${codes%%/*}" "${codes##*/}" "$rate" "$mis" "$dup" "$mean" "$p95" \
    >> "$SUMMARY"
  printf '  %-26s %-12s m%-3s %6s장  %-11s %6s%%  오디%s\n' "$name" "$sym" "$mod" "$frames" "$codes" "$rate" "$mis"
}

echo "== 10만 장 풀테스트 시작 =="
echo "   출력: $OUT   병렬 $JOBS   심볼로지: $(echo $SYMS | wc -w)종"
START=$(date +%s)

# --- 격자 정의 -----------------------------------------------------------
# [주의] 격자에는 `--bucket` 필터를 걸지 않는다. 물리적으로 불가능한 모서리
# (모듈 2px + 대비 0.05 + 노이즈 60 같은)를 **일부러 포함**한다 — 목적이
# "합격률을 좋게 내는 것"이 아니라 **어디서 끊기는지 경계를 그리는 것**이라
# 그렇다. 그래서 격자의 검출률 절대값은 낮게 나오는 것이 정상이고, 봐야 할
# 것은 심볼로지·축 사이의 **차이**다.
# 각 격자는 "이 축들이 서로 곱해질 때 무엇이 죽는가"를 본다. 단축만 훑으면
# 조합에서 무너지는 것을 못 본다(§3.101).
for SYM in $SYMS; do
  for M in $MODULES; do
    # A 대비 x 밝기 — 저조도/과노출의 실제 축(§3.61)
    run_grid A_contrast_bright "$SYM" "$M" "contrast:0.05:1.0:0.05" "bright:0.3:1.8:0.1"
    # C 노이즈 x 블러 — 센서와 광학
    run_grid C_noise_blur      "$SYM" "$M" "noise:0:60:5" "blur:0:4:0.25"
    # E 반사 x 그림자 — 조명 불균일
    run_grid E_glare_shadow    "$SYM" "$M" "glare:0:1:0.05" "shadow:0.2:1.0:0.05"
    # F 오염 x 손상
    run_grid F_dirty_damaged   "$SYM" "$M" "dirty:0:1:0.05" "damaged:0:1:0.05"
    # G 정지대 x 인쇄불량 x 반전 x DPM
    run_grid G_zone_print      "$SYM" "$M" "quietzone:0:1:0.25" "printdefect:0:1:0.25" "invert:0:1:1" "dpm:0:1:1"
  done
  # B 모듈 x 각도 — 모듈 자체가 축이라 기준 모듈을 안 나눈다
  run_grid B_module_angle "$SYM" 8 "module:2:10:0.25" "angle:0:90:5"
  # D 원근 x 곡면
  run_grid D_persp_curve  "$SYM" 8 "persp:0:0.9:0.05" "curve:0:0.9:0.05"
done

# --- 난수 혼합 (다중 코드 + 열화 겹침) -----------------------------------
# 격자는 코드 1개짜리다. 현장은 여러 개가 섞이고 열화가 겹친다(§3.100).
if [ "$RANDOM_N" -gt 0 ]; then
  echo "  -- 난수 혼합 ${RANDOM_N}장 --"
  for SEED in 101 11 7 23 41; do
    N=$((RANDOM_N / 5))
    csv="$OUT/H_random_s${SEED}.csv"
    [ -s "$csv" ] && continue
    line="$(python3 "$ROOT/tools/generate_corpus.py" -n "$N" --difficulty mixed --bucket ok \
              --seed "$SEED" --stream --jobs "$JOBS" 2>/dev/null \
            | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --no-min-expected \
              --csv "$csv" 2>/dev/null | grep -E '^2stage ' | tr -s ' ')"
    [ -z "$line" ] && continue
    printf 'H_random\tMIXED\t-\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$(( $(wc -l < "$csv") - 1 ))" \
      "$(echo "$line" | cut -d' ' -f3 | cut -d/ -f1)" \
      "$(echo "$line" | cut -d' ' -f3 | cut -d/ -f2)" \
      "$(echo "$line" | cut -d' ' -f4 | tr -d '%')" \
      "$(echo "$line" | cut -d' ' -f5)" "$(echo "$line" | cut -d' ' -f6)" \
      "$(echo "$line" | cut -d' ' -f7)" "$(echo "$line" | cut -d' ' -f9)" >> "$SUMMARY"
    printf '  %-26s seed %-5s %s\n' "H_random" "$SEED" "$(echo "$line" | cut -d' ' -f3,4,5)"
  done
fi

END=$(date +%s)
TOTF=$(awk -F'\t' 'NR>1{s+=$4} END{print s+0}' "$SUMMARY")
TOTC=$(awk -F'\t' 'NR>1{s+=$5} END{print s+0}' "$SUMMARY")
TOTT=$(awk -F'\t' 'NR>1{s+=$6} END{print s+0}' "$SUMMARY")
TOTM=$(awk -F'\t' 'NR>1{s+=$8} END{print s+0}' "$SUMMARY")
echo "== 끝 =="
printf '   프레임 %s장 / 코드 %s / %s = %.1f%% / 오디코딩 %s / %d분\n' \
  "$TOTF" "$TOTC" "$TOTT" "$(awk -v a="$TOTC" -v b="$TOTT" 'BEGIN{print (b?100*a/b:0)}')" \
  "$TOTM" "$(( (END-START)/60 ))"
echo "   요약: $SUMMARY"
