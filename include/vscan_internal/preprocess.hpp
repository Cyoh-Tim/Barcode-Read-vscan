#pragma once
#include "vscan_internal/frame.hpp"

namespace vscan {

// Frame -> GrayView 변환.
// - format == GREY: ISP가 이미 그레이스케일을 준 경우. 복사 없이 frame.data를
//   그대로 가리키는 뷰만 반환한다 (진짜 zero-copy 경로, 이게 기본 경로여야 함).
// - format == YUYV/NV12: ISP 설정이 잘못됐을 때의 폴백. 이 경우엔 GrayImage로
//   실제 변환(복사 1회, NEON 가속)이 발생하고, 반환된 GrayView는 그 GrayImage를
//   가리킨다 — 그래서 GrayImage& storage 파라미터로 수명을 보장받아야 한다.
GrayView toGrayView(const Frame& frame, GrayImage& fallbackStorage);

// NxN 박스 평균으로 1/N 해상도 사본을 만든다 (N = 2 또는 3).
// 용도: 2단계 디코드의 "coarse locate" — 이진화가 locate 비용의 대부분이고
// 픽셀 수에 비례하므로 1/N이면 1/N^2이 된다.
// 실측(악조건 36종, locate 합계): 원본 283ms / 1/2 93ms / 1/3 59ms.
// 1/3이 1/2와 검출 개수가 같으면서 1.6배 빨라서 기본값이다.
// 나머지 픽셀은 버림(마지막 행/열 무시) — locate 용도라 무해하다.
// 자동 벡터화가 걸리는 형태로 작성돼 있다(A53 NEON 확인).
void downsampleBox(const GrayView& src, GrayImage& dst, int factor);

// [1D 바코드 회전 구제용] 프레임(또는 crop)을 임의 각도로 회전한다.
// DDA 방식(행마다 삼각함수 1회만 계산, 나머지는 증분)이라 픽셀당
// 삼각함수를 다시 부르는 나이브한 구현보다 5~6배 빠르다(실측).
// 최근접 이웃 샘플링(품질보다 속도 우선 — 이 용도는 회전 후 바로
// zxing이 재이진화하므로 약간의 앨리어싱은 무해하다).
// out은 src와 동일 크기(중심 기준 회전, 프레임 밖으로 나간 부분은
// 흰색(255)으로 채움 — zxing 배경색과 동일해 안전).
void rotateAroundPoint(const GrayView& src, float degrees, float pivotX, float pivotY, GrayImage& out);

// [DPM/점각인 구제용] "어두운 점들을 연속된 면으로 합치기".
//
// DPM(레이저 점각인) 코드는 모듈 하나가 통짜 면이 아니라 작은 점
// 하나라서, zxing의 8x8 블록 이진화가 지역 평균을 내면 흰 배경과
// 검은 점이 섞여 애매한 회색으로 뭉개진다 — 파인더 패턴(모서리
// 정사각형)조차 점 격자라 위치 탐색 첫 단계부터 막힌다.
//
// 실측(실물 비교 대상 리더기 대조 검증까지 완료): 반전 -> 닫힘(팽창 후 침식) ->
// 재반전 하나로 해결된다. "닫힘"이 아니라 "반전 후 닫힘"인 이유 —
// 그레이스케일 팽창은 밝은 영역을 키우는 연산(이웃 중 최댓값)인데,
// 우리가 키우고 싶은 건 어두운 점이므로 먼저 반전해서 점을 밝게
// 만든 다음 팽창해야 한다.
//
// 구현은 커널 크기와 무관하게 O(1)/픽셀인 monotonic deque 기반
// 슬라이딩 최댓값/최솟값(가로 패스 + 세로 패스로 분리) — 나이브한
// O(k^2)/픽셀 대비 커널이 커질수록 이득이 크다.
//
// kernelSize: 점 사이 간격보다 살짝 크게. 실측(점 간격 5px 기준)
// k=5~9 전부 성공, 커널이 클수록 비용은 늘고 두꺼운 선이 뭉개질
// 위험도 커지므로 기본값을 5로 둔다(가장 작은 값도 성공했으므로
// 부작용 최소화 우선).
void morphologicalCloseInverted(const GrayView& src, int kernelSize, GrayImage& out);

/*
 * 3x3 박스 블러 (가로/세로 분리형).
 *
 * 용도는 하나다: **노이즈 구제 단계**. 센서 노이즈가 심하면 이진화가 무너져
 * 코드가 통째로 안 읽히는데, 모듈 크기가 노이즈 상관거리보다 크면 살짝만
 * 뭉개도 신호가 살아난다. 실측(모듈 4px, 노이즈 시그마 20~60 스윕):
 * Code128 검출 23.8% -> 100%, QR 76.2% -> 100%.
 *
 * 분리형이라 픽셀당 덧셈 4회 수준이고, 자동 벡터화가 잘 걸리는 형태로
 * 썼다(누적 변수 없이 인접 3픽셀 합 — [[vscan-lite-deskew-neon]]과 같은 이유).
 */
void boxBlur3x3(const GrayView& src, GrayImage& out);

} // namespace vscan

