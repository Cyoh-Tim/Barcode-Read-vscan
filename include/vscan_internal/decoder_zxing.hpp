#pragma once
#include <cstdint>
#include "vscan_internal/decoder.hpp"

namespace vscan {

// zxing-cpp의 ReadBarcodes()로 한 프레임 안의 여러 심볼을 한번에 찾는다.
// 지원 포맷: QR, MicroQR, DataMatrix, GS1 DataMatrix, PDF417, MicroPDF417,
//           Code39(+FullASCII), Code93, Code128, GS1-128, GS1 DataBar,
//           ITF, Codabar, EAN/UPC
// 미지원: GS1 Composite, DotCode, Pharmacode, Postal(Japan/IMB)
class ZXingDecoder : public IDecoder {
public:
    // zxing-cpp가 제공하는 4가지 이진화 방식.
    //   LocalAverage    - 픽셀별 주변 평균(기본). 가장 정확하지만 가장 비쌈
    //   GlobalHistogram - 줄당 히스토그램 valley. 2~3배 빠르지만 작은 코드
    //                     (90px 이하)/저대비 코드를 놓친다
    //   FixedThreshold  - T=127 고정. 조명 조건 바뀌면 깨짐
    //   BoolCast        - T=0, 최속이지만 실제 조명 그라디언트 이미지에서 검출 실패
    // 8종 테스트 이미지 실측은 [[vscan-lite-locate-matrix]] 참고.
    // 정밀 디코드에서는 LocalAverage 외 선택지가 사실상 없다.
    enum class Binarizer { LocalAverage, GlobalHistogram, FixedThreshold, BoolCast };

    // formatMask==0이면 전체 포맷(vscan.h VSCAN_FMT_* 비트 OR). 기본값은
    // 전체 포맷 + TryHarder/회전/반전 탐색 다 켜짐 — 가장 느리지만 가장 안전한 조합.
    explicit ZXingDecoder(uint32_t formatMask = 0, bool tryRotate = true, bool tryInvert = true,
                           bool tryHarder = true, Binarizer binarizer = Binarizer::LocalAverage,
                           bool tryDownscale = true, bool tryCode39ExtendedMode = true,
                           uint16_t downscaleThreshold = 100)
        : formatMask_(formatMask), tryRotate_(tryRotate), tryInvert_(tryInvert), tryHarder_(tryHarder),
          binarizer_(binarizer), tryDownscale_(tryDownscale),
          tryCode39ExtendedMode_(tryCode39ExtendedMode), downscaleThreshold_(downscaleThreshold) {}

    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    void setFormatMask(uint32_t mask) override { formatMask_ = mask; }
    std::string name() const override { return "zxing-cpp"; }

private:
    uint32_t formatMask_;
    bool tryRotate_;
    bool tryInvert_;
    bool tryHarder_;
    Binarizer binarizer_;
    bool tryDownscale_;
    bool tryCode39ExtendedMode_;
    uint16_t downscaleThreshold_;
};

} // namespace vscan
