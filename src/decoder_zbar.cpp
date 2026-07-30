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

    zbar::ImageScanner scanner;
    scanner.set_config(zbar::ZBAR_NONE, zbar::ZBAR_CFG_ENABLE, 1);
    // 2D는 zxing-cpp가 담당하므로 ZBar에서는 끈다 (속도를 위해 스캔 범위 축소)
    scanner.set_config(zbar::ZBAR_QRCODE, zbar::ZBAR_CFG_ENABLE, 0);

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
