#pragma once
#include "vscan_internal/frame.hpp"
#include <vector>

namespace vscan {

/*
 * [곡면(원통 라벨) 보정 — 국소 바 피치를 균등하게]
 *
 * 원통에 감긴 라벨은 위치에 따라 가로 배율이 달라진다. 원근(§3.31)과
 * 다른 점은 **상자가 직사각형 그대로**라는 것이다 — 가로 좌표만 비선형으로
 * 밀리고 세로는 그대로다. 그래서 호모그래피로는 원리적으로 못 편다
 * (perspectiveRectify()는 "이미 직사각형"이라며 손을 뗀다).
 *
 * 실측(module 8, 곡면 축 — 강도 1.0이 왜곡 없음, 낮을수록 강함):
 *   DataBar 0.8 미만 전부 실패 / DataBarExp 0.7 미만 / UPCE 0.5 미만 /
 *   PDF417 0.4 미만
 * 그러면서 코드는 멀쩡하다 — 중앙행 런이 45개로 왜곡 강도와 무관하게
 * 유지되고 최소 바 폭도 6~9px다. 즉 정보는 있고 리더가 못 읽는 것이다.
 *
 * 방법은 모델 없이 간다. "한 모듈이 몇 픽셀인가"를 x의 함수 m(x)로
 * 추정하고, du/dx = 1/m(x)로 정의되는 좌표 u(모듈 단위)에서 균등하게
 * 다시 샘플링한다. 생성기가 쓰는 특정 원통 모델을 가정하지 않으므로
 * 하네스에 과적합되지 않는다.
 *
 * m(x)는 대표 행의 런 길이에서 낸다. 런 길이는 모듈의 정수배(1~4배)이므로
 * 이동창 안의 **하위 백분위수**가 곧 1모듈이다. 창 13런 / 10퍼센타일이
 * 실측에서 가장 좋았다(원래 실패하던 12개 중 8개 복구). 이 두 값은
 * 민감하다 — 30퍼센타일로 올리면 0/12가 된다. 런 길이 분포가 코드마다
 * 다르고, 백분위수가 높으면 2모듈짜리 런을 1모듈로 착각하기 때문이다.
 *
 * [한계] 가장 강한 왜곡(DataBarExp 0.1, PDF417 0.1~0.2)은 어떤 설정으로도
 * 안 열렸다. PDF417은 적층 코드라 한 행의 런이 그 열의 모듈을 대표하지
 * 못하는 것이 원인으로 보인다.
 * [[vscan-lite-pitch-equalize]]
 */
struct PitchMap {
    std::vector<float> srcX;   // 출력 x(여백 제외) -> 원본 x
    int marginX = 0, marginY = 0;
};

bool pitchEqualize(const GrayView& src, GrayImage& out, PitchMap* map = nullptr,
                   int winRuns = 13, int pct = 10, float outModule = 8.0f);

} // namespace vscan
