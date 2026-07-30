#pragma once
#include <map>
#include <string>
#include "vscan_internal/decoder.hpp"

namespace vscan {

// GS1-128, GS1 DataBar, GS1 DataMatrix 등에서 나온 페이로드를
// AI(Application Identifier) -> 값 맵으로 분해한다.
// 예: "0101234567890128" -> {"01": "01234567890128"}
// 가변 길이 필드는 FNC1(GS, 0x1D)로 구분된다고 가정한다.
struct GS1Result {
    std::map<std::string, std::string> fields; // AI -> value
    bool parsedOk = false;
};

GS1Result parseGS1(const DecodedSymbol& symbol);

} // namespace vscan
