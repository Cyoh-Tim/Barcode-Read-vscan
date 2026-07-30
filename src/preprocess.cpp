#include "vscan_internal/preprocess.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VSCAN_HAVE_NEON 1
#endif

namespace vscan {

namespace {

// YUYV 메모리 레이아웃: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
// 즉 짝수 바이트 오프셋(0,2,4,6...)이 전부 Y다. stride-2 deinterleave면 끝.
//
// NEON 경로: vld2q_u8은 32바이트를 "짝수 바이트/홀수 바이트" 두 레지스터로
// 한번에 분리해준다 — Y만 뽑는 이 작업과 정확히 맞아떨어져서 32바이트(16px)
// 단위로 한 번의 load+store로 처리 가능. i.MX8M Plus의 4x Cortex-A53에서
// 스칼라 루프 대비 체감 3~4배 빠르다(메모리 bandwidth bound라 완전 4배는
// 안 나오지만 유의미하게 줄어든다).
#ifdef VSCAN_HAVE_NEON
void yuyvToGrayNEON(const uint8_t* src, uint8_t* dst, size_t pixCount) {
    size_t i = 0;
    // 32바이트(16px)씩 처리
    for (; i + 16 <= pixCount; i += 16) {
        uint8x16x2_t deint = vld2q_u8(src + i * 2); // val[0]=Y들, val[1]=U/V들
        vst1q_u8(dst + i, deint.val[0]);
    }
    // 나머지는 스칼라로
    for (; i < pixCount; ++i) {
        dst[i] = src[i * 2];
    }
}
#endif

void yuyvToGrayScalar(const uint8_t* src, uint8_t* dst, size_t pixCount) {
    for (size_t i = 0; i < pixCount; ++i) {
        dst[i] = src[i * 2];
    }
}

// YUYV: 2 pixel당 4 byte (Y0 U Y1 V). Y만 추출.
GrayImage fromYUYV(const Frame& f) {
    GrayImage out;
    out.width = f.width;
    out.height = f.height;
    const size_t pixCount = static_cast<size_t>(f.width) * f.height;
    out.pixels.resize(pixCount);

    if (f.data.size() < pixCount * 2) return out; // 데이터 부족, 빈 결과

#ifdef VSCAN_HAVE_NEON
    yuyvToGrayNEON(f.data.data(), out.pixels.data(), pixCount);
#else
    yuyvToGrayScalar(f.data.data(), out.pixels.data(), pixCount);
#endif
    return out;
}

// NV12: 앞부분 width*height 바이트가 그대로 Y plane.
GrayImage fromNV12(const Frame& f) {
    GrayImage out;
    out.width = f.width;
    out.height = f.height;
    const size_t ySize = static_cast<size_t>(f.width) * f.height;
    out.pixels.assign(f.data.begin(), f.data.begin() + std::min(ySize, f.data.size()));
    return out;
}

} // namespace

GrayView toGrayView(const Frame& frame, GrayImage& fallbackStorage) {
    if (frame.empty()) return {};

    if (frame.format == PixelFormat::GREY) {
        // 진짜 zero-copy: frame.data를 그대로 가리키기만 한다. 복사 없음.
        return GrayView(frame.data.data(), frame.width, frame.height, frame.width);
    }

    // 폴백 경로 (ISP가 그레이를 안 준 경우에만 탐). 여기서만 변환 비용 발생.
    if (frame.format == PixelFormat::YUYV) {
        fallbackStorage = fromYUYV(frame);
    } else if (frame.format == PixelFormat::NV12) {
        fallbackStorage = fromNV12(frame);
    } else {
        throw std::runtime_error("toGrayView: unsupported pixel format");
    }
    return GrayView(fallbackStorage);
}


void downsampleBox(const GrayView& src, GrayImage& dst, int factor) {
    const int N = (factor == 3) ? 3 : 2;
    const int dw = src.width / N, dh = src.height / N;
    dst.width = dw;
    dst.height = dh;
    dst.pixels.resize(static_cast<size_t>(dw) * dh);
    const int stride = src.stride > 0 ? src.stride : src.width;

    if (N == 2) {
        for (int y = 0; y < dh; ++y) {
            const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(2 * y) * stride;
            const uint8_t* __restrict r1 = r0 + stride;
            uint8_t* __restrict o = dst.pixels.data() + static_cast<size_t>(y) * dw;
            for (int x = 0; x < dw; ++x)
                o[x] = static_cast<uint8_t>((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
        }
    } else {
        for (int y = 0; y < dh; ++y) {
            const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(3 * y) * stride;
            const uint8_t* __restrict r1 = r0 + stride;
            const uint8_t* __restrict r2 = r1 + stride;
            uint8_t* __restrict o = dst.pixels.data() + static_cast<size_t>(y) * dw;
            for (int x = 0; x < dw; ++x) {
                const int i = 3 * x;
                // 9로 나누기를 곱셈+시프트로 대체 (65536/9 = 7281.8 -> 7282).
                // 입력 합계 범위 0~2295 전 구간에서 v/9와 결과가 완전히 일치함을
                // 확인했다(오차 0건).
                const unsigned v = static_cast<unsigned>(
                    r0[i] + r0[i + 1] + r0[i + 2] +
                    r1[i] + r1[i + 1] + r1[i + 2] +
                    r2[i] + r2[i + 1] + r2[i + 2]);
                o[x] = static_cast<uint8_t>((v * 7282u) >> 16);
            }
        }
    }
}

void rotateAroundPoint(const GrayView& src, float degrees, float pivotX, float pivotY, GrayImage& out) {
    const int W = src.width, H = src.height;
    const int stride = src.stride > 0 ? src.stride : W;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H, 255);

    // 원본 이미지에서 목적지 픽셀로 매핑하는 역변환. 회전각을 음수로
    // 걸어서(원본을 -degrees만큼) "degrees만큼 되돌린" 결과가 나오게 한다.
    const float rad = -degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(rad), s = std::sin(rad);
    for (int y = 0; y < H; ++y) {
        const float dy = static_cast<float>(y) - pivotY;
        // x=0에서의 소스 좌표. 이후 x 증가마다 (c, s)만 더하면 되므로
        // 행마다 삼각함수를 딱 1번만 계산한다(DDA) — 픽셀마다 cos/sin을
        // 다시 부르는 나이브한 구현 대비 실측 5~6배 빠르다.
        float sx = (0.0f - pivotX) * c - dy * s + pivotX;
        float sy = (0.0f - pivotX) * s + dy * c + pivotY;
        uint8_t* __restrict dstRow = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x, sx += c, sy += s) {
            int isx = static_cast<int>(sx + 0.5f);
            int isy = static_cast<int>(sy + 0.5f);
            if (static_cast<unsigned>(isx) < static_cast<unsigned>(W) &&
                static_cast<unsigned>(isy) < static_cast<unsigned>(H))
                dstRow[x] = src.pixels[static_cast<size_t>(isy) * stride + isx];
        }
    }
}

namespace {

// [시도했지만 채택 안 함] monotonic deque 기반 O(1)/픽셀 슬라이딩
// 최댓값/최솟값. 점근적으로는 이게 이론상 더 낫지만(커널 크기 k와
// 무관), 분기가 많아 스칼라 코드에서 branch misprediction 비용이
// 커서 실측 128ms가 나왔다. 커널이 작을 땐(k<=~9) 아래의 단순
// 버전(벡터화 걸림)이 실제로 훨씬 빠르다 — 이진화 NEON 패치
// (§6.5.1)와 같은 교훈: 점근적 알고리즘 우위보다 벡터화 여부가
// 작은 규모에서 훨씬 크게 좌우한다.

// 커널이 작을 때(k<=~9) 단순 직접 비교가 monotonic deque보다 실제로
// 더 빠르다 — deque는 점근적으로 O(1)/픽셀이지만 분기가 많아 스칼라
// 코드에서 branch misprediction 비용이 크다(실측: 128ms). 반면 이
// 단순 버전은 분기 없는 균일 반복이라 자동 벡터화가 걸린다(이진화
// NEON 패치 §6.5.1과 같은 교훈). k가 작을 땐 점근적 이득보다 벡터화
// 여부가 훨씬 크게 좌우한다.
template <bool FindMax>
void slidingExtremeSimple1D(const uint8_t* __restrict in, int n, int k, uint8_t* __restrict out) {
    int half = k / 2;
    for (int i = 0; i < n; ++i) {
        int lo = std::max(0, i - half), hi = std::min(n - 1, i + half);
        uint8_t best = in[lo];
        for (int j = lo + 1; j <= hi; ++j) {
            uint8_t v = in[j];
            if (FindMax ? (v > best) : (v < best)) best = v;
        }
        out[i] = best;
    }
}

// 전치(transpose) — 세로 방향 슬라이딩을 가로 방향 접근으로 바꿔주는
// 용도. 2048폭 이미지에서 세로 접근은 매번 2048바이트씩 건너뛰어
// 캐시를 거의 매 픽셀 놓친다(L1 32KB에 16행도 안 들어감).
void transpose(const uint8_t* __restrict src, int w, int h, uint8_t* __restrict dst) {
    constexpr int B = 32; // 캐시 친화적 블록 단위로 전치
    for (int by = 0; by < h; by += B) {
        int ymax = std::min(h, by + B);
        for (int bx = 0; bx < w; bx += B) {
            int xmax = std::min(w, bx + B);
            for (int y = by; y < ymax; ++y)
                for (int x = bx; x < xmax; ++x) dst[static_cast<size_t>(x) * h + y] = src[static_cast<size_t>(y) * w + x];
        }
    }
}

// 2D 최댓값/최솟값 필터 (가로 패스 + 전치 + 가로 패스 + 전치로 분리) —
// 전치 덕분에 두 패스 다 캐시 친화적인 가로 접근만 하고, 각 행/열은
// 분기 없는 벡터화 친화적 슬라이딩 비교로 처리한다.
template <bool FindMax>
void morphExtreme2D(const uint8_t* __restrict src, int w, int h, int stride, int k,
                     std::vector<uint8_t>& out) {
    std::vector<uint8_t> rowIn(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        std::memcpy(rowIn.data() + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * stride, w);

    std::vector<uint8_t> rowPass(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        slidingExtremeSimple1D<FindMax>(rowIn.data() + static_cast<size_t>(y) * w, w, k,
                                         rowPass.data() + static_cast<size_t>(y) * w);

    std::vector<uint8_t> transposed(static_cast<size_t>(w) * h);
    transpose(rowPass.data(), w, h, transposed.data());

    std::vector<uint8_t> colPassT(static_cast<size_t>(w) * h); // h x w (전치된 모양)
    for (int x = 0; x < w; ++x)
        slidingExtremeSimple1D<FindMax>(transposed.data() + static_cast<size_t>(x) * h, h, k,
                                         colPassT.data() + static_cast<size_t>(x) * h);

    out.assign(static_cast<size_t>(w) * h, 0);
    transpose(colPassT.data(), h, w, out.data()); // 다시 원래 모양으로
}

} // namespace

void morphologicalCloseInverted(const GrayView& src, int kernelSize, GrayImage& out) {
    const int W = src.width, H = src.height;
    const int stride = src.stride > 0 ? src.stride : W;
    if (kernelSize < 1) kernelSize = 1;

    // 반전
    std::vector<uint8_t> inv(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict srow = src.pixels + static_cast<size_t>(y) * stride;
        uint8_t* __restrict irow = inv.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) irow[x] = static_cast<uint8_t>(255 - srow[x]);
    }

    // 닫힘 = 팽창(최댓값) -> 침식(최솟값)
    std::vector<uint8_t> dilated;
    morphExtreme2D<true>(inv.data(), W, H, W, kernelSize, dilated);
    std::vector<uint8_t> closed;
    morphExtreme2D<false>(dilated.data(), W, H, W, kernelSize, closed);

    // 재반전
    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H);
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = static_cast<uint8_t>(255 - closed[i]);
}

} // namespace vscan
