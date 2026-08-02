#include "vscan_internal/decoder_postal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace vscan {
namespace {

/* ---------------------------------------------------------------------------
 * 일본우편 고객 바코드 표
 * BWIPP의 japanpost 인코더를 ghostscript로 덤프해서 얻었다.
 * 각 심볼은 막대 3개, 각 막대는 0=tracker 1=descender 2=ascender 3=full.
 * ------------------------------------------------------------------------- */
const char* kJpEnc[19] = {
    "300", "330", "312", "132", "321", "303", "123", "231", "213", "033",  // 0~9
    "030",                                                                  // 10 = '-'
    "120", "102", "210",                                                    // 11~13 = 제어(A~J/K~T/U~Z)
    "012", "201", "021", "003", "333",                                      // 14~18 (14는 채움)
};
const char* kJpStart = "31";
const char* kJpStop  = "13";
constexpr int kJpSymbols = 21;                 // 데이터 20 + 검사 1
constexpr int kJpBars = 2 + kJpSymbols * 3 + 2; // 67

struct Bar {
    int x0, x1;     // 가로 범위
    int top, bot;   // 세로 범위 (이미지 좌표, 아래로 증가)
};

/*
 * ROI에서 막대를 찾는다. 열 단위로 어두운 화소가 있는지 보고, 이어진 열을
 * 한 막대로 묶는다. 4-state는 막대 폭이 전부 같아서 폭으로는 정보가 없다 —
 * 필요한 것은 각 막대의 위/아래 끝이다.
 */
bool findBars(const GrayView& img, bool rotated, std::vector<Bar>& out) {
    out.clear();
    int lo = 255, hi = 0;
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels + static_cast<size_t>(y) * img.stride;
        for (int x = 0; x < img.width; ++x) {
            lo = std::min(lo, static_cast<int>(row[x]));
            hi = std::max(hi, static_cast<int>(row[x]));
        }
    }
    if (hi - lo < 40) return false;
    const int thr = (lo + hi) / 2;

    /*
     * [90도로 세운 것도 읽는다] rotated면 x와 y의 역할을 바꾼다 — 막대가
     * 가로로 눕고 위아래로 쌓인 모양이다. 4-state는 정보가 높이에 있으므로
     * 축을 바꾸면 "높이"가 "가로 폭"이 된다.
     */
    const int nAlong = rotated ? img.height : img.width;   // 막대가 늘어선 방향
    const int nCross = rotated ? img.width : img.height;   // 막대의 길이 방향
    auto dark = [&](int a, int c) {
        const int x = rotated ? c : a, y = rotated ? a : c;
        return img.pixels[static_cast<size_t>(y) * img.stride + x] < thr;
    };

    std::vector<int> aTop(nAlong, -1), aBot(nAlong, -1);
    for (int a = 0; a < nAlong; ++a) {
        for (int c = 0; c < nCross; ++c) {
            if (!dark(a, c)) continue;
            if (aTop[a] < 0) aTop[a] = c;
            aBot[a] = c;
        }
    }

    int a = 0;
    while (a < nAlong) {
        if (aTop[a] < 0) { ++a; continue; }
        const int start = a;
        int t = aTop[a], b = aBot[a];
        while (a < nAlong && aTop[a] >= 0) {
            t = std::min(t, aTop[a]);
            b = std::max(b, aBot[a]);
            ++a;
        }
        out.push_back({start, a - 1, t, b});
        if (out.size() > 4096) return false;
    }
    return out.size() >= static_cast<size_t>(kJpBars);
}

/*
 * 막대들을 4-state로 분류한다.
 *
 * 전체 심볼의 위/아래 끝은 full 막대가 정한다. 그런데 **full 막대가 하나도
 * 없을 수는 없다** — 시작 패턴이 "31"이라 첫 막대가 full이다. 그래서
 * 관측된 최상단/최하단을 그대로 기준선으로 쓴다.
 *
 * 판정은 "위 끝에 닿는가 / 아래 끝에 닿는가"다. 경계는 전체 높이의 1/4로
 * 둔다 — tracker는 3/8~5/8 구간이라 여유가 크다.
 */
bool classify(const std::vector<Bar>& bars, size_t at, int count, std::string& out) {
    int top = 1 << 30, bot = -1;
    for (int i = 0; i < count; ++i) {
        top = std::min(top, bars[at + i].top);
        bot = std::max(bot, bars[at + i].bot);
    }
    const double h = bot - top;
    if (h < 4) return false;

    out.clear();
    for (int i = 0; i < count; ++i) {
        const bool hitTop = (bars[at + i].top - top) < h * 0.25;
        const bool hitBot = (bot - bars[at + i].bot) < h * 0.25;
        // 막대 높이도 검증한다 — tracker는 전체의 1/4, 나머지는 5/8 또는 1
        const double frac = (bars[at + i].bot - bars[at + i].top) / h;
        if (hitTop && hitBot) {
            if (frac < 0.8) return false;
            out.push_back('3');
        } else if (hitTop) {
            if (frac < 0.4 || frac > 0.85) return false;
            out.push_back('2');
        } else if (hitBot) {
            if (frac < 0.4 || frac > 0.85) return false;
            out.push_back('1');
        } else {
            if (frac > 0.55) return false;
            out.push_back('0');
        }
    }
    return true;
}

int jpSymbol(const std::string& three) {
    for (int i = 0; i < 19; ++i)
        if (three == kJpEnc[i]) return i;
    return -1;
}

// 막대 간격이 고르게 이어지는지 본다. 우편 바코드는 등간격이다.
bool evenlySpaced(const std::vector<Bar>& bars, size_t at, int count) {
    if (count < 3) return false;
    std::vector<int> gaps;
    for (int i = 0; i + 1 < count; ++i) gaps.push_back(bars[at + i + 1].x0 - bars[at + i].x0);
    std::sort(gaps.begin(), gaps.end());
    const int med = gaps[gaps.size() / 2];
    if (med < 2) return false;
    for (int g : gaps)
        if (std::abs(g - med) > std::max(2, med / 3)) return false;
    return true;
}

bool decodeJapanPattern(const std::string& pat, std::string& out);

/*
 * [평면 대칭 넷을 다 본다]
 * 심볼이 180도 돌면 막대 순서가 뒤집히고 ascender/descender도 서로 바뀐다.
 * 90도로 세운 것을 축을 바꿔 읽을 때는 어느 쪽으로 돌았느냐에 따라
 * **순서는 맞는데 극성만 뒤집히는** 경우가 생긴다 — 실측으로 90도에서
 * 시작/정지("31"..."13")는 맞는데 심볼이 하나도 표에 없었다.
 * 그래서 항등 / 극성만 / 순서만 / 둘 다, 넷을 다 시도한다.
 * 잘못된 변환은 시작·정지 패턴과 검사 심볼(mod 19)에서 걸린다.
 */
std::string swapAD(const std::string& p) {
    std::string r = p;
    for (char& c : r) { if (c == '1') c = '2'; else if (c == '2') c = '1'; }
    return r;
}
std::string reversed(const std::string& p) { return std::string(p.rbegin(), p.rend()); }

bool decodeJapanPost(const std::vector<Bar>& bars, size_t at, std::string& out) {
    if (at + kJpBars > bars.size()) return false;
    if (!evenlySpaced(bars, at, kJpBars)) return false;

    std::string raw;
    if (!classify(bars, at, kJpBars, raw)) return false;
    if (decodeJapanPattern(raw, out)) return true;
    if (decodeJapanPattern(swapAD(raw), out)) return true;
    if (decodeJapanPattern(reversed(raw), out)) return true;
    return decodeJapanPattern(reversed(swapAD(raw)), out);
}

bool decodeJapanPattern(const std::string& pat, std::string& out) {
    if (pat.compare(0, 2, kJpStart) != 0) return false;
    if (pat.compare(kJpBars - 2, 2, kJpStop) != 0) return false;

    std::vector<int> sym;
    for (int i = 0; i < kJpSymbols; ++i) {
        const int v = jpSymbol(pat.substr(2 + i * 3, 3));
        if (v < 0) return false;
        sym.push_back(v);
    }

    // 검사 심볼: (데이터 합 + 검사) mod 19 == 0
    int sum = 0;
    for (int i = 0; i < kJpSymbols - 1; ++i) sum += sym[i];
    if ((sum + sym[kJpSymbols - 1]) % 19 != 0) return false;

    std::string text;
    for (int i = 0; i < kJpSymbols - 1; ++i) {
        const int v = sym[i];
        if (v <= 9) { text.push_back(static_cast<char>('0' + v)); continue; }
        if (v == 10) { text.push_back('-'); continue; }
        if (v == 14) break;                       // 채움 — 여기서 데이터 끝
        if (v >= 11 && v <= 13) {                 // 제어 + 숫자 = 영문자
            if (i + 1 >= kJpSymbols - 1) return false;
            const int d = sym[++i];
            if (d > 9) return false;
            const char base = (v == 11) ? 'A' : (v == 12) ? 'K' : 'U';
            const char ch = static_cast<char>(base + d);
            if (ch > 'Z') return false;
            text.push_back(ch);
            continue;
        }
        return false;                             // 15~18은 예약
    }
    if (text.empty()) return false;
    out = text;
    return true;
}

} // namespace

std::vector<DecodedSymbol> PostalDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (!opt_.japanPost) return results;
    if (image.empty() || std::max(image.width, image.height) < kJpBars * 2) return results;

    for (int axis = 0; axis < 2; ++axis) {
        std::vector<Bar> bars;
        if (!findBars(image, axis == 1, bars)) continue;
        for (size_t i = 0; i + kJpBars <= bars.size(); ++i) {
            std::string text;
            if (!decodeJapanPost(bars, i, text)) continue;
            DecodedSymbol s;
            s.symbology = Symbology::POSTAL_JAPAN;
            s.text = text;
            s.rawBytes.assign(text.begin(), text.end());
            int top = 1 << 30, bot = -1;
            for (int k = 0; k < kJpBars; ++k) {
                top = std::min(top, bars[i + k].top);
                bot = std::max(bot, bars[i + k].bot);
            }
            const int a0 = bars[i].x0, a1 = bars[i + kJpBars - 1].x1;
            const int x0 = axis ? top : a0, x1 = axis ? bot : a1;
            const int y0 = axis ? a0 : top, y1 = axis ? a1 : bot;
            s.position = {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
            results.push_back(std::move(s));
            i += kJpBars - 1;
        }
        if (!results.empty()) break;
    }
    return results;
}

} // namespace vscan
