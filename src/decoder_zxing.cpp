#include "vscan_internal/decoder_zxing.hpp"

// 이 버전(zxing-cpp v2.2.1)은 헤더가 "ZXing/" 서브폴더 없이 core/src 루트에
// 바로 있다 (include dir이 core/src로 잡혀 있음). 결과 타입은 Barcode가 아니라
// Result(복수형 Results = std::vector<Result>)이다.
#include "ReadBarcode.h"

namespace vscan {

namespace {

Symbology mapFormat(ZXing::BarcodeFormat fmt, bool isGS1Hint) {
    using ZXing::BarcodeFormat;
    switch (fmt) {
        case BarcodeFormat::QRCode:        return Symbology::QR;
        case BarcodeFormat::MicroQRCode:   return Symbology::MICRO_QR;
        case BarcodeFormat::DataMatrix:    return isGS1Hint ? Symbology::GS1_DATA_MATRIX
                                                              : Symbology::DATA_MATRIX;
        case BarcodeFormat::PDF417:        return Symbology::PDF417;
        // 참고: 이 zxing-cpp 버전(v2.2.1)에는 MicroPDF417 별도 포맷이 없다.
        // Symbology::MICRO_PDF417은 항상 미검출로 남는다 — 필요하면 커스텀
        // 디코더로 채워야 함.
        case BarcodeFormat::Code39:        return Symbology::CODE39;
        case BarcodeFormat::Code93:        return Symbology::CODE93;
        case BarcodeFormat::Code128:       return isGS1Hint ? Symbology::GS1_128
                                                              : Symbology::CODE128;
        case BarcodeFormat::ITF:           return Symbology::ITF;
        case BarcodeFormat::Codabar:       return Symbology::CODABAR;
        case BarcodeFormat::DataBar:
        case BarcodeFormat::DataBarExpanded:
                                            return Symbology::GS1_DATABAR;
        case BarcodeFormat::EAN13:
        case BarcodeFormat::EAN8:
        case BarcodeFormat::UPCA:
        case BarcodeFormat::UPCE:
                                            return Symbology::EAN_UPC;
        default:                           return Symbology::UNKNOWN;
    }
}

// vscan.h의 VSCAN_FMT_* 비트를 zxing-cpp BarcodeFormat으로 매핑.
// formatMask==0(전체)이면 이 함수는 호출하지 않고 아래에서 전체 포맷을 쓴다.
ZXing::BarcodeFormats maskToFormats(uint32_t mask) {
    using ZXing::BarcodeFormat;
    ZXing::BarcodeFormats out;
    if (mask & (1u << 0))  out |= BarcodeFormat::QRCode;
    if (mask & (1u << 1))  out |= BarcodeFormat::MicroQRCode;
    if (mask & (1u << 2))  out |= BarcodeFormat::DataMatrix;
    if (mask & (1u << 3))  out |= BarcodeFormat::PDF417;
    if (mask & (1u << 4))  out |= BarcodeFormat::Code39;
    if (mask & (1u << 5))  out |= BarcodeFormat::Code93;
    if (mask & (1u << 6))  out |= BarcodeFormat::Code128;
    if (mask & (1u << 7))  out |= BarcodeFormat::ITF;
    if (mask & (1u << 8))  out |= BarcodeFormat::Codabar;
    if (mask & (1u << 9))  out |= BarcodeFormat::DataBar;
    if (mask & (1u << 10)) out |= BarcodeFormat::DataBarExpanded;
    if (mask & (1u << 11)) out |= BarcodeFormat::EAN13;
    if (mask & (1u << 12)) out |= BarcodeFormat::EAN8;
    if (mask & (1u << 13)) out |= BarcodeFormat::UPCA;
    if (mask & (1u << 14)) out |= BarcodeFormat::UPCE;
    return out;
}

} // namespace

std::vector<DecodedSymbol> ZXingDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (image.empty()) return results;

    // rowStride를 명시해서 상위 프레임의 부분 뷰(타일)도 복사 없이 그대로 넘긴다.
    ZXing::ImageView view(image.pixels, image.width, image.height,
                           ZXing::ImageFormat::Lum, image.stride);

    ZXing::ReaderOptions options;
    options.setTryHarder(tryHarder_);
    options.setTryRotate(tryRotate_);
    options.setTryInvert(tryInvert_);
    // 내장 다운스케일 패스. 노이즈/저대비 코드의 안전망이라 기본 ON —
    // 끄면 그런 이미지에서 검출 자체를 잃는다. [[vscan-lite-zxing-opts-bench]]
    options.setTryDownscale(tryDownscale_);
    // zxing 기본값 500은 "min(width,height)>=500일 때만 다운스케일 패스를
    // 돈다"는 뜻이라, 2단계 디코드가 잘라낸 작은 crop(예: 500x500)은 이
    // 구제 패스를 아예 못 받는다 — 실측: 노이즈/저대비 crop이 임계값 500에선
    // n=0, 400 이하로 낮추면 n=1로 살아나고 시간 비용은 거의 0
    // (15.58ms -> 15.66ms). 그래서 기본을 100으로 낮춰둔다.
    // 풀프레임(1536px)에는 500이든 100이든 영향 없다.
    // [[vscan-lite-crop-downscale-threshold]]
    options.setDownscaleThreshold(downscaleThreshold_);
    // CODE39 Full ASCII (벤치마킹 대상 스펙 포함 항목). zxing 기본은 OFF.
    options.setTryCode39ExtendedMode(tryCode39ExtendedMode_);
    // [부분 디코딩 대책] 헤더의 minLineCount_ 주석 참고.
    options.setMinLineCount(static_cast<uint8_t>(minLineCount_));
    options.setValidateITFCheckSum(validateITFCheckSum_);
    // 주의: setMaxNumberOfSymbols()는 절대 쓰지 말 것. 조기 종료로 빨라지지만
    // 프레임 안의 코드를 개수 제한만큼만 반환해서 다중 코드 동시 판독
    // (벤치마킹 대상 리더기의 핵심 기능)을 깨뜨린다 — 실측: 6개 있는 이미지에서 2로
    // 제한하면 2개만 반환. [[vscan-lite-zxing-opts-bench]]
    switch (binarizer_) {
        case Binarizer::LocalAverage:    options.setBinarizer(ZXing::Binarizer::LocalAverage); break;
        case Binarizer::GlobalHistogram: options.setBinarizer(ZXing::Binarizer::GlobalHistogram); break;
        case Binarizer::FixedThreshold:  options.setBinarizer(ZXing::Binarizer::FixedThreshold); break;
        case Binarizer::BoolCast:        options.setBinarizer(ZXing::Binarizer::BoolCast); break;
    }
    if (formatMask_ == 0) {
        options.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode |
                            ZXing::BarcodeFormat::DataMatrix | ZXing::BarcodeFormat::PDF417 |
                            ZXing::BarcodeFormat::Code39 | ZXing::BarcodeFormat::Code93 |
                            ZXing::BarcodeFormat::Code128 | ZXing::BarcodeFormat::ITF |
                            ZXing::BarcodeFormat::Codabar | ZXing::BarcodeFormat::DataBar |
                            ZXing::BarcodeFormat::DataBarExpanded |
                            ZXing::BarcodeFormat::EAN13 | ZXing::BarcodeFormat::EAN8 |
                            ZXing::BarcodeFormat::UPCA | ZXing::BarcodeFormat::UPCE);
    } else {
        options.setFormats(maskToFormats(formatMask_));
    }
    // 다중 심볼: ReadBarcodes()는 기본적으로 프레임 안의 모든 심볼을 찾아
    // std::vector<Result>로 돌려준다.

    ZXing::Results barcodes = ZXing::ReadBarcodes(view, options);

    for (const auto& b : barcodes) {
        if (!b.isValid()) continue;

        DecodedSymbol sym;
        bool gs1Hint = b.symbologyIdentifier().find("]e0") != std::string::npos ||
                       b.contentType() == ZXing::ContentType::GS1;
        sym.symbology = mapFormat(b.format(), gs1Hint);
        sym.text = b.text();
        const auto& bytes = b.bytes();
        sym.rawBytes.assign(bytes.begin(), bytes.end());
        sym.isGS1 = gs1Hint;

        const auto& pos = b.position();
        sym.position = {{
            {pos.topLeft().x, pos.topLeft().y},
            {pos.topRight().x, pos.topRight().y},
            {pos.bottomRight().x, pos.bottomRight().y},
            {pos.bottomLeft().x, pos.bottomLeft().y},
        }};

        results.push_back(std::move(sym));
    }

    return results;
}

} // namespace vscan
