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
                           int minLineCount = 4, bool validateITFCheckSum = false,
                           uint16_t downscaleThreshold = 100)
        : formatMask_(formatMask), tryRotate_(tryRotate), tryInvert_(tryInvert), tryHarder_(tryHarder),
          binarizer_(binarizer), tryDownscale_(tryDownscale),
          tryCode39ExtendedMode_(tryCode39ExtendedMode), minLineCount_(minLineCount),
          validateITFCheckSum_(validateITFCheckSum), downscaleThreshold_(downscaleThreshold) {}

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
    /*
     * [부분 디코딩 대책] 같은 결과가 나와야 하는 스캔 라인 최소 개수.
     * zxing 기본은 2인데, 그러면 1D 바코드의 **일부만** 훑은 스캔이
     * 유효한 짧은 코드로 통과한다. 실측(ITF 0~90도 19장): 정답
     * "12345670"과 함께 "345670"/"123456"이 같이 나오고, 25/65도에서는
     * **틀린 것만** 나왔다(오디코딩 11건). ITF는 체크디짓이 규격상
     * 필수가 아니라 부분 스캔이 그대로 유효해 보인다 — 산업 현장에서
     * 품번을 잘못 읽는 것이라 미검출보다 나쁘다.
     *
     * 4로 올리면 오디코딩 11 -> 2, 14종 각도 스윕이 전부 100%가 된다.
     * 화질 축(노이즈/블러/모듈크기/대비) 전 구간에서 검출 손실 0이고,
     * 난수 코퍼스에서만 -0.5%p, 시간 +2.5%다.
     */
    int minLineCount_;
    /*
     * [ITF 체크섬 강제] 기본 OFF.
     * 켜면 ITF 부분 디코딩이 **완전히** 사라진다(오디코딩 2 -> 0).
     * 대신 체크디짓 없는 ITF를 전부 거부한다 — ITF-14(물류 카톤 GTIN-14)는
     * 체크디짓이 필수라 그런 배치에서는 켜는 게 맞지만, 일반 ITF는
     * 체크디짓이 선택이라 기본으로 켤 수 없다.
     * 실측: 체크디짓 없는 난수 ITF가 섞인 코퍼스에서 검출 -4.1%p,
     * p50 46 -> 107ms(거부된 코드가 구제 체인을 끝까지 돈다).
     */
    bool validateITFCheckSum_;
    uint16_t downscaleThreshold_;
};

} // namespace vscan
