#!/usr/bin/env bash
# verify_aarch64.sh — **보드와 같은 아키텍처(aarch64)에서 결과가 같은지** 본다.
#
# 이 저장소의 측정은 전부 x86에서 하고 보드는 x8로 환산한다(§3.60). 환산은
# **시간**에 대한 것이고 **검출 결과가 같다**는 것은 따로 확인해야 한다.
# 실제로 §3.52에서 크로스 빌드 자체가 깨져 있던 적이 있다.
#
# Yocto SDK 없이 배포판 크로스 툴체인 + qemu 사용자 모드로 확인된다:
#
#   sudo apt install g++-aarch64-linux-gnu qemu-user-static
#   tools/verify_aarch64.sh
#
# **시간은 보지 않는다.** qemu 사용자 모드는 명령 단위 에뮬레이션이라
# 실기 속도와 무관하다(실측상 x86 네이티브의 5~15배 느리다). 여기서 보는
# 것은 **검출 개수와 오디코딩**뿐이다.
#
# 기본 게이트(ci_gate.sh)에 안 넣은 이유: qemu가 느려서 40종 한 바퀴에
# 몇 분이 든다. SIMD/빌드/서드파티를 건드렸을 때 돌리면 된다.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${VSCAN_A64_BUILD:-$ROOT/build-aarch64}"
X86_BUILD="${BUILD_DIR_X86:-$ROOT/build}"

for t in aarch64-linux-gnu-g++ qemu-aarch64-static; do
  command -v "$t" >/dev/null || { echo "!! $t 가 없다 — 위 주석의 apt 명령 참고"; exit 2; }
done

echo ">> [1/4] aarch64 크로스 빌드"
CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
  VSCAN_HOST_TRIPLE=aarch64-linux-gnu VSCAN_BUILD_DIR="$BUILD_DIR" \
  bash "$ROOT/build-aarch64.sh" >/dev/null
file "$BUILD_DIR/libvscan.so" | grep -q "ARM aarch64" \
  || { echo "!! 산출물이 aarch64가 아니다"; exit 1; }
echo "   libvscan.so = ARM aarch64"

echo ">> [2/4] 검증 도구 빌드 (양쪽)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
aarch64-linux-gnu-g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$BUILD_DIR" -lvscan -o "$WORK/va_a64"
g++ -O3 -std=c++17 -I"$ROOT/include" "$ROOT/tools/verify_accuracy.cpp" \
    -L"$X86_BUILD" -lvscan -o "$WORK/va_x86"

echo ">> [3/4] 고정 40종 — 두 아키텍처 비교"
python3 "$ROOT/tools/generate_stress_images.py" --outdir "$WORK/stress" >/dev/null

LD_LIBRARY_PATH="$X86_BUILD:$X86_BUILD/third_party/zxing-cpp/core:$X86_BUILD/zbar_install/lib" \
  "$WORK/va_x86" "$WORK/stress" --paths full,2stage --reps 1 --quiet \
  --csv "$WORK/x86.csv" >/dev/null 2>&1

QEMU_LD_PREFIX=/usr/aarch64-linux-gnu \
LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/third_party/zxing-cpp/core:$BUILD_DIR/zbar_install/lib" \
  qemu-aarch64-static "$WORK/va_a64" "$WORK/stress" --paths full,2stage --reps 1 --quiet \
  --csv "$WORK/a64.csv" >/dev/null 2>&1

echo ">> [4/4] 비교 (개수와 오디코딩만 — 시간은 qemu라 뜻이 없다)"
python3 - "$WORK/x86.csv" "$WORK/a64.csv" <<'PY'
import csv, sys
def load(p):
    d = {}
    for r in csv.DictReader(open(p)):
        d[(r['file'], r['path'])] = (int(r['found']), int(r['misdecode']))
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
diff = [(k, a[k], b.get(k)) for k in a if a[k] != b.get(k)]
tot_a = sum(v[0] for v in a.values()); tot_b = sum(v[0] for v in b.values())
mis_a = sum(v[1] for v in a.values()); mis_b = sum(v[1] for v in b.values())
print(f"   x86      코드 {tot_a} / 오디코딩 {mis_a}")
print(f"   aarch64  코드 {tot_b} / 오디코딩 {mis_b}")
if not diff:
    print(">> ✅ 두 아키텍처의 결과가 프레임 단위까지 같다")
    sys.exit(0)
print(f"!! 프레임 {len(diff)}개가 다르다:")
for k, va, vb in diff[:12]:
    print(f"   {k[0]:<32} {k[1]:<7} x86 {va}  aarch64 {vb}")
# [알려진 흔들림] 21_dpm_dotpeen의 full 경로는 마감이 벽시계라 실행마다
# 뒤집힌다(§8). qemu는 훨씬 느려서 더 자주 걸린다. 그것만이면 통과로 본다.
only_known = all(k[0].startswith('21_dpm_dotpeen') and k[1] == 'full' for k, _, _ in diff)
if only_known:
    print(">> ⚠️ 통과 — 차이가 §8에 기록된 21_dpm_dotpeen/full 흔들림뿐이다")
    sys.exit(0)
sys.exit(1)
PY
