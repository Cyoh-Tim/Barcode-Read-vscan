#include "vscan_internal/deskew_persp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vscan {
namespace {

struct Line { double a, b, c; };   // a*x + b*y + c = 0, (a,b)는 단위 법선
struct Pt { double x, y; };

/*
 * 반복 절단 총최소자승 직선 맞춤.
 *
 * 총최소자승(직교거리 최소화)을 쓰는 이유: 세로에 가까운 변(좌/우)을
 * y = f(x)로 맞추면 발산한다. 공분산 행렬의 작은 고유값에 대응하는
 * 고유벡터가 법선이고, 2x2라 닫힌 해가 있다(SVD 필요 없음).
 *
 * 절단이 핵심이다. EAN/UPC의 가드바는 본체보다 아래로 삐져나오고
 * DataBar는 아래쪽에 보조 패턴이 있어서, 단순 최소자승이면 아래 변이
 * 그 소수 점들에 끌려간다. 잔차 상위 25%를 버리고 다시 맞추기를
 * 네 번 반복한다.
 */
bool fitLineRobust(std::vector<Pt> pts, Line& out) {
    if (pts.size() < 8) return false;
    for (int iter = 0; iter < 5; ++iter) {
        double mx = 0, my = 0;
        for (const Pt& p : pts) { mx += p.x; my += p.y; }
        mx /= static_cast<double>(pts.size());
        my /= static_cast<double>(pts.size());

        double sxx = 0, syy = 0, sxy = 0;
        for (const Pt& p : pts) {
            const double dx = p.x - mx, dy = p.y - my;
            sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
        }
        // 2x2 대칭행렬의 작은 고유값에 대응하는 고유벡터 = 법선
        const double tr = sxx + syy;
        const double det = sxx * syy - sxy * sxy;
        double disc = tr * tr / 4.0 - det;
        if (disc < 0) disc = 0;
        const double lmin = tr / 2.0 - std::sqrt(disc);
        double nx, ny;
        if (std::fabs(sxy) > 1e-9) { nx = lmin - syy; ny = sxy; }
        else if (sxx <= syy)       { nx = 1.0; ny = 0.0; }
        else                       { nx = 0.0; ny = 1.0; }
        const double nn = std::hypot(nx, ny);
        if (nn < 1e-12) return false;
        nx /= nn; ny /= nn;
        out.a = nx; out.b = ny; out.c = -(nx * mx + ny * my);

        if (iter == 4 || pts.size() < 16) break;
        // 잔차 상위 25% 절단
        std::vector<double> d;
        d.reserve(pts.size());
        for (const Pt& p : pts) d.push_back(std::fabs(nx * p.x + ny * p.y + out.c));
        std::vector<double> sorted = d;
        const size_t keep = sorted.size() * 3 / 4;
        std::nth_element(sorted.begin(), sorted.begin() + keep, sorted.end());
        const double thr = std::max(sorted[keep], 1e-6);
        std::vector<Pt> next;
        next.reserve(keep + 1);
        for (size_t i = 0; i < pts.size(); ++i)
            if (d[i] <= thr) next.push_back(pts[i]);
        if (next.size() < 8) break;
        pts.swap(next);
    }
    return true;
}

bool intersect(const Line& l1, const Line& l2, Pt& out) {
    const double det = l1.a * l2.b - l2.a * l1.b;
    if (std::fabs(det) < 1e-9) return false;   // 평행
    out.x = (-l1.c * l2.b + l2.c * l1.b) / det;
    out.y = (-l1.a * l2.c + l2.a * l1.c) / det;
    return true;
}

} // namespace

bool perspectiveRectify(const GrayView& src, GrayImage& out, int marginPx, RectifyMap* map) {
    const int W = src.width, H = src.height;
    if (W < 48 || H < 24) return false;
    const int stride = src.stride > 0 ? src.stride : W;

    // [임계] 크롭 자신의 명암에서 잡는다 — 노출이 프레임마다 다르므로
    // 고정 상수를 쓸 수 없다(tightenToContent와 같은 이유).
    int hist[256] = {0};
    for (int y = 0; y < H; y += 2) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < W; x += 2) ++hist[row[x]];
    }
    long total = 0;
    for (int v : hist) total += v;
    if (total <= 0) return false;
    const long cut = total / 50;   // 하위/상위 2%
    int lo = 0, hi = 255;
    for (long acc = 0, i = 0; i < 256; ++i) { acc += hist[i]; if (acc > cut) { lo = static_cast<int>(i); break; } }
    for (long acc = 0, i = 255; i >= 0; --i) { acc += hist[i]; if (acc > cut) { hi = static_cast<int>(i); break; } }
    if (hi - lo < 24) return false;             // 명암이 없으면 윤곽도 없다
    const int thr = (lo + hi) / 2;

    auto dark = [&](int x, int y) -> bool {
        return src.pixels[static_cast<size_t>(y) * stride + x] < thr;
    };

    // 열/행마다 가장자리 점을 모은다. 양끝 5%는 빼둔다 — 모서리 근처는
    // 두 변이 만나는 자리라 어느 변에 속하는지 애매하다.
    std::vector<Pt> top, bot, left, right;
    top.reserve(W); bot.reserve(W); left.reserve(H); right.reserve(H);
    int cx0 = W, cx1 = -1, cy0 = H, cy1 = -1;
    for (int x = 0; x < W; ++x)
        for (int y = 0; y < H; ++y)
            if (dark(x, y)) { if (x < cx0) cx0 = x; if (x > cx1) cx1 = x; break; }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (dark(x, y)) { if (y < cy0) cy0 = y; if (y > cy1) cy1 = y; break; }
    if (cx1 - cx0 < 40 || cy1 - cy0 < 16) return false;

    // [좌/우 변을 먼저] 행마다의 최좌/최우 점은 볼록한 도형에서 항상
    // 좌/우 변 위에 있으므로 신뢰할 수 있다. 위/아래 변은 그렇지 않다 —
    // 모서리 근처 열에서는 최상/최하 점이 **좌우 변** 위에 놓인다.
    const int my = std::max(2, (cy1 - cy0) / 20);
    for (int y = cy0 + my; y <= cy1 - my; ++y) {
        int xl = -1, xr = -1;
        for (int x = 0; x < W; ++x) if (dark(x, y)) { xl = x; break; }
        for (int x = W - 1; x >= 0; --x) if (dark(x, y)) { xr = x; break; }
        if (xl >= 0) left.push_back({static_cast<double>(xl), static_cast<double>(y)});
        if (xr >= 0) right.push_back({static_cast<double>(xr), static_cast<double>(y)});
    }
    Line ll, lr;
    if (!fitLineRobust(left, ll) || !fitLineRobust(right, lr)) return false;

    // [모서리 제외 폭을 기울기에서 계산한다]
    // 평행사변형에서 좌변이 높이 Hc에 걸쳐 가로로 |dx/dy| * Hc 만큼
    // 움직이면, 그 폭만큼의 열에서는 최상단 점이 좌변 위에 있다.
    // 고정 5%로 잘랐더니 전단이 큰 코드에서 위 변 표본에 좌우 변이 섞여
    // 직선 맞춤이 무너졌다 — 실측(PDF417 persp 0.3, 크롭 1098x221):
    // 모서리 교점이 x=-3547로 나와 사각형 추정이 통째로 실패했다.
    auto slopeOf = [](const Line& L) {
        return (std::fabs(L.a) < 1e-9) ? 1e9 : std::fabs(L.b / L.a);
    };
    const int codeH = cy1 - cy0, codeW = cx1 - cx0;
    const double sl = std::min(slopeOf(ll), slopeOf(lr));
    int mx = static_cast<int>(sl * codeH) + std::max(2, codeW / 20);
    mx = std::min(mx, static_cast<int>(codeW * 0.45));
    for (int x = cx0 + mx; x <= cx1 - mx; ++x) {
        int ytop = -1, ybot = -1;
        for (int y = 0; y < H; ++y) if (dark(x, y)) { ytop = y; break; }
        for (int y = H - 1; y >= 0; --y) if (dark(x, y)) { ybot = y; break; }
        if (ytop >= 0) top.push_back({static_cast<double>(x), static_cast<double>(ytop)});
        if (ybot >= 0) bot.push_back({static_cast<double>(x), static_cast<double>(ybot)});
    }

    Line lt, lb;
    if (!fitLineRobust(top, lt) || !fitLineRobust(bot, lb)) return false;

    Pt TL, TR, BR, BL;
    if (!intersect(lt, ll, TL) || !intersect(lt, lr, TR) ||
        !intersect(lb, lr, BR) || !intersect(lb, ll, BL)) return false;

    // 사각형이 크롭 밖으로 크게 벗어나면 변 추정이 실패한 것이다.
    const double slack = 0.5;
    for (const Pt& p : {TL, TR, BR, BL}) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
        if (p.x < -slack * W || p.x > W * (1 + slack)) return false;
        if (p.y < -slack * H || p.y > H * (1 + slack)) return false;
    }

    const double wTop = std::hypot(TR.x - TL.x, TR.y - TL.y);
    const double wBot = std::hypot(BR.x - BL.x, BR.y - BL.y);
    const double hL   = std::hypot(BL.x - TL.x, BL.y - TL.y);
    const double hR   = std::hypot(BR.x - TR.x, BR.y - TR.y);
    const int dw = static_cast<int>(std::max(wTop, wBot));
    const int dh = static_cast<int>(std::max(hL, hR));
    if (dw < 40 || dh < 16 || dw > 4000 || dh > 4000) return false;

    // [이미 직사각형이면 하지 않는다] 마주보는 변의 길이가 거의 같고
    // 기울기도 거의 같으면 펴봐야 결과가 그대로다 — 리샘플 비용만 든다.
    const double wRatio = std::min(wTop, wBot) / std::max(wTop, wBot);
    const double hRatio = std::min(hL, hR) / std::max(hL, hR);
    if (wRatio > 0.97 && hRatio > 0.97) return false;

    /*
     * [호모그래피] 목적지(직사각형) -> 원본(사각형). 목적지가 항상 축
     * 정렬 직사각형이므로 "단위 정사각형 -> 사각형"의 닫힌 해를 쓰고
     * 앞에 1/dw, 1/dh 스케일만 곱하면 된다. 일반 8x8 선형해가 필요 없다.
     */
    const double x0 = TL.x, y0 = TL.y, x1 = TR.x, y1 = TR.y;
    const double x2 = BR.x, y2 = BR.y, x3 = BL.x, y3 = BL.y;
    const double dx1 = x1 - x2, dx2 = x3 - x2, sx = x0 - x1 + x2 - x3;
    const double dy1 = y1 - y2, dy2 = y3 - y2, sy = y0 - y1 + y2 - y3;
    double ha, hb, hc, hd, he, hf, hg, hh;
    const double den = dx1 * dy2 - dx2 * dy1;
    if (std::fabs(sx) < 1e-9 && std::fabs(sy) < 1e-9) {
        hg = hh = 0.0;
        ha = x1 - x0; hb = x3 - x0; hc = x0;
        hd = y1 - y0; he = y3 - y0; hf = y0;
    } else {
        if (std::fabs(den) < 1e-9) return false;
        hg = (sx * dy2 - dx2 * sy) / den;
        hh = (dx1 * sy - sx * dy1) / den;
        ha = x1 - x0 + hg * x1; hb = x3 - x0 + hh * x3; hc = x0;
        hd = y1 - y0 + hg * y1; he = y3 - y0 + hh * y3; hf = y0;
    }

    if (map) {
        map->a = ha; map->b = hb; map->c = hc;
        map->d = hd; map->e = he; map->f = hf;
        map->g = hg; map->h = hh;
        map->margin = marginPx; map->dw = dw; map->dh = dh;
    }

    const int ow = dw + 2 * marginPx, oh = dh + 2 * marginPx;
    out.width = ow;
    out.height = oh;
    out.pixels.assign(static_cast<size_t>(ow) * oh, 255);
    const double iw = 1.0 / static_cast<double>(dw), ih = 1.0 / static_cast<double>(dh);
    for (int y = 0; y < dh; ++y) {
        const double v = (static_cast<double>(y) + 0.5) * ih;
        uint8_t* __restrict o = out.pixels.data() + static_cast<size_t>(y + marginPx) * ow + marginPx;
        for (int x = 0; x < dw; ++x) {
            const double u = (static_cast<double>(x) + 0.5) * iw;
            const double w = hg * u + hh * v + 1.0;
            if (std::fabs(w) < 1e-9) continue;
            const double sxf = (ha * u + hb * v + hc) / w;
            const double syf = (hd * u + he * v + hf) / w;
            const int ix = static_cast<int>(sxf), iy = static_cast<int>(syf);
            if (static_cast<unsigned>(ix) >= static_cast<unsigned>(W - 1) ||
                static_cast<unsigned>(iy) >= static_cast<unsigned>(H - 1))
                continue;
            // 쌍선형 — 회전 구제와 같은 이유로 최근접은 모듈 폭 비율을
            // 흔든다([[vscan-lite-rotate-bilinear]]).
            const double fx = sxf - ix, fy = syf - iy;
            const uint8_t* __restrict r0 = src.pixels + static_cast<size_t>(iy) * stride + ix;
            const uint8_t* __restrict r1 = r0 + stride;
            const double t = r0[0] + (r0[1] - r0[0]) * fx;
            const double b = r1[0] + (r1[1] - r1[0]) * fx;
            o[x] = static_cast<uint8_t>(t + (b - t) * fy + 0.5);
        }
    }
    return true;
}

void rectifyMapBack(const RectifyMap& m, double x, double y, double& sx, double& sy) {
    const double u = (x - m.margin + 0.5) / static_cast<double>(m.dw);
    const double v = (y - m.margin + 0.5) / static_cast<double>(m.dh);
    const double w = m.g * u + m.h * v + 1.0;
    if (std::fabs(w) < 1e-9) { sx = x; sy = y; return; }
    sx = (m.a * u + m.b * v + m.c) / w;
    sy = (m.d * u + m.e * v + m.f) / w;
}

} // namespace vscan
