#pragma once
#include "vscan_internal/decoder.hpp"

namespace vscan {

// ZBar는 유지보수가 활발하진 않지만 1D(EAN/UPC/Code128/Code39/Codabar/
// DataBar) 스캔 알고리즘이 zxing-cpp보다 가벼워서 fast-path로 쓸 만하다.
// QR 정도는 되지만 DataMatrix/PDF417 등은 지원하지 않으므로 2D는 여전히
// ZXingDecoder가 맡는다. Pipeline에 둘 다 addDecoder()로 등록하면
// 겹치는 결과는 dedup()이 걸러준다.
class ZBarDecoder : public IDecoder {
public:
    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    std::string name() const override { return "zbar"; }
};

} // namespace vscan
