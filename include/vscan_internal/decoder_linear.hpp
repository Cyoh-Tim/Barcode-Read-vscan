#pragma once
#include "vscan_internal/decoder.hpp"

namespace vscan {

/*
 * [zxing이 안 가진 1D 심볼로지 — 직접 스캔한다]
 *
 * §4의 "미구현 갭"에는 zxing-cpp가 포맷 자체를 갖고 있지 않은 1D 코드가
 * 몇 개 있다. 실측으로 확인한 것(BWIPP로 생성 -> 현재 파이프라인에 투입):
 *
 *   Industrial 2of5 (Standard 2of5)  검출 0    <- zxing에 포맷 없음
 *   COOP 2of5                        검출 0    <- zxing에 포맷 없음
 *   Pharmacode                       검출 0    <- zxing에 포맷 없음
 *   (대조군) ITF                     검출 1    <- zxing의 ITF와는 다른 코드다
 *
 * 노트에는 "COOP 2of5 / Trioptic CODE39가 zxing의 ITF/Code39 옵션으로
 * 커버되는지 미검증"으로 남아 있었다. 재보니 **Trioptic은 커버된다**
 * (Code39 심볼 자체라 zxing이 그대로 읽는다). **COOP 2of5는 안 된다** —
 * 2of5 계열이라도 인코딩 표가 ITF와 완전히 다르다.
 *
 * [왜 별도 디코더인가] §4의 설계 방침 그대로다. zxing 경로를 건드리면
 * 이미 100%인 14종의 동작이 흔들릴 수 있다. 여기는 자기 표만 보고
 * 자기 심볼만 낸다.
 *
 * [왜 기본이 꺼짐인가] 셋 다 **오디코딩에 극도로 취약**하다.
 * Industrial 2of5는 체크디짓이 없고, Pharmacode는 "굵은 막대/가는 막대의
 * 아무 나열"이 항상 유효한 값으로 읽힌다 — Code128의 막대열도 규칙만
 * 안 걸리면 Pharmacode로 읽힌다. 산업 현장에서 오디코딩은 미검출보다
 * 나쁘다는 게 이 저장소의 기준이므로(§3.18), 쓰는 배치에서만 켠다.
 * 대신 켰을 때의 오검출을 막으려고 아래 세 겹을 건다:
 *   1) 정지대(quiet zone) 요구 — 앞뒤로 모듈 6배 이상의 흰 구간
 *   2) 여러 주사선 합의 — minLineCount개 행이 같은 값을 내야 채택
 *   3) 폭 분포 검사 — Pharmacode는 막대가 두 무리로 갈리고 공백이
 *      균일해야 한다(Code128은 막대 폭이 네 가지라 여기서 걸린다)
 *
 * [인코딩 표의 출처] BWIPP(uk.co.terryburton.bwipp, code2of5/pharmacode)의
 * 표를 두 경로로 교차 확인했다 — (a) PostScript 리소스를 직접 파싱,
 * (b) 실제로 렌더한 이미지의 런 길이를 세그먼트. 둘이 일치한다.
 * [[vscan-lite-linear-extra]]
 */
struct LinearDecoderOptions {
    bool industrial2of5 = false;
    bool coop2of5       = false;
    bool pharmacode     = false;

    /*
     * [주사선 합의] 같은 값을 낸 행이 이 수 이상이어야 채택한다.
     * zxing의 minLineCount와 같은 취지다(§3.18에서 ITF 부분 디코드를
     * 4로 막았다). 체크디짓이 없는 코드라 이게 사실상 유일한 방어선이다.
     */
    int minLineCount = 3;

    /*
     * [자릿수 하한] 2of5는 시작/정지 패턴이 짧아서(각 6/5 원소) 한두
     * 자리짜리는 다른 코드의 막대 일부와 우연히 맞을 수 있다. 실제
     * 산업 용례가 4자리 이상이라 기본을 4로 둔다.
     */
    int min2of5Digits = 4;

    /*
     * [Pharmacode 최소 막대 수] 규격상으로는 막대 2개(값 3)부터 유효하다.
     * 그런데 막대가 적으면 우연한 막대열과 이미지가 완전히 같아지고,
     * 체크디짓이 없어 검산도 못 한다. 실측(Pharmacode가 한 장도 없는
     * 200장 코퍼스): 최소 4 -> 유령 6장, 6 -> 0장. 대가는 값 63 미만을
     * 못 읽는 것이다(막대 n개 = 2^n-1 .. 2^(n+1)-2).
     */
    int minPharmacodeBars = 6;
};

class LinearDecoder : public IDecoder {
public:
    explicit LinearDecoder(LinearDecoderOptions opt) : opt_(opt) {}

    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    std::string name() const override { return "linear-extra"; }

private:
    LinearDecoderOptions opt_;
};

} // namespace vscan
