#pragma once
#include <array>
#include <string>
#include <vector>
#include "vscan_internal/frame.hpp"

namespace vscan {

// 벤치마킹 대상 스펙에 맞춘 심볼로지 열거형.
// zxing-cpp가 지원하지 않는 것은 자체 디코더가 채운다. 지금 남은 자리표시자는
// DOTCODE 하나뿐이다(§4 로드맵).
enum class Symbology {
    QR, MICRO_QR,
    DATA_MATRIX, GS1_DATA_MATRIX,
    PDF417, MICRO_PDF417,
    GS1_COMPOSITE,      // 자체 (CC-A/CC-B/CC-C, 기본 OFF)
    DOTCODE,            // TODO: 미구현
    CODE39, CODE39_FULL_ASCII, TRIOPTIC_CODE39,
    ITF, INDUSTRIAL_2OF5, COOP_2OF5,
    CODABAR,
    CODE128, GS1_128,
    GS1_DATABAR,
    CODE93,
    EAN_UPC,
    PHARMACODE,          // 자체 (기본 OFF)
    POSTAL_JAPAN,        // 자체 (기본 OFF)
    POSTAL_IMB,          // 자체 (기본 OFF)
    UNKNOWN,
};

// 코드 4개 꼭짓점 (이미지 좌표계, 픽셀 단위)
using Quad = std::array<std::pair<int,int>, 4>;

struct DecodedSymbol {
    Symbology symbology = Symbology::UNKNOWN;
    std::string text;               // 사람이 읽을 수 있는 형태로 디코딩된 문자열
    std::vector<uint8_t> rawBytes;  // 원본 바이트 (GS1 등 이진 페이로드 대비)
    Quad position{};
    bool isGS1 = false;              // GS1 AI 파싱 대상 여부
};

// 하나의 디코더는 GrayView(복사 없는 뷰) 전체를 받아 그 안의 모든 심볼
// (다중 코드 포함)을 찾아 반환한다. 소유권이 없으므로 decode() 호출이
// 끝나기 전까지 호출자가 버퍼 수명을 보장해야 한다.
class IDecoder {
public:
    virtual ~IDecoder() = default;
    virtual std::vector<DecodedSymbol> decode(const GrayView& image) = 0;
    virtual std::string name() const = 0;
    // 심볼로지 마스크를 런타임에 바꾼다(적응형 배치 프로파일용).
    // 마스크 개념이 없는 디코더는 무시하면 된다.
    virtual void setFormatMask(uint32_t) {}
};

} // namespace vscan
