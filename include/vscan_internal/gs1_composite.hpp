#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vscan {

/*
 * [GS1 Composite의 2D 성분 — ISO 24723 범용 인코딩]
 *
 * MicroPDF417 심볼 자체는 ISO 24728이고 그건 decoder_micropdf417.cpp가 읽는다.
 * 그런데 Composite의 2D 성분은 **데이터 계층이 다른 규격**이다. 코드워드를
 * PDF417의 압축해제(Text/Byte/Numeric)에 넣으면 valid=1이 나오면서 글자가
 * 깨진다 — 실측으로 `(99)1234-abcd`가 `N\tHF PGRPS}IBD`로 나왔다.
 *
 * [규격을 어떻게 확보했나] 규격 문서를 보고 옮겨 적지 않았다. BWIPP의
 * `gs1-cc` 인코더를 **ghostscript로 실행해서 프로시저를 그대로 덤프**했다
 * (`gs -q ... (barcode.ps) run /gs1-cc /uk.co.terryburton.bwipp findresource ==`).
 * 압축된 PostScript를 파싱하면 숫자가 토큰으로 눌려 있어 못 읽는데, 실행
 * 시점에는 풀린 배열이라 실제 값이 보인다. 거기서 얻은 표를 그대로 뒤집었고,
 * BWIPP가 만든 심볼 34종 + 무작위 페이로드로 왕복 검증했다.
 *
 * [구조]
 *   비트열 = 인코딩방식 필드 + 압축 데이터 필드(cdf) + 범용 필드(gpf) + 패딩
 *
 *   인코딩 방식:
 *     "0"   일반. cdf 없음, gpf가 numeric 모드로 시작한다.
 *     "10"  첫 AI가 11/17이면 날짜 16비트 + 종류 1비트가 붙고,
 *           첫 AI가 10이면 그 자리에 "11"이 온다.
 *           **날짜 값의 최댓값이 38399라 상위 2비트가 절대 "11"이 될 수 없어서**
 *           두 형태가 구분된다. 이게 이 인코딩에서 가장 헷갈리는 자리다.
 *     "11"  첫 AI가 90. 모드 2비트 + 두 번째 AI 표시 + 숫자/글자 필드.
 *
 *   gpf의 네 모드 — numeric(숫자쌍 7비트), alphanumeric, iso646, alpha.
 *   **FNC1은 alphanumeric/iso646에서 numeric으로 되돌린다.** 이걸 빠뜨리면
 *   패딩이 글자로 읽혀서 꼬리에 쓰레기가 붙는다(실측으로 잡았다).
 *
 * [암시된 AI 번호] 비트열에 없는 AI 번호가 있다 — 방식 10의 AI 10,
 * 방식 11의 AI 21/8004. 표시 비트로만 오므로 디코더가 넣어줘야 한다.
 *
 * 출력은 이 저장소의 GS1 관례를 따른다: AI와 값을 이어 붙이고 가변 길이
 * 필드 뒤에 0x1D를 넣는다(gs1.cpp의 parseGS1이 그대로 먹는다).
 * [[vscan-lite-gs1-composite]]
 */

// CC-A: 코드워드를 69비트 단위 base-928로 풀어 비트열(0/1)로 만든다.
std::vector<uint8_t> gs1CompositeBitsFromCCA(const std::vector<int>& codewords);

// CC-B/CC-C: 첫 코드워드 920 뒤의 PDF417 Byte 압축(901)을 풀어 비트열로.
// 실패하면 빈 벡터.
std::vector<uint8_t> gs1CompositeBitsFromByteCompaction(const std::vector<int>& codewords);

/*
 * 비트열을 GS1 요소 문자열로 푼다. 성공하면 true.
 * 인코딩 방식을 못 알아보거나 중간에 규격 밖 값이 나오면 false —
 * 반쯤 푼 문자열을 내보내지 않는다(오디코딩이 미검출보다 나쁘다, §3.18).
 */
bool decodeGs1CompositeBits(const std::vector<uint8_t>& bits, std::string& out);

} // namespace vscan
