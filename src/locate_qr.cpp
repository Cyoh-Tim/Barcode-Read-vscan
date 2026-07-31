#include "vscan_internal/locate_qr.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace vscan {
namespace {

// 파인더 하나를 한 행에서 본 흔적.
struct FinderHit {
    float cx;      // 패턴 중심 x (부분화소)
    float cy;      // 행 y
    float module;  // 이 행에서 잰 모듈 크기 = 전체폭 / 7
};

// 여러 행에 걸쳐 같은 자리에서 나온 흔적을 뭉친 것.
struct FinderPoint {
    float cx = 0, cy = 0, module = 0;
    int votes = 0;
};

/*
 * [국소 이진화] 블록 평균에서 조금 낮은 값을 임계로 쓴다.
 *
 * 전역 임계를 쓰면 안 되는 이유는 이 탐지기가 노리는 상황 그 자체다 —
 * 조명이 기울고 대비가 낮은 프레임에서 작은 코드를 찾는 것이라, 프레임
 * 한쪽의 흰색이 다른 쪽의 검정보다 어두울 수 있다. 블록 평균이면
 * 그런 기울기가 자동으로 상쇄된다.
 *
 * 블록 크기 16px은 "코드보다 크고 조명 변화보다 작은" 값이다. 45px짜리
 * 코드 안에 블록이 3x3으로 들어가므로 코드 자체가 임계를 끌어올리는
 * 일은 생기지만, 파인더는 흑백이 반반이라 그 평균 근처에서 잘 갈린다.
 */
void binarizeLocal(const GrayView& src, std::vector<uint8_t>& out, int block = 16, int bias = 8) {
    const int W = src.width, H = src.height;
    const int stride = src.stride > 0 ? src.stride : W;
    const int bw = (W + block - 1) / block, bh = (H + block - 1) / block;

    std::vector<uint16_t> mean(static_cast<size_t>(bw) * bh, 0);
    for (int by = 0; by < bh; ++by) {
        const int y0 = by * block, y1 = std::min(H, y0 + block);
        for (int bx = 0; bx < bw; ++bx) {
            const int x0 = bx * block, x1 = std::min(W, x0 + block);
            unsigned sum = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
                for (int x = x0; x < x1; ++x) sum += row[x];
            }
            const int n = (y1 - y0) * (x1 - x0);
            mean[static_cast<size_t>(by) * bw + bx] = static_cast<uint16_t>(n ? sum / n : 128);
        }
    }

    out.assign(static_cast<size_t>(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const uint8_t* __restrict row = src.pixels + static_cast<size_t>(y) * stride;
        uint8_t* __restrict o = out.data() + static_cast<size_t>(y) * W;
        const uint16_t* __restrict m = mean.data() + static_cast<size_t>(y / block) * bw;
        for (int x = 0; x < W; ++x) o[x] = row[x] < static_cast<int>(m[x / block]) - bias ? 1 : 0;
    }
}

// 한 행에서 1:1:3:1:1(흑백흑백흑)을 찾는다.
void scanRow(const uint8_t* row, int W, int y, float minModulePx, float maxModulePx,
             std::vector<FinderHit>& out) {
    // 런 길이 5개를 굴리며 본다. 새 런이 나올 때마다 한 칸 밀어서,
    // 항상 "직전 5개"를 검사한다 — 행당 한 번만 훑는다.
    int len[5] = {0, 0, 0, 0, 0};
    int val[5] = {0, 0, 0, 0, 0};
    int start5 = 0;   // len[0] 런이 시작한 x
    int runStart = 0;

    auto push = [&](int s, int n, int v) {
        start5 += len[0];
        for (int i = 0; i < 4; ++i) { len[i] = len[i + 1]; val[i] = val[i + 1]; }
        len[4] = n; val[4] = v;
        (void)s;
        // 흑-백-흑-백-흑 순서가 아니면 볼 것도 없다.
        if (!(val[0] && !val[1] && val[2] && !val[3] && val[4])) return;
        const float total = static_cast<float>(len[0] + len[1] + len[2] + len[3] + len[4]);
        const float u = total / 7.0f;
        if (u < minModulePx || u > maxModulePx) return;
        // 각 런이 기대치(1,1,3,1,1)*u에서 0.7u 이내.
        // 0.5u면 규격상 정확하지만 흐린 이미지에서 경계 한 칸이 이웃으로
        // 새는 일이 잦아 놓친다 — 최종 판정은 어차피 디코더가 한다.
        const float exp[5] = {u, u, 3.0f * u, u, u};
        for (int i = 0; i < 5; ++i)
            if (std::fabs(static_cast<float>(len[i]) - exp[i]) > 0.7f * u) return;
        FinderHit h;
        h.cx = static_cast<float>(start5) + total * 0.5f;
        h.cy = static_cast<float>(y);
        h.module = u;
        out.push_back(h);
    };

    int cur = row[0];
    for (int x = 1; x < W; ++x) {
        if (row[x] == cur) continue;
        push(runStart, x - runStart, cur);
        runStart = x;
        cur = row[x];
    }
    push(runStart, W - runStart, cur);
}

/*
 * [부분화소 정합] 파인더 중심을 원본 그레이스케일에서 다시 잡는다.
 *
 * 이진화한 런 경계는 정수 좌표라 오차가 ±0.5px인데, 모듈이 2.1px이면
 * 그게 4분의 1 모듈이고 격자를 세우면 반대편에서 누적된다. 대신 원본의
 * **50% 교차점**을 선형 보간으로 잡으면 픽셀 사이를 나눌 수 있다.
 *
 * 파인더의 중심을 지나는 선은 항상 이 모양이다:
 *   흰(정지대) 검(1) 흰(1) 검(3) 흰(1) 검(1) 흰(정지대)
 * 그래서 **처음 흰->검 교차점과 마지막 검->흰 교차점의 중점**이 중심이다.
 * 안쪽 경계들은 흐림에 더 오염되므로 바깥 두 개만 쓴다.
 *
 * 여러 줄에서 재서 평균 낸다 — 한 줄만 보면 그 줄의 잡음이 그대로 남는다.
 */
float refineAxis(const GrayView& img, float cx, float cy, float module, bool horizontal) {
    const int stride = img.stride > 0 ? img.stride : img.width;
    const int reach = std::max(4, static_cast<int>(5.0f * module));
    const int lines = std::max(1, static_cast<int>(module));   // 중심 ±(모듈/2)줄

    // 국소 흑/백 수준: 창 안의 최소/최대에서 조금 들어온 값
    int lo = 255, hi = 0;
    for (int d = -reach; d <= reach; ++d) {
        for (int e = -lines; e <= lines; ++e) {
            const int x = static_cast<int>(horizontal ? cx + d : cx + e);
            const int y = static_cast<int>(horizontal ? cy + e : cy + d);
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const int v = img.pixels[static_cast<size_t>(y) * stride + x];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (hi - lo < 20) return horizontal ? cx : cy;
    const float mid = 0.5f * (lo + hi);

    double sum = 0;
    int n = 0;
    for (int e = -lines; e <= lines; ++e) {
        // 이 줄의 값을 뽑는다
        std::vector<float> prof;
        prof.reserve(2 * reach + 1);
        bool ok = true;
        for (int d = -reach; d <= reach; ++d) {
            const int x = static_cast<int>(horizontal ? cx + d : cx + e);
            const int y = static_cast<int>(horizontal ? cy + e : cy + d);
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) { ok = false; break; }
            prof.push_back(static_cast<float>(img.pixels[static_cast<size_t>(y) * stride + x]));
        }
        if (!ok || prof.size() < 5) continue;

        // [교차점 전부 모으기] 부분화소 위치와 방향(내려감=흰->검)을 함께.
        std::vector<std::pair<float, bool>> cross;   // (위치, 내려감?)
        for (size_t i = 1; i < prof.size(); ++i) {
            if (prof[i - 1] > mid && prof[i] <= mid) {
                const float t = (prof[i - 1] - mid) / (prof[i - 1] - prof[i]);
                cross.emplace_back(static_cast<float>(i - 1) + t, true);
            } else if (prof[i - 1] <= mid && prof[i] > mid) {
                const float t = (mid - prof[i - 1]) / (prof[i] - prof[i - 1]);
                cross.emplace_back(static_cast<float>(i - 1) + t, false);
            }
        }
        if (cross.size() < 6) continue;

        /*
         * [1:1:3:1:1로 검증한다] 파인더 중심을 지나는 줄에는 교차점이
         * 정확히 여섯 개 있고(흰검흰검흰검흰), 그 사이 다섯 구간의 비가
         * 1:1:3:1:1이다. 창 안에서 그 조건을 만족하는 연속 6개를 찾아
         * 바깥 두 개의 중점을 쓴다.
         *
         * 앞서 "창 안의 첫 교차 ~ 마지막 교차"로 했다가 두 팔 길이가
         * 10%까지 어긋났다 — 정지대에 먼지가 있거나 옆 데이터 칸이
         * 붙어 있으면 엉뚱한 교차점을 집는다. 중심을 안 지나는 줄
         * (파인더 위/아래를 스치는 줄)도 그렇게 걸러진다.
         */
        float best = -1;
        float bestErr = 1e9f;
        for (size_t i = 0; i + 5 < cross.size(); ++i) {
            if (!cross[i].second) continue;   // 흰->검으로 시작해야 한다
            float g[5];
            for (int k = 0; k < 5; ++k) g[k] = cross[i + k + 1].first - cross[i + k].first;
            const float tot = g[0] + g[1] + g[2] + g[3] + g[4];
            if (tot < 4.0f) continue;
            const float u = tot / 7.0f;
            const float want[5] = {u, u, 3.0f * u, u, u};
            float err = 0;
            for (int k = 0; k < 5; ++k) err += std::fabs(g[k] - want[k]) / u;
            // 이 줄에서 잰 모듈이 후보 전체의 모듈과 크게 다르면 다른 것이다.
            if (u < 0.55f * module || u > 1.8f * module) continue;
            if (err < bestErr) { bestErr = err; best = (cross[i].first + cross[i + 5].first) * 0.5f; }
        }
        // 다섯 구간 합쳐 평균 0.35모듈 이내로 안 맞으면 신뢰하지 않는다.
        if (best < 0 || bestErr > 1.75f) continue;
        sum += best - reach;   // 창 중심 기준 오프셋
        ++n;
    }
    if (n == 0) return horizontal ? cx : cy;
    return (horizontal ? cx : cy) + static_cast<float>(sum / n);
}

} // namespace

std::vector<QrCandidate> findQrCandidates(const GrayView& image, int maxCandidates,
                                          float minModulePx, float maxModulePx, int rowStep) {
    std::vector<QrCandidate> out;
    if (image.empty() || image.width < 32 || image.height < 32 || maxCandidates <= 0) return out;

    std::vector<uint8_t> bw;
    binarizeLocal(image, bw);

    std::vector<FinderHit> hits;
    for (int y = 0; y < image.height; y += std::max(1, rowStep))
        scanRow(bw.data() + static_cast<size_t>(y) * image.width, image.width, y, minModulePx,
                maxModulePx, hits);

    // [세로도 훑는다] 파인더는 상하좌우 대칭이라 세로로 잘라도 1:1:3:1:1이다.
    // 가로만 보면, 흐림이 방향에 따라 다르거나(렌즈 수차·모션) 코드가
    // 조금 기울어졌을 때 통과하는 행이 하나도 없는 파인더가 생긴다.
    // 세로 스캔은 같은 파인더를 다른 축에서 다시 잡아주므로 그런 것들을
    // 건진다. 두 축의 결과는 아래에서 같은 자리로 뭉쳐지므로 중복은
    // 표(votes)만 늘릴 뿐이다.
    {
        std::vector<uint8_t> col(static_cast<size_t>(image.height));
        std::vector<FinderHit> vhits;
        for (int x = 0; x < image.width; x += std::max(1, rowStep)) {
            for (int y = 0; y < image.height; ++y)
                col[y] = bw[static_cast<size_t>(y) * image.width + x];
            vhits.clear();
            scanRow(col.data(), image.height, x, minModulePx, maxModulePx, vhits);
            // scanRow는 "행" 기준이라 (cx, cy)가 (세로위치, x)로 나온다. 되돌린다.
            for (auto& h : vhits) { const float t = h.cx; h.cx = h.cy; h.cy = t; hits.push_back(h); }
        }
    }
    if (hits.size() < 3) return out;

    // [뭉치기] 같은 파인더는 인접한 여러 행에서 거의 같은 x로 나온다.
    // 허용 반경을 모듈 크기에 비례시킨다 — 큰 코드는 파인더도 커서
    // 행마다 중심이 더 흔들린다.
    std::vector<FinderPoint> pts;
    for (const auto& h : hits) {
        bool merged = false;
        for (auto& p : pts) {
            const float tol = std::max(4.0f, 2.5f * p.module);
            if (std::fabs(p.cx - h.cx) < tol && std::fabs(p.cy - h.cy) < tol) {
                const float n = static_cast<float>(p.votes);
                p.cx = (p.cx * n + h.cx) / (n + 1);
                p.cy = (p.cy * n + h.cy) / (n + 1);
                p.module = (p.module * n + h.module) / (n + 1);
                ++p.votes;
                merged = true;
                break;
            }
        }
        if (!merged) pts.push_back({h.cx, h.cy, h.module, 1});
    }
    // [부분화소 정합] 원본 그레이스케일의 50% 교차점으로 중심을 다시 잡는다.
    // 근거는 refineAxis() 주석. 무게중심으로도 해봤는데(창 ±2.5모듈)
    // 두 팔 길이 비만 0.94~1.13 -> 0.98~1.08로 좋아지고 격자는 여전히
    // 밀렸다 — 무게중심은 옆 데이터 칸의 흑백 분포에 끌린다.
    // 두 축을 각각 정합한다. x는 가로 줄에서, y는 세로 줄에서.
    // 두 번 돌리는 이유는 첫 번째 정합으로 중심이 옮겨가면 두 번째 줄
    // 위치가 달라져 더 정확해지기 때문이다(1회 추가로 수렴한다).
    for (auto& p : pts) {
        for (int pass = 0; pass < 2; ++pass) {
            const float nx = refineAxis(image, p.cx, p.cy, p.module, true);
            const float ny = refineAxis(image, p.cx, p.cy, p.module, false);
            p.cx = nx;
            p.cy = ny;
        }
    }

    // 표가 적은 것은 잡음이다 — 파인더는 가로 7모듈 x 세로 7모듈이라
    // 가로 스캔에서도 세로 스캔에서도 여러 번 걸린다. 임계 3은 실측값:
    // 2로 두면 후보가 9곳 나오는데 그중 하나가 가짜였고(ROI 디코드를
    // 헛돌린다), 3이면 8곳 전부 진짜였다.
    pts.erase(std::remove_if(pts.begin(), pts.end(), [](const FinderPoint& p) { return p.votes < 3; }),
              pts.end());
    if (pts.size() < 3) return out;

    // [삼각형 맞추기] 파인더 셋은 코드의 세 모서리라, 중심끼리는
    // **직각이등변**을 이룬다(두 변이 같고 빗변이 그 1.414배).
    // 후보가 많으면 조합이 폭발하므로 개수를 제한한다 — 파인더가 아주
    // 많이 나오는 프레임은 대개 텍스처(격자무늬 등)라 얻을 게 없다.
    // 자르기 전에 **표(votes) 많은 순**으로 정렬한다. 정렬 없이 자르면
    // 스캔 순서(위에서 아래)로 남아서, 프레임 아래쪽 코드가 통째로
    // 날아간다 — 실측으로 후보가 11곳에서 4곳으로 줄었다.
    std::sort(pts.begin(), pts.end(),
              [](const FinderPoint& a, const FinderPoint& b) { return a.votes > b.votes; });
    if (pts.size() > 96) pts.resize(96);

    // [삼각형 후보 모으기] 조건을 통과한 조합을 모아 **변이 짧은 순**으로
    // 고른다. 짧은 순인 이유가 핵심이다 — 같은 크기의 코드가 격자로
    // 늘어선 프레임(해상도 차트가 정확히 그렇다)에서는 **서로 다른 코드의
    // 파인더끼리도 직각이등변을 이룬다.** 실측: 그냥 첫 조합부터 쓰면
    // 705x870, 1149x839 같은 상자가 나왔다(코드 하나는 45x45px인데).
    // 진짜 파인더 셋은 자기 코드 안에 있으므로 언제나 가장 가깝다.
    struct Tri { size_t i, j, k; float leg, mod; };
    std::vector<Tri> tris;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            for (size_t k = j + 1; k < n; ++k) {
                const FinderPoint* P[3] = {&pts[i], &pts[j], &pts[k]};
                // 한 코드의 파인더 셋은 모듈 크기가 같다. 많이 다르면
                // 서로 다른 코드(혹은 잡음)에서 온 것이다.
                const float mlo = std::min({P[0]->module, P[1]->module, P[2]->module});
                const float mhi = std::max({P[0]->module, P[1]->module, P[2]->module});
                if (mhi > 1.5f * mlo) continue;
                const float mod = (P[0]->module + P[1]->module + P[2]->module) / 3.0f;

                float d[3];
                d[0] = std::hypot(P[0]->cx - P[1]->cx, P[0]->cy - P[1]->cy);
                d[1] = std::hypot(P[0]->cx - P[2]->cx, P[0]->cy - P[2]->cy);
                d[2] = std::hypot(P[1]->cx - P[2]->cx, P[1]->cy - P[2]->cy);
                std::sort(d, d + 3);
                if (d[1] < 1e-3f) continue;
                if (std::fabs(d[0] / d[1] - 1.0f) > 0.25f) continue;
                if (std::fabs(d[2] / d[1] - 1.414f) > 0.25f) continue;

                // 변 길이 = (버전모듈수 - 7) 모듈. 버전1(21모듈)이면 14모듈,
                // 최대 버전40(177모듈)이면 170모듈이다. 이 범위를 벗어나면
                // QR일 수 없다 — 위의 "다른 코드끼리 짝지음"을 걸러내는
                // 가장 강한 조건이기도 하다.
                // [크기 상한] 이 탐지기는 **작은 코드 전용**이다. 큰 QR은
                // 일반 경로가 이미 잘 찾으므로 여기서 볼 이유가 없고,
                // 상한을 두지 않으면 같은 크기 코드가 격자로 늘어선
                // 프레임에서 **이웃 코드의 파인더끼리** 짝지어진 거대한
                // 가짜 상자가 나온다(실측: 45x45 진짜 5곳과 함께 298x280,
                // 486x374 같은 가짜 6곳).
                //
                // 규격 격자(변이 14, 18, 22, ... 모듈)로 걸러보려 했지만
                // 안 된다 — 변이 90모듈쯤 되면 격자 간격 4모듈이 전체의
                // 4%라 모듈 추정 오차(±10%)에 묻힌다. 위 가짜 하나는
                // 정확히 90.0모듈로 나와 격자를 통과했다.
                // 크기 상한이 그보다 확실하고 뜻도 분명하다.
                const float legMods = d[1] / mod;
                if (legMods < 12.0f || legMods > 40.0f) continue;   // 버전 1~8

                // [버전 격자] QR 버전 V의 한 변은 17 + 4v 모듈이고
                // (v = 0..39, 즉 21,25,29,...,177), 파인더 중심 사이의
                // 변은 그보다 7 모듈 짧다. 따라서 legMods는 14, 18, 22, ...
                // 로 **4 간격 격자 위에만** 있을 수 있다.
                //
                // 허용 오차 0.3은 모듈 추정이 흐린 이미지에서 ±10% 흔들리는
                // 것을 감안한 값이다. 위 크기 상한(40모듈) 안에서는 격자
                // 간격 4모듈이 전체의 10% 이상이라 이 조건이 실제로 듣는다.
                const float steps = (legMods - 14.0f) / 4.0f;
                if (steps < -0.3f || steps > 6.8f) continue;
                if (std::fabs(steps - std::round(steps)) > 0.3f) continue;

                tris.push_back({i, j, k, d[1], mod});
            }
        }
    }
    std::sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.leg < b.leg; });

    std::vector<uint8_t> used(pts.size(), 0);
    for (const auto& t : tris) {
        if (static_cast<int>(out.size()) >= maxCandidates) break;
        if (used[t.i] || used[t.j] || used[t.k]) continue;
        {
            {
                const size_t i = t.i, j = t.j, k = t.k;
                const FinderPoint* P[3] = {&pts[i], &pts[j], &pts[k]};
                const float mod = t.mod;

                // [상자] 파인더 중심 셋의 외접 상자에 3.5모듈씩 덧대고,
                // 네 번째 모서리(대각)를 더한다 — 회전돼 있어도 이 네 점의
                // 외접 상자면 코드가 온전히 들어간다.
                float xs[4] = {P[0]->cx, P[1]->cx, P[2]->cx, 0};
                float ys[4] = {P[0]->cy, P[1]->cy, P[2]->cy, 0};
                // 빗변의 양 끝이 아닌 점이 직각(좌상단)이다.
                int corner = 0;
                {
                    const float dd[3] = {std::hypot(P[0]->cx - P[1]->cx, P[0]->cy - P[1]->cy),
                                         std::hypot(P[0]->cx - P[2]->cx, P[0]->cy - P[2]->cy),
                                         std::hypot(P[1]->cx - P[2]->cx, P[1]->cy - P[2]->cy)};
                    if (dd[0] >= dd[1] && dd[0] >= dd[2]) corner = 2;        // 0-1이 빗변 -> 2가 직각
                    else if (dd[1] >= dd[0] && dd[1] >= dd[2]) corner = 1;   // 0-2가 빗변 -> 1이 직각
                    else corner = 0;
                }
                const int a = (corner + 1) % 3, b = (corner + 2) % 3;
                xs[3] = P[a]->cx + P[b]->cx - P[corner]->cx;
                ys[3] = P[a]->cy + P[b]->cy - P[corner]->cy;

                const float pad = 3.5f * mod;
                float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
                for (int t = 1; t < 4; ++t) {
                    x0 = std::min(x0, xs[t]); x1 = std::max(x1, xs[t]);
                    y0 = std::min(y0, ys[t]); y1 = std::max(y1, ys[t]);
                }
                QrCandidate c;
                c.cornerX = P[corner]->cx; c.cornerY = P[corner]->cy;
                c.armAX = P[a]->cx;        c.armAY = P[a]->cy;
                c.armBX = P[b]->cx;        c.armBY = P[b]->cy;
                // 변 길이에서 버전을 역산해 유효 격자(17+4k)로 맞춘다.
                {
                    const int v = static_cast<int>(std::lround((t.leg / mod + 7.0f - 17.0f) / 4.0f));
                    c.versionModules = 17 + 4 * std::max(1, std::min(40, v));
                }
                c.bbox.x0 = std::max(0, static_cast<int>(x0 - pad));
                c.bbox.y0 = std::max(0, static_cast<int>(y0 - pad));
                c.bbox.x1 = std::min(image.width, static_cast<int>(x1 + pad) + 1);
                c.bbox.y1 = std::min(image.height, static_cast<int>(y1 + pad) + 1);
                c.modulePx = mod;
                if (c.bbox.x1 - c.bbox.x0 < 16 || c.bbox.y1 - c.bbox.y0 < 16) continue;

                out.push_back(c);
                used[i] = used[j] = used[k] = 1;
            }
        }
    }
    return out;
}

} // namespace vscan
