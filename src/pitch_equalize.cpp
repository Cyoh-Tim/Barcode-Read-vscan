#include "vscan_internal/pitch_equalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vscan {

bool pitchEqualize(const GrayView& src, GrayImage& out, PitchMap* map,
                   int winRuns, int pct, float outModule) {
    const int W = src.width, H = src.height;
    if (W < 64 || H < 24 || winRuns < 3 || outModule < 2.0f) return false;
    const int stride = src.stride > 0 ? src.stride : W;

    // [임계] 크롭 자신의 명암에서 (perspectiveRectify와 같은 이유).
    int hist[256] = {0};
    for (int y = 0; y < H; y += 2) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < W; x += 2) ++hist[row[x]];
    }
    long total = 0;
    for (int v : hist) total += v;
    if (total <= 0) return false;
    const long cut = total / 50;
    int lo = 0, hi = 255;
    for (long acc = 0, i = 0; i < 256; ++i) { acc += hist[i]; if (acc > cut) { lo = static_cast<int>(i); break; } }
    for (long acc = 0, i = 255; i >= 0; --i) { acc += hist[i]; if (acc > cut) { hi = static_cast<int>(i); break; } }
    if (hi - lo < 24) return false;
    const int thr = (lo + hi) / 2;

    auto dark = [&](int x, int y) { return src.pixels[static_cast<size_t>(y) * stride + x] < thr; };

    int cy0 = H, cy1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (dark(x, y)) { if (y < cy0) cy0 = y; cy1 = y; break; }
    if (cy1 - cy0 < 12) return false;

    // [대표 행] 전환이 가장 많은 행을 쓴다. 1D는 어느 행이나 같지만
    // 적층 코드(PDF417/DataBarExp)는 행마다 내용이 달라, 런이 많은 행이
    // 모듈 추정에 표본을 더 준다.
    int bestY = -1, bestN = 0, bx0 = 0, bx1 = 0;
    for (int i = 0; i < 15; ++i) {
        const int y = cy0 + (cy1 - cy0) * (2 + i * 6) / 100;
        if (y < 0 || y >= H) continue;
        int x0 = -1, x1 = -1;
        for (int x = 0; x < W; ++x) if (dark(x, y)) { x0 = x; break; }
        for (int x = W - 1; x >= 0; --x) if (dark(x, y)) { x1 = x; break; }
        if (x0 < 0 || x1 - x0 < 40) continue;
        int n = 0;
        bool prev = dark(x0, y);
        for (int x = x0 + 1; x <= x1; ++x) {
            const bool c = dark(x, y);
            if (c != prev) { ++n; prev = c; }
        }
        if (n > bestN) { bestN = n; bestY = y; bx0 = x0; bx1 = x1; }
    }
    if (bestY < 0 || bestN < 20) return false;

    // 런 길이와 중심
    std::vector<float> runLen, runCent;
    runLen.reserve(bestN + 2);
    runCent.reserve(bestN + 2);
    {
        int start = bx0;
        bool prev = dark(bx0, bestY);
        for (int x = bx0 + 1; x <= bx1 + 1; ++x) {
            const bool c = (x <= bx1) ? dark(x, bestY) : !prev;
            if (c != prev) {
                runLen.push_back(static_cast<float>(x - start));
                runCent.push_back(0.5f * static_cast<float>(x + start));
                start = x;
                prev = c;
            }
        }
    }
    if (runLen.size() < 12) return false;

    // [국소 모듈] 이동창 안 런 길이의 하위 백분위수. 런은 모듈의 정수배
    // (1~4)라 하위값이 곧 1모듈이다.
    const int n = static_cast<int>(runLen.size());
    std::vector<float> mod(n);
    std::vector<float> buf;
    buf.reserve(winRuns + 1);
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - winRuns / 2), b = std::min(n, i + winRuns / 2 + 1);
        buf.assign(runLen.begin() + a, runLen.begin() + b);
        const size_t k = static_cast<size_t>(buf.size() * pct / 100);
        std::nth_element(buf.begin(), buf.begin() + std::min(k, buf.size() - 1), buf.end());
        mod[i] = std::max(1.0f, buf[std::min(k, buf.size() - 1)]);
    }

    // [모듈 좌표] u(x) = 적분 1/m. m은 런 중심에서만 알므로 선형 보간한다.
    std::vector<double> u(static_cast<size_t>(W) + 1, 0.0);
    int seg = 0;
    for (int x = 0; x < W; ++x) {
        while (seg + 1 < n && runCent[seg + 1] < static_cast<float>(x)) ++seg;
        double m;
        if (static_cast<float>(x) <= runCent[0]) m = mod[0];
        else if (static_cast<float>(x) >= runCent[n - 1]) m = mod[n - 1];
        else {
            const float t = (static_cast<float>(x) - runCent[seg]) /
                            std::max(1e-3f, runCent[seg + 1] - runCent[seg]);
            m = mod[seg] * (1.0f - t) + mod[seg + 1] * t;
        }
        u[x + 1] = u[x] + 1.0 / std::max(1.0, m);
    }

    const double u0 = u[std::max(0, bx0)], u1 = u[std::min(W, bx1 + 1)];
    const double span = u1 - u0;
    if (!(span > 4.0)) return false;
    const int dw = static_cast<int>(span * outModule) + 1;
    if (dw < 40 || dw > 6000) return false;

    const int marginX = 60, marginY = 40;
    const int ow = dw + 2 * marginX, oh = H + 2 * marginY;
    out.width = ow;
    out.height = oh;
    out.pixels.assign(static_cast<size_t>(ow) * oh, 255);
    if (map) { map->srcX.assign(dw, 0.0f); map->marginX = marginX; map->marginY = marginY; }

    // 균등한 u 격자 -> 원본 x (u가 단조증가라 한 방향 훑기로 역함수를 얻는다)
    int xs = 0;
    for (int i = 0; i < dw; ++i) {
        const double target = u0 + static_cast<double>(i) / outModule;
        while (xs + 1 < W && u[xs + 1] < target) ++xs;
        double sx = xs;
        const double du = u[xs + 1] - u[xs];
        if (du > 1e-9) sx = xs + (target - u[xs]) / du;
        sx = std::max(0.0, std::min(static_cast<double>(W - 2), sx));
        if (map) map->srcX[i] = static_cast<float>(sx);

        const int ix = static_cast<int>(sx);
        const double fx = sx - ix;
        for (int y = 0; y < H; ++y) {
            const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride + ix;
            const double v = row[0] + (row[1] - row[0]) * fx;
            out.pixels[static_cast<size_t>(y + marginY) * ow + marginX + i] =
                static_cast<uint8_t>(v + 0.5);
        }
    }
    return true;
}

} // namespace vscan
