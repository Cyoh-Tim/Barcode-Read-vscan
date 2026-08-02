#pragma once
#include "vscan_internal/decoder.hpp"

namespace vscan {

/*
 * [4-state 우편 바코드 — 일본우편 고객 바코드]
 *
 * 앞의 1D들과 근본적으로 다르다. 정보가 **막대의 폭이 아니라 높이**에 있다.
 * 막대는 넷 중 하나다:
 *
 *   full(3)      위아래 끝까지
 *   ascender(2)  위쪽만
 *   descender(1) 아래쪽만
 *   tracker(0)   가운데만
 *
 * 그래서 한 줄만 훑어서는 아무것도 못 읽는다 — 막대마다 위/아래 끝을
 * 재야 한다. `decoder_linear.cpp`의 런 기반 코드를 재사용할 수 없는 이유다.
 *
 * [구조] 67개 막대 = 시작 "31" + 심볼 21개 x 3막대 + 정지 "13".
 * 21개 중 마지막은 검사 심볼이다(합 mod 19).
 * 문자는 "0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ"이고, 영문자는 제어 심볼
 * 하나 + 숫자 하나로 **두 심볼**을 쓴다(11=A~J, 12=K~T, 13=U~Z).
 *
 * [표의 출처] BWIPP의 japanpost 인코더를 ghostscript로 실행해서 덤프했다
 * (§3.50과 같은 방법). 막대 상태와 높이 단위(8분할)도 거기서 그대로 얻었다.
 *
 * [기본 OFF] 다른 opt-in 심볼로지와 같다. 켜면 ROI마다 막대 상하단을
 * 재는 훑기가 붙는다.
 * [[vscan-lite-postal]]
 */
struct PostalOptions {
    bool japanPost = false;
    /*
     * [IMB — USPS Intelligent Mail] 같은 4-state지만 구조가 딴판이다.
     * 시작/정지 패턴이 없고 65개 막대의 어센더/디센더 비트가 13비트 문자
     * 10개에 흩뿌려져 있다. CRC-11이 강한 검증이라 오디코딩 위험은 낮다.
     */
    bool imb = false;
};

class PostalDecoder : public IDecoder {
public:
    explicit PostalDecoder(PostalOptions opt) : opt_(opt) {}

    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    std::string name() const override { return "postal"; }

private:
    PostalOptions opt_;
};

} // namespace vscan
