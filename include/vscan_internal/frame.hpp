#pragma once
#include <cstdint>
#include <vector>

namespace vscan {

// 이미지 좌표계 사각 영역. 외부에서 이미 알고 있는 ROI(예: 상위 검출기가
// 준 라벨 위치)를 넘길 때 쓴다.
struct Rect {
    int x0, y0, x1, y1;
};


// V4L2에서 흔히 나오는 픽셀 포맷 중 이 프로젝트가 다루는 것들.
// IMX900/i.MX8MP ISP 파이프라인은 보통 YUYV 또는 NV12를 뽑는다.
enum class PixelFormat {
    GREY,   // 8bit 단일 채널 (이미 그레이스케일)
    YUYV,   // V4L2_PIX_FMT_YUYV
    NV12,   // V4L2_PIX_FMT_NV12
};

// 디코더 파이프라인은 항상 8bit 그레이스케일만 다룬다.
// YUYV/NV12는 preprocess 단계에서 GREY로 변환한다.
struct Frame {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::GREY;
    std::vector<uint8_t> data; // format에 맞는 raw bytes

    bool empty() const { return data.empty() || width <= 0 || height <= 0; }
};

// 파이프라인 내부에서 실제 디코딩에 사용하는 순수 그레이스케일 버퍼(소유형).
// YUYV/NV12처럼 변환이 꼭 필요한 경우에만 이걸 만든다.
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // width*height, row-major, 1 byte/px
};

// 복사 없이 기존 버퍼(mmap 버퍼, GrayImage 등)를 참조만 하는 뷰.
// ISP가 이미 그레이스케일을 뽑아주는 우리 경우엔 캡처 → 디코더까지
// 이 타입 하나로 관통시켜서 memcpy를 아예 없앨 수 있다.
// stride는 한 행의 바이트 수(패딩이 있을 수 있어 width와 다를 수 있음).
struct GrayView {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0; // 0이면 width와 동일하다고 간주

    GrayView() = default;
    GrayView(const uint8_t* p, int w, int h, int s = 0)
        : pixels(p), width(w), height(h), stride(s ? s : w) {}

    // GrayImage를 복사 없이 뷰로 감싸는 편의 생성자
    GrayView(const GrayImage& img)
        : pixels(img.pixels.data()), width(img.width), height(img.height), stride(img.width) {}

    bool empty() const { return pixels == nullptr || width <= 0 || height <= 0; }

    // y0~y1 구간(행 단위)만 잘라낸 부분 뷰. 포인터 산술만 하므로 비용 0.
    GrayView rowSlice(int y0, int y1) const {
        y0 = y0 < 0 ? 0 : y0;
        y1 = y1 > height ? height : y1;
        return GrayView(pixels + static_cast<size_t>(y0) * stride, width, y1 - y0, stride);
    }

    // (x0,y0)-(x1,y1) 사각 영역만 잘라낸 부분 뷰. stride는 원본 그대로 유지하고
    // 포인터만 옮기므로 여기도 복사 비용 0 (2단계 locate-then-refine 디코드의
    // crop 단계에서 씀 — [[vscan-lite-two-stage-decode]]).
    GrayView crop(int x0, int y0, int x1, int y1) const {
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > width ? width : x1;
        y1 = y1 > height ? height : y1;
        if (x1 <= x0 || y1 <= y0) return GrayView();
        return GrayView(pixels + static_cast<size_t>(y0) * stride + x0, x1 - x0, y1 - y0, stride);
    }
};

} // namespace vscan
