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
# **시간 단위**: 요약의 mean_ms_x86 / p95_ms_x86은 전부 x86 실측이다.
# 보드(i.MX8MP) 값은 x8을 곱한다(§3.60). 그리고 이 스크립트가 로그에 찍는
# 진행 시간(장당 몇 ms, 전체 몇 시간)은 **파이썬 생성기 시간까지 포함한
# 벽시계**라 라이브러리 성능 지표가 아니다 — 성능은 요약 표의 두 열이다.
#
# 환경변수:
#   FT_JOBS     생성 병렬도 (기본 nproc)
#   FT_MODULES  격자를 돌릴 기준 모듈 크기 (기본 "4" — 아래 주석 참고)
#   FT_RANDOM_N 난수 혼합 코퍼스 장수 (기본 40000)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/fulltest-out}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${FT_JOBS:-$(nproc)}"
# [기준 모듈은 하나다 — 2026-08-15에 바꿨다]
# 예전에는 "4 8"로 A/C/E/F/G 격자를 두 번 돌렸다. 그런데 생성기가 셀에
# 안 들어가는 코드의 모듈을 말없이 깎고 있어서, 폭이 넓은 1D(CODE39,
# CODABAR, CODE93)는 4와 8이 **같은 그림**이었다(§3.105). 격자 절반이
# 중복이었던 것이다.
#
# 생성기를 고쳤으니 이제 4와 8이 정말 다른 그림이 된다. 그래도 하나로
# 줄이는 이유는 두 가지다:
#   - 크기 축은 **S 격자가 이미 갖고 있다**(모듈 1~3, 1.5~6, 해상도 4종).
#     A/C/E/F/G를 두 모듈로 도는 것은 그 위에 얹는 중복이다.
#   - 모든 심볼로지가 **똑같이 4.00px**을 받으므로 심볼로지 사이 비교가
#     크기 교란 없이 성립한다. 예전 표는 EAN13이 8.00px, CODE39가 3.66px을
#     받은 채 나란히 놓여 있었다.
MODULES="${FT_MODULES:-4}"
RANDOM_N="${FT_RANDOM_N:-46000}"

export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/third_party/zxing-cpp/core:$BUILD_DIR/zbar_install/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "$OUT"
VERIFY="$OUT/verify_accuracy"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$VERIFY" || exit 1

SYMS="$(python3 -c "import sys;sys.path.insert(0,'$ROOT/tools');import generate_corpus as g;print(' '.join(sorted(g._SWEEP_PAYLOAD)))")"

SUMMARY="$OUT/summary.tsv"
ROWS="$OUT/rows"; mkdir -p "$ROWS"
# [단위] mean_ms / p95_ms는 **x86 실측**이다. 이 저장소 기준 보드(i.MX8MP)는
# 약 8배 느리므로 보드 값은 x8이다(§3.60). 열 이름에 박아 둔다 — 라벨이
# 없으면 다음 사람이 보드 값으로 읽는다(게이트에서 실제로 그렇게 틀렸다).
SUMMARY_HDR='grid\tsym\tmodule\tframes\tcodes_found\tcodes_total\trate\tmisdec\tdup\tmean_ms_x86\tp95_ms_x86\n'

# 요약은 **rows/의 조각을 모아서 매번 새로 쓴다.** 이어붙이지 않는 이유:
# 재개하면 건너뛴 격자는 요약 줄을 안 남기는데, 예전 방식은 이전 실행이
# 남긴 줄에 기대고 있었다. 그 줄이 없으면(=요약 줄을 쓰기 직전에 죽었으면)
# CSV는 있는데 요약에서 통째로 빠진다 — 실제로 A_contrast_bright/CODE128/m4가
# 그렇게 빠져 있었다. 조각 파일로 두면 재개해도 결과가 같다(멱등).
rebuild_summary() {
  { printf "$SUMMARY_HDR"; cat "$ROWS"/*.row 2>/dev/null | sort; } > "$SUMMARY.tmp" \
    && mv -f "$SUMMARY.tmp" "$SUMMARY"
}

# 한 격자를 돌리고 요약 한 줄을 남긴다. 축은 여러 개를 곱한다(데카르트 곱).
# FT_WH="가로 세로"를 주면 그 해상도로 만든다(기본은 생성기 기본값).
# FT_COUNT를 주면 코드 개수를 바꾼다(기본 1).
run_grid() {  # $1=격자이름 $2=심볼로지 $3=모듈 $4.. = --sweep 인자들
  local name="$1" sym="$2" mod="$3"; shift 3
  local wh=() whtag=""
  if [ -n "${FT_WH:-}" ]; then
    local ww hh
    ww="${FT_WH%% *}"; hh="${FT_WH##* }"
    # **해상도를 지정하면 캔버스 확장을 끈다.** 생성기는 요청 모듈이 안
    # 들어가면 프레임을 키워서라도 맞춰 주는데(§3.105), 그러면 여기서 정한
    # 해상도가 무의미해진다 — 해상도 자체가 축인 격자에서는 반대로 작용한다.
    wh=(--width "$ww" --height "$hh" --fixed-canvas); whtag="_${ww}x${hh}"
  fi
  local tag="${name}_${sym}_m${mod}${whtag}"
  local csv="$OUT/${tag}.csv"
  local part="$OUT/.${tag}.part"
  local row="$ROWS/${tag}.row"
  # [재개] 완성된 격자는 건너뛴다. 완성의 정의가 **"$csv가 존재한다"**이고,
  # $csv는 마지막에 rename으로만 생긴다(아래 참고). 그래서 중간에 죽은
  # 격자는 .part로 남지 $csv가 되지 않는다.
  #
  # 예전에는 `[ -s "$csv" ]`로 봤다. 그러면 **쓰다 만 CSV를 완성으로 읽는다.**
  # 실제로 그랬다 — 15:02에 죽은 실행이 남긴 A_contrast_bright/CODE128/m4가
  # 320줄 중 294줄(블록 경계에서 잘림)이었는데 재개가 그냥 건너뛰었다.
  # 조용히 표본이 8% 빠진 격자가 결과에 섞이는 것이라, 죽는 것보다 나쁘다.
  [ -f "$csv" ] && [ -f "$row" ] && return 0
  rm -f "$part"
  local sweeps=()
  for a in "$@"; do sweeps+=(--sweep "$a"); done
  local line
  line="$(python3 "$ROOT/tools/generate_corpus.py" "${sweeps[@]}" "${wh[@]}" \
            --base "sym=$sym,module=$mod,count=${FT_COUNT:-1}" --stream --jobs "$JOBS" 2>/dev/null \
          | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --csv "$part" 2>/dev/null \
          | grep -E '^2stage ' | tr -s ' ')"
  [ -z "$line" ] && { echo "  !! $tag 실패"; rm -f "$part"; return 1; }
  local codes rate mis dup mean p95 frames
  codes="$(echo "$line" | cut -d' ' -f3)"
  rate="$(echo "$line" | cut -d' ' -f4 | tr -d '%')"
  mis="$(echo "$line" | cut -d' ' -f5)"
  dup="$(echo "$line" | cut -d' ' -f6)"
  mean="$(echo "$line" | cut -d' ' -f7)"
  p95="$(echo "$line" | cut -d' ' -f9)"
  frames="$(( $(wc -l < "$part") - 1 ))"
  # 격자 이름에 해상도 꼬리표를 붙인다. S4는 같은 (격자,심볼로지,모듈)로
  # 해상도만 바꾸므로, 안 붙이면 요약에서 네 줄이 서로 구분이 안 된다.
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${name}${whtag}" "$sym" "$mod" "$frames" "${codes%%/*}" "${codes##*/}" "$rate" "$mis" "$dup" "$mean" "$p95" \
    > "$row.tmp" && mv -f "$row.tmp" "$row"
  # **CSV 확정은 맨 마지막이다.** 요약 조각이 먼저 자리를 잡은 뒤에만
  # $csv가 생기므로, "CSV가 있다 = 요약도 있다 = 정말 다 돌았다"가 된다.
  mv -f "$part" "$csv"
  rebuild_summary
  printf '  %-26s %-12s m%-3s %6s장  %-11s %6s%%  오디%s\n' "${name}${whtag}" "$sym" "$mod" "$frames" "$codes" "$rate" "$mis"
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
  # 모듈 상한을 10에서 8로 내렸다. 이제 모듈이 진짜로 반영되므로 10은
  # 폭 넓은 1D에서 프레임을 12MP 상한까지 밀어 올린다 — 그 구간은 어차피
  # 상한에 걸려 깎이고(태그의 modpx가 알려 준다), 값은 못 하면서 면적에
  # 비례해 시간만 쓴다. 벽은 큰 쪽이 아니라 **작은 쪽**에 있다(S 격자).
  run_grid B_module_angle "$SYM" 8 "module:2:8:0.25" "angle:0:90:5"
  # D 원근 x 곡면
  run_grid D_persp_curve  "$SYM" 8 "persp:0:0.9:0.05" "curve:0:0.9:0.05"
done

# --- S 격자: **크기** -----------------------------------------------------
# 위 격자들은 전부 기준 모듈 4px에 코드 1개다. 즉 **크기 축이 사실상
# 빠져 있었다.** 그런데 이 저장소가 실제로 막힌
# 자리는 대부분 크기였다 — §3.17(회전이 아니라 탐색 면적), §3.20/§3.103
# (실물 차트의 모듈 2.1px), §3.61(모듈 2px의 SNR 벽).
#
# 크기는 세 가지 뜻이 있고 셋 다 따로 재야 한다:
#   (1) 모듈 절대 크기 px    — 디코더가 모듈당 표본을 몇 개 얻나
#   (2) 프레임 대비 코드 크기 — 로케이터가 찾을 수 있나(§3.17)
#   (3) 한 프레임의 코드 밀도 — 타일 하나에 몇 개가 뭉치나(§3.103)
# S1/S2가 (1), S4가 (2), S3이 (3)이다.
#
# 모듈은 **2px 미만까지** 내려간다. 예비 실측에서 벽이 1.75px 근처였다
# (1.75 읽힘 / 1.5 이하 실패). 기존 격자는 2px에서 시작해 그 벽을 못 봤다.
echo "  -- S 격자 (크기) --"
for SYM in $SYMS; do
  run_grid S1_tinymod_contrast "$SYM" 8 "module:1:3:0.125" "contrast:0.1:1.0:0.05"
  run_grid S2_tinymod_noise    "$SYM" 8 "module:1:3:0.125" "noise:0:40:4"
done
# S3 모듈 x 코드 밀도 — 실물 차트가 막힌 자리(작은 코드 여럿)
for SYM in QR DATAMATRIX CODE128 EAN13 PDF417; do
  run_grid S3_mod_density "$SYM" 8 "module:1.5:6:0.5" "count:1:16:1"
done
# S4 프레임 해상도 x 모듈 — 같은 물리 코드를 센서 해상도만 바꿔 본다.
# 해상도가 바뀌면 "프레임 대비 코드 크기"가 바뀌므로 로케이터 쪽 축이다.
for WH in "1024 768" "1280 960" "2048 1536" "2592 1944"; do
  for SYM in QR DATAMATRIX CODE128 EAN13 PDF417; do
    FT_WH="$WH" run_grid S4_res_module "$SYM" 8 "module:1:6:0.25"
  done
done

# --- 난수 혼합 (다중 코드 + 열화 겹침) -----------------------------------
# 격자는 코드 1개짜리다. 현장은 여러 개가 섞이고 열화가 겹친다(§3.100).
if [ "$RANDOM_N" -gt 0 ]; then
  echo "  -- 난수 혼합 ${RANDOM_N}장 --"
  # [재개는 원자적으로] 이 자리는 오래 `[ -s "$csv" ]`로 판정했다. §3.104에서
  # 격자 쪽은 고쳤는데 **이 분기만 남아 있었다.** 컨테이너가 두 번 죽은 뒤
  # 이어 돌렸더니 H_random_s11이 2,171줄로 잘린 채 "완성"으로 건너뛰어졌다
  # (5,453줄이 정상이다). 같은 결함이 같은 파일 안에 두 벌 있으면 한 벌만
  # 고치고 끝내기 쉽다 — 그래서 여기도 rows/ 조각 + rename으로 맞춘다.
  for SEED in 101 11 7 23 41; do
    N=$((RANDOM_N / 5))
    csv="$OUT/H_random_s${SEED}.csv"
    part="$OUT/.H_random_s${SEED}.part"
    row="$ROWS/H_random_s${SEED}.row"
    [ -f "$csv" ] && [ -f "$row" ] && continue
    rm -f "$part"
    line="$(python3 "$ROOT/tools/generate_corpus.py" -n "$N" --difficulty mixed --bucket ok \
              --seed "$SEED" --stream --jobs "$JOBS" 2>/dev/null \
            | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --no-min-expected \
              --csv "$part" 2>/dev/null | grep -E '^2stage ' | tr -s ' ')"
    [ -z "$line" ] && { rm -f "$part"; continue; }
    printf 'H_random_s%s\tMIXED\t-\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$SEED" \
      "$(( $(wc -l < "$part") - 1 ))" \
      "$(echo "$line" | cut -d' ' -f3 | cut -d/ -f1)" \
      "$(echo "$line" | cut -d' ' -f3 | cut -d/ -f2)" \
      "$(echo "$line" | cut -d' ' -f4 | tr -d '%')" \
      "$(echo "$line" | cut -d' ' -f5)" "$(echo "$line" | cut -d' ' -f6)" \
      "$(echo "$line" | cut -d' ' -f7)" "$(echo "$line" | cut -d' ' -f9)" \
      > "$row.tmp" && mv -f "$row.tmp" "$row"
    mv -f "$part" "$csv"          # **확정은 맨 마지막이다**
    rebuild_summary
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
