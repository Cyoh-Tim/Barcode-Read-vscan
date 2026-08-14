cmake_minimum_required(VERSION 3.16)
project(vscan_lite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VSCAN_BUILD_EXAMPLE "Build v4l2 capture example CLI" ON)
option(VSCAN_USE_OPENCV "Enable OpenCV-based preprocessing/ROI helpers" OFF)
option(VSCAN_USE_ZBAR "Bundle ZBar as an additional fast 1D decoder" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_C_FLAGS_RELEASE "-O3")

# LTO(링크 타임 최적화) — 디코딩 결과/정확도에는 전혀 영향 없는 순수
# 컴파일러 레벨 최적화. 번역 단위 경계를 넘어서 인라이닝/데드코드 제거를
# 하기 때문에 zxing-cpp처럼 작은 함수가 많은 코드베이스에서 특히 잘 먹는다.
# 컴파일러가 LTO를 지원 못 하면(구형 크로스 툴체인 등) 자동으로 꺼진다 —
# 빌드 실패로 이어지지 않는다.
include(CheckIPOSupported)
check_ipo_supported(RESULT VSCAN_IPO_SUPPORTED OUTPUT VSCAN_IPO_ERROR)
if(VSCAN_IPO_SUPPORTED)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
  message(STATUS "LTO not supported by this toolchain, skipping: ${VSCAN_IPO_ERROR}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm.*)")
  add_compile_options(-mcpu=cortex-a53)
endif()

# ---------------------------------------------------------------------------
# zxing-cpp: 벤더 소스에서 직접 빌드해서 vscan.so에 필요한 부분만 포함시킨다.
# Meta(Yocto)쪽에서 별도로 받지 않겠다는 요구사항 반영 —
# third_party/zxing-cpp는 이 레포에 통째로 벤더링돼 있다.
# ---------------------------------------------------------------------------
set(BUILD_READERS ON CACHE BOOL "" FORCE)
set(BUILD_WRITERS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_C_API OFF CACHE BOOL "" FORCE) # 우리 자체 C API(vscan.h)를 쓰므로 불필요
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/zxing-cpp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# ZBar: CMake가 없는 autotools 프로젝트라 ExternalProject로 감싼다.
# CMake가 고른 C 컴파일러를 그대로 넘겨서, 크로스 툴체인 파일을 지정하면
# 자동으로 aarch64용으로 빌드되게 한다(Yocto SDK 환경변수로 CC를 이미
# 잡아준 상태에서 호출해도 동일하게 동작).
# ---------------------------------------------------------------------------
if(VSCAN_USE_ZBAR)
  include(ExternalProject)
  set(ZBAR_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zbar)
  set(ZBAR_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/zbar_install)
  set(ZBAR_HOST_TRIPLE "" CACHE STRING "autotools --host= triple for cross build, e.g. aarch64-poky-linux")

  set(ZBAR_CONFIGURE_ARGS
    --prefix=${ZBAR_INSTALL_DIR}
    --disable-video --disable-doc --without-jpeg --without-imagemagick
    # --without-xshm이 없으면 **크로스 빌드에서 configure가 죽는다.**
    # --without-x를 줘도 zbar는 XShm 확장을 따로 검사해서 -lXext를 찾는다.
    # 호스트(x86)에는 libXext가 깔려 있어 통과하지만 Yocto sysroot에는 보통
    # 없다 — 그래서 실기 빌드에서만 터진다(2026-08-02 aarch64 크로스로 확인).
    --without-x --without-xshm --without-dbus --with-gtk=no --with-python=no
    --without-qt --without-java
    --enable-shared --disable-static --disable-nls
    --enable-codes=ean\ upc\ code39\ code93\ code128\ codabar\ databar\ composite\ qrcode\ i25
  )
  if(ZBAR_HOST_TRIPLE)
    list(APPEND ZBAR_CONFIGURE_ARGS --host=${ZBAR_HOST_TRIPLE})
  endif()

  ExternalProject_Add(zbar_ext
    SOURCE_DIR ${ZBAR_SRC_DIR}
    CONFIGURE_COMMAND ${ZBAR_SRC_DIR}/configure ${ZBAR_CONFIGURE_ARGS}
      CC=${CMAKE_C_COMPILER}\ ${CMAKE_C_COMPILER_ARG1}
      CXX=${CMAKE_CXX_COMPILER}\ ${CMAKE_CXX_COMPILER_ARG1}
    BUILD_COMMAND make -j
    INSTALL_COMMAND make install
    # [소스 트리에서 빌드하지 않는다]
    # 예전에는 BUILD_IN_SOURCE 1이었다. 두 가지가 걸렸다:
    #  - 호스트(x86)로 한 번 빌드한 뒤 aarch64로 크로스 빌드하면 **x86 오브젝트가
    #    남아 있어 링크가 죽는다**("Relocations in generic ELF (EM: 62)").
    #    실기 빌드 직전에만 터지는 종류라 더 나쁘다(2026-08-02 확인).
    #  - 산출물이 벤더 소스 트리를 더럽혀서 .gitignore로 덮어야 했다.
    # zbar는 autoreconf가 끝난 상태로 커밋돼 있어 VPATH 빌드가 된다.
    BUILD_BYPRODUCTS
      ${ZBAR_INSTALL_DIR}/lib/libzbar${CMAKE_SHARED_LIBRARY_SUFFIX}
  )

  add_library(zbar SHARED IMPORTED GLOBAL)
  file(MAKE_DIRECTORY ${ZBAR_INSTALL_DIR}/include) # ExternalProject가 나중에 채움, 구성 단계 통과용
  set_target_properties(zbar PROPERTIES
    IMPORTED_LOCATION ${ZBAR_INSTALL_DIR}/lib/libzbar${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${ZBAR_INSTALL_DIR}/include
  )
  add_dependencies(zbar zbar_ext)
endif()

# ---------------------------------------------------------------------------
# vscan_lite (내부 C++ 파이프라인 + 공개 C API) — 공유 라이브러리로 빌드
# ---------------------------------------------------------------------------
set(VSCAN_SOURCES
  src/preprocess.cpp
  src/deskew1d.cpp
  src/locate.cpp
  src/locate_qr.cpp
  src/deskew_persp.cpp
  src/pitch_equalize.cpp
  src/decoder_zxing.cpp
  src/decoder_linear.cpp
  src/decoder_micropdf417.cpp
  src/gs1_composite.cpp
  src/decoder_dotcode.cpp
  src/decoder_postal.cpp
  src/pipeline.cpp
  src/gs1.cpp
  src/capture_v4l2.cpp
  src/vscan_c_api.cpp
)
if(VSCAN_USE_ZBAR)
  list(APPEND VSCAN_SOURCES src/decoder_zbar.cpp)
endif()

add_library(vscan SHARED ${VSCAN_SOURCES})
set_target_properties(vscan PROPERTIES OUTPUT_NAME "vscan")

target_include_directories(vscan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(vscan PUBLIC ZXing::ZXing)
if(VSCAN_USE_ZBAR)
  target_link_libraries(vscan PRIVATE zbar)
  target_compile_definitions(vscan PRIVATE VSCAN_HAVE_ZBAR=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(vscan PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
  target_compile_definitions(vscan PRIVATE VSCAN_HAVE_V4L2=1)
endif()

if(VSCAN_USE_OPENCV)
  find_package(OpenCV REQUIRED core imgproc)
  target_link_libraries(vscan PRIVATE ${OpenCV_LIBS})
  target_include_directories(vscan PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_compile_definitions(vscan PRIVATE VSCAN_HAVE_OPENCV=1)
endif()

if(VSCAN_BUILD_EXAMPLE)
  add_executable(vscan_cli src/main.cpp)
  target_link_libraries(vscan_cli PRIVATE vscan)

  add_executable(decode_image examples/decode_image.cpp)
  target_link_libraries(decode_image PRIVATE vscan)

  add_executable(decode_rois examples/decode_rois.cpp)
  target_link_libraries(decode_rois PRIVATE vscan)

  add_executable(decode_workers examples/decode_workers.cpp)
  target_link_libraries(decode_workers PRIVATE vscan Threads::Threads)

  # 노출/조명을 통제할 수 있는 컨베이어 배치의 권장 설정 예제.
  # 카메라 연결 부분이 비어 있어 그대로는 아무 일도 안 한다 — 설정과
  # 그 근거를 코드로 남기는 것이 목적이다(§3.62~§3.74).
  add_executable(conveyor_capture_controlled examples/conveyor_capture_controlled.cpp)
  target_link_libraries(conveyor_capture_controlled PRIVATE vscan)

  # ultravc CLI(vscantest 명령)가 fork+exec로 호출하는 스탠드얼론 디코더.
  # ultravc 실행파일 자체에 vscan-lite를 직접 링크하지 않는 이유는
  # tools/vscan_decode_file.cpp 상단 주석 참고 (Zxing/Zbar 버전 충돌 방지).
  add_executable(vscan_decode_file tools/vscan_decode_file.cpp)
  target_link_libraries(vscan_decode_file PRIVATE vscan)

  # 현장 자가진단. 정답표 없이 현장 프레임만으로 "어디서 막히는지 +
  # 어느 손잡이를 돌릴지"를 뽑는다 — tools/field_diagnose.cpp 상단 주석 참고.
  add_executable(field_diagnose tools/field_diagnose.cpp)
  target_link_libraries(field_diagnose PRIVATE vscan)
endif()

# ---------------------------------------------------------------------------
# 설치 규칙: libvscan.so + 의존 so들 + 공개 헤더 하나만 배포하면 되는 구조
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS vscan LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/vscan.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
