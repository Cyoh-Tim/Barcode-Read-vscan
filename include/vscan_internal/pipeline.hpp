#pragma once
#include <chrono>
#include <memory>
#include <vector>
#include "vscan_internal/decoder.hpp"
#include "vscan_internal/decoder_zxing.hpp"
#include "vscan_internal/frame.hpp"
#include "vscan_internal/gs1.hpp"

namespace vscan {

struct PipelineResult {
    DecodedSymbol symbol;
    GS1Result gs1; // symbol.isGS1일 때만 유효
};

struct PipelineConfig {
    // 0이면 std::thread::hardware_concurrency() 사용 (i.MX8M Plus면 보통 4)
    unsigned tileThreads = 0;
    // 타일 간 겹침 폭(px). 타일 경계에 걸친 코드를 놓치지 않기 위함 — 겹침이
    // 실제 코드 세로 크기보다 작으면 코드가 타일 사이에서 잘려 미검출된다.
    // 500px는 실기(i.MX8MP, 2048x1536, QR 세로 ~460-600px 테스트 차트) 4타일
    // 기준 100% 검출을 확인한 값 + 여유분(2026-07-24 벤치, [[vscan-lite-tile-overlap-fix]]).
    // 작업 거리/렌즈/해상도가 바뀌어 화면상 코드 크기가 달라지면 재튜닝 필요.
    int tileOverlapPx = 500;
    // zxing-cpp 탐색 옵션. 기본값(전체 포맷 + 회전/반전 탐색 켜짐)이 가장
    // 안전하다 — 좁히면 빨라지지만 그만큼 놓칠 수 있는 케이스가 생긴다.
    // 0 = 전체 포맷 (vscan.h의 VSCAN_FMT_* 비트 OR)
    uint32_t formatMask = 0;
    bool tryRotate = true;
    bool tryInvert = true;
    // TryHarder=false는 zxing-cpp의 다중 이진화/후보 패스를 줄여 상당히
    // 빨라지지만(실측 threads=1, 전체 포맷 기준 host에서 ~40% 추가 가속,
    // TryRotate/Invert와는 독립적) 대비 낮은 코드는 놓칠 수 있다.
    bool tryHarder = true;
    // zxing-cpp 이진화 방식. 기본은 LocalAverage(픽셀별 주변 평균, 가장 정확).
    ZXingDecoder::Binarizer binarizer = ZXingDecoder::Binarizer::LocalAverage;
    // zxing-cpp 내장 다운스케일 패스(기본 ON). 원본 해상도 외에 축소본으로도
    // 한 번 더 탐색한다 — 노이즈/저대비 코드를 살려내는 안전망이라 정밀
    // 디코드에서는 반드시 켜둘 것. 실측: 노이즈/저대비 이미지에서 이걸 끄면
    // 검출 자체를 잃는다(n=1 -> n=0). [[vscan-lite-zxing-opts-bench]]
    bool tryDownscale = true;
    // CODE39 확장 모드(Full ASCII). zxing 기본은 OFF인데 벤치마킹 대상 스펙에는
    // "CODE39 Full ASCII"가 포함돼 있어 기본 ON으로 둔다. 실측상 추가 비용이
    // 거의 없다(35.16ms -> 35.97ms, 측정 노이즈 범위).
    bool tryCode39ExtendedMode = true;
    // 내장 다운스케일 패스가 동작하기 시작하는 이미지 최소변 크기.
    // zxing 기본 500 -> 100으로 낮춤. 2단계 디코드의 crop(수백 px)도
    // 노이즈/저대비 구제 패스를 받게 하려는 것이고, 비용은 거의 없다.
    // [[vscan-lite-crop-downscale-threshold]]
    uint16_t downscaleThreshold = 100;

    // --- 2단계 locate-then-refine 디코드의 1단계 전용 설정 ---
    // 1단계는 "위치만 빠르게 찾기"가 목적이라 옵션을 좁힐 수 있는데,
    // 좁힐수록 검출력이 떨어진다. 8종 테스트 이미지 실측 결과
    // ([[vscan-lite-locate-matrix]]):
    //   LocalAverage    + downscale ON : 7/8 검출 (12-27ms)  <- 기본값, 검출력 최고
    //   LocalAverage    + downscale OFF: 5/8 검출 ( 9-23ms)
    //   GlobalHistogram + downscale ON : 5/8 검출 ( 1- 6ms)
    //   GlobalHistogram + downscale OFF: 5/8 검출 ( 0- 4ms)  <- 최속
    // GlobalHistogram은 작은 코드(90px 이하)/저대비 코드를 놓친다.
    // 속도가 정말 급하고 배치 환경의 코드가 충분히 크고 선명하다는 게
    // 확인됐을 때만 GlobalHistogram + downscale OFF로 좁힐 것.
    ZXingDecoder::Binarizer locateBinarizer = ZXingDecoder::Binarizer::LocalAverage;
    bool locateTryDownscale = true;

    // 빠른 경로(two_stage의 locate, tracked의 ROI 단계들)가 이 개수 미만을
    // 찾으면 다음 단계로 승격한다. 기본 1 = "하나도 못 찾으면 폴백"(기존 동작).
    //
    // 왜 필요한가: 빠른 경로는 탐색 옵션을 좁혀서 도는데, 품질이 섞인
    // 다중 코드 프레임(예: 깨끗한 코드 + 반전 코드)에서 쉬운 것만 찾고
    // "비어있지 않으니" 폴백이 안 걸려 부분 결과를 반환할 수 있다.
    // 실측: 깨끗+반전 2코드 이미지에서 full은 2개, two_stage는 1개.
    // 아이템당 코드 개수를 아는 현장(모든 박스에 라벨 N장)이라면 이 값을
    // N으로 설정 — N개를 못 채우면 자동으로 풀스캔까지 승격되므로
    // 검출력이 full과 같아진다. [[vscan-lite-partial-detection]]
    int minExpectedCodes = 1;

    // [coarse locate] 2단계 디코드의 1단계를 "절반 해상도 먼저" 시도한다.
    // 이진화가 locate 비용의 대부분이고 픽셀 수에 비례하므로 1/4로 줄어든다.
    // 실측(악조건 36종): 절반 해상도에서 성공한 28장 기준 locate 2.9배 단축.
    // 놓치는 것은 아주 작은 코드(60px -> 30px)와 심하게 손상된 코드뿐이고,
    // 그 경우 자동으로 풀해상도 -> 풀스캔으로 승격하므로 검출력은 동일하다.
    // 절반 해상도에서 나온 좌표는 내부에서 2배로 환산해 돌려준다.
    bool coarseLocate = true;
    // coarse 축소 배율 (2 또는 3). 3이 기본 — 36종에서 1/2와 검출 개수가
    // 같으면서 1.6배 빠르다. 다만 1/3은 화면상 ~90px 코드를 놓치므로
    // (1/2는 잡음), 배치 코드가 그 정도로 작으면 2로 낮출 것.
    int coarseFactor = 3;
    // coarse가 연속으로 이 횟수만큼 실패하면 한동안 건너뛴다(적응형).
    // 작은 코드만 나오는 배치에서 매 프레임 헛수고하는 것을 막는다.
    int coarseMaxConsecutiveMisses = 3;

    // [1D 바코드 회전 구제] §3.2.15/16 참고. 20~75도 부근 회전 1D
    // 바코드는 zxing이 풀옵션으로도 못 읽는데, 이 최종 5단계가 그걸
    // 구제한다. 기본 ON — "언제든 나올 수 있는 환경"이 배치 목표라면
    // 항상 켜두는 게 맞고, 실패한 프레임에서만 도는 비용이라 정상
    // 프레임에는 영향이 없다. 걱정되면 false로 끌 것.
    bool enable1DDeskewRescue = true;

    // [DPM/점각인 구제] §6.4 대화 참고. 실물 비교 대상 리더기 대조 검증까지
    // 완료됨 — 반전->닫힘->재반전으로 점각인 코드의 이진화 실패를
    // 구제한다.
    //
    // 기본 OFF — enable_zbar_fastpath와 같은 패턴의 opt-in 도구다.
    // 처음엔 기본 ON으로 넣었다가, "4단계로 나눈 의미가 없어진다"는
    // 지적으로 재설계했다: 이 4단계 체인의 철학은 "각 단계가 특정
    // 실패 유형을 저비용으로 겨냥한다"인데, DPM 구제를 무조건 실패시
    // 시도하면 DPM과 무관한 실패(저대비/강한원근/1D회전 등)에서도
    // 매번 헛되이 ~100ms를 태운다(실측: 39번이 52ms->163ms로 늘어남).
    //
    // 신뢰할 만한 "이거 DPM 같다" 사전 판별기를 만들어보려 했으나
    // (그래디언트 방향 일관성, 작은 블롭 개수 등 2가지 시도) 저대비/
    // 강한원근 케이스와 깔끔히 구분이 안 됐고, 어설픈 판별기를 넣으면
    // 오히려 실물 검증까지 끝난 DPM 검출 자체가 오탐으로 깨질 위험이
    // 있었다.
    //
    // 그래서 프레임 단위 자동판별 대신 "설치 시점의 튜닝"으로
    // 해결한다 — 이런 류의 상용 리더의 UI를 보면 운영자한테 노출된
    // "DPM 켜기" 스위치는 없지만, 설치 시 그 라인의 실제 샘플로
    // 뱅크를 튜닝하는 워크플로(코드 탐색 -> 파라미터 조정, 판독
    // 테스트/매칭률)가 있다. 그 라인이 DPM을 쓰는지는 운영자가
    // 매번 판단할 게 아니라 설치할 때 한 번 정해지는 사실이다.
    //
    // 이 플래그는 그래서 "현장 운영자가 매번 만지는 스위치"가
    // 아니라, **상위 시스템의 설치/튜닝 단계(비교 대상 리더기의 뱅크 튜닝과 같은
    // 역할)가 그 현장의 실제 샘플로 한 번 판단해서 고정하는 값**으로
    // 설계됐다. vscan-lite 자체는 그 튜닝 로직을 갖고 있지 않다 —
    // 상위 시스템이 설치 시 "DPM 샘플 몇 장 넣고 켰을 때/껐을 때
    // 검출률·속도 비교" 같은 과정을 거쳐 결정해서 넘겨줄 값이다.
    bool enableDPMRescue = false;
    // 닫힘 연산 커널 크기. 점 간격보다 살짝 크게. 실측(점 간격 5px
    // 기준) k=5~9 전부 성공했고, 가장 작은 값(부작용 최소)을 기본으로.
    int dpmKernelSize = 5;

    /*
     * [프레임 시간 예산] 0 = 무제한(기본, 기존 동작 그대로).
     *
     * 대량 코퍼스 실측(§3.9): **전체 디코드 시간의 51%가 "결국 하나도 못
     * 찾는" 프레임에 쓰인다.** 성공 프레임 평균 56ms인데 실패 프레임은
     * 150ms(full) / 209ms(2stage) — 폴백 체인을 끝까지 돌고 실패하는 비용이다.
     *
     * 이 값을 두면 폴백 **단계 경계마다** 경과 시간을 확인해서, 예산을 넘겼
     * 으면 남은 단계를 생략하고 그때까지 찾은 결과를 돌려준다.
     * - 이미 성공한 프레임은 영향이 없다(성공하면 즉시 반환하므로).
     * - 실행 중인 단계를 중간에 끊지는 않는다. 따라서 실제 소요는 예산을
     *   한 단계만큼 초과할 수 있다 — 하드 리얼타임 보장이 아니라
     *   "꼬리를 자르는" 장치다.
     * - 컨베이어처럼 프레임 주기가 정해진 배치에서는 평균보다 **최악값**이
     *   마감을 결정하므로, 여기에 주기의 60~80%를 넣는 게 보통 맞다.
     */
    int maxFrameMs = 0;

    /*
     * [빈 프레임 조기 종료] 0 = 켜짐(기본), 1 = 끔.
     *
     * 컨베이어는 아이템 사이에 **아무것도 없는 프레임**이 계속 들어온다.
     * 지금 구조는 그런 프레임에도 4단 폴백 체인을 전부 돌리고 실패한다 —
     * 실측(§3.9): 실패 프레임 하나가 150~230ms.
     *
     * 코드가 존재하려면 어딘가에 **국소 명암차**가 반드시 있어야 한다
     * (§3.8 판독 가능성 분류의 대비 기준과 같은 논리). 축소본에서 블록별
     * 최대-최소 범위를 보고, 어느 블록도 임계를 못 넘으면 코드가 물리적으로
     * 존재할 수 없으므로 즉시 빈손을 반환한다.
     *
     * 비용은 축소본 1회 스캔(~1ms 미만)이고, 코드가 있는 프레임은 첫 블록
     * 몇 개에서 바로 통과하므로 사실상 공짜다.
     */
    /*
     * [노이즈 구제] 0 = 켜짐(기본), 1 = 끔.
     *
     * 모든 단계가 실패했을 때, 3x3 박스 블러를 한 번 먹이고 다시 본다.
     * 센서 노이즈가 심하면 이진화가 무너져 코드가 통째로 안 읽히는데,
     * 모듈이 노이즈 상관거리보다 크면 살짝 뭉개는 것만으로 신호가 살아난다.
     * 실측(모듈 4px, 노이즈 시그마 20~60 스윕): Code128 23.8% -> 100%,
     * QR 76.2% -> 100%. 게다가 **더 빨라진다** — 실패해서 폴백 체인을
     * 끝까지 도는 대신 첫 패스에서 성공하기 때문(441ms -> 22ms).
     *
     * 실패한 프레임에서만 도는 비용이고, 작은 모듈(2px 이하)은 블러로
     * 오히려 뭉개지므로 그 경우엔 여기서도 실패한다 — 손해는 한 패스뿐.
     */
    int disableDenoiseRescue = 0;

    int disableBlankFrameSkip = 0;
    // 블록 하나가 "구조가 있다"고 인정받는 최소 명암차(LSB). 노이즈 시그마의
    // 몇 배로 잡아야 안전하다 — 기본 24는 §3.9에서 측정한 대비 붕괴점
    // (진폭 약 34 LSB)보다 넉넉히 아래다.
    int blankFrameMinRange = 24;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig cfg = {});

    // 추가 디코더(자체 구현한 Pharmacode/DotCode, ZBar 등)를 등록할 때 사용
    void addDecoder(std::unique_ptr<IDecoder> decoder);

    // 편의 경로: Frame(소유형)을 받아 처리. format==GREY면 내부적으로
    // 복사 없이 뷰만 만들어 processView()에 위임한다.
    std::vector<PipelineResult> process(const Frame& frame);

    // 핵심 경로(zero-copy): 이미 그레이스케일인 버퍼를 가리키는 뷰를 직접 받는다.
    // V4L2Capture::grabView()로 얻은 mmap 포인터를 복사 없이 바로 여기에 넘기면
    // 캡처→디코드까지 프레임당 memcpy가 0번 발생한다.
    // 내부적으로 프레임을 수평 타일로 나눠 스레드풀에서 병렬 디코드한다
    // (4코어 A53 기준 이론상 최대 ~4배, 실제로는 3~3.5배 정도).
    std::vector<PipelineResult> processView(const GrayView& image);

    // [선택: 속도/정확도 트레이드오프] 2단계 locate-then-refine 디코드.
    // 1단계: 이 파이프라인의 formatMask/tileThreads/tileOverlapPx는 그대로
    //        쓰되 TryHarder/TryRotate/TryInvert를 강제로 꺼서 빠르게 후보
    //        위치만 찾는다.
    // 2단계: 각 후보 주변을 cropPadPx만큼 여유를 두고 잘라(zero-copy,
    //        GrayView::crop) TryHarder/TryRotate/TryInvert를 전부 켠 채로
    //        재디코드한다. 후보가 여럿이면 서로 독립적이므로 병렬로 돌린다.
    //
    // 실측(i.MX8MP 3코어, 2048x1536): 순수 풀옵션 풀프레임 스캔 545ms ->
    // 이 경로 130-200ms.
    //
    // 주의: 1단계가 후보의 대략적 위치조차 못 찾으면(예: 반전 극성 코드인데
    // 1단계는 TryInvert가 꺼져있음) 2단계는 그 코드를 시도할 기회가 없다 —
    // 순수 풀옵션 풀프레임 스캔과 정확도가 완전히 동일하지 않다. 실측(합성
    // 열화 이미지 9종): 흑백 반전 케이스에서만 순수 풀옵션 대비 검출 누락
    // 확인됨(블러/회전/저대비는 차이 없었음). 실제 배치 조건으로 검증
    // 전에는 processView()(순수 풀옵션)를 기본으로 쓸 것.
    // [[vscan-lite-two-stage-decode]], [[vscan-lite-stress-validation]] 참고.
    std::vector<PipelineResult> processViewTwoStage(const GrayView& image, int cropPadPx = 50);

    // 외부에서 이미 알고 있는 ROI 좌표로 디코드한다(예: 상위 검출기가 준
    // 라벨 위치, 사람이 지정한 관심영역). processViewTwoStage()와 달리
    // 자체 locate 단계가 없다 — rects를 그대로 신뢰하고 각 영역만 crop해서
    // 이 파이프라인에 설정된 옵션(formatMask/tryRotate/tryInvert/tryHarder)
    // 그대로 디코드한다. rects가 여럿이면 서로 독립적이므로 병렬로 돌린다.
    //
    // padPx만큼 각 rect 주변에 여유를 둔다 — 외부 좌표가 코드 경계에 딱
    // 맞아떨어지지 않는 경우(바운딩박스가 약간 타이트한 경우 등)를 위한
    // 안전마진. 좌표가 이미 정확하다고 확신하면 0으로 줘도 된다.
    std::vector<PipelineResult> processViewROIs(const GrayView& image, const std::vector<Rect>& rects,
                                                 int padPx = 20);

    // [연속 프레임 추적 모드] 직전 호출에서 검출된 위치 주변 ROI만 먼저
    // 시도하고, 비어있으면 processViewTwoStage()로 풀스캔한다.
    // 컨베이어처럼 프레임 간 코드 위치가 조금씩만 이동하는 환경에서
    // 이진화/스캔 면적이 ROI 크기로 줄어 크게 빨라진다(실측 §3.2.8: 1.9배).
    //
    // trackPadPx: 프레임 간 최대 이동량보다 크게 잡을 것 (기본 120px).
    // fullScanInterval: N프레임마다 ROI를 건너뛰고 풀스캔을 강제한다.
    //   ROI 추적만 하면 "새로 화면에 진입한 코드"를 놓치기 때문. 0이면
    //   강제 풀스캔 없음(코드 개수가 절대 안 변하는 환경에서만).
    // 상태(직전 위치)는 이 Pipeline 인스턴스에 저장된다 — 워커당 인스턴스
    // 1개 구조와 자연스럽게 맞는다. resetTracking()으로 초기화 가능.
    std::vector<PipelineResult> processViewTracked(const GrayView& image, int trackPadPx = 120,
                                                    int fullScanInterval = 10);
    void resetTracking() { lastPositions_.clear(); framesSinceFullScan_ = 0; }

private:
    std::vector<std::unique_ptr<IDecoder>> decoders_;
    PipelineConfig cfg_;

    // processViewTracked() 상태: 직전 프레임에서 검출된 심볼들의 bounding box
    std::vector<Rect> lastPositions_;
    int framesSinceFullScan_ = 0;

    // 프레임 시간 예산 (maxFrameMs). 최상위 호출에서만 시작/해제한다 —
    // 폴백이 processView()를 다시 부를 때 예산이 리셋되면 안 되기 때문.
    std::chrono::steady_clock::time_point deadline_{};
    bool deadlineActive_ = false;
    // 예산을 시작하고 스코프를 벗어날 때 해제하는 가드. 이미 활성이면(=중첩
    // 호출이면) 아무것도 하지 않는다.
    struct BudgetGuard {
        Pipeline* p; bool owner;
        explicit BudgetGuard(Pipeline* pp);
        ~BudgetGuard();
    };
    // 예산을 넘겼는가. 예산이 0이거나 미설정이면 항상 false.
    bool budgetExceeded() const;
    // 코드가 존재할 만한 국소 명암차가 프레임에 있는가(빈 프레임 조기 종료).
    bool frameHasStructure(const GrayView& image) const;

    // coarse locate 적응형 스킵 상태
    int coarseMisses_ = 0;      // 연속 실패 횟수
    int coarseSkipLeft_ = 0;    // 남은 스킵 프레임 수
    GrayImage coarseBuf_;       // 절반 해상도 버퍼 (프레임마다 재사용)

    std::vector<PipelineResult> decodeTile(const GrayView& tile, int yOffset);
    static std::vector<PipelineResult> dedup(std::vector<PipelineResult> in);

    // processView()의 실제 작업(타일링/병합). 공개 processView()는 이걸
    // 호출한 뒤 빈손이면 tryDeskewRescue1D()를 마저 시도하는 얇은
    // 래퍼다 — full() 경로도 5단계 구제 혜택을 받게 하려는 것
    // (예전엔 processViewTwoStage() 안에서만 구제가 걸렸는데, 이건
    // "속도 최적화 전용 편의기능"이 아니라 "진짜로 새로 얻은 검출
    // 능력"이라 순수 풀옵션 경로에도 있어야 맞다).
    // [[vscan-lite-1d-deskew-rescue]]
    std::vector<PipelineResult> processViewCore(const GrayView& image);

    // [1D 바코드 회전 구제] §3.2.15~17 참고. processView()와
    // processViewTwoStage() 양쪽에서 공유하는 최종 안전망.
    // 다른 모든 방법이 실패했을 때만 호출되므로 비용은 실패한
    // 프레임에서만 발생한다.
    std::vector<PipelineResult> tryDeskewRescue1D(const GrayView& image, int need);

    // processViewTwoStage()의 2단계와 processViewROIs()가 공유하는 실제 작업:
    // 주어진 rects를 각각 padPx만큼 여유 두고 crop -> packed 버퍼로 복사
    // (zero-copy 대신 일부러 복사하는 이유는 캐시 지역성 때문 —
    // [[vscan-lite-two-stage-decode]] 참고) -> regionCfg로 병렬 디코드 ->
    // 원본 좌표계로 보정 후 병합.
    std::vector<PipelineResult> decodeRegionsParallel(const GrayView& image, const std::vector<Rect>& rects,
                                                       int padPx, const PipelineConfig& regionCfg);
};

} // namespace vscan
