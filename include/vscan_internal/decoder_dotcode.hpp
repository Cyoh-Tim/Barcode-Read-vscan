#pragma once
#include "vscan_internal/decoder.hpp"

namespace vscan {

/*
 * [DotCode — 점 격자, 파인더 패턴이 없다]
 *
 * 이 프로젝트가 만든 자체 디코더 중 가장 어려운 것이다. **오픈소스 디코더가
 * 존재하지 않는다** — 상용 SDK들이 "DotCode 읽기"를 마케팅 포인트로 삼는
 * 영역이고, 참고할 구현이 없어서 인코더(BWIPP)를 정확히 읽어 뒤집었다.
 *
 * [구조]
 *   - rows x columns 격자인데 **(x+y)가 짝수인 칸에만** 점이 놓인다(체커보드).
 *     rows+columns는 홀수여야 하고 둘 다 5 이상이다.
 *   - 코드워드 하나가 9개 점자리를 쓰고, 그 9비트는 **팝카운트가 정확히 5**인
 *     패턴 113개 중 하나다(C(9,5)=126에서 13개를 뺀 것).
 *   - 비트열 = 마스크 2비트 + 코드워드 nw개 x 9비트 + 나머지는 전부 1.
 *     앞에서부터 훑으며 빈 칸에 채우고, **마지막 6비트는 여섯 모서리 자리**에
 *     들어간다.
 *   - 검사: GF(113) 리드-솔로몬, 검사 코드워드 nc = nd/2 + 3.
 *
 * [파인더가 없는데 어떻게 찾나]
 *   여섯 모서리 점이 사실상 파인더 노릇을 한다. 그 자리가 켜져 있으면
 *   **어두운 점들의 경계상자가 곧 심볼의 크기**가 된다. 실측으로 60/60
 *   심볼에서 경계상자가 정확히 일치했다. 다만 규격이 그것을 보장하지는
 *   않으므로(모서리가 데이터 비트를 받는 형상이 40% 있다) 검출은 ±1 칸
 *   가설을 같이 던지고 RS가 고른다.
 *
 * [기본 OFF] 다른 자체 심볼로지와 같다. 담배/제약 업계 심볼이라
 * 그 배치가 아니면 켤 이유가 없다. 비용도 자체 심볼로지 중 가장 크다
 * (난수 코퍼스 120장 full 경로 +33%).
 *
 * [진단] `VSCAN_DOTCODE_DEBUG=1`을 주면 검출 단계마다 왜 끊겼는지 stderr에
 * 찍는다. 단계가 많아서 이게 없으면 실패를 못 고친다 — 실제로 이 디코더의
 * 결함 여덟 개를 전부 이 출력으로 짚었다(§3.54).
 * [[vscan-lite-dotcode]]
 */
struct DotCodeOptions {
    bool enabled = false;
};

class DotCodeDecoder : public IDecoder {
public:
    explicit DotCodeDecoder(DotCodeOptions opt) : opt_(opt) {}

    std::vector<DecodedSymbol> decode(const GrayView& image) override;
    std::string name() const override { return "dotcode"; }

private:
    DotCodeOptions opt_;
};

} // namespace vscan
