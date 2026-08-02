#include "vscan_internal/gs1_composite.hpp"

#include <algorithm>
#include <cstdio>

namespace vscan {
namespace {

constexpr char kFNC1 = 0x1D;

// 비트열에서 n비트를 읽어 정수로. 범위를 넘으면 false.
bool take(const std::vector<uint8_t>& b, size_t& i, int n, int& out) {
    if (i + static_cast<size_t>(n) > b.size()) return false;
    int v = 0;
    for (int k = 0; k < n; ++k) v = (v << 1) | (b[i + k] & 1);
    i += n;
    out = v;
    return true;
}

// 소비하지 않고 앞의 n비트를 본다.
bool peek(const std::vector<uint8_t>& b, size_t i, int n, int& out) {
    return take(b, i, n, out);
}

enum class Mode { Numeric, Alphanumeric, Iso646 };

/*
 * 범용 필드(gpf)를 푼다. 패딩에 닿으면 조용히 끝난다 —
 * 패딩은 "00100" 반복이라 alphanumeric/iso646에서 서로를 가리키는 래치로만
 * 읽히고 글자를 만들지 않는다. 그게 이 인코딩의 설계다.
 */
bool decodeGpf(const std::vector<uint8_t>& b, size_t& i, Mode mode, std::string& out) {
    while (true) {
        if (mode == Mode::Numeric) {
            int v;
            if (peek(b, i, 4, v) && v == 0) { i += 4; mode = Mode::Alphanumeric; continue; }
            if (!take(b, i, 7, v)) break;
            v -= 8;
            if (v < 0 || v > 119) return false;
            const int hi = v / 11, lo = v % 11;
            for (int d : {hi, lo}) out.push_back(d == 10 ? kFNC1 : static_cast<char>('0' + d));
            continue;
        }

        // alphanumeric과 iso646은 앞부분이 같다 — 숫자 5비트, FNC1 01111,
        // lnumeric 000. 다른 것은 '1'로 시작하는 자리와 00100의 뜻뿐이다.
        int v;
        if (peek(b, i, 3, v) && v == 0) { i += 3; mode = Mode::Numeric; continue; }
        if (i >= b.size()) break;
        if (b[i] == 0) {
            if (!take(b, i, 5, v)) break;
            if (v == 4) {
                mode = (mode == Mode::Alphanumeric) ? Mode::Iso646 : Mode::Alphanumeric;
            } else if (v >= 5 && v <= 14) {
                out.push_back(static_cast<char>(v + 43));
            } else if (v == 15) {
                out.push_back(kFNC1);
                mode = Mode::Numeric;       // FNC1은 numeric으로 되돌린다
            } else {
                return false;
            }
            continue;
        }

        if (mode == Mode::Alphanumeric) {
            if (!take(b, i, 6, v)) break;
            if (v >= 32 && v <= 57) out.push_back(static_cast<char>(v + 33));
            else if (v == 58)       out.push_back('*');
            else if (v >= 59 && v <= 62) out.push_back(static_cast<char>(v - 15));
            else return false;
            continue;
        }

        // iso646: 7비트(영문자)와 8비트(기호)가 섞인다.
        if (!peek(b, i, 7, v)) break;
        if (v >= 64 && v <= 89)  { i += 7; out.push_back(static_cast<char>(v + 1)); continue; }
        if (v >= 90 && v <= 115) { i += 7; out.push_back(static_cast<char>(v + 7)); continue; }
        int w;
        if (!take(b, i, 8, w)) break;
        if (w == 232)                 out.push_back('!');
        else if (w == 233)            out.push_back('"');
        else if (w >= 234 && w <= 244) out.push_back(static_cast<char>(w - 197));
        else if (w >= 245 && w <= 250) out.push_back(static_cast<char>(w - 187));
        else if (w == 251)            out.push_back('_');
        else if (w == 252)            out.push_back(' ');
        else return false;
    }
    return true;
}

// alpha 모드(방식 11 전용): A-Z 5비트, 0-9 6비트, FNC1 11111으로 끝난다.
bool decodeAlpha(const std::vector<uint8_t>& b, size_t& i, std::string& out) {
    while (true) {
        int v5;
        if (!peek(b, i, 5, v5)) break;
        if (v5 <= 25) { i += 5; out.push_back(static_cast<char>('A' + v5)); continue; }
        if (v5 == 31) { i += 5; out.push_back(kFNC1); break; }   // alpha 구간 끝
        int v6;
        if (!peek(b, i, 6, v6)) break;
        if (v6 >= 52 && v6 <= 61) { i += 6; out.push_back(static_cast<char>(v6 - 4)); continue; }
        break;
    }
    return true;
}

const char* kAlphaTail = "BDHIJKLNPQRSTVXZ";

void stripTrailingFnc1(std::string& s) {
    while (!s.empty() && s.back() == kFNC1) s.pop_back();
}

} // namespace

std::vector<uint8_t> gs1CompositeBitsFromCCA(const std::vector<int>& cws) {
    std::vector<uint8_t> bits;
    size_t c = 0;
    while (c < cws.size()) {
        const int csl = static_cast<int>(std::min<size_t>(7, cws.size() - c));
        const int bsl = (csl == 7) ? 69 : csl * 10 - 1;
        /*
         * 69비트는 64비트에 안 들어간다. __int128을 쓴다 — 이 프로젝트가
         * 쓰는 컴파일러(x86 gcc, aarch64 크로스 gcc) 둘 다 지원한다.
         * 928^7이 2^69보다 아주 조금 크므로, 값이 69비트를 넘으면 그건
         * 손상된 코드워드다(아래에서 거른다).
         */
        unsigned __int128 n = 0;
        for (int j = 0; j < csl; ++j) {
            if (cws[c + j] < 0 || cws[c + j] > 928) return {};
            n = n * 928 + static_cast<unsigned>(cws[c + j]);
        }
        if (bsl < 127 && (n >> bsl) != 0) return {};
        for (int k = bsl - 1; k >= 0; --k)
            bits.push_back(static_cast<uint8_t>((n >> k) & 1));
        c += csl;
    }
    return bits;
}

std::vector<uint8_t> gs1CompositeBitsFromByteCompaction(const std::vector<int>& cws) {
    // [920] [901] [바이트 압축 코드워드...]
    if (cws.size() < 3 || cws[0] != 920 || (cws[1] != 901 && cws[1] != 924)) return {};
    std::vector<int> body(cws.begin() + 2, cws.end());
    std::vector<uint8_t> bytes;
    size_t i = 0;
    while (i + 5 <= body.size()) {
        unsigned long long n = 0;
        for (int j = 0; j < 5; ++j) {
            if (body[i + j] < 0 || body[i + j] > 899) return {};
            n = n * 900 + static_cast<unsigned>(body[i + j]);
        }
        for (int k = 5; k >= 0; --k) bytes.push_back(static_cast<uint8_t>((n >> (8 * k)) & 0xff));
        i += 5;
    }
    for (; i < body.size(); ++i) bytes.push_back(static_cast<uint8_t>(body[i] & 0xff));

    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t byte : bytes)
        for (int k = 7; k >= 0; --k) bits.push_back(static_cast<uint8_t>((byte >> k) & 1));
    return bits;
}

bool decodeGs1CompositeBits(const std::vector<uint8_t>& b, std::string& out) {
    out.clear();
    if (b.size() < 8) return false;
    size_t i = 0;

    if (b[0] == 0) {                        // 방식 0
        i = 1;
        std::string text;
        if (!decodeGpf(b, i, Mode::Numeric, text)) return false;
        stripTrailingFnc1(text);
        if (text.empty()) return false;
        out = text;
        return true;
    }

    int two;
    if (!peek(b, 0, 2, two)) return false;

    if (two == 2) {                         // "10" — 첫 AI가 10 / 11 / 17
        i = 2;
        std::string head;
        int t2;
        if (peek(b, i, 2, t2) && t2 == 3) {
            i += 2;                         // 날짜 없음. 첫 AI가 10이다.
        } else {
            int v, flag;
            if (!take(b, i, 16, v)) return false;
            if (!take(b, i, 1, flag)) return false;
            const int yy = v / 384, rem = v % 384;
            const int mm = rem / 32 + 1, dd = rem % 32;
            if (yy > 99 || mm > 12 || dd > 31) return false;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s%02d%02d%02d", flag ? "17" : "11", yy, mm, dd);
            head = buf;
        }
        std::string text;
        if (!decodeGpf(b, i, Mode::Numeric, text)) return false;
        // gpf가 FNC1로 시작하면 그 뒤는 평범한 AI 데이터다. 아니면 첫 필드가
        // **AI 10의 값**이고 AI 번호는 비트열에 없다 — 넣어줘야 한다.
        if (!text.empty() && text[0] == kFNC1) text.erase(0, 1);
        else if (!text.empty()) text.insert(0, "10");
        out = head + text;
        stripTrailingFnc1(out);
        return !out.empty();
    }

    if (two == 3) {                         // "11" — 첫 AI가 90
        i = 2;
        // 모드: 0 = alphanumeric, 10 = numeric, 11 = alpha
        Mode mode = Mode::Alphanumeric;
        bool alphaMode = false;
        {
            int bit;
            if (!take(b, i, 1, bit)) return false;
            if (bit != 0) {
                if (!take(b, i, 1, bit)) return false;
                if (bit == 0) mode = Mode::Numeric;
                else          alphaMode = true;
            }
        }
        // 두 번째 AI 표시: 0 = 없음, 10 = AI 21, 11 = AI 8004
        std::string second;
        {
            int bit;
            if (!take(b, i, 1, bit)) return false;
            if (bit != 0) {
                if (!take(b, i, 1, bit)) return false;
                second = (bit == 0) ? "21" : "8004";
            }
        }
        int nval = 0;
        char letter = 0;
        int probe;
        if (peek(b, i, 5, probe) && probe == 31) {
            i += 5;
            int a;
            if (!take(b, i, 10, nval)) return false;
            if (!take(b, i, 5, a)) return false;
            if (a > 25) return false;
            letter = static_cast<char>('A' + a);
        } else {
            int a;
            if (!take(b, i, 5, nval)) return false;
            if (!take(b, i, 4, a)) return false;
            letter = kAlphaTail[a];
        }
        std::string head = "90";
        if (nval > 0) head += std::to_string(nval);
        head.push_back(letter);

        std::string text;
        if (alphaMode) {
            if (!decodeAlpha(b, i, text)) return false;
            std::string tail;
            if (!decodeGpf(b, i, Mode::Numeric, tail)) return false;
            text += tail;
        } else {
            if (!decodeGpf(b, i, mode, text)) return false;
        }
        // AI 21 / 8004의 번호는 비트열에 없다 — 첫 FNC1 뒤에 넣어준다.
        if (!second.empty()) {
            const size_t g = text.find(kFNC1);
            if (g != std::string::npos) text.insert(g + 1, second);
        }
        out = head + text;
        stripTrailingFnc1(out);
        return !out.empty();
    }

    return false;
}

} // namespace vscan
