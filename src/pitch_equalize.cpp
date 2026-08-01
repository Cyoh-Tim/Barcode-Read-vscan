#include "vscan_internal/pitch_equalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vscan {

float pitchVariation(const GrayView& src) {
    const int W = src.width, H = src.height;
    if (W < 96 || H < 24) return 0.0f;
    const int stride = src.stride > 0 ? src.stride : W;

    int hist[256] = {0};
    for (int y = 0; y < H; y += 4) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < W; x += 2) ++hist[row[x]];
    }
    long total = 0;
    for (int v : hist) total += v;
    if (total <= 0) return 0.0f;
    const long cut = total / 50;
    int lo = 0, hi = 255;
    for (long acc = 0, i = 0; i < 256; ++i) { acc += hist[i]; if (acc > cut) { lo = static_cast<int>(i); break; } }
    for (long acc = 0, i = 255; i >= 0; --i) { acc += hist[i]; if (acc > cut) { hi = static_cast<int>(i); break; } }
    if (hi - lo < 24) return 0.0f;
    const int thr = (lo + hi) / 2;

    // 코드가 있는 행 몇 개에서 런 길이를 모아 좌/우 1/3으로 나눈다.
    std::vector<float> left, right;
    for (int i = 0; i < 5; ++i) {
        const int y = H * (25 + i * 12) / 100;
        if (y < 0 || y >= H) continue;
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        int start = -1;
        bool prev = false;
        for (int x = 0; x < W; ++x) {
            const bool d = row[x] < thr;
            if (start < 0) { start = x; prev = d; continue; }
            if (d != prev) {
                const float len = static_cast<float>(x - start);
                const int mid = (x + start) / 2;
                if (mid < W / 3) left.push_back(len);
                else if (mid > 2 * W / 3) right.push_back(len);
                start = x;
                prev = d;
            }
        }
    }
    if (left.size() < 6 || right.size() < 6) return 0.0f;
    auto lowPct = [](std::vector<float>& v) {
        const size_t k = v.size() / 4;
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return std::max(1.0f, v[k]);
    };
    const float a = lowPct(left), b = lowPct(right);
    return (a > b) ? a / b : b / a;
}

bool pitchEqualize(const GrayView& src, GrayImage& out, PitchMap* map,
                   int rows, int winRuns, int pct, float outModule) {
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

    // [표본 행 고르기] rows==1이면 전환이 가장 많은 한 행, rows>1이면
    // 코드 높이를 균등 분할한 여러 행. 어느 쪽이 맞는지는 코드 종류에
    // 달렸다 — 둘 다 필요해서 파이프라인이 두 번 부른다(아래 주석).
    std::vector<int> ys;
    if (rows <= 1) {
        int bestY = -1, bestN = 0;
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
            if (n > bestN) { bestN = n; bestY = y; }
        }
        if (bestY < 0 || bestN < 20) return false;
        ys.push_back(bestY);
    } else {
        for (int i = 0; i < rows; ++i) {
            const int y = cy0 + (cy1 - cy0) * (10 + i * 80 / (rows - 1)) / 100;
            if (y >= 0 && y < H) ys.push_back(y);
        }
    }

    // [런을 모은다] 여러 행이면 x 위치로 묶어 백분위수를 내므로 행마다
    // 따로 볼 필요가 없다 — 그냥 한 통에 붓는다.
    std::vector<float> cent, len;
    cent.reserve(512);
    len.reserve(512);
    int gx0 = W, gx1 = -1;
    for (int y : ys) {
        int x0 = -1, x1 = -1;
        for (int x = 0; x < W; ++x) if (dark(x, y)) { x0 = x; break; }
        for (int x = W - 1; x >= 0; --x) if (dark(x, y)) { x1 = x; break; }
        if (x0 < 0 || x1 - x0 < 40) continue;
        int nCh = 0;
        int start = x0;
        bool prev = dark(x0, y);
        for (int x = x0 + 1; x <= x1 + 1; ++x) {
            const bool c = (x <= x1) ? dark(x, y) : !prev;
            if (c != prev) {
                len.push_back(static_cast<float>(x - start));
                cent.push_back(0.5f * static_cast<float>(x + start));
                start = x;
                prev = c;
                ++nCh;
            }
        }
        if (nCh >= 8) { gx0 = std::min(gx0, x0); gx1 = std::max(gx1, x1); }
    }
    if (len.size() < (rows <= 1 ? 12u : 24u) || gx1 - gx0 < 40) return false;

    // 중심 기준 정렬 (창을 밀며 훑기 위해). 1행이면 이미 정렬돼 있다.
    if (rows > 1) {
        std::vector<int> idx(len.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
        std::sort(idx.begin(), idx.end(), [&](int a2, int b2) { return cent[a2] < cent[b2]; });
        std::vector<float> c2(len.size()), l2(len.size());
        for (size_t i = 0; i < idx.size(); ++i) { c2[i] = cent[idx[i]]; l2[i] = len[idx[i]]; }
        cent.swap(c2); len.swap(l2);
    }

    // [국소 모듈] 창 안 런 길이의 **하위 백분위수**. 런은 모듈의 정수배
    // (1~4)라 하위값이 곧 1모듈이다.
    //
    // 창을 어떻게 잡느냐가 두 갈래로 갈린다. 1행일 때는 런 인덱스로
    // 세는 창(앞뒤 winRuns개)이 맞다 — 배율이 변해도 항상 같은 개수의
    // 모듈을 본다. 여러 행일 때는 런이 x축에서 뒤섞이므로 인덱스 창이
    // 물리적으로 얼마나 넓은지 알 수 없다. 그래서 픽셀 창으로 바꾼다.
    // 두 추정기는 결과가 다르다 — 섞지 말고 따로 둘 것.
    std::vector<double> u(static_cast<size_t>(W) + 1, 0.0);
    if (rows <= 1) {
        const int n = static_cast<int>(len.size());
        std::vector<float> mod(static_cast<size_t>(n));
        std::vector<float> buf;
        buf.reserve(static_cast<size_t>(winRuns) + 1);
        for (int i = 0; i < n; ++i) {
            const int a = std::max(0, i - winRuns / 2), b = std::min(n, i + winRuns / 2 + 1);
            buf.assign(len.begin() + a, len.begin() + b);
            const size_t k = std::min(static_cast<size_t>(buf.size() * pct / 100), buf.size() - 1);
            std::nth_element(buf.begin(), buf.begin() + k, buf.end());
            mod[static_cast<size_t>(i)] = std::max(1.0f, buf[k]);
        }
        // m은 런 중심에서만 알므로 중심 사이를 선형 보간한다.
        int seg = 0;
        for (int x = 0; x < W; ++x) {
            while (seg + 1 < n && cent[seg + 1] < static_cast<float>(x)) ++seg;
            double m;
            if (static_cast<float>(x) <= cent[0]) m = mod[0];
            else if (static_cast<float>(x) >= cent[n - 1]) m = mod[n - 1];
            else {
                const float t = (static_cast<float>(x) - cent[seg]) /
                                std::max(1e-3f, cent[seg + 1] - cent[seg]);
                m = mod[seg] * (1.0f - t) + mod[seg + 1] * t;
            }
            u[x + 1] = u[x] + 1.0 / std::max(1.0, m);
        }
    } else {
        float medLen;
        {
            std::vector<float> t = len;
            std::nth_element(t.begin(), t.begin() + t.size() / 2, t.end());
            medLen = t[t.size() / 2];
        }
        const float winPx = std::max(24.0f, medLen * static_cast<float>(winRuns));
        size_t l = 0, r = 0;
        float last = std::max(1.0f, medLen);
        std::vector<float> buf;
        for (int x = 0; x < W; ++x) {
            const float a2 = static_cast<float>(x) - winPx * 0.5f;
            const float b2 = static_cast<float>(x) + winPx * 0.5f;
            while (l < cent.size() && cent[l] < a2) ++l;
            while (r < cent.size() && cent[r] <= b2) ++r;
            if (r > l + 3) {
                buf.assign(len.begin() + l, len.begin() + r);
                const size_t k = std::min(buf.size() - 1, static_cast<size_t>(buf.size() * pct / 100));
                std::nth_element(buf.begin(), buf.begin() + k, buf.end());
                last = std::max(1.0f, buf[k]);
            }
            u[x + 1] = u[x] + 1.0 / static_cast<double>(last);
        }
    }

    const double u0 = u[std::max(0, gx0)], u1 = u[std::min(W, gx1 + 1)];
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
