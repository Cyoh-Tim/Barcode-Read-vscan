#!/usr/bin/env bash
# ghost_floor.sh — **유령 바닥**을 잰다. 코드가 하나도 없는데 배경/열화는
# 난수 코퍼스와 똑같은 프레임을 대량으로 흘려서, 나오는 코드를 전부 센다.
# 여기서 나오는 것은 정의상 전부 유령이다(§3.18: 미검출 < 오디코딩).
#
# [왜 게이트의 [3.75]로는 부족한가]
# 그 단계는 잡동사니 프레임 9장 + 무늬 10장이다. 풀테스트에서 관측된
# 유령률이 프레임당 0.1~1.2% 수준이라 19장으로는 기대값이 0.1건이다 —
# **0이 나오는 것이 당연해서 아무것도 증명하지 못한다.** 그리고 그 9장은
# 열화가 하나도 안 걸린 깨끗한 배경이다. 실제로 유령을 낳는 것은
# 흐림/저대비/그림자가 얹힌 잡동사니다.
#
# [무엇을 가르는가]
# 사이클 2 풀테스트의 오디코딩 87건을 2x2로 갈랐더니 두 원인이 **독립적**
# 으로 겹쳐 있었다(H_random 27,470장 기준):
#
#            미끼 없음   미끼 있음
#   ITF 없음   0.082%     0.576%
#   ITF 있음   0.669%     1.232%
#
# 세로(ITF)와 가로(가짜 막대열 미끼)가 각각 7~8배를 따로 올린다. 이
# 도구는 그 중 **가로축만** 떼어내서 잰다 — 진짜 코드가 없으니 ITF 유무가
# 섞이지 않는다.
#
#   bash tools/ghost_floor.sh [프레임수] [출력파일]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
N="${1:-3000}"
OUT="${2:-$ROOT/ghost-floor.csv}"
export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/third_party/zxing-cpp/core:$BUILD_DIR/zbar_install/lib:${LD_LIBRARY_PATH:-}"

VERIFY="$(mktemp -d)/verify_accuracy"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$VERIFY" || exit 1

: > "$OUT.tmp"
for SEED in 7 41; do
  echo ">> 씨드 $SEED / ${N}장 (코드 0개)"
  python3 "$ROOT/tools/generate_corpus.py" -n "$N" --max-codes 0 --bucket empty \
          --difficulty mixed --seed "$SEED" --stream --jobs "$(nproc)" 2>/dev/null \
    | "$VERIFY" --stdin --paths 2stage --reps 1 --quiet --csv "$OUT.s$SEED" >/dev/null 2>&1
  tail -n +2 "$OUT.s$SEED" >> "$OUT.tmp"
  rm -f "$OUT.s$SEED"
done
{ echo "file,path,expected,found,text_ok,misdecode,dup,ms,tags"; cat "$OUT.tmp"; } > "$OUT"
rm -f "$OUT.tmp"

python3 - "$OUT" <<'PY'
import csv, sys
from collections import Counter
rows = list(csv.DictReader(open(sys.argv[1])))
tot = len(rows)
ghost_frames = sum(1 for r in rows if int(r["found"]) > 0)
ghosts = sum(int(r["found"]) for r in rows)
print(f"\n== 유령 바닥 ==")
print(f"프레임 {tot:,}장 (코드 0개) / 유령 {ghosts}개 / 유령이 난 프레임 {ghost_frames}장"
      f" = {ghost_frames/tot*100:.3f}%")
by = Counter(); tt = Counter()
for r in rows:
    st = next((t for t in r["tags"].split(";") if t.startswith("clutter-")), "clutter-plain")
    tt[st] += 1
    if int(r["found"]) > 0: by[st] += 1
print(f"\n{'배경':<22} {'유령 프레임':>10} {'전체':>8} {'비율':>9}")
for st, n in tt.most_common():
    print(f"{st:<22} {by[st]:10d} {n:8d} {by[st]/n*100:8.3f}%")
PY
