#include "vscan_internal/locate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace vscan {
namespace {

// 임의 배율 박스 다운샘플. deskew1d.cpp에도 같은 함수가 있지만 그쪽은
// 파일 내부(익명 네임스페이스)라 공유가 안 된다 — 20줄짜리를 헤더로
// 끌어올리는 것보다 여기 두는 편이 의존 관계가 단순하다.
void downsampleN(const GrayView& src, int factor, std::vector<uint8_t>& dst, int& dw, int& dh) {
    dw = src.width / factor;
    dh = src.height / factor;
    dst.assign(static_cast<size_t>(dw) * dh, 0);
    const int stride = src.stride > 0 ? src.stride : src.width;
    for (int y = 0; y < dh; ++y) {
        uint8_t* __restrict out = dst.data() + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            int sum = 0;
            for (int j = 0; j < factor; ++j) {
                const uint8_t* __restrict row =
                    src.pixels + static_cast<size_t>(y * factor + j) * stride + x * factor;
                for (int i = 0; i < factor; ++i) sum += row[i];
            }
            out[x] = static_cast<uint8_t>(sum / (factor * factor));
        }
    }
}

} // namespace

std::vector<CodeRegion> findCodeRegions(const GrayView& image, int maxRegions, int tileSize,
                                        int downscale, float energyRatio) {
    std::vector<CodeRegion> out;
    if (image.empty() || maxRegions <= 0) return out;
    if (image.width < downscale * tileSize * 2 || image.height < downscale * tileSize * 2) return out;

    std::vector<uint8_t> small;
    int dw, dh;
    downsampleN(image, downscale, small, dw, dh);

    const int tilesX = dw / tileSize;
    const int tilesY = dh / tileSize;
    if (tilesX < 2 || tilesY < 2) return out;

    // [타일별 에너지] Sobel 배열을 따로 만들지 않고 타일 루프 안에서
    // 바로 누적한다 — deskew1d는 구조텐서(방향)까지 필요해서 gx/gy를
    // 프레임 전체 크기로 들고 있어야 했지만, 여기는 스칼라 하나만
    // 있으면 되므로 중간 버퍼(float 2장 = 다운샘플 픽셀당 8바이트)를
    // 통째로 없앨 수 있다. 512x384 다운샘플 기준 1.5MB 절약.
    //
    // 에너지 척도는 |gx| + |gy| (L1). sqrt를 안 쓰는 이유는 deskew1d와
    // 같다 — 리덕션 루프 안의 sqrt가 자동 벡터화를 막는다. 여기서는
    // 타일 간 상대 랭킹만 쓰므로 L1으로 충분하다.
    // [[vscan-lite-locate-regions]]
    std::vector<float> energy(static_cast<size_t>(tilesX) * tilesY, 0.0f);
    // 방향 추정용 구조텐서. 후보 판정에는 안 쓰지만(2D 코드를 걸러내면
    // 안 되므로) 같은 루프에서 곱셈 세 번이면 나오는 값이라, 나중에
    // 회전이 필요한 심볼로지(PDF417/1D)를 위해 미리 모아둔다.
    std::vector<float> tjxx(energy.size(), 0.0f), tjyy(energy.size(), 0.0f), tjxy(energy.size(), 0.0f);
    float maxEnergy = 0.0f;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int y0 = ty * tileSize, x0 = tx * tileSize;
            // Sobel은 3x3이라 다운샘플 이미지 경계 1px을 못 본다.
            const int ys = std::max(1, y0), ye = std::min(dh - 1, y0 + tileSize);
            const int xs = std::max(1, x0), xe = std::min(dw - 1, x0 + tileSize);
            float acc = 0.0f;
            float jxx = 0.0f, jyy = 0.0f, jxy = 0.0f;
            for (int y = ys; y < ye; ++y) {
                const uint8_t* __restrict r0 = small.data() + static_cast<size_t>(y - 1) * dw;
                const uint8_t* __restrict r1 = small.data() + static_cast<size_t>(y) * dw;
                const uint8_t* __restrict r2 = small.data() + static_cast<size_t>(y + 1) * dw;
                for (int x = xs; x < xe; ++x) {
                    int sx = (r0[x + 1] + 2 * r1[x + 1] + r2[x + 1]) -
                             (r0[x - 1] + 2 * r1[x - 1] + r2[x - 1]);
                    int sy = (r2[x - 1] + 2 * r2[x] + r2[x + 1]) -
                             (r0[x - 1] + 2 * r0[x] + r0[x + 1]);
                    acc += static_cast<float>(std::abs(sx) + std::abs(sy));
                    const float fx = static_cast<float>(sx), fy = static_cast<float>(sy);
                    jxx += fx * fx;
                    jyy += fy * fy;
                    jxy += fx * fy;
                }
            }
            float e = acc / static_cast<float>(tileSize * tileSize);
            const size_t ti = static_cast<size_t>(ty) * tilesX + tx;
            energy[ti] = e;
            tjxx[ti] = jxx; tjyy[ti] = jyy; tjxy[ti] = jxy;
            maxEnergy = std::max(maxEnergy, e);
        }
    }
    if (maxEnergy <= 0.0f) return out;

    const float thresh = maxEnergy * energyRatio;
    std::vector<uint8_t> hot(energy.size(), 0);
    for (size_t i = 0; i < energy.size(); ++i) hot[i] = energy[i] >= thresh ? 1 : 0;

    // [연결 성분] 8이웃. 코드가 대각으로 기울어져 있으면 4이웃으로는
    // 한 덩어리가 여러 조각으로 갈라진다 — 정확히 회전된 코드에서
    // 문제가 되는 케이스라 대각까지 잇는다.
    std::vector<uint8_t> visited(energy.size(), 0);
    std::vector<int> stack;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            size_t start = static_cast<size_t>(ty) * tilesX + tx;
            if (!hot[start] || visited[start]) continue;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            visited[start] = 1;
            int minX = tx, maxX = tx, minY = ty, maxY = ty;
            float sum = 0.0f;
            double sJxx = 0, sJyy = 0, sJxy = 0;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % tilesX, cy = cur / tilesX;
                sum += energy[cur];
                sJxx += tjxx[cur]; sJyy += tjyy[cur]; sJxy += tjxy[cur];
                minX = std::min(minX, cx); maxX = std::max(maxX, cx);
                minY = std::min(minY, cy); maxY = std::max(maxY, cy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || nx >= tilesX || ny < 0 || ny >= tilesY) continue;
                        size_t nidx = static_cast<size_t>(ny) * tilesX + nx;
                        if (hot[nidx] && !visited[nidx]) {
                            visited[nidx] = 1;
                            stack.push_back(static_cast<int>(nidx));
                        }
                    }
                }
            }
            const int scale = tileSize * downscale;
            CodeRegion r;
            r.bbox.x0 = std::max(0, minX * scale);
            r.bbox.y0 = std::max(0, minY * scale);
            r.bbox.x1 = std::min(image.width, (maxX + 1) * scale);
            r.bbox.y1 = std::min(image.height, (maxY + 1) * scale);
            r.energy = sum;

            // [방향 추정] 구조텐서의 지배 방향. 그래디언트는 막대에
            // 수직이므로 theta가 곧 막대의 기울기다. 90도 배수 차이는
            // zxing의 TryRotate가 처리하므로 (-45, 45]로 접는다.
            // 임계 0.25는 deskew1d.cpp와 같은 값 — 방향성이 이보다 약하면
            // 2D 행렬코드이거나 배경이라, 회전해봐야 얻을 게 없다.
            const double denom = sJxx - sJyy;
            const double coh = std::sqrt(denom * denom + 4.0 * sJxy * sJxy) / (sJxx + sJyy + 1e-6);
            if (coh >= 0.25) {
                double theta = 0.5 * std::atan2(2.0 * sJxy, denom) * 180.0 / 3.14159265358979323846;
                while (theta > 45.0) theta -= 90.0;
                while (theta <= -45.0) theta += 90.0;
                r.angleDeg = static_cast<float>(theta);
            }
            out.push_back(r);
        }
    }

    // 에너지가 큰 덩어리부터. "면적이 큰 순"이 아닌 이유는, 배경의
    // 넓고 옅은 텍스처가 면적으로는 코드를 이길 수 있어서다.
    std::sort(out.begin(), out.end(),
              [](const CodeRegion& a, const CodeRegion& b) { return a.energy > b.energy; });
    if (static_cast<int>(out.size()) > maxRegions) out.resize(maxRegions);
    return out;
}

float refineRegionAngle(const GrayView& image, const Rect& bbox) {
    const int x0 = std::max(0, bbox.x0), y0 = std::max(0, bbox.y0);
    const int x1 = std::min(image.width, bbox.x1), y1 = std::min(image.height, bbox.y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w < 8 || h < 8) return CodeRegion::kAngleUnknown;

    const int stride = image.stride > 0 ? image.stride : image.width;
    double jxx = 0, jyy = 0, jxy = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        const uint8_t* __restrict r0 = image.pixels + static_cast<size_t>(y - 1) * stride;
        const uint8_t* __restrict r1 = image.pixels + static_cast<size_t>(y) * stride;
        const uint8_t* __restrict r2 = image.pixels + static_cast<size_t>(y + 1) * stride;
        for (int x = x0 + 1; x < x1 - 1; ++x) {
            const double gx = (r0[x + 1] + 2 * r1[x + 1] + r2[x + 1]) -
                              (r0[x - 1] + 2 * r1[x - 1] + r2[x - 1]);
            const double gy = (r2[x - 1] + 2 * r2[x] + r2[x + 1]) -
                              (r0[x - 1] + 2 * r0[x] + r0[x + 1]);
            jxx += gx * gx;
            jyy += gy * gy;
            jxy += gx * gy;
        }
    }
    const double denom = jxx - jyy;
    const double coh = std::sqrt(denom * denom + 4.0 * jxy * jxy) / (jxx + jyy + 1e-6);
    if (coh < 0.25) return CodeRegion::kAngleUnknown;
    double theta = 0.5 * std::atan2(2.0 * jxy, denom) * 180.0 / 3.14159265358979323846;
    while (theta > 45.0) theta -= 90.0;
    while (theta <= -45.0) theta += 90.0;
    return static_cast<float>(theta);
}

namespace {

// 프로파일의 "코드 구간"을 찾는다. 최대값이 아니라 **백분위수**를 기준으로
// 삼는 것이 핵심이다.
//
// 왜 최대값이면 안 되나. 라벨 종이의 가장자리는 한 행/열에 수백 픽셀짜리
// 강한 단차가 통째로 들어가서 프로파일의 최대값을 혼자 정해버린다.
// 실측(Code128 module 8, 대비 0.10): 그 최대값 기준으로 보면 코드 행이
// 0.19, 배경 행이 0.15로 둘이 거의 붙어 있어서 어떤 임계로도 안 갈린다
// (0.12~0.55 전부 창 전체를 반환했다). p10을 바닥, p90을 꼭대기로 삼으면
// 그 두 이상치가 빠지면서 996x354(참값 973x331)로 정확히 잡힌다.
bool profileBounds(const std::vector<float>& v, float ratio, int& lo, int& hi) {
    const int n = static_cast<int>(v.size());
    if (n < 8) return false;

    // 이동 평균(창 24표본 = 원본 48px). 1D 바코드는 바 사이에서 프로파일이
    // 0으로 떨어지는 자리가 있어서, 원시값에 임계를 걸면 그 골에서 코드가
    // 끊긴다.
    std::vector<double> pre(n + 1, 0.0);
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + v[i];
    const int half = 12;
    std::vector<float> sv(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - half), b = std::min(n, i + half + 1);
        sv[i] = static_cast<float>((pre[b] - pre[a]) / (b - a));
    }

    std::vector<float> sorted = sv;
    std::nth_element(sorted.begin(), sorted.begin() + n / 10, sorted.end());
    const float p10 = sorted[n / 10];
    sorted = sv;
    std::nth_element(sorted.begin(), sorted.begin() + (n * 9) / 10, sorted.end());
    const float p90 = sorted[(n * 9) / 10];
    if (p90 <= p10) return false;

    const float th = p10 + ratio * (p90 - p10);
    int a = -1, b = -1;
    for (int i = 0; i < n; ++i) if (sv[i] >= th) { a = i; break; }
    for (int i = n - 1; i >= 0; --i) if (sv[i] >= th) { b = i; break; }
    if (a < 0 || b < a) return false;
    lo = a; hi = b;
    return true;
}

} // namespace

Rect refineRegionBox(const GrayView& image, const Rect& bbox, float energyRatio, int searchMargin) {
    const int wx0 = std::max(0, bbox.x0 - searchMargin);
    const int wy0 = std::max(0, bbox.y0 - searchMargin);
    const int wx1 = std::min(image.width, bbox.x1 + searchMargin);
    const int wy1 = std::min(image.height, bbox.y1 + searchMargin);
    if (wx1 - wx0 < 32 || wy1 - wy0 < 32) return bbox;

    const int stride = image.stride > 0 ? image.stride : image.width;
    // 2픽셀 간격 표본. **빈 칸을 남기지 않고 압축해서** 담는 것이 중요하다 —
    // 표본이 없는 자리를 0으로 채우면 백분위수가 그 0들에 끌려간다.
    const int ys0 = wy0 + 1, xs0 = wx0 + 1;
    const int hs = std::max(0, (wy1 - 1 - ys0 + 1) / 2);
    const int ws = std::max(0, (wx1 - 1 - xs0 + 1) / 2);
    if (hs < 8 || ws < 8) return bbox;

    auto energyAt = [&](int y, int x) -> float {
        const uint8_t* __restrict r0 = image.pixels + static_cast<size_t>(y - 1) * stride;
        const uint8_t* __restrict r1 = image.pixels + static_cast<size_t>(y) * stride;
        const uint8_t* __restrict r2 = image.pixels + static_cast<size_t>(y + 1) * stride;
        return static_cast<float>(std::abs(static_cast<int>(r1[x + 1]) - static_cast<int>(r1[x - 1])) +
                                  std::abs(static_cast<int>(r2[x]) - static_cast<int>(r0[x])));
    };

    // [1차] 행 프로파일. 행은 코드 폭 전체를 지나므로 배경과 잘 갈린다.
    std::vector<float> rowE(static_cast<size_t>(hs), 0.0f);
    for (int i = 0; i < hs; ++i) {
        const int y = ys0 + 2 * i;
        float acc = 0.0f;
        for (int j = 0; j < ws; ++j) acc += energyAt(y, xs0 + 2 * j);
        rowE[static_cast<size_t>(i)] = acc;
    }
    int ri0, ri1;
    if (!profileBounds(rowE, energyRatio, ri0, ri1)) return bbox;

    // [2차] 열 프로파일 — **1차에서 찾은 행 범위 안에서만** 잰다.
    // 창 전체에서 재면 코드 없는 행들이 모든 열에 똑같이 더해져서 코드
    // 열과 배경 열의 차이가 희석된다. 실제로 그것 때문에 열 경계가
    // 창 전체로 나왔었다(행은 제대로 잡혔는데 열만 안 잡혔다).
    std::vector<float> colE(static_cast<size_t>(ws), 0.0f);
    for (int i = ri0; i <= ri1; ++i) {
        const int y = ys0 + 2 * i;
        for (int j = 0; j < ws; ++j) colE[static_cast<size_t>(j)] += energyAt(y, xs0 + 2 * j);
    }
    int ci0, ci1;
    if (!profileBounds(colE, energyRatio, ci0, ci1)) return bbox;

    Rect out{xs0 + 2 * ci0, ys0 + 2 * ri0, xs0 + 2 * ci1 + 1, ys0 + 2 * ri1 + 1};
    out.x0 = std::max(0, out.x0); out.y0 = std::max(0, out.y0);
    out.x1 = std::min(image.width, out.x1); out.y1 = std::min(image.height, out.y1);
    const long ow = out.x1 - out.x0, oh = out.y1 - out.y0;
    if (ow < 16 || oh < 16) return bbox;
    // 좁히려고 부른 함수다. 결과가 더 넓으면 판정이 실패한 것이므로
    // 원래 상자를 쓴다(창을 넓혀 잡은 만큼 커지는 것을 그대로 두면
    // ROI가 오히려 비싸진다 — 실측에서 그것 때문에 대비 구간이
    // 28 -> 108ms로 뒤집혔다).
    if (ow * oh >= static_cast<long>(bbox.x1 - bbox.x0) * (bbox.y1 - bbox.y0)) return bbox;
    return out;
}

} // namespace vscan
