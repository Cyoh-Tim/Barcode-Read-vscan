#include "vscan_internal/decoder_dotcode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace vscan {
namespace {

// ---------------------------------------------------------------------------
// 표 — tools/extract_dotcode_tables.py 가 BWIPP dotcode 리소스에서 뽑고
// 검증한 것이다(개수 113, 전부 팝카운트 5, 중복 0).
// ---------------------------------------------------------------------------
constexpr uint16_t kEncs[113] = {
    0x155, 0x0ab, 0x0ad, 0x0b5, 0x0d5, 0x156, 0x15a, 0x16a,
    0x1aa, 0x0ae, 0x0b6, 0x0ba, 0x0d6, 0x0da, 0x0ea, 0x12b,
    0x12d, 0x135, 0x14b, 0x14d, 0x153, 0x159, 0x165, 0x169,
    0x195, 0x1a5, 0x1a9, 0x057, 0x05b, 0x05d, 0x06b, 0x06d,
    0x075, 0x097, 0x09b, 0x09d, 0x0a7, 0x0b3, 0x0b9, 0x0cb,
    0x0cd, 0x0d3, 0x0d9, 0x0e5, 0x0e9, 0x12e, 0x136, 0x13a,
    0x14e, 0x15c, 0x166, 0x16c, 0x172, 0x174, 0x196, 0x19a,
    0x1a6, 0x1ac, 0x1b2, 0x1b4, 0x1ca, 0x1d2, 0x1d4, 0x05e,
    0x06e, 0x076, 0x07a, 0x09e, 0x0bc, 0x0ce, 0x0dc, 0x0e6,
    0x0ec, 0x0f2, 0x0f4, 0x117, 0x11b, 0x11d, 0x127, 0x133,
    0x139, 0x147, 0x163, 0x171, 0x18b, 0x18d, 0x193, 0x199,
    0x1a3, 0x1b1, 0x1c5, 0x1c9, 0x1d1, 0x02f, 0x037, 0x03b,
    0x03d, 0x04f, 0x067, 0x073, 0x079, 0x08f, 0x0c7, 0x0e3,
    0x0f1, 0x11e, 0x13c, 0x178, 0x18e, 0x19c, 0x1b8, 0x1c6,
    0x1cc,
};

constexpr int kMod = 113;
constexpr int kMaskOffsets[4] = {0, 3, 7, 17};

// 9비트 패턴 -> 코드워드. 512칸 직접 색인이 가장 싸다(0.5KB).
struct RevTable {
    int8_t v[512];
    RevTable() {
        for (int i = 0; i < 512; ++i) v[i] = -1;
        for (int i = 0; i < 113; ++i) v[kEncs[i]] = static_cast<int8_t>(i);
    }
};
const RevTable kRev;

// ---------------------------------------------------------------------------
// GF(113) — 소수체다. 2의 거듭제곱 체가 아니므로 **더하기와 빼기가 다르다**.
// MicroPDF417의 GF(929)에서 이미 한 번 여기서 틀렸다(§3.49): 베를레캄프-매시의
// 갱신은 더하기가 아니라 빼기여야 한다.
// ---------------------------------------------------------------------------
struct GF {
    int exp[224];   // 3^k, k=0..223 (주기 112)
    int logt[113];
    GF() {
        int a = 1;
        for (int i = 0; i < 224; ++i) {
            exp[i] = a;
            if (i < 112) logt[a] = i;
            a = a * 3 % kMod;
        }
        logt[0] = -1;
    }
    static int add(int a, int b) { return (a + b) % kMod; }
    static int sub(int a, int b) { return (a - b + kMod) % kMod; }
    static int mul(int a, int b) { return a * b % kMod; }
    int inv(int a) const { return a ? exp[(112 - logt[a]) % 112] : 0; }
    int pow3(int e) const { return exp[((e % 112) + 112) % 112]; }
};
const GF kGF;

using Poly = std::vector<int>;

void polySubShift(Poly& a, const Poly& b, int c, int m) {
    if (a.size() < b.size() + m) a.resize(b.size() + m, 0);
    for (size_t i = 0; i < b.size(); ++i)
        a[i + m] = GF::sub(a[i + m], GF::mul(c, b[i]));
}

Poly syndromes(const std::vector<int>& r, int nc) {
    Poly s(nc, 0);
    const int n = static_cast<int>(r.size());
    for (int i = 1; i <= nc; ++i) {
        const int a = kGF.pow3(i);
        int acc = 0, x = 1;
        // r[n-1] 이 최저차. 뒤에서부터 호너로 접는 편이 pow 호출이 없다.
        for (int k = n - 1; k >= 0; --k) {
            acc = GF::add(acc, GF::mul(r[k], x));
            x = GF::mul(x, a);
        }
        s[i - 1] = acc;
    }
    return s;
}

/*
 * RS 복호. r[0]이 최고차. 고쳤으면 true.
 *
 * 실측(코드워드 26개, nc=10, 각 오류 개수마다 200회):
 *   오류 1~5개 -> 200/200 복구, 6개 이상 -> 200/200 거부, **오정정 0**.
 * 이론 한계(nc/2)와 정확히 일치하고, 한계를 넘으면 만들어내지 않고 거절한다.
 */
bool rsCorrect(std::vector<int>& r, int nc) {
    Poly s = syndromes(r, nc);
    if (std::all_of(s.begin(), s.end(), [](int v) { return v == 0; })) return true;

    Poly lam{1}, B{1};
    int L = 0, m = 1, b = 1;
    for (int n = 0; n < nc; ++n) {
        int d = s[n];
        for (int i = 1; i <= L && i < static_cast<int>(lam.size()); ++i)
            d = GF::add(d, GF::mul(lam[i], s[n - i]));
        if (d == 0) { ++m; continue; }
        const Poly T = lam;
        polySubShift(lam, B, GF::mul(d, kGF.inv(b)), m);
        if (2 * L <= n) { L = n + 1 - L; B = T; b = d; m = 1; }
        else ++m;
    }
    if (L <= 0 || L > nc / 2) return false;

    const int n = static_cast<int>(r.size());
    std::vector<int> pos;
    for (int k = 0; k < n; ++k) {
        const int xi = kGF.inv(kGF.pow3(n - 1 - k));
        int v = 0, p = 1;
        for (size_t i = 0; i < lam.size(); ++i) { v = GF::add(v, GF::mul(lam[i], p)); p = GF::mul(p, xi); }
        if (v == 0) pos.push_back(k);
    }
    if (static_cast<int>(pos.size()) != L) return false;

    Poly omega(L, 0);
    for (int i = 0; i < L; ++i) {
        int acc = 0;
        for (int j = 0; j <= i && j < static_cast<int>(lam.size()); ++j)
            acc = GF::add(acc, GF::mul(lam[j], s[i - j]));
        omega[i] = acc;
    }
    for (int k : pos) {
        const int xi = kGF.inv(kGF.pow3(n - 1 - k));
        int num = 0, p = 1;
        for (int i = 0; i < L; ++i) { num = GF::add(num, GF::mul(omega[i], p)); p = GF::mul(p, xi); }
        // Λ'(x) = Σ i·λ_i·x^(i-1). GF(2^m)의 "홀수항만" 요령은 여기서 안 통한다.
        int den = 0; p = 1;
        for (size_t i = 1; i < lam.size(); ++i) {
            den = GF::add(den, GF::mul(static_cast<int>(i) % kMod, GF::mul(lam[i], p)));
            p = GF::mul(p, xi);
        }
        if (den == 0) return false;
        r[k] = GF::add(r[k], GF::mul(num, kGF.inv(den)));
    }
    s = syndromes(r, nc);
    return std::all_of(s.begin(), s.end(), [](int v) { return v == 0; });
}

// ---------------------------------------------------------------------------
// 심볼 계층
// ---------------------------------------------------------------------------
struct Geom { int ndots, nd, nc, nw, rembits; };

bool geometry(int rows, int cols, Geom& g) {
    g.ndots = rows * cols / 2;
    int nd = 0;
    while (((nd + 1) + (nd + 1) / 2 + 3) * 9 + 2 <= g.ndots) ++nd;
    if (nd <= 0) return false;
    g.nd = nd;
    g.nc = nd / 2 + 3;
    g.nw = g.nd + g.nc;
    g.rembits = g.ndots - (g.nw * 9 + 2);
    return g.rembits >= 0;
}

// 여섯 모서리 자리. rows의 홀짝에 따라 배치가 다르다.
void sixEdges(int rows, int cols, std::array<std::pair<int, int>, 6>& e) {
    if (rows % 2 == 0) {
        e = {{{cols - 1, rows - 2}, {0, rows - 2}, {cols - 2, rows - 1},
              {1, rows - 1}, {cols - 1, 0}, {0, 0}}};
    } else {
        e = {{{cols - 2, 0}, {cols - 2, rows - 1}, {cols - 1, 1},
              {cols - 1, rows - 2}, {0, 0}, {0, rows - 1}}};
    }
}

// 점 격자 -> 코드워드. 실패하면 false.
bool decodeMatrix(const std::vector<uint8_t>& grid, int rows, int cols, std::vector<int>& cws) {
    if ((rows + cols) % 2 != 1 || rows < 5 || cols < 5) return false;
    Geom g;
    if (!geometry(rows, cols, g)) return false;

    std::array<std::pair<int, int>, 6> six;
    sixEdges(rows, cols, six);
    std::vector<uint8_t> freeCell(static_cast<size_t>(rows) * cols, 0);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            freeCell[static_cast<size_t>(y) * cols + x] = ((x + y) % 2 == 0) ? 1 : 0;
    for (auto& p : six) freeCell[static_cast<size_t>(p.second) * cols + p.first] = 0;

    std::vector<uint8_t> bits;
    bits.reserve(g.ndots);
    int px = 0, py = (rows % 2 == 0) ? 0 : rows - 1;
    for (int i = 0; i < g.ndots - 6; ++i) {
        int guard = 0;
        while (!freeCell[static_cast<size_t>(py) * cols + px]) {
            if (rows % 2 == 0) {
                if (++py == rows) { py = 0; ++px; }
            } else {
                if (++px == cols) { px = 0; --py; }
            }
            if (px < 0 || px >= cols || py < 0 || py >= rows) return false;
            if (++guard > rows * cols) return false;
        }
        bits.push_back(grid[static_cast<size_t>(py) * cols + px]);
        freeCell[static_cast<size_t>(py) * cols + px] = 0;
    }
    for (auto& p : six) bits.push_back(grid[static_cast<size_t>(p.second) * cols + p.first]);

    std::vector<int> rscws(g.nw + 1, 0);
    rscws[0] = bits[0] * 2 + bits[1];
    int invalid = 0;
    for (int i = 1; i <= g.nw; ++i) {
        int pat = 0;
        for (int k = 0; k < 9; ++k) pat = (pat << 1) | bits[(i - 1) * 9 + 2 + k];
        const int v = kRev.v[pat];
        // 표에 없는 패턴(팝카운트가 5가 아닌 것)은 손상이다. 0으로 두고
        // RS에게 맡긴다 — 소거로 다루면 정정력이 두 배가 되지만, 그건
        // 위치를 아는 대신 오정정 위험이 커지는 거래라 지금은 안 한다.
        if (v < 0) ++invalid;
        rscws[i] = (v >= 0) ? v : 0;
    }
    /*
     * [전부 0인 부호어는 RS를 그냥 통과한다 — 여기서 막아야 한다]
     *
     * 표에 없는 패턴을 0으로 바꾸는 순간, 빈 격자는 **전부 0인 벡터**가 된다.
     * 그런데 0 벡터는 어떤 RS 부호에서도 신드롬이 전부 0이다. 즉 검사에
     * 걸리지 않고, 모드 C에서 코드워드 0이 "00"이므로 "0000000..."이
     * 튀어나온다. 실측으로 20장 중 3장이 이 경로로 오디코딩됐다
     * (블러 2, 회전 30도, 그리고 깨끗한 것 하나).
     *
     * 못 읽은 패턴이 RS 정정 한계(nc/2)를 넘으면 어차피 못 고친다. 그때는
     * 고치는 시늉을 하지 말고 바로 거절한다 — 미검출이 오디코딩보다 낫다(§3.18).
     */
    if (invalid * 2 > g.nc) return false;

    const int step = g.nw / 112 + 1;
    if (step == 1) {
        if (!rsCorrect(rscws, g.nc)) return false;
    } else {
        for (int start = 0; start < step; ++start) {
            std::vector<int> sub;
            for (int i = start; i <= g.nw; i += step) sub.push_back(rscws[i]);
            const int ND = (g.nd + 1 - start + step - 1) / step;
            const int NC = static_cast<int>(sub.size()) - ND;
            if (NC <= 0 || !rsCorrect(sub, NC)) return false;
            int j = 0;
            for (int i = start; i <= g.nw; i += step) rscws[i] = sub[j++];
        }
    }

    // 마스크 비트도 RS가 지키는 자리다. 읽은 값이 아니라 **정정된 값**을 쓴다.
    const int mask = rscws[0];
    if (mask < 0 || mask > 3) return false;
    const int off = kMaskOffsets[mask];
    cws.resize(g.nd);
    for (int k = 0; k < g.nd; ++k)
        cws[k] = ((rscws[k + 1] - k * off) % kMod + kMod) % kMod;
    return true;
}

// ---------------------------------------------------------------------------
// 데이터 계층 (Code 128 계보: A/B/C/BIN 모드)
// ---------------------------------------------------------------------------
enum Mode { MA = 0, MB = 1, MC = 2, MBIN = 3 };

char achar(int v) { return static_cast<char>(v < 64 ? 32 + v : v - 64); }
char bchar(int v) { return static_cast<char>(32 + v); }

bool flushBin(std::vector<int>& buf, std::string& out) {
    const int L = static_cast<int>(buf.size());
    if (L == 0) return true;
    const int full = L / 6, rem = L % 6;
    if (rem == 1) return false;
    auto emit = [&out](unsigned long long v, int nbytes) {
        for (int k = nbytes - 1; k >= 0; --k) {
            unsigned long long d = v;
            for (int t = 0; t < k; ++t) d /= 259ULL;
            out.push_back(static_cast<char>(d % 259ULL));
        }
    };
    for (int gidx = 0; gidx < full; ++gidx) {
        unsigned long long v = 0;
        for (int j = 0; j < 6; ++j) v = v * 103ULL + static_cast<unsigned>(buf[gidx * 6 + j]);
        emit(v, 5);
    }
    if (rem) {
        unsigned long long v = 0;
        for (int j = full * 6; j < L; ++j) v = v * 103ULL + static_cast<unsigned>(buf[j]);
        emit(v, rem - 1);
    }
    buf.clear();
    return true;
}

bool decodeCws(const std::vector<int>& cws, std::string& out, bool& gs1) {
    out.clear();
    gs1 = false;
    int mode = MC, shiftMode = -1, shiftLeft = 0;
    std::vector<int> binbuf;
    const int n = static_cast<int>(cws.size());

    for (int i = 0; i < n;) {
        const int v = cws[i++];
        if (v < 0 || v > 112) return false;

        if (mode == MBIN) {
            if (v <= 102) { binbuf.push_back(v); continue; }
            if (!flushBin(binbuf, out)) return false;
            if (v >= 103 && v <= 108) { shiftMode = MC; shiftLeft = v - 101; mode = MC; continue; }
            if (v == 109) { mode = MA; continue; }
            if (v == 110) { mode = MB; continue; }
            if (v == 111) { mode = MC; continue; }
            return false;                       // tms(112): 구조적 추가 — 미지원
        }

        int cur = mode;
        if (shiftLeft > 0) { cur = shiftMode; if (--shiftLeft == 0) shiftMode = -1; }

        bool handled = true;
        if (cur == MC) {
            if (v <= 99) { char b[4]; std::snprintf(b, sizeof(b), "%02d", v); out += b; }
            else if (v == 100) {                // 17+YYMMDD+10 축약
                if (i + 3 > n) return false;
                const int a0 = cws[i], a1 = cws[i + 1], a2 = cws[i + 2];
                i += 3;
                if (a0 > 99 || a1 > 99 || a2 > 99) return false;
                char b[16];
                std::snprintf(b, sizeof(b), "17%02d%02d%02d10", a0, a1, a2);
                out += b;
            }
            else if (v == 101) mode = MA;
            else if (v == 102) { shiftMode = MB; shiftLeft = 1; }
            else if (v >= 103 && v <= 105) { shiftMode = MB; shiftLeft = v - 101; }
            else if (v == 106) mode = MB;
            else handled = false;
        } else if (cur == MA) {
            if (v <= 95) out.push_back(achar(v));
            else if (v == 96) { shiftMode = MB; shiftLeft = 1; }
            else if (v >= 97 && v <= 101) { shiftMode = MB; shiftLeft = v - 95; }
            else if (v == 102) mode = MB;
            else if (v >= 103 && v <= 105) { shiftMode = MC; shiftLeft = v - 101; }
            else if (v == 106) mode = MC;
            else handled = false;
        } else {                                 // MB
            if (v <= 95) out.push_back(bchar(v));
            else if (v == 96) out += "\r\n";
            else if (v >= 97 && v <= 100) return false;   // 매크로 — 뜻 미확정, 거부
            else if (v == 101) { shiftMode = MA; shiftLeft = 1; }
            else if (v == 102) mode = MA;
            else if (v >= 103 && v <= 105) { shiftMode = MC; shiftLeft = v - 101; }
            else if (v == 106) mode = MC;
            else handled = false;
        }
        if (handled) continue;

        // 모드 공통 제어 (107..112)
        if (v == 107) {                          // FNC1
            if (out.empty() && i == 1) gs1 = true;
            else out.push_back('\x1d');
        } else if (v == 108 || v == 109) {
            return false;                        // ECI / 구조적 추가 — 미지원
        } else if (v == 110 || v == 111) {
            if (i >= n) return false;
            const int w = cws[i++];
            if (w > 95) return false;
            out.push_back(static_cast<char>(128 + (v == 110 ? achar(w) : bchar(w))));
        } else if (v == 112) {
            mode = MBIN;
        } else {
            return false;
        }
    }
    if (mode == MBIN && !flushBin(binbuf, out)) return false;
    return !out.empty();
}

// ---------------------------------------------------------------------------
// 검출 — 점 뭉치 -> 격자 -> rows/cols 가설 -> 복호
// ---------------------------------------------------------------------------
struct Blob { double x, y; int area; };

/*
 * Otsu 임계. **반환값은 "이하가 어두움"이다** — 비교를 `<`로 하면 안 된다.
 *
 * 합성 이진 이미지(0과 255만 있는 것)에서는 0~254 어느 값으로 잘라도 같은
 * 분할이라 분산이 전부 같고, 첫 최대값인 **0**이 나온다. 그걸 `< 0`으로
 * 비교하면 어두운 화소가 하나도 없다 — 실측으로 깨끗한 DotCode에서 뭉치
 * 0개가 나왔다.
 */
int otsu(const GrayView& img) {
    long long hist[256] = {0};
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels + static_cast<size_t>(y) * img.stride;
        for (int x = 0; x < img.width; ++x) ++hist[row[x]];
    }
    const long long total = static_cast<long long>(img.width) * img.height;
    long long sum = 0;
    for (int i = 0; i < 256; ++i) sum += static_cast<long long>(i) * hist[i];
    long long sumB = 0, wB = 0;
    double best = -1;
    int th = 128;
    for (int i = 0; i < 256; ++i) {
        wB += hist[i];
        if (!wB) continue;
        const long long wF = total - wB;
        if (!wF) break;
        sumB += static_cast<long long>(i) * hist[i];
        const double mB = static_cast<double>(sumB) / wB;
        const double mF = static_cast<double>(sum - sumB) / wF;
        const double v = static_cast<double>(wB) * wF * (mB - mF) * (mB - mF);
        if (v > best) { best = v; th = i; }
    }
    return th;
}

/*
 * 어두운 화소의 연결 성분 중심. **4-연결이다.**
 *
 * 체커보드 배치라 대각 이웃이 곧 옆 칸이고, 점이 조금만 커도 모서리끼리
 * 닿는다. 8-연결로 세면 심볼 하나가 통째로 한 뭉치가 된다(실측: 139점이
 * 59뭉치로 뭉쳤다).
 *
 * 화소마다 라벨 배열을 잡으면 12MP 프레임에서 48MB다. 그래서 **런 단위**로
 * 유니온-파인드를 돌린다 — 메모리가 런 개수에 비례하고 캐시도 훨씬 낫다.
 */
void findBlobs(const GrayView& img, int th, std::vector<Blob>& out) {
    struct Run { int y, x0, x1, parent; };
    std::vector<Run> runs;
    std::vector<int> prevIdx, curIdx;
    runs.reserve(4096);

    auto find = [&runs](int a) {
        while (runs[a].parent != a) { runs[a].parent = runs[runs[a].parent].parent; a = runs[a].parent; }
        return a;
    };

    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels + static_cast<size_t>(y) * img.stride;
        curIdx.clear();
        int x = 0;
        while (x < img.width) {
            if (row[x] > th) { ++x; continue; }
            const int x0 = x;
            while (x < img.width && row[x] <= th) ++x;
            const int idx = static_cast<int>(runs.size());
            runs.push_back({y, x0, x, idx});
            curIdx.push_back(idx);
            for (int p : prevIdx) {
                if (runs[p].x0 < x && x0 < runs[p].x1) {          // 가로로 겹친다 = 4-연결
                    const int a = find(p), b = find(idx);
                    if (a != b) runs[a].parent = b;
                }
            }
        }
        prevIdx.swap(curIdx);
    }

    if (runs.empty()) return;
    // 뿌리별로 화소 합을 모은다.
    std::vector<double> sx, sy;
    std::vector<int> cnt, map(runs.size(), -1);
    for (size_t i = 0; i < runs.size(); ++i) {
        const int r = find(static_cast<int>(i));
        if (map[r] < 0) { map[r] = static_cast<int>(sx.size()); sx.push_back(0); sy.push_back(0); cnt.push_back(0); }
        const int k = map[r];
        const int w = runs[i].x1 - runs[i].x0;
        sx[k] += (runs[i].x0 + runs[i].x1 - 1) * 0.5 * w;
        sy[k] += static_cast<double>(runs[i].y) * w;
        cnt[k] += w;
    }
    out.reserve(sx.size());
    for (size_t k = 0; k < sx.size(); ++k)
        out.push_back({sx[k] / cnt[k], sy[k] / cnt[k], cnt[k]});
}

/*
 * 점을 균일 격자에 담아 "가까운 것만" 훑게 해주는 통. 셀 크기를 찾으려는
 * 반경 이상으로 잡으면 3x3 이웃만 보면 된다.
 */
struct Bucket {
    double cell = 1.0, x0 = 0, y0 = 0;
    int nx = 1, ny = 1;
    std::vector<int> head, next;
    const std::vector<double>* px = nullptr;
    const std::vector<double>* py = nullptr;

    void build(const std::vector<double>& X, const std::vector<double>& Y, double c) {
        px = &X; py = &Y;
        cell = c > 1e-6 ? c : 1.0;
        const int n = static_cast<int>(X.size());
        x0 = *std::min_element(X.begin(), X.end());
        y0 = *std::min_element(Y.begin(), Y.end());
        const double x1 = *std::max_element(X.begin(), X.end());
        const double y1 = *std::max_element(Y.begin(), Y.end());
        /*
         * 칸이 너무 많아지면 **셀을 키운다**. 통 하나로 퇴화시키면 안 된다 —
         * 처음에 그렇게 짰다가 큰 프레임에서 전수 비교로 되돌아가서 코퍼스
         * 평균이 196 -> 485ms로 오히려 나빠졌다. 셀을 키우는 쪽은 후보가
         * 늘 뿐 3x3이 반경을 덮는다는 성질이 그대로 유지된다.
         */
        constexpr long long kMaxCells = 1 << 20;
        for (int guard = 0; guard < 40; ++guard) {
            nx = std::max(1, static_cast<int>((x1 - x0) / cell) + 1);
            ny = std::max(1, static_cast<int>((y1 - y0) / cell) + 1);
            if (static_cast<long long>(nx) * ny <= kMaxCells) break;
            cell *= 2.0;
        }
        head.assign(static_cast<size_t>(nx) * ny, -1);
        next.assign(n, -1);
        for (int i = 0; i < n; ++i) {
            const int cx = std::min(nx - 1, std::max(0, static_cast<int>((X[i] - x0) / cell)));
            const int cy = std::min(ny - 1, std::max(0, static_cast<int>((Y[i] - y0) / cell)));
            const size_t k = static_cast<size_t>(cy) * nx + cx;
            next[i] = head[k];
            head[k] = i;
        }
    }

    template <class F>
    void forEachNear(double X, double Y, F f) const {
        const int cx = std::min(nx - 1, std::max(0, static_cast<int>((X - x0) / cell)));
        const int cy = std::min(ny - 1, std::max(0, static_cast<int>((Y - y0) / cell)));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = cx + dx, gy = cy + dy;
                if (gx < 0 || gx >= nx || gy < 0 || gy >= ny) continue;
                for (int i = head[static_cast<size_t>(gy) * nx + gx]; i >= 0; i = next[i]) f(i);
            }
    }
};

// 3x3 정규방정식으로 P ~ O + I*u + J*w 를 푼다. 실패하면 false.
bool fitAffine(const std::vector<double>& I, const std::vector<double>& J,
               const std::vector<double>& PX, const std::vector<double>& PY,
               const std::vector<uint8_t>& use, double out[3][2]) {
    double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    double bx[3] = {0, 0, 0}, by[3] = {0, 0, 0};
    int cnt = 0;
    for (size_t k = 0; k < I.size(); ++k) {
        if (!use[k]) continue;
        ++cnt;
        const double v[3] = {1.0, I[k], J[k]};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) A[a][b] += v[a] * v[b];
            bx[a] += v[a] * PX[k];
            by[a] += v[a] * PY[k];
        }
    }
    if (cnt < 6) return false;
    // 가우스 소거 (부분 피벗)
    double M[3][5];
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) M[a][b] = A[a][b];
        M[a][3] = bx[a];
        M[a][4] = by[a];
    }
    for (int c = 0; c < 3; ++c) {
        int piv = c;
        for (int r = c + 1; r < 3; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
        if (std::fabs(M[piv][c]) < 1e-9) return false;
        if (piv != c) for (int b = 0; b < 5; ++b) std::swap(M[c][b], M[piv][b]);
        for (int r = 0; r < 3; ++r) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int b = c; b < 5; ++b) M[r][b] -= f * M[c][b];
        }
    }
    for (int a = 0; a < 3; ++a) { out[a][0] = M[a][3] / M[a][a]; out[a][1] = M[a][4] / M[a][a]; }
    return true;
}

// 격자를 8가지 이면군 변형으로 돌린다.
void dihedral(const std::vector<uint8_t>& m, int rows, int cols, int k,
              std::vector<uint8_t>& out, int& orows, int& ocols) {
    const bool flip = (k & 1) != 0;
    const int rot = k >> 1;
    int r = rows, c = cols;
    for (int t = 0; t < rot; ++t) std::swap(r, c);
    orows = r; ocols = c;
    out.assign(static_cast<size_t>(r) * c, 0);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            int nx = x, ny = y, w = cols, h = rows;
            for (int t = 0; t < rot; ++t) { const int tx = nx; nx = ny; ny = w - 1 - tx; std::swap(w, h); }
            if (flip) nx = w - 1 - nx;
            out[static_cast<size_t>(ny) * c + nx] = m[static_cast<size_t>(y) * cols + x];
        }
    }
}


/*
 * 임계 하나로 검출을 한 번 시도한다. 성공하면 res에 넣고 true.
 * 임계를 바깥에서 사다리로 넣는 이유는 decode()의 주석 참고.
 */
bool detectAt(const GrayView& image, int th, bool dbg, std::vector<DecodedSymbol>& res) {
#define DCDBG(...) do { if (dbg) std::fprintf(stderr, "[dotcode] " __VA_ARGS__); } while (0)
    DCDBG("임계 %d (%dx%d)\n", th, image.width, image.height);

    std::vector<Blob> bl;
    findBlobs(image, th, bl);
    if (bl.size() < 24 || bl.size() > 60000) { DCDBG("뭉치 %zu개 — 범위 밖\n", bl.size()); return false; }

    // 넓이가 튀는 것을 먼저 턴다(글자, 선, 얼룩).
    std::vector<int> areas;
    areas.reserve(bl.size());
    for (auto& b : bl) areas.push_back(b.area);
    std::nth_element(areas.begin(), areas.begin() + areas.size() / 2, areas.end());
    const double med = areas[areas.size() / 2];
    /*
     * DotCode의 점은 크기가 거의 똑같으므로, 중앙값에서 크게 벗어난 것은
     * 글자·선·얼룩이다.
     *
     * 범위를 [0.45, 2.2]배로 좁혀서 비용이 줄어드는지 재봤다 — **안 줄었다**
     * (난수 코퍼스 132.4 -> 129.6ms, 실행 간 편차 범위). 비용은 여기가
     * 아니라 findBlobs의 전체 훑기에 있다. 그래서 넉넉한 쪽을 유지한다:
     * 좁히면 흐린 프레임에서 살아남는 점이 줄어 검출만 위태로워진다.
     */
    std::vector<Blob> pts;
    for (auto& b : bl)
        if (b.area >= 0.25 * med && b.area <= 4.0 * med) pts.push_back(b);
    if (pts.size() < 24) { DCDBG("넓이 필터 후 %zu개\n", pts.size()); return false; }

    const int n = static_cast<int>(pts.size());
    std::vector<double> PX(n), PY(n);
    for (int i = 0; i < n; ++i) { PX[i] = pts[i].x; PY[i] = pts[i].y; }

    /*
     * [최근접 이웃을 전수 비교하면 프레임 값이 두 배가 된다]
     *
     * 처음에는 O(n^2)로 짰다. DotCode 시험셋(점 100여 개)에서는 안 보이는데,
     * 난수 코퍼스(글자·질감이 있는 큰 프레임)에서는 뭉치가 수천 개라
     * 프레임 평균이 102 -> 196ms로 뛰었다. 균일 격자에 담아 3x3 이웃만
     * 보면 사실상 O(n)이다.
     */
    Bucket bk;
    bk.build(PX, PY, std::max(4.0, std::sqrt(static_cast<double>(image.width) * image.height / n)));
    std::vector<double> nns(n);
    for (int i = 0; i < n; ++i) {
        double best = 1e18;
        bk.forEachNear(PX[i], PY[i], [&](int j) {
            if (j == i) return;
            const double dx = PX[i] - PX[j], dy = PY[i] - PY[j];
            const double dd = dx * dx + dy * dy;
            if (dd < best) best = dd;
        });
        nns[i] = std::sqrt(best);
    }
    std::nth_element(nns.begin(), nns.begin() + n / 2, nns.end());
    const double d = nns[n / 2];
    if (d < 2.0 || d > 1e6) { DCDBG("피치 %.2f 범위 밖\n", d); return false; }

    /*
     * [각도 평균이 아니라 이웃 벡터를 군집한다]
     *
     * 4중 대칭 원형평균(atan2(Σsin4θ, Σcos4θ)/4)을 먼저 썼는데, 회전 15도에서
     * 29.6도가 나왔다(참값 -30.4도). 격자의 절반만 켜져 있어서 방향별 표본이
     * 균등하지 않고, 원형평균은 그 불균형에 그대로 끌려간다.
     *
     * 반경은 1.25d로 잡는다 — 2셀 간격 이웃이 sqrt2·d 이므로 그 사이다.
     * 1.45d로 넓혔더니 그것들이 섞여 들어와 격자가 두 배로 잡혔다(실측).
     */
    constexpr int kBins = 36;
    int hist[kBins] = {0};
    std::vector<double> VX, VY, VA;
    const double lim2 = (d * 1.25) * (d * 1.25);
    bk.build(PX, PY, d * 1.25);
    for (int i = 0; i < n; ++i)
        bk.forEachNear(PX[i], PY[i], [&](int j) {
            if (j == i) return;
            double vx = PX[j] - PX[i], vy = PY[j] - PY[i];
            if (vx * vx + vy * vy > lim2) return;
            if (vx < 0 || (vx == 0 && vy < 0)) { vx = -vx; vy = -vy; }
            const double a = std::atan2(vy, vx);            // 반평면이라 [-pi/2, pi/2)
            int b = static_cast<int>((a + M_PI / 2) / M_PI * kBins);
            if (b < 0) b = 0;
            if (b >= kBins) b = kBins - 1;
            ++hist[b];
            VX.push_back(vx); VY.push_back(vy); VA.push_back(a);
        });
    if (VX.size() < 16) { DCDBG("이웃 벡터 %zu개\n", VX.size()); return false; }
    int bestBin = 0;
    for (int i = 1; i < kBins; ++i) if (hist[i] > hist[bestBin]) bestBin = i;
    const double a1 = (bestBin + 0.5) / kBins * M_PI - M_PI / 2;

    auto medianVec = [&](double centre, double& ox, double& oy) {
        std::vector<double> xs, ys;
        for (size_t k = 0; k < VA.size(); ++k) {
            double dd = std::fmod(VA[k] - centre + M_PI * 1.5, M_PI) - M_PI / 2;
            if (std::fabs(dd) < 18.0 * M_PI / 180.0) { xs.push_back(VX[k]); ys.push_back(VY[k]); }
        }
        if (xs.size() < 4) return false;
        std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        ox = xs[xs.size() / 2]; oy = ys[ys.size() / 2];
        return true;
    };
    double ux, uy, wx, wy;
    if (!medianVec(a1, ux, uy)) { DCDBG("기저 u 실패\n"); return false; }
    if (!medianVec(a1 + M_PI / 2, wx, wy)) {
        /*
         * 직교 방향 표본이 모자라면 u를 90도 돌려 쓴다. DotCode의 점 격자는
         * **정확히 정사각**이라(간격 같고 직교) 근거가 추측이 아니다. 흐린
         * 이미지에서 점들이 한쪽으로 먼저 붙어 그 방향 표본만 남는 일이
         * 실제로 있었다(블러 2에서 여기서 끊겼다). 작은 오차는 아래
         * 최소제곱 재적합이 잡는다.
         */
        DCDBG("직교 표본 부족 — u를 90도 돌려 쓴다\n");
        wx = -uy; wy = ux;
    }
    for (double L : {std::hypot(ux, uy), std::hypot(wx, wy)})
        if (L < 0.78 * d || L > 1.28 * d) {
            DCDBG("기저 길이 %.2f (피치 %.2f) 범위 밖\n", L, d); return false;
        }

    double det = ux * wy - uy * wx;
    if (std::fabs(det) < 1e-6) return false;
    std::vector<double> I(n), J(n);
    for (int i = 0; i < n; ++i) {
        I[i] = std::round((PX[i] * wy - PY[i] * wx) / det);
        J[i] = std::round((PY[i] * ux - PX[i] * uy) / det);
    }

    /*
     * [붙어버린 뭉치는 반 칸에 앉는다 — 이상점으로 빼고 다시 맞춘다]
     *
     * 회전 리샘플링을 거치면 이웃 점 몇 쌍이 닿아 한 뭉치가 된다(실측:
     * 45도에서 111점이 76뭉치). 그 중심은 격자점 둘의 한가운데라 잔차가
     * 정확히 0.5이고, 전체 적합을 끌어당겨 회전 15/30/45도가 전부 죽었다.
     * 넓이로 거르면 임계가 애매하니(중앙값 자체가 오염된다) 잔차로 거른다.
     */
    std::vector<uint8_t> use(n, 1);
    double sol[3][2];
    for (int iter = 0; iter < 6; ++iter) {
        if (!fitAffine(I, J, PX, PY, use, sol)) return false;
        det = sol[1][0] * sol[2][1] - sol[1][1] * sol[2][0];
        if (std::fabs(det) < 1e-6) return false;
        std::vector<uint8_t> ni(n, 0);
        bool same = true;
        for (int i = 0; i < n; ++i) {
            const double rx = PX[i] - sol[0][0], ry = PY[i] - sol[0][1];
            const double qi = (rx * sol[2][1] - ry * sol[2][0]) / det;
            const double qj = (ry * sol[1][0] - rx * sol[1][1]) / det;
            I[i] = std::round(qi);
            J[i] = std::round(qj);
            ni[i] = (std::fabs(qi - I[i]) < 0.28 && std::fabs(qj - J[i]) < 0.28) ? 1 : 0;
            if (ni[i] != use[i]) same = false;
        }
        use.swap(ni);
        if (same) break;
    }
    const int inl = std::accumulate(use.begin(), use.end(), 0);
    DCDBG("점 %d개, 피치 %.2f, 안쪽 %d개\n", n, d, inl);
    if (inl * 2 < n || inl < 20) { DCDBG("안쪽 점이 모자라다\n"); return false; }

    // 안쪽 점만 남기고 셀 좌표로. x = I+J, y = I-J (점은 (x+y) 짝수 칸에 있다)
    std::vector<double> CX, CY, QX, QY;
    for (int i = 0; i < n; ++i) {
        if (!use[i]) continue;
        CX.push_back(I[i] + J[i]);
        CY.push_back(I[i] - J[i]);
        QX.push_back(PX[i]);
        QY.push_back(PY[i]);
    }
    const int m = static_cast<int>(CX.size());
    double xmin = CX[0], xmax = CX[0], ymin = CY[0], ymax = CY[0];
    for (int i = 1; i < m; ++i) {
        xmin = std::min(xmin, CX[i]); xmax = std::max(xmax, CX[i]);
        ymin = std::min(ymin, CY[i]); ymax = std::max(ymax, CY[i]);
    }
    const int spanX = static_cast<int>(xmax - xmin) + 1;
    const int spanY = static_cast<int>(ymax - ymin) + 1;
    DCDBG("격자 범위 %d x %d\n", spanY, spanX);
    if (spanX < 5 || spanY < 5 || spanX > 200 || spanY > 200) return false;

    std::vector<double> SX(m), SY(m);
    for (int i = 0; i < m; ++i) { SX[i] = CX[i] - xmin; SY[i] = CY[i] - ymin; }
    const std::vector<uint8_t> allUse(m, 1);

    /*
     * [패리티는 원점 이동이 아니라 반사로 맞춘다]
     *
     * 점은 (x+y)가 짝수인 칸에만 있는데, x의 최소값과 y의 최소값은 **서로
     * 다른 점**에서 나오므로 그 합이 홀수인 것이 정상이다. 한쪽 축만 1 밀어
     * 맞추면 앞에 빈 줄이 하나 생겨 참 크기가 가설 목록에서 아예 빠진다 —
     * 실측으로 참값 16x25가 가설(17x26, 18x25)에 없어서 전부 놓쳤다.
     * 그래서 모든 칸을 다 읽고, 필요하면 뒤집는다(rows+cols가 홀수라
     * 둘 중 정확히 하나가 짝수이고, 그 축으로 뒤집으면 패리티가 바뀐다).
     * 덤으로, 점이 없어야 할 패리티 칸이 검게 나오면 격자가 틀린 것이라
     * 바로 거른다.
     */
    for (int ey = 0; ey <= 1; ++ey) {
        for (int ex = 0; ex <= 1; ++ex) {
            const int cols = spanX + ex, rows = spanY + ey;
            if ((rows + cols) % 2 != 1 || rows < 5 || cols < 5) continue;
            double s2[3][2];
            if (!fitAffine(SX, SY, QX, QY, allUse, s2)) continue;
            double worst = 0;
            for (int i = 0; i < m; ++i) {
                const double ex2 = s2[0][0] + s2[1][0] * SX[i] + s2[2][0] * SY[i] - QX[i];
                const double ey2 = s2[0][1] + s2[1][1] * SX[i] + s2[2][1] * SY[i] - QY[i];
                worst = std::max(worst, std::max(std::fabs(ex2), std::fabs(ey2)));
            }
            if (worst > std::max(1.5, d * 0.45)) {
                DCDBG("가설 %dx%d 아핀 잔차 %.2f > %.2f\n", rows, cols, worst, std::max(1.5, d * 0.45));
                continue;
            }

            std::vector<uint8_t> grid(static_cast<size_t>(rows) * cols, 0);
            bool bad = false;
            for (int y = 0; y < rows && !bad; ++y) {
                for (int x = 0; x < cols; ++x) {
                    const int px = static_cast<int>(std::lround(s2[0][0] + s2[1][0] * x + s2[2][0] * y));
                    const int py = static_cast<int>(std::lround(s2[0][1] + s2[1][1] * x + s2[2][1] * y));
                    if (px < 0 || px >= image.width || py < 0 || py >= image.height) { bad = true; break; }
                    grid[static_cast<size_t>(y) * cols + x] =
                        image.pixels[static_cast<size_t>(py) * image.stride + px] <= th ? 1 : 0;
                }
            }
            if (bad) continue;

            const int par = (static_cast<int>(SX[0]) + static_cast<int>(SY[0])) & 1;
            int offParity = 0;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < cols; ++x)
                    if (((x + y) & 1) != par) offParity += grid[static_cast<size_t>(y) * cols + x];
            if (offParity * 4 > m) {
                DCDBG("가설 %dx%d 빈 패리티에 점 %d개\n", rows, cols, offParity);
                continue;   // 격자가 틀렸다
            }
            DCDBG("가설 %dx%d 샘플링 완료 (패리티 %d)\n", rows, cols, par);

            std::vector<uint8_t> base = grid;
            if (par) {
                std::vector<uint8_t> f(base.size());
                if (rows % 2 == 0) {
                    for (int y = 0; y < rows; ++y)
                        std::copy(base.begin() + static_cast<size_t>(rows - 1 - y) * cols,
                                  base.begin() + static_cast<size_t>(rows - y) * cols,
                                  f.begin() + static_cast<size_t>(y) * cols);
                } else {
                    for (int y = 0; y < rows; ++y)
                        for (int x = 0; x < cols; ++x)
                            f[static_cast<size_t>(y) * cols + x] = base[static_cast<size_t>(y) * cols + (cols - 1 - x)];
                }
                base.swap(f);
            }

            for (int k = 0; k < 8; ++k) {
                std::vector<uint8_t> v;
                int vr = 0, vc = 0;
                dihedral(base, rows, cols, k, v, vr, vc);
                std::vector<int> cws;
                if (!decodeMatrix(v, vr, vc, cws)) continue;
                std::string text;
                bool gs1 = false;
                if (!decodeCws(cws, text, gs1)) continue;

                DecodedSymbol s;
                s.symbology = Symbology::DOTCODE;
                s.text = text;
                s.rawBytes.assign(text.begin(), text.end());
                s.isGS1 = gs1;
                int x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
                for (int i = 0; i < m; ++i) {
                    x0 = std::min(x0, static_cast<int>(QX[i]));
                    x1 = std::max(x1, static_cast<int>(QX[i]));
                    y0 = std::min(y0, static_cast<int>(QY[i]));
                    y1 = std::max(y1, static_cast<int>(QY[i]));
                }
                s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
                res.push_back(std::move(s));
                return true;
            }
        }
    }
    DCDBG("모든 가설 실패\n");
    return false;
    DCDBG("모든 가설 실패\n");
    return false;
#undef DCDBG
}

} // namespace

std::vector<DecodedSymbol> DotCodeDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> res;
    if (!opt_.enabled || image.empty()) return res;
    if (image.width < 40 || image.height < 40) return res;
    const bool dbg = std::getenv("VSCAN_DOTCODE_DEBUG") != nullptr;

    /*
     * [임계를 하나만 쓰면 흐린 심볼에서 점이 붙는다]
     *
     * Otsu 임계는 인쇄된 점의 **가장자리까지** 어둡게 잡는 쪽으로 나오는데,
     * 흐린 이미지에서는 그러면 대각으로 이웃한 점들이 서로 닿아 한 뭉치가
     * 된다. 실측(블러 2): 임계 153에서 111점이 56뭉치가 되고 피치 추정이
     * 16.97 -> 18.98로 틀어져 검출이 통째로 실패했다.
     *
     * 임계를 낮추면 점의 심(core)만 남아 다시 떨어진다. 그래서 Otsu부터
     * 시작해 낮은 쪽으로 몇 번 더 시도한다. RS가 틀린 가설을 전부 거부하므로
     * 여러 번 던지는 것 자체는 안전하고, 비용은 **실패한 프레임에서만** 든다
     * (첫 임계에서 성공하면 바로 끝난다).
     */
    const int base = otsu(image);
    const int cand[4] = {base, base * 3 / 4, base / 2, base * 5 / 4};
    for (int k = 0; k < 4; ++k) {
        const int th = cand[k];
        if (th < 0 || th > 250) continue;
        if (k && th == cand[0]) continue;
        if (detectAt(image, th, dbg, res)) return res;
    }
    return res;
}

} // namespace vscan
