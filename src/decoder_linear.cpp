#include "vscan_internal/decoder_linear.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>
#include <string>
#include <vector>

namespace vscan {
namespace {

/* ---------------------------------------------------------------------------
 * 인코딩 표 (BWIPP code2of5 / pharmacode에서 가져와 렌더 이미지로 교차 확인)
 *
 * 값은 모듈 배수다 — 1 = 가는 것, 3 = 굵은 것. 한 원소씩 막대/공백이
 * 번갈아 나오고 항상 막대로 시작한다.
 * ------------------------------------------------------------------------- */

// Industrial(Standard) 2of5: 숫자당 막대 5 + 공백 5 = 원소 10개.
// 정보는 막대에만 있고(5개 중 2개가 굵다) 공백은 전부 가늘다.
const int kInd25Digit[10][10] = {
    {1,1,1,1,3,1,3,1,1,1}, {3,1,1,1,1,1,1,1,3,1}, {1,1,3,1,1,1,1,1,3,1},
    {3,1,3,1,1,1,1,1,1,1}, {1,1,1,1,3,1,1,1,3,1}, {3,1,1,1,3,1,1,1,1,1},
    {1,1,3,1,3,1,1,1,1,1}, {1,1,1,1,1,1,3,1,3,1}, {3,1,1,1,1,1,3,1,1,1},
    {1,1,3,1,1,1,3,1,1,1},
};
const int kInd25Start[6] = {3,1,3,1,1,1};
const int kInd25Stop[5]  = {3,1,1,1,3};

// COOP 2of5: 숫자당 원소 6개(막대 3 + 공백 3), 그중 굵은 것이 정확히 2개.
// 2of5 계열이지만 표가 ITF/Industrial과 전혀 다르다 — 그래서 zxing이 못 읽는다.
const int kCoop25Digit[10][6] = {
    {3,3,1,1,1,1}, {1,1,1,3,3,1}, {1,1,3,1,3,1}, {1,1,3,3,1,1}, {1,3,1,1,3,1},
    {1,3,1,3,1,1}, {1,3,3,1,1,1}, {3,1,1,1,3,1}, {3,1,1,3,1,1}, {3,1,3,1,1,1},
};
const int kCoop25Start[4] = {3,1,3,1};
const int kCoop25Stop[3]  = {1,3,3};

/* ---------------------------------------------------------------------------
 * 런 추출
 * ------------------------------------------------------------------------- */

struct Run {
    bool bar;    // 어두우면 true
    int  start;  // x 시작
    int  len;
};

/*
 * 한 주사선을 런 배열로 바꾼다. 임계는 그 선의 최소/최대 중간값이다 —
 * 선마다 따로 잡는 이유는 §3.43(대비 축)과 같다. 조명 기울기가 있어도
 * 한 선 안에서는 대체로 단조롭기 때문에 전역 임계보다 훨씬 잘 버틴다.
 *
 * 대비가 없으면(최대-최소가 작으면) 그 선은 버린다. 노이즈만 있는 선에서
 * 런을 뽑으면 수천 개가 나와서 뒤 단계가 쓸데없이 비싸진다.
 *
 * 가로/세로 두 방향으로 훑는다. 90도로 세워진 1D 코드는 가로로 훑으면
 * 한 행이 통째로 같은 색이라 런이 하나도 안 나온다 — 실측에서 셋 다 90도가
 * 전멸했다. 파이프라인의 회전 구제에 기대는 방법도 있지만 그건 기본이
 * 꺼져 있고(enable1DDeskewRescue), 세로 훑기는 같은 코드 몇 줄이면 된다.
 */
bool lineRuns(const GrayView& img, bool vertical, int k, std::vector<Run>& out) {
    out.clear();
    const int n = vertical ? img.height : img.width;
    const uint8_t* base = img.pixels + (vertical ? k : static_cast<size_t>(k) * img.stride);
    const int step = vertical ? img.stride : 1;

    int lo = 255, hi = 0;
    for (int i = 0; i < n; ++i) {
        const int v = base[static_cast<size_t>(i) * step];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi - lo < 40) return false;
    const int thr = (lo + hi) / 2;

    bool cur = base[0] < thr;
    int start = 0;
    for (int i = 1; i < n; ++i) {
        const bool b = base[static_cast<size_t>(i) * step] < thr;
        if (b != cur) {
            out.push_back({cur, start, i - start});
            cur = b;
            start = i;
        }
    }
    out.push_back({cur, start, n - start});
    return out.size() >= 4;
}

/*
 * [패턴 일치도] zxing의 patternMatchVariance와 같은 방식.
 * 관측 폭 합과 패턴 폭 합의 비로 모듈 크기를 정하고, 원소별 편차를 본다.
 * 원소 하나라도 허용치를 넘으면 즉시 탈락 — 평균만 보면 한 원소가
 * 크게 틀려도 나머지가 덮어버린다.
 * 반환값이 작을수록 잘 맞는 것이고, 실패는 무한대다.
 */
double patternMatch(const std::vector<Run>& runs, size_t at, const int* pat, int n,
                    double maxIndividual) {
    if (at + static_cast<size_t>(n) > runs.size()) return std::numeric_limits<double>::max();
    int total = 0, patTotal = 0;
    for (int i = 0; i < n; ++i) { total += runs[at + i].len; patTotal += pat[i]; }
    if (total < patTotal) return std::numeric_limits<double>::max();  // 모듈이 1px 미만
    const double unit = static_cast<double>(total) / patTotal;
    const double maxVar = unit * maxIndividual;
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double expect = pat[i] * unit;
        const double var = std::fabs(runs[at + i].len - expect);
        if (var > maxVar) return std::numeric_limits<double>::max();
        sum += var;
    }
    return sum / total;
}

// 정지대 검사: 시작 패턴 바로 앞(또는 정지 패턴 바로 뒤)에 모듈의 kQuiet배
// 이상 되는 밝은 구간이 있어야 한다. 코드가 프레임/ROI 경계에 딱 붙어
// 잘린 경우도 통과시킨다 — 이미 잘라온 ROI가 주 입력이기 때문이다.
constexpr double kQuietModules = 6.0;

bool quietBefore(const std::vector<Run>& runs, size_t at, double unit) {
    if (at == 0) return true;                       // 경계에 붙음
    const Run& r = runs[at - 1];
    if (r.bar) return false;
    if (at == 1) return true;                       // 앞이 곧 화면 끝
    return r.len >= unit * kQuietModules;
}

bool quietAfter(const std::vector<Run>& runs, size_t end, double unit) {
    if (end >= runs.size()) return true;
    const Run& r = runs[end];
    if (r.bar) return false;
    if (end + 1 >= runs.size()) return true;
    return r.len >= unit * kQuietModules;
}

/* ---------------------------------------------------------------------------
 * 2of5 계열 (Industrial / COOP) — 표만 다르고 구조는 같다
 * ------------------------------------------------------------------------- */

struct Code25Spec {
    const int* start; int startN;
    const int* stop;  int stopN;
    const int* digits; int digitN;   // digits는 10 x digitN 평탄 배열
    Symbology sym;
};

/*
 * [역방향도 본다] 세로로 훑을 때 코드가 아래에서 위로 놓여 있으면 런 배열이
 * 뒤집혀 들어온다 — 시작 패턴이 안 맞아 통째로 놓친다(실측: 90도 Industrial
 * 2of5 미검출). 2of5는 시작/정지 패턴이 방향을 증명해주므로 뒤집어서 한 번
 * 더 시도해도 오디코딩이 늘지 않는다. Pharmacode와 다른 점이 이것이다.
 */
// 주사선마다 벡터를 새로 만들면 프레임당 수백 번 malloc이 돈다.
// A53처럼 할당이 비싼 쪽에서 특히 나쁘다 — 버퍼를 밖에서 받아 재사용한다.
void reversedRuns(const std::vector<Run>& in, std::vector<Run>& out) {
    out.clear();
    out.reserve(in.size());
    int pos = 0;
    for (auto it = in.rbegin(); it != in.rend(); ++it) {
        out.push_back({it->bar, pos, it->len});
        pos += it->len;
    }
}

bool decode25At(const std::vector<Run>& runs, size_t at, const Code25Spec& spec,
                int minDigits, std::string& outText, size_t& outEnd) {
    constexpr double kMaxIndividual = 0.55;
    if (patternMatch(runs, at, spec.start, spec.startN, kMaxIndividual) ==
        std::numeric_limits<double>::max())
        return false;

    int startTotal = 0, startPat = 0;
    for (int i = 0; i < spec.startN; ++i) { startTotal += runs[at + i].len; startPat += spec.start[i]; }
    const double unit = static_cast<double>(startTotal) / startPat;
    if (unit < 1.0) return false;
    if (!quietBefore(runs, at, unit)) return false;

    std::string text;
    size_t p = at + spec.startN;
    while (true) {
        // 정지 패턴이 먼저인지 본다. 숫자와 정지가 동시에 맞을 수 있으므로
        // 정지를 먼저 시도하고, 정지 뒤가 정지대가 아니면 숫자로 계속 간다.
        if (patternMatch(runs, p, spec.stop, spec.stopN, kMaxIndividual) !=
                std::numeric_limits<double>::max() &&
            quietAfter(runs, p + spec.stopN, unit)) {
            if (static_cast<int>(text.size()) < minDigits) return false;
            outText = text;
            outEnd = p + spec.stopN;
            return true;
        }

        int best = -1;
        double bestVar = std::numeric_limits<double>::max();
        for (int d = 0; d < 10; ++d) {
            const double v = patternMatch(runs, p, spec.digits + d * spec.digitN, spec.digitN,
                                          kMaxIndividual);
            if (v < bestVar) { bestVar = v; best = d; }
        }
        if (best < 0) return false;
        text.push_back(static_cast<char>('0' + best));
        p += spec.digitN;
        if (text.size() > 64) return false;          // 폭주 방지
    }
}

/* ---------------------------------------------------------------------------
 * Pharmacode
 *
 * 막대만 정보를 갖는다. 왼쪽 막대부터 b0..b(n-1) (가는 것 0, 굵은 것 1)일 때
 *      value = sum (b_i + 1) * 2^(n-1-i),   n = 2..16,  값 3..131070
 * BWIPP 렌더로 확인: "1234" -> 막대 10개 n,n,w,w,n,w,n,n,w,w -> 1234.
 *
 * [방향을 못 정한다 — 그래서 가로 훑기만 쓴다]
 * 시작/정지 패턴이 없어서 뒤집어 읽어도 **항상 유효한 다른 값**이 나온다.
 * 세로 훑기를 넣었더니 90도로 세운 Pharmacode가 1234 대신 307/50으로
 * 읽혔다 — 위에서 아래로 읽은 것이고, 그게 틀렸다고 판정할 근거가
 * 코드 안에 없다. 미검출이 오디코딩보다 낫다는 기준(§3.18)에 따라
 * 세로 훑기에서는 아예 Pharmacode를 안 본다. 90도로 세워 찍는 배치라면
 * 프레임을 돌려서 넣어야 한다.
 *
 * [Industrial 2of5와 구조가 겹친다]
 * Industrial 2of5는 공백이 전부 가늘고 막대만 가늘/굵으로 갈린다 —
 * 그게 정확히 Pharmacode의 구조다. 실제로 90도 Industrial 2of5가
 * Pharmacode 2612로 읽혔다. 둘을 같이 켜면 겹치는 자리에서는 2of5를
 * 남기고 Pharmacode를 버린다(아래 dropOverlappedPharmacode).
 * ------------------------------------------------------------------------- */

bool decodePharmacode(const std::vector<Run>& runs, size_t at, size_t end, int minBars,
                      std::string& outText) {
    std::vector<int> bars, spaces;
    for (size_t i = at; i < end; ++i) {
        if (runs[i].bar) bars.push_back(runs[i].len);
        else spaces.push_back(runs[i].len);
    }
    const int n = static_cast<int>(bars.size());
    if (n < std::max(2, minBars) || n > 16) return false;
    if (static_cast<int>(spaces.size()) != n - 1) return false;   // 막대-공백 교대여야 한다

    // 공백은 전부 같은 폭이어야 한다. Code128 같은 코드는 공백이 1~4모듈로
    // 흩어져서 여기서 걸린다 — 이게 오디코딩 방어의 핵심이다.
    const auto sp = std::minmax_element(spaces.begin(), spaces.end());
    if (!spaces.empty() && *sp.second > *sp.first * 1.35 + 1.0) return false;

    /*
     * [공백을 기준자로 쓴다 — 상대 비교로는 원리적으로 안 갈린다]
     *
     * 처음엔 막대 최소/최대의 비로 가늘/굵음을 갈랐다. 그러면 **막대가
     * 전부 같은 폭일 때 전부 가는 것인지 전부 굵은 것인지 판정할 근거가
     * 없다.** "전부 가늘다"로 찍었더니 200장 코퍼스에서 유령 29장이
     * 나왔다(전부 값 15 = 같은 폭 막대 4개). 실제로는 코드도 아니었다.
     *
     * Pharmacode는 폭이 규격으로 고정돼 있다 — Laetus 규격이 가는 막대
     * 0.5mm / 공백 1.0mm / 굵은 막대 1.5mm(= 1 : 2 : 3)이고, BWIPP 렌더는
     * 3 : 5 : 9(= 1 : 1.67 : 3)다. 둘 다 **공백을 1로 놓으면 가는 막대는
     * 0.5~0.6, 굵은 막대는 1.5~1.8**이다. 공백은 값과 무관하게 항상 같은
     * 폭이므로 기준자로 쓸 수 있고, 그러면 절대 판정이 된다.
     *
     * 부수 효과가 방어다 — 막대 폭이 공백과 비슷한(비 1.0 근처) 패턴은
     * 두 부류 어디에도 안 들어가서 탈락한다. 우연히 생기는 막대열은
     * 대부분 여기 걸린다.
     */
    std::vector<int> sorted = spaces;
    std::sort(sorted.begin(), sorted.end());
    const double gap = sorted[sorted.size() / 2];       // 공백 중앙값
    if (gap < 2.0) return false;                        // 모듈이 너무 작아 못 잰다

    const auto bm = std::minmax_element(bars.begin(), bars.end());
    const int bmin = *bm.first;
    if (bmin <= 0) return false;

    std::vector<int> bits(n, 0);
    for (int i = 0; i < n; ++i) {
        const double r = bars[i] / gap;
        if (r >= 0.35 && r <= 0.85)      bits[i] = 0;   // 가는 막대
        else if (r >= 1.20 && r <= 2.20) bits[i] = 1;   // 굵은 막대
        else return false;
    }

    // 정지대. 시작/정지 패턴이 없는 심볼로지라 "여기서부터 여기까지"를
    // 정지대로만 증명할 수 있다 — 이게 없으면 다른 코드의 막대열 일부를
    // 잘라내 값으로 읽어버린다.
    const double unit = bmin;
    if (!quietBefore(runs, at, unit) || !quietAfter(runs, end, unit)) return false;

    long long value = 0;
    for (int i = 0; i < n; ++i) value = value * 2 + (bits[i] + 1);
    if (value < 3 || value > 131070) return false;
    outText = std::to_string(value);
    return true;
}

/* ---------------------------------------------------------------------------
 * 한 행에서 나온 후보를 모으는 그릇
 * ------------------------------------------------------------------------- */

struct Hit {
    Symbology sym;
    std::string text;
    int x0, y0, x1, y1;
    int count = 0;
    bool vertical = false;
};

/*
 * 합의는 **축별로, 그리고 자리별로** 센다.
 *
 * 축별: 가로 훑기 몇 줄과 세로 훑기 몇 줄이 섞여서 minLineCount를 채우면
 * 안 된다. 한 코드는 한 방향으로만 제대로 읽히므로 섞였다면 우연이다.
 *
 * 자리별: 처음엔 (심볼로지, 텍스트)만 열쇠로 썼는데, 노이즈 이미지에서
 * **같은 값의 유령이 프레임 여기저기서 한 번씩 나와** 합쳐지면서 합의
 * 3줄을 채웠다(실측: 노이즈 40의 Industrial 2of5 이미지에 Pharmacode 25가
 * 딸려 나옴). 같은 코드에서 나온 줄이라면 자리가 겹쳐야 한다.
 */
void addHit(std::vector<Hit>& acc, bool vertical, Symbology sym, const std::string& text,
            int x0, int y0, int x1, int y1) {
    for (Hit& h : acc) {
        if (h.vertical != vertical || h.sym != sym || h.text != text) continue;
        // 겹침은 **주사 방향으로만** 본다. 수직 방향은 주사선 번호라 줄마다
        // 다른 게 당연하고, 그걸 겹침 조건에 넣으면 합의가 영영 1이 된다.
        const bool apart = vertical ? (y0 > h.y1 || h.y0 > y1) : (x0 > h.x1 || h.x0 > x1);
        if (apart) continue;
        h.count++;
        h.x0 = std::min(h.x0, x0);
        h.y0 = std::min(h.y0, y0);
        h.x1 = std::max(h.x1, x1);
        h.y1 = std::max(h.y1, y1);
        return;
    }
    acc.push_back(Hit{sym, text, x0, y0, x1, y1, 1, vertical});
}

} // namespace

std::vector<DecodedSymbol> LinearDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (image.empty() || image.height < 8 || image.width < 16) return results;
    if (!opt_.industrial2of5 && !opt_.coop2of5 && !opt_.pharmacode) return results;

    const Code25Spec kIndustrial{kInd25Start, 6, kInd25Stop, 5,
                                 &kInd25Digit[0][0], 10, Symbology::INDUSTRIAL_2OF5};
    const Code25Spec kCoop{kCoop25Start, 4, kCoop25Stop, 3,
                           &kCoop25Digit[0][0], 6, Symbology::COOP_2OF5};

    constexpr int kScanLines = 21;
    std::vector<Hit> acc;
    std::vector<Run> runs, rev;

    // 가로/세로 두 방향. 주사선은 양 끝 8%를 피해서 고르게 뿌린다 —
    // 잘라온 ROI라도 가장자리 한두 줄은 이웃 픽셀이 섞여 있는 경우가 많다.
    for (int axis = 0; axis < 2; ++axis) {
        const bool vertical = (axis == 1);
        const int extent = vertical ? image.width : image.height;
        const int a0 = std::max(1, extent * 8 / 100);
        const int a1 = std::min(extent - 1, extent - extent * 8 / 100);
        const int span = std::max(1, a1 - a0);

        // 주사선 위의 (start,len)을 이미지 좌표 사각형으로 옮긴다.
        auto box = [&](int from, int to, int line, int& x0, int& y0, int& x1, int& y1) {
            if (vertical) { x0 = line; x1 = line; y0 = from; y1 = to; }
            else          { x0 = from; x1 = to;  y0 = line; y1 = line; }
        };

        for (int k = 0; k < kScanLines; ++k) {
            const int line = a0 + span * k / kScanLines;
            if (line <= 0 || line >= extent) continue;
            if (!lineRuns(image, vertical, line, runs)) continue;
            if (runs.size() > 4096) continue;              // 노이즈 선

            // 2of5는 정방향/역방향 둘 다 본다. 역방향 좌표는 선 길이에서 되돌린다.
            if (opt_.industrial2of5 || opt_.coop2of5) {
                const int lineLen = vertical ? image.height : image.width;
                reversedRuns(runs, rev);
                for (int dir = 0; dir < 2; ++dir) {
                    const std::vector<Run>& rr = dir == 0 ? runs : rev;
                    for (size_t i = 0; i < rr.size(); ++i) {
                        if (!rr[i].bar) continue;
                        for (int which = 0; which < 2; ++which) {
                            const bool on = which == 0 ? opt_.industrial2of5 : opt_.coop2of5;
                            if (!on) continue;
                            const Code25Spec& spec = which == 0 ? kIndustrial : kCoop;
                            std::string t; size_t end = 0;
                            if (!decode25At(rr, i, spec, opt_.min2of5Digits, t, end)) continue;
                            int from = rr[i].start;
                            int to = rr[end - 1].start + rr[end - 1].len;
                            if (dir == 1) { const int f = lineLen - to, g = lineLen - from; from = f; to = g; }
                            int x0, y0, x1, y1;
                            box(from, to, line, x0, y0, x1, y1);
                            addHit(acc, vertical, spec.sym, t, x0, y0, x1, y1);
                        }
                    }
                }
            }

            /*
             * Pharmacode는 시작/정지 패턴이 없어서 "어디부터 어디까지"를
             * 스스로 못 정한다. 그래서 정지대로 끊는다 — 밝은 런이 충분히
             * 길면 그 사이를 한 덩어리로 본다. 덩어리 안이 막대-공백 교대가
             * 아니면 어차피 탈락한다.
             */
            if (opt_.pharmacode && !vertical) {
                size_t i = 0;
                while (i < runs.size()) {
                    while (i < runs.size() && !runs[i].bar) ++i;
                    if (i >= runs.size()) break;
                    size_t j = i;
                    int barSum = 0, barCnt = 0;
                    while (j < runs.size()) {
                        if (runs[j].bar) { barSum += runs[j].len; barCnt++; j++; continue; }
                        // 공백이 지금까지 본 평균 막대 폭의 4배를 넘으면 덩어리 끝
                        const double avgBar = barCnt ? static_cast<double>(barSum) / barCnt : 1.0;
                        if (runs[j].len > avgBar * 4.0) break;
                        if (j + 1 >= runs.size() || !runs[j + 1].bar) break;
                        j++;
                    }
                    std::string t;
                    if (decodePharmacode(runs, i, j, opt_.minPharmacodeBars, t)) {
                        int x0, y0, x1, y1;
                        box(runs[i].start, runs[j - 1].start + runs[j - 1].len, line, x0, y0, x1, y1);
                        addHit(acc, vertical, Symbology::PHARMACODE, t, x0, y0, x1, y1);
                    }
                    i = j;
                }
            }
        }
    }

    /*
     * [겹치면 2of5가 이긴다] Industrial 2of5의 막대열은 그 자체로 유효한
     * Pharmacode다(위 주석). 같은 자리에서 둘 다 나오면 시작/정지 패턴으로
     * 증명된 2of5를 남긴다 — Pharmacode 쪽은 증명할 것이 없다.
     */
    std::vector<const Hit*> proven;
    for (const Hit& h : acc)
        if (h.count >= opt_.minLineCount && h.sym != Symbology::PHARMACODE)
            proven.push_back(&h);
    auto overlaps = [](const Hit& a, const Hit& b) {
        return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
    };

    for (Hit& h : acc) {
        if (h.count < opt_.minLineCount) continue;
        if (h.sym == Symbology::PHARMACODE) {
            bool shadowed = false;
            for (const Hit* p : proven) if (overlaps(h, *p)) { shadowed = true; break; }
            if (shadowed) continue;
        }
        DecodedSymbol s;
        s.symbology = h.sym;
        s.text = h.text;
        s.rawBytes.assign(h.text.begin(), h.text.end());
        const int x0 = std::max(0, h.x0), x1 = std::min(image.width - 1, h.x1);
        const int y0 = std::max(0, h.y0), y1 = std::min(image.height - 1, h.y1);
        s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
        results.push_back(std::move(s));
    }
    return results;
}

} // namespace vscan
