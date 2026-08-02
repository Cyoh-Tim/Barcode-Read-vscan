#include "vscan_internal/preprocess.hpp"
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
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

    // [샘플링: 쌍선형]
    // 예전엔 최근접 이웃이었다. 회전하면 원본 픽셀 격자와 목적지 격자가
    // 어긋나는데, 최근접은 그 어긋남을 반올림으로 흡수한다 — 모듈 경계가
    // 격자에 스냅되면서 막대 폭 비율이 흔들리고, 디코더는 폭 비율로
    // 심볼을 읽으므로 그걸 거부한다.
    //
    // [주의 — 처음 도입할 때의 근거는 틀렸다]
    // 이 변경은 원래 "DataMatrix가 48도부터 전부 실패하고 PDF417이
    // 7/46인 것"의 원인이라고 보고 넣었다. 그 진단은 **틀렸다**. 그
    // 실패의 진짜 원인은 리샘플 품질이 아니라 탐색 면적이었다 — 코드
    // 주변만 잘라내면 회전을 한 번도 안 하고 전 각도가 읽힌다(§3.17,
    // [[vscan-lite-region-rescue]]). 쌍선형으로 바꿔도 그 수치는 거의
    // 안 움직였다.
    //
    // 그래도 남기는 이유는 따로 있다. 영역 구제까지 들어간 상태에서
    // 최근접/쌍선형을 A/B 하면:
    //   고정 40종      36/40 -> 37/40   (45도 1D인 39번이 쌍선형에서만 붙는다)
    //   단일코드 300장 60.3% -> 60.8%
    //   PDF417 각도스윕 94.7% -> 89.5%  (여기는 최근접이 낫다)
    // 즉 이득은 있지만 원래 주장했던 크기가 아니고, PDF417 회전에서는
    // 오히려 손해다. 40종 게이트가 걸려 있어 유지한다.
    //
    // 비용(실측): 최근접 대비 3배(1200x900에서 1.62 -> 4.81ms). 회전은
    // 구제/ROI 경로에서만 돌고 그 다음 디코드가 13.3ms라, 시도 1회 기준
    // 14.9 -> 18.1ms(+21%)다. 실패 프레임 전체(150~230ms) 대비 +2% 수준.
    // [[vscan-lite-rotate-bilinear]]
    for (int y = 0; y < H; ++y) {
        const float dy = static_cast<float>(y) - pivotY;
        // x=0에서의 소스 좌표. 이후 x 증가마다 (c, s)만 더하면 되므로
        // 행마다 삼각함수를 딱 1번만 계산한다(DDA) — 픽셀마다 cos/sin을
        // 다시 부르는 나이브한 구현 대비 실측 5~6배 빠르다.
        float sx = (0.0f - pivotX) * c - dy * s + pivotX;
        float sy = (0.0f - pivotX) * s + dy * c + pivotY;
        uint8_t* __restrict dstRow = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x, sx += c, sy += s) {
            const int ix = static_cast<int>(sx);
            const int iy = static_cast<int>(sy);
            if (static_cast<unsigned>(ix) < static_cast<unsigned>(W - 1) &&
                static_cast<unsigned>(iy) < static_cast<unsigned>(H - 1)) {
                const float fx = sx - static_cast<float>(ix);
                const float fy = sy - static_cast<float>(iy);
                const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(iy) * stride + ix;
                const uint8_t* __restrict r1 = r0 + stride;
                const float top = r0[0] + (r0[1] - r0[0]) * fx;
                const float bot = r1[0] + (r1[1] - r1[0]) * fx;
                dstRow[x] = static_cast<uint8_t>(top + (bot - top) * fy + 0.5f);
            } else {
                // 가장자리 1픽셀은 이웃이 없으므로 최근접으로 떨어뜨린다.
                const int nx = static_cast<int>(sx + 0.5f), ny = static_cast<int>(sy + 0.5f);
                if (static_cast<unsigned>(nx) < static_cast<unsigned>(W) &&
                    static_cast<unsigned>(ny) < static_cast<unsigned>(H))
                    dstRow[x] = src.pixels[static_cast<size_t>(ny) * stride + nx];
            }
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

void boxBlur3x3(const GrayView& src, GrayImage& out) {
    const int W = src.width, H = src.height;
    const int stride = src.stride > 0 ? src.stride : W;
    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H);
    if (W < 3 || H < 3) {
        for (int y = 0; y < H; ++y)
            std::memcpy(out.pixels.data() + static_cast<size_t>(y) * W,
                        src.pixels + static_cast<size_t>(y) * stride, W);
        return;
    }

    // 1패스: 가로 3탭 합 (16비트 중간 버퍼)
    std::vector<uint16_t> tmp(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict in = src.pixels + static_cast<size_t>(y) * stride;
        uint16_t* __restrict t = tmp.data() + static_cast<size_t>(y) * W;
        t[0] = static_cast<uint16_t>(in[0] + in[0] + in[1]);
        for (int x = 1; x < W - 1; ++x)
            t[x] = static_cast<uint16_t>(in[x - 1] + in[x] + in[x + 1]);
        t[W - 1] = static_cast<uint16_t>(in[W - 2] + in[W - 1] + in[W - 1]);
    }
    // 2패스: 세로 3탭 합 -> /9
    for (int y = 0; y < H; ++y) {
        const uint16_t* __restrict t0 = tmp.data() + static_cast<size_t>(y > 0 ? y - 1 : 0) * W;
        const uint16_t* __restrict t1 = tmp.data() + static_cast<size_t>(y) * W;
        const uint16_t* __restrict t2 = tmp.data() + static_cast<size_t>(y < H - 1 ? y + 1 : H - 1) * W;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x)
            o[x] = static_cast<uint8_t>((t0[x] + t1[x] + t2[x]) / 9);
    }
}

float estimateNoise(const GrayView& src, int rowStep, int colStep) {
    const int W = src.width, H = src.height;
    if (W < 8 || H < 8) return 0.0f;
    const int stride = src.stride > 0 ? src.stride : W;
    if (rowStep < 1) rowStep = 1;
    if (colStep < 1) colStep = 1;

    // 라플라시안 응답 |2v - 좌 - 우| + |2v - 상 - 하|. 최대 4*255*2이지만
    // 노이즈 판정에 쓸 범위는 아래쪽뿐이라 1020에서 자른 히스토그램이면
    // 충분하다(그 위는 전부 "엣지"라 어차피 백분위수 밖이다).
    constexpr int kBins = 1021;
    std::vector<int> hist(kBins, 0);
    long total = 0;
    for (int y = 1; y < H - 1; y += rowStep) {
        const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(y - 1) * stride;
        const uint8_t* __restrict r1 = src.pixels + static_cast<size_t>(y) * stride;
        const uint8_t* __restrict r2 = src.pixels + static_cast<size_t>(y + 1) * stride;
        for (int x = 1; x < W - 1; x += colStep) {
            const int c = 2 * r1[x];
            const int lx = std::abs(c - r1[x - 1] - r1[x + 1]);
            const int ly = std::abs(c - r0[x] - r2[x]);
            int v = lx + ly;
            if (v >= kBins) v = kBins - 1;
            ++hist[v];
            ++total;
        }
    }
    if (total <= 0) return 0.0f;

    const long cut = total / 4;   // 하위 25%
    long acc = 0;
    int p25 = 0;
    for (int i = 0; i < kBins; ++i) {
        acc += hist[i];
        if (acc >= cut) { p25 = i; break; }
    }
    // 라플라시안 응답 두 방향 합이라 진폭 환산은 /2. 절대 눈금이 아니라
    // 임계와 비교하기 위한 값이므로 이 정도 근사로 충분하다.
    return static_cast<float>(p25) * 0.5f;
}

int estimateLocalRange(const GrayView& src, int rowStep, int colStep) {
    const int W = src.width, H = src.height;
    if (W < 16 || H < 16) return 255;
    const int stride = src.stride > 0 ? src.stride : W;
    if (rowStep < 1) rowStep = 1;
    if (colStep < 1) colStep = 1;

    // 8x8 블록(원본 좌표)을 표본 간격대로 훑으며 범위를 모은다. 블록 안은
    // 4점(모서리 근처)만 봐도 범위가 거의 같고 — 모듈 경계가 블록을
    // 가로지르면 어느 두 점을 잡아도 밝고 어두운 쪽이 섞인다 — 비용이
    // 8x8 전수 대비 1/16이다.
    int hist[256] = {0};
    long total = 0;
    for (int y = 0; y + 8 <= H; y += rowStep) {
        for (int x = 0; x + 8 <= W; x += colStep) {
            const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(y) * stride + x;
            const uint8_t* __restrict r3 = r0 + 3 * static_cast<size_t>(stride);
            const uint8_t* __restrict r7 = r0 + 7 * static_cast<size_t>(stride);
            int mn = r0[0], mx = r0[0];
            const int vs[8] = {r0[3], r0[7], r3[0], r3[3], r3[7], r7[0], r7[3], r7[7]};
            for (int v : vs) { if (v < mn) mn = v; if (v > mx) mx = v; }
            ++hist[mx - mn];
            ++total;
        }
    }
    if (total <= 0) return 255;

    // 상위 1%
    const long cut = total / 100;
    long acc = 0;
    for (int i = 255; i >= 0; --i) {
        acc += hist[i];
        if (acc >= cut) return i;
    }
    return 0;
}

void verticalBlur(const GrayView& src, int radius, GrayImage& out) {
    const int W = src.width, H = src.height;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H, 0);
    if (W <= 0 || H <= 0) return;
    const int stride = src.stride > 0 ? src.stride : W;
    const int r = std::max(1, std::min(radius, H / 2));
    const int n = 2 * r + 1;
    // 열 단위로 훑으면 캐시가 죽는다(boxBlur의 세로 패스 주석 참고).
    auto rowAt = [&](int y) {
        return src.pixels + static_cast<size_t>(std::min(H - 1, std::max(0, y))) * stride;
    };
    std::vector<int> colSum(static_cast<size_t>(W), 0);
    for (int y = -r; y <= r; ++y) {
        const uint8_t* __restrict in = rowAt(y);
        for (int x = 0; x < W; ++x) colSum[x] += in[x];
    }
    for (int y = 0; y < H; ++y) {
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) o[x] = static_cast<uint8_t>(colSum[x] / n);
        const uint8_t* __restrict sub = rowAt(y - r);
        const uint8_t* __restrict add = rowAt(y + r + 1);
        for (int x = 0; x < W; ++x) colSum[x] += static_cast<int>(add[x]) - static_cast<int>(sub[x]);
    }
}

void boxBlur(const GrayView& src, int radius, GrayImage& out) {
    const int W = src.width, H = src.height;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H, 0);
    if (W <= 0 || H <= 0) return;
    const int stride = src.stride > 0 ? src.stride : W;
    const int r = std::max(1, std::min(radius, std::min(W, H) / 2));
    const int n = 2 * r + 1;

    std::vector<int> tmp(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        auto px = [&](int x) { return static_cast<int>(row[std::min(W - 1, std::max(0, x))]); };
        int acc = 0;
        for (int x = -r; x <= r; ++x) acc += px(x);
        int* __restrict o = tmp.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            o[x] = acc / n;
            acc -= px(x - r);
            acc += px(x + r + 1);
        }
        // (가로 패스는 누적합이 순차 의존이라 원리적으로 벡터화가 안 된다)
    }
    // [세로 패스는 행 단위로] 열을 하나씩 세로로 훑으면 매 접근이 다른
    // 캐시 라인이라(스트라이드 W) 반경과 무관하게 느려진다 — 실측
    // (2048x1536, 반경 32): 열 단위 57ms. 열별 누적합을 한 줄 배열에
    // 들고 y를 바깥 루프로 돌리면 안쪽이 순차 접근이 된다.
    std::vector<int> colSum(static_cast<size_t>(W), 0);
    auto rowAt = [&](int y) { return tmp.data() + static_cast<size_t>(std::min(H - 1, std::max(0, y))) * W; };
    for (int y = -r; y <= r; ++y) {
        const int* __restrict in = rowAt(y);
        for (int x = 0; x < W; ++x) colSum[x] += in[x];
    }
    /*
     * [나눗셈 하나가 NEON을 통째로 막는다]
     * `colSum[x] / n`은 n이 실행시 결정되는 정수 나눗셈이라 gcc가 벡터화를
     * 포기한다 — aarch64 -O3 -mcpu=cortex-a53로 뽑아보면 이 함수 전체에서
     * SIMD 명령이 3개뿐이었다. 곱셈+시프트로 바꾸면 4레인씩 돈다(24개).
     *
     * [정확해야 한다 — 근사로 넘어가면 안 된다]
     * 처음엔 M=ceil(2^16/n)로 그냥 바꿨는데 **전 범위 전수 검사에서 최대 2까지
     * 어긋났다.** 이 블러는 flattenIllumination의 나눗수로 쓰이므로 값이
     * 흔들리면 조용히 검출이 달라진다.
     *
     * 정확 조건은 알려져 있다: M = ceil(2^K/n)일 때
     *     floor(x*M / 2^K) == floor(x/n)  (0 <= x <= N)
     * 이 성립할 필요충분조건은 (M*n - 2^K) * N <= 2^K 이다. 여기에 int
     * 오버플로가 안 나는 조건(N*M < 2^31)을 더해 **실행시에 검사하고,
     * 안 맞으면 나눗셈으로 되돌린다.** K=21에서 n<=387이면 대체로 통과한다
     * (파이썬으로 n=3..401 전수 x 전 범위 검증: 불일치 0).
     */
    constexpr int kShift = 21;
    const int64_t N = 255LL * n;
    const int M = static_cast<int>((1 << kShift) / n + (((1 << kShift) % n) ? 1 : 0));
    const bool fastDiv = (static_cast<int64_t>(M) * n - (1 << kShift)) * N <= (1 << kShift) &&
                         N * M < (1LL << 31);

    for (int y = 0; y < H; ++y) {
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        const int* __restrict cs = colSum.data();
        if (fastDiv) {
            for (int x = 0; x < W; ++x) o[x] = static_cast<uint8_t>((cs[x] * M) >> kShift);
        } else {
            for (int x = 0; x < W; ++x) o[x] = static_cast<uint8_t>(cs[x] / n);
        }
        const int* __restrict sub = rowAt(y - r);
        const int* __restrict add = rowAt(y + r + 1);
        int* __restrict cw = colSum.data();
        for (int x = 0; x < W; ++x) cw[x] += add[x] - sub[x];
    }
}

bool flattenIllumination(const GrayView& src, int radius, GrayImage& out, int minSpread) {
    const int W = src.width, H = src.height;
    if (W < 32 || H < 32) return false;

    // [배경은 축소본에서 낸다] 배경 추정은 정의상 저주파라 원본 해상도가
    // 필요 없다. 절반으로 줄이면 화소가 1/4이고 반경도 절반으로 줄어든다 —
    // 실측(2048x1536, 반경 32): 원본 해상도 29ms -> 축소본 8ms.
    // (downsampleBox는 배율 2와 3만 받는다. 4를 넘기면 조용히 2가 되므로
    //  여기 상수와 어긋나 배경을 엉뚱한 자리에서 읽는다 — 한 번 당했다.)
    // 되돌릴 때는 **쌍선형**이어야 한다. 최근접으로 했더니 배경이 4px
    // 격자로 계단이 지고, 그 계단이 나눗셈에 그대로 실려 모듈 4~8px짜리
    // 막대 구조를 부숴버렸다 — 그림자 축이 도로 열 종 실패로 돌아갔다.
    constexpr int kDown = 2;
    GrayImage small, sbg;
    downsampleBox(src, small, kDown);
    boxBlur(GrayView(small), std::max(1, radius / kDown), sbg);

    // 배경이 이미 평평하면 할 일이 없다. 상하위 2% 백분위수로 본다 —
    // 최소/최대는 먼지 한 점이 정한다.
    int hist[256] = {0};
    for (size_t i = 0; i < sbg.pixels.size(); ++i) ++hist[sbg.pixels[i]];
    long total = 0;
    for (int v : hist) total += v;
    if (total <= 0) return false;
    const long cut = total / 50;
    int lo = 0, hi = 255;
    for (long acc = 0, i = 0; i < 256; ++i) { acc += hist[i]; if (acc > cut) { lo = static_cast<int>(i); break; } }
    for (long acc = 0, i = 255; i >= 0; --i) { acc += hist[i]; if (acc > cut) { hi = static_cast<int>(i); break; } }
    if (hi - lo < minSpread) return false;

    const int stride = src.stride > 0 ? src.stride : W;
    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H);
    // 되살리는 루프는 화소당 도는 자리라 정수 고정소수점으로 간다.
    // x 사상은 y와 무관하므로 한 번만 만들어 쓴다(부동소수 floor/clamp를
    // 화소마다 돌렸더니 축소로 번 것을 그대로 까먹었다 — 40ms).
    const int sw = sbg.width, sh = sbg.height;
    std::vector<int> mx0(static_cast<size_t>(W)), mx1(static_cast<size_t>(W)), mtx(static_cast<size_t>(W));
    for (int x = 0; x < W; ++x) {
        const int num = (2 * x + 1) * 256 / (2 * kDown) - 128;   // (x+0.5)/k - 0.5, 8비트 소수
        int i0 = num >> 8;
        const int t = num & 255;
        i0 = std::max(0, std::min(sw - 1, i0));
        mx0[static_cast<size_t>(x)] = i0;
        mx1[static_cast<size_t>(x)] = std::max(0, std::min(sw - 1, i0 + 1));
        mtx[static_cast<size_t>(x)] = (num < 0) ? 0 : t;
    }
    for (int y = 0; y < H; ++y) {
        const int numY = (2 * y + 1) * 256 / (2 * kDown) - 128;
        int j0 = numY >> 8;
        const int ty = (numY < 0) ? 0 : (numY & 255);
        j0 = std::max(0, std::min(sh - 1, j0));
        const int j1 = std::max(0, std::min(sh - 1, j0 + 1));
        const uint8_t* __restrict r0 = sbg.pixels.data() + static_cast<size_t>(j0) * sw;
        const uint8_t* __restrict r1 = sbg.pixels.data() + static_cast<size_t>(j1) * sw;
        const uint8_t* __restrict in = src.pixels + static_cast<size_t>(y) * stride;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const int i0 = mx0[static_cast<size_t>(x)], i1 = mx1[static_cast<size_t>(x)];
            const int tx = mtx[static_cast<size_t>(x)];
            const int a = r0[i0] + (((r0[i1] - r0[i0]) * tx) >> 8);
            const int b2 = r1[i0] + (((r1[i1] - r1[i0]) * tx) >> 8);
            const int d = std::max(1, a + (((b2 - a) * ty) >> 8));
            o[x] = static_cast<uint8_t>(std::min(255, 128 * static_cast<int>(in[x]) / d));
        }
    }
    return true;
}

bool rowBinarize(const GrayView& src, GrayImage& out, int pct, int minRange) {
    const int W = src.width, H = src.height;
    if (W < 16 || H < 4 || pct <= 0 || pct >= 50) return false;
    const int stride = src.stride > 0 ? src.stride : W;
    out.width = W;
    out.height = H;
    out.pixels.assign(static_cast<size_t>(W) * H, 255);

    std::vector<uint8_t> buf(static_cast<size_t>(W));
    const size_t kl = static_cast<size_t>(std::min(W - 1, W * pct / 100));
    const size_t kh = static_cast<size_t>(std::min(W - 1, W * (100 - pct) / 100));
    int structured = 0;
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        std::memcpy(buf.data(), row, static_cast<size_t>(W));
        std::nth_element(buf.begin(), buf.begin() + kl, buf.end());
        const int lo = buf[kl];
        std::memcpy(buf.data(), row, static_cast<size_t>(W));
        std::nth_element(buf.begin(), buf.begin() + kh, buf.end());
        const int hi = buf[kh];
        if (hi - lo < minRange) continue;   // 구조가 없는 행은 흰색으로 둔다
        const int thr = (lo + hi) / 2;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) o[x] = row[x] < thr ? 0 : 255;
        ++structured;
    }
    return structured * 4 >= H;
}

bool stretchContrast(const GrayView& src, GrayImage& out, int minSpan,
                     int measureNum, int measureDen) {
    const int W = src.width, H = src.height;
    if (W <= 0 || H <= 0) return false;
    const int stride = src.stride > 0 ? src.stride : W;

    // [범위를 재는 창을 안쪽으로 좁힐 수 있다]
    // 크롭에 딸려 들어온 고대비 장면이 lo/hi를 정해버리면 정작 코드는
    // 안 펴진다 — 근거는 헤더 주석의 실측.
    int mx0 = 0, my0 = 0, mx1 = W, my1 = H;
    if (measureDen > 0 && measureNum > 0 && measureNum < measureDen) {
        const int mw = std::max(8, W * measureNum / measureDen);
        const int mh = std::max(8, H * measureNum / measureDen);
        mx0 = (W - mw) / 2; my0 = (H - mh) / 2;
        mx1 = mx0 + std::min(mw, W); my1 = my0 + std::min(mh, H);
        if (mx1 > W) { mx0 = 0; mx1 = W; }
        if (my1 > H) { my0 = 0; my1 = H; }
    }

    int hist[256] = {0};
    for (int y = my0; y < my1; ++y) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = mx0; x < mx1; ++x) ++hist[row[x]];
    }

    // 하위/상위 0.5%를 잘라낸다. 최소/최대를 그대로 쓰면 먼지 한 점이나
    // 반사광 한 픽셀이 전체 범위를 정해버린다.
    const long total = static_cast<long>(mx1 - mx0) * (my1 - my0);
    const long cut = static_cast<long>(total * 0.005);
    int lo = 0, hi = 255;
    for (long acc = 0, i = 0; i < 256; ++i) { acc += hist[i]; if (acc > cut) { lo = static_cast<int>(i); break; } }
    for (long acc = 0, i = 255; i >= 0; --i) { acc += hist[i]; if (acc > cut) { hi = static_cast<int>(i); break; } }

    const int span = hi - lo;
    if (span >= minSpan || span <= 0) return false;   // 이미 계조를 거의 다 쓰고 있다

    uint8_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = static_cast<uint8_t>(std::min(255, std::max(0, (i - lo) * 255 / span)));

    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict in = src.pixels + static_cast<size_t>(y) * stride;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) o[x] = lut[in[x]];
    }
    return true;
}

bool localAdaptiveBinarize(const GrayView& src, GrayImage& out, bool midpoint, int block,
                           int minRange) {
    const int W = src.width, H = src.height;
    if (W < block * 2 || H < block * 2 || block < 8 || minRange < 1) return false;

    // 1) 노이즈를 먼저 죽인다. 임계 판정이 픽셀 하나하나에 걸리므로
    //    노이즈가 그대로면 결과가 소금후추가 된다. 노이즈는 상관거리가
    //    1px이라 3x3 평균에서 시그마가 1/3로 줄지만, 모듈 몇 px짜리
    //    신호는 거의 그대로 남는다.
    GrayImage blurred;
    boxBlur3x3(src, blurred);
    const uint8_t* __restrict bp = blurred.pixels.data();

    // 2) 블록별 min/max와 "구조 있음" 판정. 통계용이라 2픽셀씩 건너뛰어도
    //    값이 거의 같고 비용은 1/4이다.
    const int bx = (W + block - 1) / block, by = (H + block - 1) / block;
    const size_t nb = static_cast<size_t>(bx) * by;
    std::vector<uint8_t> bmin(nb, 255), bmax(nb, 0), hit(nb, 0);
    for (int gy = 0; gy < by; ++gy) {
        const int y0 = gy * block, y1 = std::min(H, y0 + block);
        for (int gx = 0; gx < bx; ++gx) {
            const int x0 = gx * block, x1 = std::min(W, x0 + block);
            int mn = 255, mx = 0;
            for (int y = y0; y < y1; y += 2) {
                const uint8_t* __restrict row = bp + static_cast<size_t>(y) * W;
                for (int x = x0; x < x1; x += 2) {
                    const int v = row[x];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
            }
            const size_t k = static_cast<size_t>(gy) * bx + gx;
            bmin[k] = static_cast<uint8_t>(mn);
            bmax[k] = static_cast<uint8_t>(mx);
            hit[k] = (mx - mn >= minRange) ? 1 : 0;
        }
    }

    // 3) [임계는 국소 (min+max)/2] 국소 **평균**을 쓰면 굵은 요소가 속이
    //    빈다. 창(반경 block/2)이 통째로 바 안에 들어가면 평균이 바 자신의
    //    밝기가 되어 임계가 그 위아래로 흔들리기 때문이다 — 실측(ITF
    //    module 8, 대비 0.10): 굵은 바가 전부 윤곽선만 남은 속 빈 막대가
    //    됐다. 밝은/어두운 요소의 중간값을 쓰면 그 자리는 확실히 검정이 된다.
    //
    //    그 중간값을 "구조가 있는 블록"에서만 가져오는 것이 핵심이다.
    //    블록이 굵은 요소 안에 통째로 들어가면 자기 min/max는 그 요소
    //    하나뿐이라 쓸 수 없으므로, 이웃의 구조 블록에서 빌린다. 반대로
    //    구조 블록은 **자기 값을 그대로 쓴다** — 이웃에서 빌리게 하면
    //    라벨 종이 가장자리 블록의 max(=밝은 종이)가 코드 쪽으로 새어들어와
    //    코드의 밝은 모듈까지 검게 만든다(그 실패는 헤더 주석 참고).
    std::vector<uint8_t> elo(nb, 0), ehi(nb, 0), valid(nb, 0);
    for (int gy = 0; gy < by; ++gy)
        for (int gx = 0; gx < bx; ++gx) {
            const size_t k = static_cast<size_t>(gy) * bx + gx;
            if (hit[k]) { elo[k] = bmin[k]; ehi[k] = bmax[k]; valid[k] = 1; continue; }
            int mn = 255, mx = 0; bool any = false;
            for (int j2 = std::max(0, gy - 1); j2 <= std::min(by - 1, gy + 1); ++j2)
                for (int i2 = std::max(0, gx - 1); i2 <= std::min(bx - 1, gx + 1); ++i2) {
                    const size_t k2 = static_cast<size_t>(j2) * bx + i2;
                    if (!hit[k2]) continue;
                    mn = std::min(mn, static_cast<int>(bmin[k2]));
                    mx = std::max(mx, static_cast<int>(bmax[k2]));
                    any = true;
                }
            if (any) { elo[k] = static_cast<uint8_t>(mn); ehi[k] = static_cast<uint8_t>(mx); valid[k] = 1; }
        }

    // 4) [구조 마스크 — 닫힘] 균일한 종이를 임계로 가르면 노이즈가 반반
    //    갈려서 없던 무늬가 생긴다. 실제로 그렇게 만들었더니 DataMatrix
    //    정지대가 통째로 검게 칠해져 L-파인더 탐지가 죽었다. 그래서 구조
    //    블록 주변만 이진화하고 나머지는 흰색으로 민다.
    //    팽창만 하면 마스크가 코드 바깥으로 한 블록씩 번져 그 띠에 소금후추가
    //    생기므로(실측: QR 정지대가 깨져 탐지 실패), 침식을 이어 붙여
    //    안쪽 구멍만 메우고 경계는 제자리로 되돌린다.
    std::vector<uint8_t> mask(nb, 0);
    for (int gy = 0; gy < by; ++gy)
        for (int gx = 0; gx < bx; ++gx) {
            int v = 0;
            for (int j2 = std::max(0, gy - 1); j2 <= std::min(by - 1, gy + 1); ++j2)
                for (int i2 = std::max(0, gx - 1); i2 <= std::min(bx - 1, gx + 1); ++i2)
                    v |= hit[static_cast<size_t>(j2) * bx + i2];
            mask[static_cast<size_t>(gy) * bx + gx] = static_cast<uint8_t>(v);
        }
    {
        std::vector<uint8_t> eroded(nb, 0);
        for (int gy = 0; gy < by; ++gy)
            for (int gx = 0; gx < bx; ++gx) {
                int v = 1;
                for (int j2 = std::max(0, gy - 1); j2 <= std::min(by - 1, gy + 1); ++j2)
                    for (int i2 = std::max(0, gx - 1); i2 <= std::min(bx - 1, gx + 1); ++i2)
                        v &= mask[static_cast<size_t>(j2) * bx + i2];
                eroded[static_cast<size_t>(gy) * bx + gx] = static_cast<uint8_t>(v);
            }
        mask.swap(eroded);
    }
    {
        int mx = 0;
        for (uint8_t v : mask) mx = std::max(mx, static_cast<int>(v));
        if (!mx) return false;
    }

    // [평균 임계용 적분영상] midpoint=false일 때만 만든다.
    // 블록 평균을 쌍선형 보간해서 대신 쓰려고 해봤지만 안 된다 — 24px
    // 블록 평균의 보간은 사실상 48px 삼각커널이라 반경 12px 박스 평균과
    // 다르고, 실측에서 Code128 8/8 -> 6/8, CODE39 7/8 -> 5/8로 무너졌다.
    std::vector<uint32_t> integral;
    const int r = block / 2;
    if (!midpoint) {
        integral.assign(static_cast<size_t>(W + 1) * (H + 1), 0);
        for (int y = 0; y < H; ++y) {
            uint32_t rowSum = 0;
            const uint8_t* __restrict in = bp + static_cast<size_t>(y) * W;
            uint32_t* __restrict cur = integral.data() + static_cast<size_t>(y + 1) * (W + 1);
            const uint32_t* __restrict prev = integral.data() + static_cast<size_t>(y) * (W + 1);
            for (int x = 0; x < W; ++x) { rowSum += in[x]; cur[x + 1] = prev[x + 1] + rowSum; }
        }
    }

    // 5) 픽셀마다 블록 중심 기준 쌍선형으로 임계를 보간해서 가른다.
    //    블록 단위로 딱딱 끊으면 그 경계가 그대로 가짜 엣지가 된다.
    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H);
    const float half = static_cast<float>(block) * 0.5f;
    for (int y = 0; y < H; ++y) {
        const int iy0 = std::max(0, y - r), iy1 = std::min(H - 1, y + r);
        const uint32_t* __restrict itop = midpoint ? nullptr : integral.data() + static_cast<size_t>(iy0) * (W + 1);
        const uint32_t* __restrict ibot = midpoint ? nullptr : integral.data() + static_cast<size_t>(iy1 + 1) * (W + 1);

        const float fy = (static_cast<float>(y) - half) / static_cast<float>(block);
        int gy0 = static_cast<int>(std::floor(fy));
        float wy = fy - static_cast<float>(gy0);
        if (gy0 < 0) { gy0 = 0; wy = 0.0f; }
        if (gy0 >= by - 1) { gy0 = by - 1; wy = 0.0f; }
        const int gy1 = std::min(by - 1, gy0 + 1);

        const uint8_t* __restrict in = bp + static_cast<size_t>(y) * W;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const float fx = (static_cast<float>(x) - half) / static_cast<float>(block);
            int gx0 = static_cast<int>(std::floor(fx));
            float wx = fx - static_cast<float>(gx0);
            if (gx0 < 0) { gx0 = 0; wx = 0.0f; }
            if (gx0 >= bx - 1) { gx0 = bx - 1; wx = 0.0f; }
            const int gx1 = std::min(bx - 1, gx0 + 1);

            const size_t k00 = static_cast<size_t>(gy0) * bx + gx0, k01 = static_cast<size_t>(gy0) * bx + gx1;
            const size_t k10 = static_cast<size_t>(gy1) * bx + gx0, k11 = static_cast<size_t>(gy1) * bx + gx1;

            const float m = (mask[k00] * (1 - wx) + mask[k01] * wx) * (1 - wy) +
                            (mask[k10] * (1 - wx) + mask[k11] * wx) * wy;
            if (m < 0.5f || !valid[k00] || !valid[k01] || !valid[k10] || !valid[k11]) { o[x] = 255; continue; }

            float thresh;
            if (midpoint) {
                const float lo = (elo[k00] * (1 - wx) + elo[k01] * wx) * (1 - wy) +
                                 (elo[k10] * (1 - wx) + elo[k11] * wx) * wy;
                const float hi = (ehi[k00] * (1 - wx) + ehi[k01] * wx) * (1 - wy) +
                                 (ehi[k10] * (1 - wx) + ehi[k11] * wx) * wy;
                thresh = (lo + hi) * 0.5f;
            } else {
                const int ix0 = std::max(0, x - r), ix1 = std::min(W - 1, x + r);
                const uint32_t sum = ibot[ix1 + 1] - ibot[ix0] - itop[ix1 + 1] + itop[ix0];
                const int area = (iy1 - iy0 + 1) * (ix1 - ix0 + 1);
                thresh = static_cast<float>(sum / static_cast<uint32_t>(area)) - 2.0f;
            }
            o[x] = (static_cast<float>(in[x]) < thresh) ? 0 : 255;
        }
    }
    return true;
}

void upscaleSharpen(const GrayView& src, int factor, GrayImage& out, int amount) {
    const int W = src.width, H = src.height;
    if (W <= 1 || H <= 1 || factor < 2) return;
    const int stride = src.stride > 0 ? src.stride : W;
    const int dw = W * factor, dh = H * factor;

    /*
     * 1) Lanczos-3 확대 (분리형).
     *
     * 쌍선형으로 시작했다가 실측에서 갈렸다 — 파인더로 찾은 8곳을
     * 확대+언샤프로 디코드했을 때 **쌍선형 6곳 / Lanczos 8곳**이었다.
     * 쌍선형은 이웃 두 픽셀의 선형 보간이라 원본에 없던 고주파를 못 만들고
     * 오히려 통과대역을 깎는다. 모듈이 2px대면 모듈 경계가 바로 그
     * 통과대역 끝에 있어서, 깎이면 언샤프로도 되살릴 게 남지 않는다.
     * Lanczos-3은 sinc 근사라 그 대역을 유지한다.
     *
     * 정수배 확대라 출력 픽셀의 소수부 위상이 factor개로 순환한다.
     * 위상별 6탭 가중치를 미리 계산해두면 픽셀마다 sinc를 부를 일이 없다.
     */
    constexpr int kA = 3;                 // Lanczos 창 반경
    constexpr int kTaps = 2 * kA;         // 6탭
    std::vector<float> wtab(static_cast<size_t>(factor) * kTaps);
    for (int ph = 0; ph < factor; ++ph) {
        // 출력 x가 ph일 때의 소스 좌표 소수부
        const float sx = (static_cast<float>(ph) + 0.5f) / static_cast<float>(factor) - 0.5f;
        const int base = static_cast<int>(std::floor(sx)) - kA + 1;
        float sum = 0.0f;
        for (int t = 0; t < kTaps; ++t) {
            const float d = sx - static_cast<float>(base + t);
            float w;
            if (std::fabs(d) < 1e-6f) {
                w = 1.0f;
            } else if (std::fabs(d) >= static_cast<float>(kA)) {
                w = 0.0f;
            } else {
                const float pd = 3.14159265358979323846f * d;
                w = std::sin(pd) / pd * std::sin(pd / kA) / (pd / kA);
            }
            wtab[static_cast<size_t>(ph) * kTaps + t] = w;
            sum += w;
        }
        // 정규화 — 안 하면 밝기가 위상마다 미세하게 출렁인다.
        if (std::fabs(sum) > 1e-6f)
            for (int t = 0; t < kTaps; ++t) wtab[static_cast<size_t>(ph) * kTaps + t] /= sum;
    }

    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

    // 가로 확대 -> 세로 확대 (분리형이라 6탭 x 2회)
    std::vector<float> mid(static_cast<size_t>(dw) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        float* __restrict o = mid.data() + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            const int ix = x / factor, ph = x % factor;
            const int base = ix + (static_cast<int>(std::floor((static_cast<float>(ph) + 0.5f) /
                                                              static_cast<float>(factor) - 0.5f)) -
                                   kA + 1);
            const float* __restrict w = wtab.data() + static_cast<size_t>(ph) * kTaps;
            float acc = 0.0f;
            for (int t = 0; t < kTaps; ++t) acc += w[t] * row[clampi(base + t, 0, W - 1)];
            o[x] = acc;
        }
    }

    GrayImage up;
    up.width = dw;
    up.height = dh;
    up.pixels.resize(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const int iy = y / factor, ph = y % factor;
        const int base = iy + (static_cast<int>(std::floor((static_cast<float>(ph) + 0.5f) /
                                                          static_cast<float>(factor) - 0.5f)) -
                               kA + 1);
        const float* __restrict w = wtab.data() + static_cast<size_t>(ph) * kTaps;
        uint8_t* __restrict o = up.pixels.data() + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            float acc = 0.0f;
            for (int t = 0; t < kTaps; ++t)
                acc += w[t] * mid[static_cast<size_t>(clampi(base + t, 0, H - 1)) * dw + x];
            const int v = static_cast<int>(acc + 0.5f);
            o[x] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }

    /*
     * 2) 언샤프: out = up + amount% * (up - blur(up)).
     *
     * 블러 반경이 핵심이다. 되살리려는 건 "원본 1픽셀" 크기의 구조이고
     * 그건 확대본에서 factor픽셀이므로, 블러의 시그마가 factor 정도라야
     * 그 대역이 차분에 남는다.
     *
     * 3x3 박스 블러를 n번 겹치면 시그마는 sqrt(n * 8/12) = sqrt(2n/3)이다.
     * 이걸 factor로 맞추려면 n = 1.5 * factor^2 — 처음에 n = factor로 뒀다가
     * (3배 확대에서 시그마 1.41, 목표 3.0) 판독이 8곳 중 6곳에 그쳤다.
     * ROI가 작아서 반복 비용은 감당된다.
     */
    const int blurIters = std::min(64, std::max(1, static_cast<int>(1.5f * factor * factor + 0.5f)));
    GrayImage blurred = up;
    for (int i = 0; i < blurIters; ++i) {
        GrayImage tmp;
        boxBlur3x3(GrayView(blurred), tmp);
        blurred = std::move(tmp);
    }

    out.width = dw;
    out.height = dh;
    out.pixels.resize(static_cast<size_t>(dw) * dh);
    for (size_t i = 0; i < out.pixels.size(); ++i) {
        const int base = up.pixels[i];
        const int v = base + (base - blurred.pixels[i]) * amount / 100;
        out.pixels[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

bool tightenToContent(const GrayView& src, GrayImage& out, int marginPx, int* offX, int* offY) {
    const int W = src.width, H = src.height;
    if (W < 64 || H < 64) return false;
    const int stride = src.stride > 0 ? src.stride : W;

    // 2픽셀 건너뛰며 훑는다 — 경계를 픽셀 단위로 정확히 잡을 필요가 없다
    // (어차피 여백을 덧붙인다). 표본이 1/4이라 비용도 1/4다.
    const int step = 2;
    int lo = 255, hi = 0;
    for (int y = 0; y < H; y += step) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < W; x += step) {
            const int v = row[x];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (hi - lo < 24) return false;   // 명암이 없으면 자를 근거도 없다
    const int thresh = lo + (hi - lo) * 2 / 5;

    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; y += step) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < W; x += step) {
            if (row[x] > thresh) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0 || y1 < y0) return false;

    x0 = std::max(0, x0 - marginPx);
    y0 = std::max(0, y0 - marginPx);
    x1 = std::min(W - 1, x1 + marginPx);
    y1 = std::min(H - 1, y1 + marginPx);
    const int tw = x1 - x0 + 1, th = y1 - y0 + 1;
    if (tw < 32 || th < 32) return false;
    // 줄어드는 게 얼마 없으면 복사 비용만 낸다.
    if (static_cast<double>(tw) * th > 0.75 * static_cast<double>(W) * H) return false;

    if (offX) *offX = x0;
    if (offY) *offY = y0;
    out.width = tw;
    out.height = th;
    out.pixels.resize(static_cast<size_t>(tw) * th);
    for (int y = 0; y < th; ++y)
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * tw,
                    src.pixels + static_cast<size_t>(y0 + y) * stride + x0, tw);
    return true;
}

} // namespace vscan
