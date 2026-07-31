#pragma once
#include "vscan_internal/frame.hpp"
#include <vector>

namespace vscan {

/*
 * [코드 후보 영역 탐지 — 심볼로지 무관]
 *
 * 왜 필요한가. 실측으로 확인된 사실인데, 실패의 원인은 "회전"이 아니라
 * **프레임 크기**였다(§3.17). 2048x1536 프레임에 128px짜리 DataMatrix를
 * 놓고 0~90도를 5도 간격으로 돌려보면 55도 이상은 전부 실패한다. 그런데
 * 같은 이미지에서 코드 주변만 205x200 ~ 665x660으로 잘라내면
 * **각도 불문 전부 읽힌다** — 회전을 한 번도 하지 않고.
 *
 *   angle=90  풀프레임 2048x1536 -> 0개
 *             여백 10 크롭 205x200 -> "VSCAN-SWEEP-01"
 *
 * 즉 zxing은 그 각도의 코드를 못 읽는 게 아니라, 큰 프레임 안에서 작은
 * 코드를 **못 찾는** 것이다. 그래서 필요한 건 더 많은 회전 시도가 아니라
 * "어디를 보라"고 알려주는 것 — 이게 산업용 리더기가 회전각과 무관하게
 * 10~20ms로 일정한 이유이기도 하다(찾기 + 작은 ROI 디코드는 각도에
 * 비례해 늘어나는 비용이 아니다).
 *
 * findDeskewCandidate()로는 안 되나. 안 된다. 그건 1D 전용이라
 * coherence(그래디언트 방향 일관성) > 0.55인 타일만 후보로 친다. 1D
 * 바코드는 막대가 전부 같은 방향이라 coherence가 높지만, QR/DataMatrix는
 * 사방으로 엣지가 뻗어 있어 coherence가 낮다 — 그래서 2D 코드에서는
 * 솔리드 L-파인더 가장자리 몇 타일만 걸리고 본체가 통째로 빠진다(실측:
 * 128x128 코드에 대해 256x128짜리 엉뚱한 bbox가 나왔다).
 *
 * 여기서는 방향을 아예 안 본다. **그래디언트 에너지만** 본다:
 * 바코드는 종류를 불문하고 주변 배경보다 단위 면적당 엣지가 압도적으로
 * 많다. 방향 조건을 빼는 것만으로 1D/2D/DPM 전부에 같은 탐지기가 쓰인다.
 */
struct CodeRegion {
    Rect bbox;      // 원본 프레임 좌표 (패딩 없음)
    float energy;   // 영역 내 그래디언트 에너지 합 — 정렬 기준

    /*
     * 영역의 지배적 기울기 추정값(도), 범위 (-45, 45].
     * rotateAroundPoint()에 -angleDeg를 넘기면 막대가 축에 정렬된다.
     *
     * 왜 findDeskewCandidate()의 추정값을 안 쓰나. 그건 후보 타일을
     * coherence > 0.55 && 에너지 상위 10%로 거른 **다음** 그 타일들만
     * 합산한다 — 1D 전용 필터라, 이미 영역을 알고 있는 상황에서는
     * 오히려 영역의 일부만 보게 된다. 실측: PDF417 크롭에서 20/30/60/75/80도
     * 전부 "추정 불가"(coherence < 0.25)가 나왔는데, 정작 그 각도들은
     * 전부 정확한 각도 한 번의 회전으로 읽혔다(각각 +20 / +30 / -30 /
     * -15 / -10도). 즉 정보가 없던 게 아니라 필터가 버린 것이다.
     * 여기서는 그 영역에 속한 hot 타일 전체의 텐서를 합산한다.
     *
     * 2D 행렬코드(QR/DataMatrix)는 사방으로 엣지가 뻗어 방향성이 없어
     * kAngleUnknown이 나오는 게 정상이다 — 그런 코드는 애초에 회전이
     * 필요 없다(ROI로 자르기만 하면 전 각도가 읽힌다).
     */
    static constexpr float kAngleUnknown = 1e9f;
    float angleDeg = kAngleUnknown;
};

/*
 * 에너지가 큰 순으로 최대 maxRegions개의 후보 영역을 돌려준다.
 *
 * tileSize/downscale은 findDeskewCandidate()와 같은 뜻이다(기본
 * 32 x 4배 = 원본 128px 타일). 이 크기는 "코드 하나가 타일 몇 개에
 * 걸치느냐"만 좌우하고, 인접 타일을 연결해서 덩어리를 만들기 때문에
 * 코드가 타일보다 크든 작든 상관없다.
 *
 * energyRatio: 프레임 내 최대 타일 에너지 대비 몇 배 이상이면 후보로
 * 볼지. 절대 임계값을 안 쓰는 이유는 노출/대비가 프레임마다 다르기
 * 때문 — 상대값이라 자동으로 적응한다.
 */
std::vector<CodeRegion> findCodeRegions(const GrayView& image, int maxRegions = 4,
                                        int tileSize = 32, int downscale = 4,
                                        float energyRatio = 0.20f);

} // namespace vscan
