#!/usr/bin/env bash
# build-aarch64.sh - i.MX8M Plus(aarch64) 타겟용 libvscan.so 크로스 빌드
#
# 전제: NXP/Yocto SDK의 environment-setup-* 스크립트를 이미 source 했다는 것.
#   예) source /opt/fsl-imx-xwayland/6.x/environment-setup-aarch64-poky-linux
# 이 스크립트가 CC/CXX/PKG_CONFIG_SYSROOT_DIR 등을 이미 아키텍처에 맞게
# 잡아주므로, 아래에서는 그 값들을 그대로 CMake/autotools에 넘기기만 한다.
#
# 이 리포의 third_party/{zxing-cpp,zbar}는 이미 벤더링돼 있어서 네트워크
# 접근 없이 오프라인으로 빌드된다(zbar는 이미 autoreconf까지 끝난 상태로
# 커밋돼 있음 — 타겟 빌드 머신에 autoconf/automake/libtool 없어도 됨).

set -euo pipefail

if [ -z "${CC:-}" ] || [ -z "${CXX:-}" ]; then
  echo "CC/CXX가 설정돼 있지 않습니다. 먼저 Yocto SDK의 environment-setup-*"
  echo "스크립트를 source 하세요. 예:"
  echo "  source /opt/fsl-imx-xwayland/6.x/environment-setup-aarch64-poky-linux"
  exit 1
fi

HOST_TRIPLE="${VSCAN_HOST_TRIPLE:-aarch64-poky-linux}"
BUILD_DIR="${VSCAN_BUILD_DIR:-build-aarch64}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo ">> CC=$CC"
echo ">> HOST_TRIPLE=$HOST_TRIPLE"
echo ">> BUILD_DIR=$BUILD_DIR"

# CC/CXX는 일부러 -DCMAKE_C_COMPILER로 넘기지 않는다. Yocto SDK의 CC/CXX는
# "컴파일러 경로 + --sysroot/-march 등 플래그"가 한 문자열로 합쳐져 있는데,
# -D로 명시하면 CMake가 그 전체 문자열을 실행파일 경로로 취급해서 찾지
# 못한다(컴파일러 못 찾음 에러). ENV{CC}/ENV{CXX}로 넘기면 CMake가 첫 토큰만
# 컴파일러로, 나머지는 CMAKE_<LANG>_COMPILER_ARG1로 알아서 분리해준다.
cmake -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DZBAR_HOST_TRIPLE="$HOST_TRIPLE" \
  -DVSCAN_USE_ZBAR=ON \
  -DVSCAN_BUILD_EXAMPLE=ON \
  "$ROOT_DIR"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ""
echo ">> 빌드 완료. 산출물:"
find "$BUILD_DIR" -maxdepth 2 -name "*.so*" -o -name "vscan_cli"

echo ""
echo ">> 타겟 보드로 복사할 때 다음도 같이 가져가세요 (ldd로 NEEDED 확인):"
echo "   $BUILD_DIR/libvscan.so"
echo "   $BUILD_DIR/third_party/zxing-cpp/core/libZXing.so*"
echo "   $BUILD_DIR/zbar_install/lib/libzbar.so*"
