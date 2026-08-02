#pragma once
#include "vscan_internal/decoder.hpp"
#include <vector>

namespace vscan {

/*
 * [MicroPDF417 — zxing에 포맷이 없어서 직접 만든다]
 *
 * §3.48에서 실측으로 확인한 것: BWIPP로 만든 MicroPDF417을 현 파이프라인
 * (zxing PDF417 포함)에 넣으면 **검출 0**이고, 같은 조건의 일반 PDF417
 * 대조군은 1이다. zxing-cpp v2.2.1에 MicroPDF417 포맷 자체가 없다.
 *
 * 이 한 종이 §4 커버 상태표의 **세 자리**를 연다 — MicroPDF417 자체,
 * GS1 Composite(EAN/UPC 위), GS1 Composite(GS1-128/DataBar 위). 뒤의 둘은
 * 2D 성분(CC-A/CC-B)이 MicroPDF417이라 이것 없이는 원리적으로 못 읽는다
 * (ZBar로 된다던 로드맵 1번이 기각된 이유가 그것이다).
 *
 * [구조] PDF417과 달리 시작/정지 패턴이 없다. 한 행이
 *     좌RAP(10모듈) + 데이터(17*k1) + [중앙RAP(10) + 데이터(17*k2)] + 우RAP(10) + 정지막대(1)
 * 이고, RAP(Row Address Pattern)이 행 번호를 알려준다. 행 r의 RAP 인덱스는
 * (변형의 시작 인덱스 + r) mod 52다. 열 수는 1~4이고 3열/4열에만 중앙 RAP이
 * 있다. 전체 폭은 각각 38 / 55 / 82 / 99 모듈로 고정이다.
 *
 * [무엇을 재사용하고 무엇을 새로 썼나]
 * 재사용(밖에서 링크되는 것을 실제로 돌려 확인했다):
 *   - 막대폭 8개 -> 심볼 -> 코드워드: Pdf417::CodewordDecoder
 *   - 고수준 압축해제(Text/Byte/Numeric): Pdf417::Decode()
 * 새로 쓴 것:
 *   - GF(929) 리드-솔로몬 복호. zxing에도 있지만 ZXING_EXPORT_TEST_ONLY가
 *     ZXING_BUILD_FOR_TEST 없이 static으로 펴져서 밖에서 못 쓴다. 벤더
 *     빌드 설정을 건드리면 zxing 올릴 때마다 재확인해야 하므로 직접 썼다.
 *   - RAP 표 2벌(좌우 52 + 중앙 52)과 변형표 34개. tools/extract_micropdf417_tables.py가
 *     BWIPP에서 뽑고 **실측으로 검증**한다(변형 34/34, RAP 충돌 0).
 *   - 검출/행 파싱/격자 샘플링.
 *
 * [주의] Pdf417::Decode()는 codewords[0]을 길이 서술자로 읽는다. MicroPDF417에는
 * 길이 서술자가 없으므로 앞에 합성해서 넣는다.
 *
 * [기본 OFF] 다른 opt-in 심볼로지와 같은 이유 + 비용이다. 켜면 ROI마다
 * 주사선을 훑으며 네 가지 열 수를 다 시도한다.
 *
 * [진단] 환경변수 `VSCAN_MPDF_DEBUG`를 주면 변형이 맞은 자리에서 데이터
 * 코드워드를 stderr로 찍는다. GS1 Composite인지 단독 심볼인지는 첫
 * 코드워드로 갈린다(900=단독, 920=CC-B) — 그걸 눈으로 확인할 때 쓴다.
 * [[vscan-lite-micropdf417]]
 */
struct MicroPdf417Options {
    /*
     * [행 합의] 한 행을 이 수 이상의 주사선에서 같은 코드워드로 읽어야
     * 그 행을 채택한다. 1이면 한 줄만 맞아도 받는다.
     * RAP + 리드-솔로몬이 이미 강한 검증이라 2of5/Pharmacode만큼 높일
     * 필요는 없다.
     */
    int minLineCount = 1;
};

class MicroPdf417Decoder : public IDecoder {
public:
    explicit MicroPdf417Decoder(MicroPdf417Options opt = {}) : opt_(opt) {}

    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    std::string name() const override { return "micropdf417"; }

private:
    MicroPdf417Options opt_;
};

/*
 * GF(929) 리드-솔로몬 복호. 시험에서 직접 부르려고 노출한다.
 * received는 [데이터... EC...] 순이고, 성공하면 제자리에서 고쳐진다.
 * numEC개의 EC 코드워드로 최대 numEC/2개의 오류를 고친다.
 */
bool microPdf417CorrectErrors(std::vector<int>& received, int numEC, int* fixedCount = nullptr);

} // namespace vscan
