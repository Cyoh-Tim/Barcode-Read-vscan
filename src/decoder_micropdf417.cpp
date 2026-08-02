#include "vscan_internal/decoder_micropdf417.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "pdf417/PDFCodewordDecoder.h"
#include "pdf417/PDFDecoder.h"
#include "DecoderResult.h"
#include "vscan_internal/gs1_composite.hpp"

namespace vscan {
namespace {

/* ---------------------------------------------------------------------------
 * 표 — tools/extract_micropdf417_tables.py가 BWIPP에서 뽑고 실측 검증한다.
 * 손으로 고치지 말 것. 스크립트를 돌려 다시 뽑을 것.
 *
 * RAP은 6원소(막대3 + 공백3), 합 10모듈, 항상 막대로 시작한다.
 * 좌 RAP과 우 RAP은 같은 표를 쓰고(실측 확인), 중앙만 별개 표다.
 * ------------------------------------------------------------------------- */

const int kRapSide[52][6] = {
    {2,2,1,3,1,1}, {3,1,1,3,1,1}, {3,1,2,2,1,1}, {2,2,2,2,1,1},
    {2,1,3,2,1,1}, {2,1,4,1,1,1}, {2,2,3,1,1,1}, {3,1,3,1,1,1},
    {3,2,2,1,1,1}, {4,1,2,1,1,1}, {4,2,1,1,1,1}, {3,3,1,1,1,1},
    {2,4,1,1,1,1}, {2,3,2,1,1,1}, {2,3,1,2,1,1}, {3,2,1,2,1,1},
    {4,1,1,2,1,1}, {4,1,1,1,2,1}, {4,1,1,1,1,2}, {3,2,1,1,1,2},
    {3,1,2,1,1,2}, {3,1,1,2,1,2}, {3,1,1,2,2,1}, {3,1,1,1,3,1},
    {3,1,1,1,2,2}, {3,1,1,1,1,3}, {2,2,1,1,1,3}, {2,2,1,1,2,2},
    {2,2,1,1,3,1}, {2,2,1,2,2,1}, {2,2,2,1,2,1}, {3,1,2,1,2,1},
    {3,2,1,1,2,1}, {2,3,1,1,2,1}, {2,3,1,1,1,2}, {2,2,2,1,1,2},
    {2,1,3,1,1,2}, {2,1,2,2,1,2}, {2,1,2,2,2,1}, {2,1,2,1,3,1},
    {2,1,2,1,2,2}, {2,1,2,1,1,3}, {2,1,1,2,1,3}, {2,1,1,1,2,3},
    {2,1,1,1,3,2}, {2,1,1,1,4,1}, {2,1,1,2,3,1}, {2,1,1,2,2,2},
    {2,1,1,3,1,2}, {2,1,1,3,2,1}, {2,1,1,4,1,1}, {2,1,2,3,1,1},
};

const int kRapCenter[52][6] = {
    {1,1,2,2,3,1}, {1,2,1,2,3,1}, {1,2,2,1,3,1}, {1,3,1,1,3,1},
    {1,3,1,2,2,1}, {1,3,2,1,2,1}, {1,4,1,1,2,1}, {1,4,1,2,1,1},
    {1,4,2,1,1,1}, {1,3,3,1,1,1}, {1,3,2,2,1,1}, {1,3,1,3,1,1},
    {1,2,2,3,1,1}, {1,2,3,2,1,1}, {1,2,4,1,1,1}, {1,1,5,1,1,1},
    {1,1,4,2,1,1}, {1,1,4,1,2,1}, {1,2,3,1,2,1}, {1,2,3,1,1,2},
    {1,2,2,2,1,2}, {1,2,2,2,2,1}, {1,2,1,3,2,1}, {1,2,1,4,1,1},
    {1,1,2,4,1,1}, {1,1,3,3,1,1}, {1,1,3,2,2,1}, {1,1,3,2,1,2},
    {1,1,3,1,2,2}, {1,2,2,1,2,2}, {1,3,1,1,2,2}, {1,3,1,1,1,3},
    {1,2,2,1,1,3}, {1,1,3,1,1,3}, {1,1,2,2,1,3}, {1,1,2,2,2,2},
    {1,1,2,3,1,2}, {1,1,2,3,2,1}, {1,1,1,4,2,1}, {1,1,1,3,3,1},
    {1,1,1,3,2,2}, {1,1,1,2,3,2}, {1,1,1,2,2,3}, {1,1,1,1,3,3},
    {1,1,1,1,2,4}, {1,1,1,2,1,4}, {1,1,2,1,1,4}, {1,2,1,1,1,4},
    {1,2,1,1,2,3}, {1,2,1,1,3,2}, {1,1,2,1,3,2}, {1,1,2,1,4,1},
};

// {열, 행, EC 코드워드 수, 좌 RAP(1기반), 중앙 RAP, 우 RAP}
const int kVariants[34][6] = {
    {1,11, 7, 1, 0, 9},
    {1,14, 7, 8, 0, 8},
    {1,17, 7,36, 0,36},
    {1,20, 8,19, 0,19},
    {1,24, 8, 9, 0,17},
    {1,28, 8,25, 0,33},
    {2, 8, 8, 1, 0, 1},
    {2,11, 9, 1, 0, 9},
    {2,14, 9, 8, 0, 8},
    {2,17,10,36, 0,36},
    {2,20,11,19, 0,19},
    {2,23,13, 9, 0,17},
    {2,26,15,27, 0,35},
    {3, 6,12, 1, 1, 1},
    {3, 8,14, 7, 7, 7},
    {3,10,16,15,15,15},
    {3,12,18,25,25,25},
    {3,15,21,37,37,37},
    {3,20,26, 1,17,33},
    {3,26,32, 1, 9,17},
    {3,32,38,21,29,37},
    {3,38,44,15,31,47},
    {3,44,50, 1,25,49},
    {4, 4, 8,47,19,43},
    {4, 6,12, 1, 1, 1},
    {4, 8,14, 7, 7, 7},
    {4,10,16,15,15,15},
    {4,12,18,25,25,25},
    {4,15,21,37,37,37},
    {4,20,26, 1,17,33},
    {4,26,32, 1, 9,17},
    {4,32,38,21,29,37},
    {4,38,44,15,31,47},
    {4,44,50, 1,25,49},
};

// GS1 Composite CC-A 전용 변형(ISO 24723). 일반 MicroPDF417과 기하는 같지만
// 열/행/EC 조합이 다른 별도 표다. 같은 스크립트가 ccametrics에서 뽑는다.
const int kVariantsCCA[17][6] = {
    {2, 5, 4,39, 0,19},
    {2, 6, 4, 1, 0,33},
    {2, 7, 5,32, 0,12},
    {2, 8, 5, 8, 0,40},
    {2, 9, 6,14, 0,46},
    {2,10, 6,43, 0,23},
    {2,12, 7,20, 0,52},
    {3, 4, 4,11,43,23},
    {3, 5, 5, 1,33,13},
    {3, 6, 6, 5,37,17},
    {3, 7, 7,15,47,27},
    {3, 8, 7,21, 1,33},
    {4, 3, 4,40,20,52},
    {4, 4, 5,43,23, 3},
    {4, 5, 6,46,26, 6},
    {4, 6, 7,34,14,46},
    {4, 7, 8,29, 9,41},
};

// 열 수별 기하. {좌 데이터 코드워드 수, 우 데이터 코드워드 수, 전체 모듈 폭}
// 3열/4열만 중앙 RAP이 있고, 데이터가 좌/우 두 덩이로 갈린다.
struct ColGeom { int k1, k2, widthModules; };
const ColGeom kColGeom[5] = {{0,0,0}, {1,0,38}, {2,0,55}, {1,2,82}, {2,2,99}};

/* ---------------------------------------------------------------------------
 * GF(929) 리드-솔로몬
 *
 * zxing에도 있지만(PDFScanningDecoder.cpp의 DecodeErrorCorrection) 우리 빌드
 * 에서는 static이라 못 쓴다 — ZXING_EXPORT_TEST_ONLY가 ZXING_BUILD_FOR_TEST
 * 없이 static으로 펴진다. 벤더 빌드 설정을 켜는 대신 직접 썼다(zxing을 올릴
 * 때마다 그 설정을 재확인할 일을 안 만든다).
 *
 * 다항식은 **낮은 차수부터** 담는다(p[0]이 상수항). zxing은 반대 순서라
 * 코드를 비교할 때 헷갈리기 쉬운 자리다.
 * ------------------------------------------------------------------------- */
constexpr int kP = 929;   // 소수
constexpr int kGen = 3;   // 원시근

struct Gf929 {
    int exp[2 * 928];
    int log[kP];
    Gf929() {
        int x = 1;
        for (int i = 0; i < 928; ++i) { exp[i] = x; log[x] = i; x = (x * kGen) % kP; }
        for (int i = 928; i < 2 * 928; ++i) exp[i] = exp[i - 928];
        log[0] = 0;   // 안 쓴다
    }
    int mul(int a, int b) const { return (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]]; }
    int inv(int a) const { return exp[928 - log[a]]; }
    int sub(int a, int b) const { return (a - b + kP) % kP; }
    int add(int a, int b) const { return (a + b) % kP; }
};
const Gf929& gf() { static const Gf929 f; return f; }

using Poly = std::vector<int>;   // p[i] = x^i 계수

void trim(Poly& p) { while (p.size() > 1 && p.back() == 0) p.pop_back(); }

Poly polyMul(const Poly& a, const Poly& b) {
    if ((a.size() == 1 && a[0] == 0) || (b.size() == 1 && b[0] == 0)) return {0};
    Poly r(a.size() + b.size() - 1, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i]) continue;
        for (size_t j = 0; j < b.size(); ++j)
            r[i + j] = gf().add(r[i + j], gf().mul(a[i], b[j]));
    }
    trim(r);
    return r;
}

Poly polyAdd(const Poly& a, const Poly& b) {
    Poly r(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i) r[i] = gf().add(r[i], b[i]);
    trim(r);
    return r;
}

int polyEval(const Poly& p, int x) {
    int r = 0;
    for (size_t i = p.size(); i-- > 0;) r = gf().add(gf().mul(r, x), p[i]);
    return r;
}

// a를 b로 나눈 나머지
Poly polyMod(Poly a, const Poly& b) {
    const int bDeg = static_cast<int>(b.size()) - 1;
    const int bInv = gf().inv(b[bDeg]);
    while (static_cast<int>(a.size()) - 1 >= bDeg && !(a.size() == 1 && a[0] == 0)) {
        const int aDeg = static_cast<int>(a.size()) - 1;
        if (a[aDeg] == 0) { a.pop_back(); continue; }
        const int coef = gf().mul(a[aDeg], bInv);
        const int shift = aDeg - bDeg;
        for (int i = 0; i <= bDeg; ++i)
            a[i + shift] = gf().sub(a[i + shift], gf().mul(coef, b[i]));
        trim(a);
        if (aDeg == 0) break;
    }
    return a;
}

Poly polySub(const Poly& a, const Poly& b) {
    Poly r(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i) r[i] = gf().sub(r[i], b[i]);
    trim(r);
    return r;
}

// p(x) * scale * x^shift
Poly polyShiftScale(const Poly& p, int shift, int scale) {
    Poly r(p.size() + shift, 0);
    for (size_t i = 0; i < p.size(); ++i) r[i + shift] = gf().mul(p[i], scale);
    trim(r);
    return r;
}

} // namespace

/*
 * [규약을 먼저 못박는다 — 여기가 틀리면 조용히 엉뚱한 값을 낸다]
 *
 * received = [데이터..., EC...] (코드워드 순서 그대로, received[0]이 첫 코드워드).
 * R(x) = sum received[i] * x^(n-1-i)  — 즉 received[0]이 최고차다.
 * 신드롬 S_j = R(a^j), j = 1..numEC.
 * 오류 위치 다항식 L(x) = prod (1 - X_k x),  X_k = a^p,  p = n-1-idx.
 * Omega(x) = S(x) L(x) mod x^numEC,  S(x) = sum_{j=1..numEC} S_j x^(j-1).
 *
 * 이 파일의 다항식은 **낮은 차수부터** 담는다(p[0]이 상수항). zxing의
 * ModulusPoly는 반대라서, 두 코드를 나란히 놓고 비교할 때 헷갈리는 자리다.
 *
 * [부호] Berlekamp-Massey의 갱신은 GF(2)에서는 덧셈과 뺄셈이 같지만
 * GF(929)에서는 다르다. **뺄셈이 맞다.** 처음에 polyAdd로 썼다가
 * 오류 정정이 전부 실패했다.
 *
 * [안전장치] 고친 뒤 신드롬을 다시 계산해 전부 0인지 확인한다. 0이 아니면
 * false를 돌려준다 — 잘못 고친 코드워드를 정답인 척 내보내는 것이
 * 이 프로젝트에서 가장 나쁜 결과다(§3.18).
 */
bool microPdf417CorrectErrors(std::vector<int>& received, int numEC, int* fixedCount) {
    if (fixedCount) *fixedCount = 0;
    if (numEC <= 0 || received.empty()) return false;
    const Gf929& f = gf();

    Poly R(received.rbegin(), received.rend());

    Poly S(numEC, 0);
    bool anyError = false;
    for (int j = 1; j <= numEC; ++j) {
        S[j - 1] = polyEval(R, f.exp[j]);
        if (S[j - 1] != 0) anyError = true;
    }
    if (!anyError) return true;

    // Berlekamp-Massey
    Poly C{1}, B{1};
    int L = 0, m = 1, b = 1;
    for (int n = 0; n < numEC; ++n) {
        int d = S[n];
        for (int i = 1; i <= L && i < static_cast<int>(C.size()); ++i)
            d = f.add(d, f.mul(C[i], S[n - i]));
        if (d == 0) {
            ++m;
        } else if (2 * L <= n) {
            const Poly T = C;
            C = polySub(C, polyShiftScale(B, m, f.mul(d, f.inv(b))));
            L = n + 1 - L;
            B = T;
            b = d;
            m = 1;
        } else {
            C = polySub(C, polyShiftScale(B, m, f.mul(d, f.inv(b))));
            ++m;
        }
    }
    trim(C);
    const int errCount = static_cast<int>(C.size()) - 1;
    if (errCount <= 0 || errCount > numEC / 2) return false;

    // Chien 탐색
    std::vector<int> positions;
    for (int p = 0; p < static_cast<int>(received.size()); ++p)
        if (polyEval(C, f.exp[(928 - p % 928) % 928]) == 0) positions.push_back(p);
    if (static_cast<int>(positions.size()) != errCount) return false;

    Poly omega = polyMul(S, C);
    if (static_cast<int>(omega.size()) > numEC) omega.resize(numEC);
    trim(omega);

    Poly dC(C.size() > 1 ? C.size() - 1 : 1, 0);
    for (size_t i = 1; i < C.size(); ++i) dC[i - 1] = f.mul(static_cast<int>(i) % kP, C[i]);
    trim(dC);

    std::vector<int> backup = received;
    for (int p : positions) {
        const int xInv = f.exp[(928 - p % 928) % 928];
        const int den = polyEval(dC, xInv);
        if (den == 0) return false;
        /*
         * Forney. 교과서 공식에는 X_k 인자가 붙지만 **여기서는 안 붙는다** —
         * S(x)를 S_1부터 담았기(x^0이 S_1) 때문에 그 인자가 이미 흡수돼 있다.
         * 부호도 음수다. 넷을 다 계산해서 실제 오차와 맞춰 확정했다
         * (14x1 심볼, 코드워드 1번에 +137을 넣고: X*om/den=171, om/den=792,
         *  -X*om/den=758, -om/den=137 <- 실제 오차와 일치).
         */
        const int mag = f.sub(0, f.mul(polyEval(omega, xInv), f.inv(den)));
        const int idx = static_cast<int>(received.size()) - 1 - p;
        received[idx] = f.sub(received[idx], mag);
    }

    Poly R2(received.rbegin(), received.rend());
    for (int j = 1; j <= numEC; ++j) {
        if (polyEval(R2, f.exp[j]) != 0) { received = backup; return false; }
    }
    if (fixedCount) *fixedCount = errCount;
    return true;
}

namespace {

/* ---------------------------------------------------------------------------
 * 검출
 *
 * [왜 이렇게 싸게 되는가] 한 행의 원소 개수와 모듈 폭이 **열 수만 정해지면
 * 완전히 고정**이다:
 *     1열 21원소/38모듈   2열 29/55   3열 43/82   4열 45/99
 * 그래서 시작 위치 i와 열 수 하나를 가정하면 그 구간 길이의 합에서
 * 모듈 크기(unit)가 바로 나오고, RAP 6원소를 정수로 반올림해 표를
 * O(1)로 조회할 수 있다. 52개 패턴을 하나씩 맞춰볼 필요가 없다.
 * ------------------------------------------------------------------------- */

struct Run { bool bar; int start; int len; };

bool lineRuns(const GrayView& img, bool vertical, int k, std::vector<Run>& out) {
    out.clear();
    const int n = vertical ? img.height : img.width;
    const uint8_t* base = img.pixels + (vertical ? k : static_cast<size_t>(k) * img.stride);
    const int step = vertical ? img.stride : 1;
    int lo = 255, hi = 0;
    for (int i = 0; i < n; ++i) {
        const int v = base[static_cast<size_t>(i) * step];
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    if (hi - lo < 40) return false;
    const int thr = (lo + hi) / 2;
    bool cur = base[0] < thr;
    int start = 0;
    for (int i = 1; i < n; ++i) {
        const bool b = base[static_cast<size_t>(i) * step] < thr;
        if (b != cur) { out.push_back({cur, start, i - start}); cur = b; start = i; }
    }
    out.push_back({cur, start, n - start});
    return out.size() >= 20;
}

std::vector<Run> reversedRuns(const std::vector<Run>& in) {
    std::vector<Run> out;
    out.reserve(in.size());
    int pos = 0;
    for (auto it = in.rbegin(); it != in.rend(); ++it) { out.push_back({it->bar, pos, it->len}); pos += it->len; }
    return out;
}

// RAP 6원소를 18비트 정수로 눌러 담는다(각 원소 1..5). 표 조회용.
int packRap(const int* r) {
    int key = 0;
    for (int i = 0; i < 6; ++i) {
        if (r[i] < 1 || r[i] > 7) return -1;
        key = (key << 3) | r[i];
    }
    return key;
}

struct RapIndex {
    std::map<int, int> side, center;
    RapIndex() {
        for (int i = 0; i < 52; ++i) {
            side[packRap(kRapSide[i])] = i;
            center[packRap(kRapCenter[i])] = i;
        }
    }
};
const RapIndex& rapIndex() { static const RapIndex r; return r; }

// runs[at..at+5]를 unit으로 정규화해 RAP 표에서 찾는다. 못 찾으면 -1.
int matchRap(const std::vector<Run>& runs, size_t at, double unit, bool centerTable) {
    if (at + 6 > runs.size() || !runs[at].bar) return -1;
    int q[6];
    for (int i = 0; i < 6; ++i) {
        const double v = runs[at + i].len / unit;
        q[i] = static_cast<int>(std::lround(v));
        if (q[i] < 1 || q[i] > 6) return -1;
        if (std::fabs(v - q[i]) > 0.45) return -1;   // 반올림이 아슬아슬하면 버린다
    }
    int sum = 0;
    for (int i = 0; i < 6; ++i) sum += q[i];
    if (sum != 10) return -1;
    const int key = packRap(q);
    const auto& m = centerTable ? rapIndex().center : rapIndex().side;
    auto it = m.find(key);
    return it == m.end() ? -1 : it->second;
}

// 17모듈 코드워드 하나(막대4 + 공백4 = 8원소)를 zxing으로 푼다.
int codewordAt(const std::vector<Run>& runs, size_t at, double unit) {
    if (at + 8 > runs.size() || !runs[at].bar) return -1;
    std::array<int, 8> counts{};
    double total = 0;
    for (int i = 0; i < 8; ++i) { counts[i] = runs[at + i].len; total += runs[at + i].len; }
    // 17모듈이어야 한다. 크게 벗어나면 정렬이 어긋난 것이다.
    const double mods = total / unit;
    if (mods < 15.0 || mods > 19.0) return -1;
    const int sym = ZXing::Pdf417::CodewordDecoder::GetDecodedValue(counts);
    return ZXing::Pdf417::CodewordDecoder::GetCodeword(sym);
}

struct RowParse {
    int cols = 0;
    int leftIdx = -1, centerIdx = -1, rightIdx = -1;
    int from = 0, to = 0;         // 주사선 위 좌표
    int line = 0;                 // 주사선 번호
    std::vector<int> cws;
};

// runs[at]부터 cols열짜리 한 행을 읽어본다.
bool parseRow(const std::vector<Run>& runs, size_t at, int cols, RowParse& out) {
    const ColGeom& g = kColGeom[cols];
    const int nRuns = 6 + 8 * g.k1 + (cols >= 3 ? 6 + 8 * g.k2 : 0) + 6 + 1;
    if (at + static_cast<size_t>(nRuns) > runs.size()) return false;

    int total = 0;
    for (int i = 0; i < nRuns; ++i) total += runs[at + i].len;
    const double unit = static_cast<double>(total) / g.widthModules;
    if (unit < 0.9) return false;

    size_t p = at;
    const int li = matchRap(runs, p, unit, false);
    if (li < 0) return false;
    p += 6;

    std::vector<int> cws;
    for (int c = 0; c < g.k1; ++c) {
        const int v = codewordAt(runs, p, unit);
        if (v < 0) return false;
        cws.push_back(v);
        p += 8;
    }
    int ci = -1;
    if (cols >= 3) {
        ci = matchRap(runs, p, unit, true);
        if (ci < 0) return false;
        p += 6;
        for (int c = 0; c < g.k2; ++c) {
            const int v = codewordAt(runs, p, unit);
            if (v < 0) return false;
            cws.push_back(v);
            p += 8;
        }
    }
    const int ri = matchRap(runs, p, unit, false);
    if (ri < 0) return false;
    p += 6;

    // 정지 막대: 1모듈짜리 막대 하나
    if (p >= runs.size() || !runs[p].bar) return false;
    if (runs[p].len > unit * 2.2) return false;

    out.cols = cols;
    out.leftIdx = li;
    out.centerIdx = ci;
    out.rightIdx = ri;
    out.from = runs[at].start;
    out.to = runs[p].start + runs[p].len;
    out.cws = std::move(cws);
    return true;
}

} // namespace

std::vector<DecodedSymbol> MicroPdf417Decoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (image.empty() || image.width < 38 || image.height < 4) return results;

    std::vector<RowParse> parses;
    std::vector<Run> runs;

    /*
     * 네 방향을 다 본다 — 가로/세로 x 정방향/역방향. 0/90/180/270도를
     * 이걸로 덮는다. 잘못된 방향은 RAP 표에서 걸리거나 뒤의 변형 대조에서
     * 걸러지므로 오디코딩을 늘리지 않는다.
     */
    for (int axis = 0; axis < 2; ++axis) {
        const bool vertical = axis == 1;
        const int extent = vertical ? image.width : image.height;
        const int lineLen = vertical ? image.height : image.width;
        // 행을 하나도 안 놓치려면 촘촘히 훑어야 한다. 행 높이는 보통 모듈의
        // 2~3배라 원본 해상도에서 한 행에 여러 줄이 걸린다.
        const int step = std::max(1, extent / 400);
        for (int line = 0; line < extent; line += step) {
            if (!lineRuns(image, vertical, line, runs)) continue;
            if (runs.size() > 4096) continue;
            const std::vector<Run> rev = reversedRuns(runs);
            for (int dir = 0; dir < 2; ++dir) {
                const std::vector<Run>& rr = dir == 0 ? runs : rev;
                for (size_t i = 0; i < rr.size(); ++i) {
                    if (!rr[i].bar) continue;
                    for (int cols = 1; cols <= 4; ++cols) {
                        RowParse rp;
                        if (!parseRow(rr, i, cols, rp)) continue;
                        if (dir == 1) {
                            const int f = lineLen - rp.to, t = lineLen - rp.from;
                            rp.from = f; rp.to = t;
                        }
                        rp.line = line;
                        rp.cols = cols;
                        // 방향/축을 열 수와 함께 묶어두려고 부호로 표시한다
                        rp.cols = cols * 4 + axis * 2 + dir;
                        /*
                         * [열 수를 하나 찾았다고 멈추면 안 된다]
                         * 처음엔 첫 성공에서 break했다. 그런데 **같은 자리에서
                         * 더 작은 열 수가 우연히 맞는 일이 있다** — 실측으로
                         * 4열 3행 심볼의 첫 행이 cols=1로 먼저 맞아버려서
                         * 그 행을 통째로 잃었고, 행이 하나 빠지니(4코드워드,
                         * EC 4개로는 2개까지만 정정) 심볼 전체가 미검출이 됐다.
                         * 전부 시도해서 다 담고, 진짜인지는 뒤의 변형 대조가 가린다.
                         */
                        parses.push_back(std::move(rp));
                    }
                }
            }
        }
    }
    if (parses.empty()) return results;

    /*
     * [행을 묶는 방법 — 겹침이 아니라 양 끝이 맞아야 한다]
     *
     * 처음엔 "주사선 위 구간이 겹치면 같은 심볼"로 묶었다. 그러면 심볼
     * 중간에서 어긋나게 시작한 가짜 파싱이 만든 무리가 진짜 행들을
     * 빨아들여서, 한 심볼의 행이 여러 무리로 쪼개졌다(실측: 23x2에서
     * 23행 중 22행짜리 무리와 11행짜리 무리로 갈렸다).
     * 한 심볼의 모든 행은 **양 끝 좌표가 거의 같으므로** 그걸 조건으로 쓴다.
     */
    struct Cluster {
        int key = 0;                        // cols*4 + axis*2 + dir
        int from = 0, to = 0;
        std::map<int, std::map<std::vector<int>, int>> byRap;  // rapIdx -> 코드워드 -> 표수
        int lineMin = 0, lineMax = 0;
    };
    std::vector<Cluster> clusters;
    for (const RowParse& rp : parses) {
        Cluster* hit = nullptr;
        for (Cluster& c : clusters) {
            if (c.key != rp.cols) continue;
            const int tol = std::max(4, (c.to - c.from) / 12);
            if (std::abs(rp.from - c.from) > tol || std::abs(rp.to - c.to) > tol) continue;
            hit = &c;
            break;
        }
        if (!hit) {
            clusters.push_back(Cluster{rp.cols, rp.from, rp.to, {}, rp.line, rp.line});
            hit = &clusters.back();
        }
        hit->byRap[rp.leftIdx][rp.cws]++;
        hit->lineMin = std::min(hit->lineMin, rp.line);
        hit->lineMax = std::max(hit->lineMax, rp.line);
    }

    for (const Cluster& c : clusters) {
        const int cols = c.key / 4;
        const int axis = (c.key % 4) / 2;

        // 행마다 가장 많이 나온 코드워드 조합을 채택한다
        std::map<int, std::vector<int>> rowCws;
        for (const auto& kv : c.byRap) {
            const std::vector<int>* best = nullptr;
            int bestN = 0;
            for (const auto& e : kv.second)
                if (e.second > bestN) { bestN = e.second; best = &e.first; }
            if (best && bestN >= opt_.minLineCount && static_cast<int>(best->size()) == cols)
                rowCws[kv.first] = *best;
        }
        if (rowCws.size() < 3) continue;

        /*
         * [행 번호는 변형 가설에서 역산한다 — 관측된 행이 연속인지 따지지 않는다]
         *
         * 처음엔 "관측된 RAP 인덱스가 1씩 이어져야 한다"고 봤는데, 주사선이
         * 한 행을 통째로 놓치면(실측: 8x3에서 RAP 11 한 행) 그 심볼을 통째로
         * 버리게 된다. 변형을 가정하면 행 번호 r = (rapIdx - (rapL-1)) mod 52이고,
         * **관측된 모든 rapIdx가 [0, rows) 안에 들어가야 한다**는 것 자체가
         * 강한 변형 판별이 된다.
         *
         * 빠진 행은 0으로 채우고 리드-솔로몬에 맡긴다. 행 하나가 비면 cols개의
         * 오류이므로 EC의 절반을 넘지 않을 때만 시도한다.
         */
        // 일반 변형 34개 + CC-A 변형 17개를 다 본다. CC-A는 GS1 Composite의
        // 2D 성분으로만 쓰이지만 기하와 RAP 규칙은 같으므로 같은 자리에서 본다.
        std::vector<std::pair<const int*, bool>> allVariants;   // {변형, CC-A 표인가}
        for (const auto& v : kVariants) allVariants.push_back({v, false});
        for (const auto& v : kVariantsCCA) allVariants.push_back({v, true});
        bool decoded = false;
        for (const auto& vv : allVariants) {
            if (decoded) break;
            const int* v = vv.first;
            const bool isCCA = vv.second;
            if (v[0] != cols) continue;
            const int rows = v[1], ec = v[2], base = (v[3] - 1) % 52;
            const int dataCount = rows * cols - ec;

            std::vector<int> cws(static_cast<size_t>(rows) * cols, 0);
            std::vector<char> known(rows, 0);
            bool fits = true;
            for (const auto& kv : rowCws) {
                const int r = ((kv.first - base) % 52 + 52) % 52;
                if (r >= rows) { fits = false; break; }
                known[r] = 1;
                std::copy(kv.second.begin(), kv.second.end(), cws.begin() + static_cast<size_t>(r) * cols);
            }
            if (!fits) continue;
            int missing = 0;
            for (int r = 0; r < rows; ++r) if (!known[r]) missing++;
            if (missing * cols > ec / 2) continue;

            int fixed = 0;
            if (!microPdf417CorrectErrors(cws, ec, &fixed)) continue;

            // Pdf417::Decode()는 codewords[0]을 길이 서술자로 읽는다.
            // MicroPDF417에는 그게 없으므로 합성해서 앞에 붙인다.
            std::vector<int> hi;
            hi.reserve(dataCount + 1);
            hi.push_back(dataCount + 1);
            hi.insert(hi.end(), cws.begin(), cws.begin() + dataCount);
            if (getenv("VSCAN_MPDF_DEBUG")) {
                fprintf(stderr, "[mpdf] 변형 %dx%d ec=%d 데이터 코드워드:", v[0], v[1], ec);
                for (int i = 0; i < dataCount; ++i) fprintf(stderr, " %d", cws[i]);
                fprintf(stderr, "\n");
            }
            /*
             * [GS1 Composite의 2D 성분은 데이터 계층이 다른 규격이다]
             *
             * 심볼 계층은 여기까지 잘 읽힌다(리드-솔로몬 통과). 그런데
             * MicroPDF417 자체는 ISO 24728이고 Composite의 2D 성분은
             * **ISO 24723의 범용 인코딩**을 쓴다. 그대로 Pdf417::Decode()에
             * 넣으면 valid=1이 나오면서 글자가 깨진다
             * (실측: (99)1234-abcd -> "N\tHF PGRPS}IBD").
             *
             * 판별은 실측으로 확인한 것이다:
             *   - CC-A: 전용 변형표(kVariantsCCA)로만 맞는다.
             *   - CC-B: 표준 변형표를 쓰지만 **첫 코드워드가 920**이다.
             *   - 단독 MicroPDF417: 첫 코드워드가 900(Text Latch)이다.
             * [[vscan-lite-gs1-composite]]
             */
            const bool isCCB = dataCount > 0 && cws[0] == 920;
            if (isCCA || isCCB) {
                std::vector<int> dataCws(cws.begin(), cws.begin() + dataCount);
                const std::vector<uint8_t> bits =
                    isCCB ? gs1CompositeBitsFromByteCompaction(dataCws)
                          : gs1CompositeBitsFromCCA(dataCws);
                std::string text;
                if (bits.empty() || !decodeGs1CompositeBits(bits, text)) continue;

                DecodedSymbol s;
                s.symbology = Symbology::GS1_COMPOSITE;
                s.text = text;
                s.rawBytes.assign(text.begin(), text.end());
                s.isGS1 = true;
                const int a0 = c.from, a1 = c.to;
                const int b0 = c.lineMin, b1 = c.lineMax;
                const int x0 = axis ? b0 : a0, x1 = axis ? b1 : a1;
                const int y0 = axis ? a0 : b0, y1 = axis ? a1 : b1;
                s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
                results.push_back(std::move(s));
                decoded = true;
                continue;
            }

            auto res = ZXing::Pdf417::Decode(hi);
            if (!res.isValid()) continue;

            DecodedSymbol s;
            s.symbology = Symbology::MICRO_PDF417;
            for (auto ch : res.text()) s.text.push_back(static_cast<char>(ch));
            s.rawBytes.assign(s.text.begin(), s.text.end());
            const int a0 = c.from, a1 = c.to;
            const int b0 = c.lineMin, b1 = c.lineMax;
            const int x0 = axis ? b0 : a0, x1 = axis ? b1 : a1;
            const int y0 = axis ? a0 : b0, y1 = axis ? a1 : b1;
            s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
            results.push_back(std::move(s));
            decoded = true;
        }
    }
    return results;
}

} // namespace vscan
