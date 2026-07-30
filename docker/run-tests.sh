#!/usr/bin/env bash
# run-tests.sh — arm64 컨테이너 안에서 vscan-lite를 네이티브 빌드하고
# 악조건 33종 정확성 검증을 돌린다. (Dockerfile.arm64-test의 CMD)
#
# 이 스크립트가 하는 일:
#   1. 아키텍처 확인 (aarch64가 아니면 즉시 실패 — 플랫폼 지정 누락 감지)
#   2. /tmp/build 에서 -mcpu=cortex-a53 로 빌드 (마운트 FS의 느린 I/O 회피)
#   3. 악조건 33종 이미지 생성
#   4. verify_accuracy 실행 → 기대값: 세 경로 모두 30/33
#   5. 로그와 산출물을 /work/docker-out/ (= 윈도우 마운트 폴더)에 저장
set -euo pipefail

ARCH="$(uname -m)"
echo ">> arch: $ARCH"
if [ "$ARCH" != "aarch64" ]; then
  echo "!! aarch64가 아닙니다. docker build/run에 --platform linux/arm64 를 빠뜨렸는지 확인하세요."
  exit 1
fi

if [ ! -f /work/CMakeLists.txt ]; then
  echo "!! /work 에 소스가 없습니다. -v \"C:\\...\\vscan-lite-v2:/work\" 마운트를 확인하세요."
  exit 1
fi

OUT=/work/docker-out
mkdir -p "$OUT"
BUILD=/tmp/build
STRESS=/tmp/stress

echo ""
echo ">> [1/4] 빌드 (-mcpu=cortex-a53, ZBar OFF — 정확성 검증에 불필요 + QEMU에서 느림)"
export CC="gcc -mcpu=cortex-a53"
export CXX="g++ -mcpu=cortex-a53"
cmake -B "$BUILD" -G Ninja -S /work \
  -DCMAKE_BUILD_TYPE=Release \
  -DVSCAN_USE_ZBAR=OFF \
  -DVSCAN_BUILD_EXAMPLE=ON \
  2>&1 | tail -3
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -3
file "$BUILD/libvscan.so" | tee "$OUT/build-info.txt"

echo ""
echo ">> [2/4] 악조건 33종 이미지 생성"
python3 /work/tools/generate_stress_images.py --outdir "$STRESS" | tail -2

echo ""
echo ">> [3/4] verify_accuracy 빌드"
$CXX -O3 -std=c++17 -I/work/include /work/tools/verify_accuracy.cpp \
  -L"$BUILD" -lvscan -Wl,-rpath,"$BUILD:$BUILD/third_party/zxing-cpp/core" \
  -o /tmp/verify_accuracy

echo ""
echo ">> [4/4] 정확성 검증 (33종 × 3경로)"
echo "   ⚠️ 아래 시간(ms)은 QEMU 에뮬레이션 값이라 무의미합니다. 검출수(n)만 보세요."
echo ""
LD_LIBRARY_PATH="$BUILD:$BUILD/third_party/zxing-cpp/core" \
  /tmp/verify_accuracy "$STRESS" | tee "$OUT/verify-result.txt"

echo ""
LAST="$(grep "검출 성공" "$OUT/verify-result.txt" || true)"
if echo "$LAST" | grep -q "30/33.*30/33.*30/33"; then
  echo ">> ✅ 통과: 세 경로 모두 30/33 — x86/실기 기준선과 동일 (PROJECT_NOTES §7)"
else
  echo ">> ❌ 기준선(30/33) 미달 — $OUT/verify-result.txt 를 확인하세요"
  exit 1
fi

# 산출물 보존 (윈도우 쪽에서 바로 열어볼 수 있게)
cp "$BUILD/libvscan.so" "$OUT/" 2>/dev/null || true
cp "$BUILD"/third_party/zxing-cpp/core/libZXing.so* "$OUT/" 2>/dev/null || true
echo ""
echo ">> 산출물: $OUT (윈도우: 마운트 폴더의 docker-out\\)"
ls -la "$OUT"
