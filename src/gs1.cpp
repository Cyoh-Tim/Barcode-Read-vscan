#include "vscan_internal/gs1.hpp"

namespace vscan {

namespace {

constexpr char kFNC1 = 0x1D; // GS separator, zxing-cpp가 보통 이 문자로 표기

// 자주 쓰이는 고정 길이 AI 테이블 (AI -> 값 길이, AI 코드 제외)
// 전체 GS1 General Specifications 표의 일부만 담았다. 필요시 계속 추가.
const std::map<std::string, int> kFixedLengthAI = {
    {"00", 18}, // SSCC
    {"01", 14}, // GTIN
    {"11", 6},  // 생산일
    {"13", 6},  // 포장일
    {"15", 6},  // 유통기한
    {"17", 6},  // 사용기한
    {"20", 2},  // 제품 변형
};

} // namespace

GS1Result parseGS1(const DecodedSymbol& symbol) {
    GS1Result result;
    const std::string& raw = symbol.text.empty()
        ? std::string(symbol.rawBytes.begin(), symbol.rawBytes.end())
        : symbol.text;

    size_t pos = 0;
    while (pos + 2 <= raw.size()) {
        std::string ai = raw.substr(pos, 2);
        // 2/3/4자리 AI가 섞여 있는 게 실제 규격이지만, 여기서는
        // 2자리 AI 위주로 단순화했다. 실제 사용시 AI 길이 판별 테이블 필요.
        pos += 2;

        auto it = kFixedLengthAI.find(ai);
        if (it != kFixedLengthAI.end()) {
            int len = it->second;
            if (pos + static_cast<size_t>(len) > raw.size()) break;
            result.fields[ai] = raw.substr(pos, len);
            pos += len;
        } else {
            // 가변 길이: 다음 FNC1까지 혹은 문자열 끝까지
            size_t gsPos = raw.find(kFNC1, pos);
            size_t end = (gsPos == std::string::npos) ? raw.size() : gsPos;
            result.fields[ai] = raw.substr(pos, end - pos);
            pos = (gsPos == std::string::npos) ? raw.size() : gsPos + 1;
        }
    }

    result.parsedOk = !result.fields.empty();
    return result;
}

} // namespace vscan
