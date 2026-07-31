#include "vscan_internal/decoder_zbar.hpp"

#include <algorithm>

#ifdef VSCAN_HAVE_ZBAR
#include <zbar.h>
#endif

namespace vscan {

#ifdef VSCAN_HAVE_ZBAR

namespace {

Symbology mapZBarType(zbar::zbar_symbol_type_t t) {
    using namespace zbar;
    switch (t) {
        case ZBAR_EAN13:
        case ZBAR_EAN8:
        case ZBAR_UPCA:
        case ZBAR_UPCE:       return Symbology::EAN_UPC;
        case ZBAR_CODE128:    return Symbology::CODE128;
        case ZBAR_CODE39:     return Symbology::CODE39;
        case ZBAR_CODE93:     return Symbology::CODE93;
        case ZBAR_CODABAR:    return Symbology::CODABAR;
        case ZBAR_I25:        return Symbology::ITF;
        case ZBAR_QRCODE:     return Symbology::QR; // 참고용, 기본은 zxing이 처리
        default:              return Symbology::UNKNOWN;
    }
}

} // namespace

std::vector<DecodedSymbol> ZBarDecoder::decode(const GrayView& image) {
    std::vector<DecodedSymbol> results;
    if (image.empty()) return results;

    /*
     * [심볼로지 선별] 전부 켜면 안 된다.
     *
     * ZBar를 켜는 이유는 **저대비**에서 zxing보다 강해서다. 실측(모듈 6px,
     * 대비 축 0.05~1.0 스윕):
     *   Code128  90% -> 100%      EAN13   90% -> 100%
     *   EAN8     85% -> 100%      UPC-A   90% -> 100%
     *   Code39   85% ->  90%
     * zxing이 대비 0.15에서 끊기는데 ZBar는 0.05까지 읽는다.
     *
     * 그런데 전부 켜면 DataBar에서 **오디코딩 19~20건**이 쏟아진다
     * (DataBar 90% -> 75%, DataBar Expanded 90% -> 75%). ZBar가 DataBar의
     * 막대 일부를 EAN/UPC나 I2/5로 잘못 읽는 것으로 보인다. Codabar도
     * 3건 나온다. 산업 현장에서 오디코딩은 미검출보다 나쁘므로(§3.18)
     * 이득이 확인된 것만 켠다.
     *
     * 2D(QR)도 끈다 — zxing이 담당하고, 스캔 범위를 줄여 비용도 아낀다.
     * [[vscan-lite-zbar-selective]]
     */
    zbar::ImageScanner scanner;
    scanner.set_config(zbar::ZBAR_NONE, zbar::ZBAR_CFG_ENABLE, 0);   // 전부 끄고 시작
    scanner.set_config(zbar::ZBAR_EAN13,   zbar::ZBAR_CFG_ENABLE, 1);
    scanner.set_config(zbar::ZBAR_EAN8,    zbar::ZBAR_CFG_ENABLE, 1);
    scanner.set_config(zbar::ZBAR_UPCA,    zbar::ZBAR_CFG_ENABLE, 1);
    scanner.set_config(zbar::ZBAR_UPCE,    zbar::ZBAR_CFG_ENABLE, 1);
    scanner.set_config(zbar::ZBAR_CODE128, zbar::ZBAR_CFG_ENABLE, 1);
    scanner.set_config(zbar::ZBAR_CODE39,  zbar::ZBAR_CFG_ENABLE, 1);

    zbar::Image zimg(image.width, image.height, "Y800",
                      image.pixels, static_cast<size_t>(image.width) * image.height);

    if (scanner.scan(zimg) <= 0) return results;

    for (auto it = zimg.symbol_begin(); it != zimg.symbol_end(); ++it) {
        DecodedSymbol sym;
        sym.symbology = mapZBarType(it->get_type());
        sym.text = it->get_data();

        // ZBar는 폴리곤 점 개수가 코드마다 다를 수 있어 bounding box로 근사
        int minX = image.width, minY = image.height, maxX = 0, maxY = 0;
        for (int i = 0; i < it->get_location_size(); ++i) {
            minX = std::min(minX, it->get_location_x(i));
            minY = std::min(minY, it->get_location_y(i));
            maxX = std::max(maxX, it->get_location_x(i));
            maxY = std::max(maxY, it->get_location_y(i));
        }
        sym.position = {{{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}}};

        results.push_back(std::move(sym));
    }

    return results;
}

#else // !VSCAN_HAVE_ZBAR

std::vector<DecodedSymbol> ZBarDecoder::decode(const GrayView&) { return {}; }

#endif

} // namespace vscan
