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
    /*
     * [위치가 근사인가]
     *
     * 영역 구제의 **회전 패스**는 크롭을 각도만큼 되돌린 뒤 디코드하고,
     * 나온 좌표를 크롭 중심 기준으로 역회전해서 되돌린다. 그 역회전은
     * 설계상 근사다(코드 주석 [[vscan-lite-region-rotate]] 참고) — 되돌린
     * 뒤 tightenToContent()가 상자를 또 줄이므로 중심이 밀린다.
     *
     * 보통은 상관없다. 그런데 **같은 코드를 다른 단이 정확한 좌표로도
     * 읽은 경우**, 두 상자가 서로 겹치지 않을 만큼 어긋날 수 있고 그러면
     * dedup의 기하 규칙이 전부 빗나가 같은 코드가 두 번 나간다. 실측
     * (씨드 101 c000132, QR 62x62): 정확한 쪽이 (1414,1313), 근사한 쪽이
     * (1300,1304)로 **114px** 어긋났다.
     *
     * 기하로 추측하는 대신 사실을 들고 다닌다. 위치를 못 믿는 결과는
     * "다른 자리에 있는 다른 코드"임을 주장할 근거가 없으므로, 같은
     * 심볼로지 + 같은 텍스트면 dedup이 중복으로 본다(꼭짓점이 겹친
     * 결과를 그렇게 다루는 [[vscan-lite-dedup-degenerate-quad]]와 같은
     * 논리다). [[vscan-lite-approx-position]]
     */
    bool approxPosition = false;
    /*
     * [이 결과를 얼마나 믿는가]
     *
     * 1D 결과의 **좁은 요소가 2px 미만**이면 표본이 요소당 둘이 안 된다
     * (나이퀴스트). 그 아래에서 나온 값은 맞아도 우연이다 — §3.61이 따로
     * 찾은 "모듈 2px 벽"과 같은 수이고, 풀테스트의 유령 ITF가 전부
     * 그 구간에서 나왔다(docs/FULLTEST_REPORT.md §7.2).
     *
     * 그렇다고 버리면 **그 해상도에서만 읽히는 진짜 코드**까지 잃는다.
     * 실측이 그렇게 나왔다. 그래서 버리지 않고 **강등**한다:
     *
     *   - 종료 판정(enough)에서 안 센다 -> 파이프라인이 계속 찾는다
     *   - **자기 자리를 덮는** 믿을 만한 결과가 있으면 그때 버린다
     *     (프레임 단위로 버리면 다중 코드 프레임에서 다른 코드를 맞게 읽은
     *      약한 결과까지 날아간다 — 실제로 그래서 245코드를 잃고 있었다)
     *   - 끝까지 이것뿐이면 그대로 돌려준다 (없는 것보다 낫다)
     *
     * 실측 (씨드 101/11, 각 9000장, 코드 29,892개, 2단계 경로, 개수 안 줌):
     *
     *   모드            코드 일치        일치%     오디코딩   평균
     *   끔            12,622/29,892     42.23%       25      59.2~61.9ms
     *   버림(drop)    12,513/29,892     41.86%       26         +1%
     *   **강등**      12,766/29,892   **42.71%**     23         +1%
     *
     * **오디코딩은 못 줄였다.** 25 -> 23은 n=25의 푸아송 잡음 안이다.
     * 값을 낸 곳은 검출(+144코드)이고 경로도 의도와 달랐다 — 해상도 미달인
     * 1D 값 하나로 프레임이 끝나지 않으니 파이프라인이 계속 찾는 것이다.
     * VSCAN_QZ_MODE=off|drop|demote 로 다시 견줄 수 있다.
     *
     * [[vscan-lite-low-confidence-1d]]
     */
    bool lowConfidence = false;
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

    /*
     * [부분 디코딩 대책] 같은 결과가 나와야 하는 스캔 라인 최소 개수.
     * zxing 기본은 2인데 그러면 1D 바코드의 일부만 훑은 스캔이 유효한
     * 짧은 코드로 통과한다 — 실측(ITF 각도 스윕 19장): 정답 "12345670"과
     * 함께 "345670"/"123456"이 나오고 25/65도는 틀린 것만 나왔다.
     * 4로 올리면 오디코딩 11 -> 2, 14종 각도 스윕 전부 100%가 되고,
     * 화질 축(노이즈/블러/모듈/대비)에서는 손실이 0이다.
     * 자세한 근거는 decoder_zxing.hpp의 minLineCount_ 주석.
     */
    int minLineCount = 4;

    /*
     * [ITF 체크섬 강제] 기본 OFF. ITF-14(물류 카톤 GTIN-14)처럼 체크디짓이
     * 규격상 필수인 배치에서만 켤 것 — 켜면 ITF 부분 디코딩이 완전히
     * 사라지지만(오디코딩 2 -> 0) 체크디짓 없는 ITF를 전부 거부한다.
     */
    bool validateITFCheckSum = false;

    /*
     * [ZBar 보조 디코더] 기본 OFF.
     *
     * 왜 addDecoder()가 아니라 설정 항목인가. 파이프라인은 내부에서
     * **임시 Pipeline을 여러 개 만든다** — 2단계의 빠른 패스, coarse
     * locate, TryHarder 단계, ROI 디코드, 각종 구제. 전부 cfg_를 복사해서
     * 생성하는데, addDecoder()로 붙인 디코더는 그 복사본에 안 따라온다.
     * 그래서 예전에는 vscan_create(enable_zbar_fastpath=1)로 켜도
     * 최상위 processViewCore()에서만 ZBar가 돌고, ROI/구제 경로에서는
     * 조용히 빠졌다 — 같은 프레임인데 경로에 따라 결과가 달라진다.
     * 설정 항목으로 두면 복사본이 자동으로 물려받는다.
     * 영역 구제가 ROI 디코드를 주 경로로 쓰기 시작하면서 이 차이가
     * 실제로 결과에 영향을 주는 자리가 됐다. [[vscan-lite-zbar-in-subpipelines]]
     */
    bool enableZBar = false;

    /*
     * [ZBar 구제 단계] 기본 ON(단, enableZBar가 켜져 있을 때만 의미가 있다).
     *
     * ZBar를 **디코더 목록에 넣으면 안 된다.** 그러면 폴백 체인의 모든
     * 패스(빠른/TryHarder/+Invert/풀옵션/각 구제)에서 매번 돌아서, 실측상
     * 단일 코드 코퍼스 평균이 149 -> 478ms(3.2배)가 된다. 검출은 61.9 ->
     * 63.4%로 오르지만 그 값으로는 비싸다.
     *
     * 대신 **구제 단계에서만** 켠다. 여기서 중요한 사실이 하나 있다:
     * ZBar를 원본 프레임에 직접 돌리면 저대비 코드를 못 읽는다(대비 0.05,
     * 0.10, 0.15 전부 0개). 그런데 상시 디코더로 켜면 대비 축 검출이
     * 오르는 게 실측된다 —
     *   Code128 90 -> 100%, EAN13 90 -> 100%, EAN8 85 -> 100%,
     *   UPC-A 90 -> 100%, Code39 85 -> 90%
     * 이유는 ZBar가 **전처리된 판본**에서 읽기 때문이다. 노이즈 구제의
     * 블러본, ROI 대비 스트레칭본, 확대·언샤프본 — 그런 이미지에서
     * ZBar가 zxing보다 강하다. 원본에 강한 게 아니다.
     *
     * 그래서 구제 서브 파이프라인에만 등록한다. 최상위 패스(빠른/
     * TryHarder/+Invert/풀옵션)에는 안 붙으므로 성공 프레임 비용은 0이다.
     *
     * false로 두면 예전처럼 모든 패스에 상시 등록된다(검출 +1.5%p,
     * 대신 단일 코드 코퍼스 평균 149 -> 478ms로 3.2배).
     */
    bool zbarAsRescue = true;

    /*
     * [zxing에 없는 1D 심볼로지 — 전부 기본 OFF(opt-in)]
     *
     * Industrial(Standard) 2of5 / COOP 2of5 / Pharmacode. zxing-cpp에
     * 포맷 자체가 없어서 실측 검출이 0이던 것들이다(decoder_linear.hpp).
     *
     * ZBar와 같은 이유로 **설정 항목**이다 — addDecoder()로 붙이면 ROI
     * 구제가 만드는 임시 Pipeline에 안 따라가서 경로마다 결과가 달라진다.
     * cfg_는 복사되므로 여기 두면 자동으로 물려받는다.
     * [[vscan-lite-zbar-in-subpipelines]]
     *
     * 켤 때는 오디코딩을 각오해야 한다 — 셋 다 체크디짓이 없다.
     * 방어 장치는 decoder_linear.hpp의 주석 참고.
     */
    bool enableIndustrial2of5 = false;
    bool enableCoop2of5       = false;
    bool enablePharmacode     = false;
    // 0이면 LinearDecoderOptions의 기본값(6)을 쓴다. vscan.h의 설명 참고.
    int  pharmacodeMinBars    = 0;

    /*
     * [MicroPDF417] 기본 OFF(opt-in). zxing-cpp에 포맷 자체가 없어서
     * 직접 만든 디코더다(decoder_micropdf417.hpp).
     *
     * 위의 셋과 달리 **오디코딩 위험은 낮다** — RAP 표 두 벌과 GF(929)
     * 리드-솔로몬을 통과해야 결과가 나온다. 기본을 끄는 이유는 비용이다:
     * ROI마다 네 방향(가로/세로 x 정/역)으로 주사선을 훑는다.
     */
    bool enableMicroPdf417 = false;

    /*
     * [일본우편 고객 바코드] 기본 OFF(opt-in). 4-state라 정보가 막대 폭이
     * 아니라 높이에 있어서 다른 1D와 코드를 공유하지 않는다
     * (decoder_postal.hpp). 검사 심볼(mod 19)이 있어 오디코딩 위험은 낮다.
     */
    bool enablePostalJapan = false;
    // IMB(USPS Intelligent Mail). CRC-11이 있어 오디코딩 위험이 낮다.
    bool enablePostalImb = false;

    /*
     * [DotCode] 점 격자. 파인더가 없어서 프레임 전체에서 점 뭉치를 찾고
     * 회전 격자를 맞춰야 한다 — 자체 심볼로지 중 가장 비싸다.
     * 우편과 같은 이유로 **타일이 아니라 프레임 전체**에 돌린다
     * (fullFrameDecoders_ 주석 참고): 점 격자가 타일 경계에 걸리면
     * 어느 타일에도 온전히 안 들어간다.
     */
    bool enableDotCode = false;

    /*
     * [큰 코드용 풀프레임 폴백 끄기] 기본 false(= 폴백 켬).
     *
     * 타일 스캔이 빈손일 때 프레임 전체를 한 번 더 훑는 단계다. 실측 값은
     * pipeline.cpp의 해당 자리 주석에 있다 — 코드 1개/504를 벌고 평균 +17%.
     * 코드가 타일 창보다 작다는 것을 아는 배치에서만 켤 것.
     */
    bool disableTileFallback = false;

    /*
     * [빠른 불판독 — **캡처를 통제할 수 있는 배치용**] 기본 false.
     *
     * 구제 체인의 뒷단(큰 코드 폴백 / 조명 평탄화 / 영역 구제)은 전부
     * **나쁜 이미지를 소프트웨어로 수술하는** 단계다. 노출·조명을 통제할 수
     * 있는 배치에서는 그게 잘못된 전략이다 — 한 장을 200ms 쥐어짜는 것보다
     * **빨리 "못 읽겠다"고 답하고 다르게 찍는 것**이 이긴다. 영역 구제가
     * 대비를 펴서 흉내내던 것을 노출이 직접 해주기 때문이다.
     *
     * 실측(2단계, reps 3). 불판독 판정 시간이 요점이다:
     *
     *   저조도 24장(QR 마스크)      코드   판정평균  판정최대
     *     기본                       25     74.5ms    131.8
     *     tilefb+flatten 끔          25     47.5       99.8
     *     + region 끔 (이 옵션)      23     42.8       52.6
     *
     *   난수 200장(마스크 없음)     코드   판정평균  판정최대
     *     기본                      280    340.1ms    970.5
     *     tilefb+flatten 끔         278    253.9      754.7
     *     + region 끔 (이 옵션)     257    110.6      150.8
     *
     * **불판독 최대가 970 -> 151ms(6.4배)이고 대가는 검출 -8%다.** 그 -8%는
     * 한 장 기준이라, 재촬영이 가능하면 대부분 다음 프레임에서 돌아온다.
     * 재촬영이 **불가능하면 켜지 말 것** — 그때는 그냥 검출을 잃는 것이다.
     *
     * 끄는 것: 큰 코드 폴백, 조명 평탄화, 영역 구제.
     * 남기는 것: 선 디노이즈와 노이즈 구제(싸고 수확이 크다 — §3.61).
     * [[vscan-lite-fast-no-read]]
     */
    bool fastNoRead = false;

    /*
     * [로케이터 두 번째 패스 — 국소 평탄화본에서도 찾는다] **기본 OFF.**
     *
     * 저대비 코드가 상대 에너지 랭킹에 묻히는 문제를 겨냥한 것이고, 실제로
     * 로케이터가 **정확한 상자를 찾아낸다**(근거는 pipeline.cpp의 해당 자리
     * 주석). 그런데 끝까지 재보니 값을 못 한다:
     *
     *   난수 코퍼스 150장 full   268/504 79ms -> 265/504 88ms
     *   난수 코퍼스 150장 2stage 279/504 151ms -> 280/504 198ms
     *
     * 후보 영역이 늘면 크롭 디코드가 그만큼 늘고, 자기 보정 예산(§3.47)이
     * 벽시계 기준이라 **되던 프레임이 뒤에서 잘린다.** full에서 코드 3개를
     * 잃는다. 노린 DataMatrix는 열리지도 않았다.
     *
     * 코드는 남긴다 — 저대비 코드가 많은 것을 아는 배치라면 켤 값이 있고,
     * 무엇보다 "찾기는 되는데 그 다음이 안 된다"는 것을 다음 사람이 다시
     * 확인하지 않아도 되게 하려는 것이다.
     */
    bool enableFlattenedLocate = false;

    /*
     * [QR 파인더 구제] **기본 OFF (opt-in)**. 다른 모든 단계가 실패했을
     * 때만 돈다.
     *
     * 에너지 로케이터가 원리적으로 못 잡는 "작은 QR이 여럿 흩뿌려진
     * 프레임"을 위한 것이다 — 파인더 패턴(1:1:3:1:1)으로 QR 자체를 찾는다.
     * 실측(실물 3.1MP 해상도 차트, QR 21x21모듈이 45px = 모듈 2.2px):
     * 자동 경로 검출 0 -> 3곳. 후보 5곳을 오탐 없이 찾는다.
     *
     * 기본을 끄는 이유는 비용이다. 전체 행을 훑어야 해서 2048x1536 기준
     * 프레임당 7.6~16ms이고, **실패한 프레임마다** 든다. 난수 코퍼스에서
     * 평균이 150 -> 186ms(+24%)로 올라 회귀 게이트를 넘었다.
     * 얻는 것이 "밀집 소형 QR"이라는 특정 상황에 한정되므로, 그 상황을
     * 아는 쪽에서 켜는 게 맞다.
     *
     * 켤 만한 배치: 해상도/한계 시험 차트, 작은 라벨이 여러 개 붙는 팔레트,
     * 상위 검출기가 없는 상태에서 화면상 코드가 50px 안팎으로 작게 잡히는 경우.
     */
    bool enableQrFinderRescue = false;

    /*
     * [로케이터 되살리기 — 노이즈에서 영역이 프레임 전체로 뭉갤 때]
     *
     * findCodeRegions()의 임계는 **프레임 최대 타일 에너지에 대한 비율**이라,
     * 센서 노이즈가 배경 타일을 전부 임계 위로 올리면 영역이 하나로 뭉쳐
     * 프레임 전체 상자가 나온다. 그 크롭은 이미 실패한 core 패스와 같은
     * 그림이라 2단계의 정밀 단이 아무것도 못 번다. 이 손잡이를 켜면 그때
     * (그리고 그때만) 임계 0.65로 다시 찾는다.
     *
     * **기본 꺼짐이다.** 버는 곳이 좁고 지연은 전반에 실리기 때문이다 —
     * enableQrFinderRescue와 같은 이유다.
     *
     * 실측:
     *   표적(모듈 x 노이즈 스윕)   PDF417 모듈3  4/10 -> 9/10
     *                              PDF417 모듈4  6/10 -> 10/10
     *                              DataMatrix 모듈3  3/10 -> 5/10
     *   난수 9000장(씨드 101)      6,336 -> 6,358코드 (+0.15%p)
     *   게이트 300장 p95           302.8 -> 304.9ms (+0.7%)
     *   게이트 기준선 p95(정규화)  139.3 -> 174.0  <- **회귀로 떨어진다**
     *
     * 마지막 줄이 기본값을 정한 근거다. 300장에서는 +0.7%인데 게이트의
     * 기준 작업량 정규화로 보면 25% 회귀다. 즉 이 구제는 **느린 프레임을
     * 더 느리게** 만든다 — 원래 빨리 포기하던 프레임이 이제 영역을 찾아
     * 정밀 단을 도는 것이라, 검출을 사는 대가가 정확히 꼬리 지연이다.
     *
     * 켤 만한 배치: 코드가 작고(모듈 4px 이하) 조명이 어두워 노이즈가 큰
     * 라인. 마감(max_frame_ms)과 같이 걸 것.
     * [[vscan-lite-locate-degenerate-escalate]]
     */
    bool enableLocateEscalation = false;

    /*
     * [작은 ROI 구제] ROI 디코드가 빈손이고 ROI가 이 크기(px) 이하면
     * 확대 + 언샤프로 한 번 더 시도한다. 0 또는 1이면 끔.
     *
     * 모듈이 2px 안팎인 코드는 인쇄/광학 흐림이 모듈 경계를 뭉개서
     * 이진화가 어느 쪽으로도 안 떨어진다. 실측(실물 3.1MP 해상도 차트,
     * QR 21x21모듈이 약 45px): 좌표를 정확히 줘도 원본 크기로는 24곳 중
     * 0곳인데, 3배 확대 + 언샤프면 6곳이 읽힌다.
     *
     * **기본은 꺼짐(0)**. processViewROIs()가 자기 호출에 한해 켠다 —
     * 거기는 "어디에 있는지는 안다, 작아서 안 읽힐 뿐"인 상황 그 자체다.
     * 자동 탐지 경로(영역 구제)에서도 켜봤더니 영역이 타일 128px + 여백이라
     * 이 임계 아래로 자주 들어와서, 작지도 않은 코드에 확대를 걸며 코퍼스
     * 평균이 7~9% 늘었다(검출은 그대로). 그래서 켜는 자리를 좁혔다.
     */
    int smallRoiMaxPx = 320;
    int smallRoiUpscale = 0;
    /*
     * 언샤프 세기(%). 150이 일반적인 값인데 이 용도에서는 200이 낫다 —
     * 실측(실물 해상도 차트, 6배 확대): 150에서 2곳, 200에서 6곳,
     * 280에서 4곳. 너무 세면 노이즈까지 세워 오히려 떨어진다.
     */
    int smallRoiSharpen = 200;

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
    // [1단계 이진화 — 2026-08-03에 기본을 바꿨다]
    // 노이즈가 심하면 LocalAverage가 불리하다: 화소를 이웃 평균과 비교하니
    // 국소적인 노이즈가 임계를 계속 넘나든다. GlobalHistogram은 임계가
    // 하나라 노이즈가 공간적으로 상쇄된다. 실측표는 vscan.h의 fast_locate
    // 주석. 되돌리려면 accurate_locate. [[vscan-lite-locate-binarizer]]
    ZXingDecoder::Binarizer locateBinarizer = ZXingDecoder::Binarizer::GlobalHistogram;
    bool locateTryDownscale = false;

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

    /*
     * [1D 바코드 회전 구제 — 아래 enableRegionRescue로 대체됨, 기본 OFF]
     *
     * 원래 §3.2.15/16의 "20~75도 회전 1D는 zxing이 풀옵션으로도 못 읽는다"를
     * 구제하려고 만든 최종 단계였다. 지금은 영역 구제가 같은 일을 더 잘한다:
     *
     *   - 후보 탐지가 1D 전용(coherence > 0.55)이 아니라 에너지 기반이라
     *     2D 행렬코드도 찾는다. 그래서 DataMatrix 55~90도 같은, 이 단계가
     *     원리적으로 못 잡던 케이스까지 커버한다(0/8 -> 8/8).
     *   - 회전 전에 "자르기만 하고" 한 번 본다. 실패 원인의 상당수는 각도가
     *     아니라 탐색 면적이라, 그것만으로 걸리는 경우가 많다(회전 없이
     *     DataMatrix 19/19).
     *
     * 실측 비교(단일 코드 300장, mixed):
     *                     text_ok   평균     p50      p95
     *   1D 회전 구제만     59.3%   165.5ms  85.4ms  688.8ms
     *   영역 구제만        60.8%   166.2ms  54.3ms  760.1ms
     *   둘 다              61.3%   195.2ms  58.1ms  888.9ms
     * 검출은 영역 구제 쪽이 위고 평균은 같은데 중앙값이 36% 낮다. 둘 다
     * 켜면 검출이 0.5%p 더 붙지만 평균이 18% 오른다 — 같은 일을 두 번
     * 하는 값으로는 비싸다. 고정 40종 게이트는 영역 구제만으로도 37/40으로
     * 동일하다(45도 1D인 39번 포함).
     *
     * 코드와 스위치는 남겨둔다 — 특정 배치에서 마지막 0.5%p가 절실하면
     * 켤 수 있게. 기본은 OFF.
     */
    bool enable1DDeskewRescue = false;

    /*
     * [영역 구제] 풀프레임 풀옵션이 빈손일 때, 그래디언트 에너지로 코드
     * 후보 영역을 찾아 그 주변만 잘라서 다시 디코드한다.
     *
     * 근거(§3.17): 실패의 원인은 회전이 아니라 **프레임 크기**였다.
     * 2048x1536 프레임의 128px DataMatrix는 55~90도에서 전부 실패하는데,
     * 코드 주변 205x200만 잘라내면 회전을 한 번도 안 하고 전 각도가
     * 읽힌다. zxing이 못 읽는 게 아니라 못 찾는 것이다.
     *
     * 1D 회전 구제와 달리 회전/리샘플링이 없고 심볼로지도 안 가린다.
     * 기본 ON — 실패한 프레임에서만 도는 비용이다.
     */
    bool enableRegionRescue = true;

    // 한 프레임에서 시도할 후보 영역 수 상한. 영역 하나당 작은 ROI
    // 디코드 한 번이라, 늘릴수록 실패 프레임의 비용이 선형으로 는다.
    int regionRescueMaxRegions = 4;

    /*
     * 후보 영역에 붙일 여백의 상한(px). 여백은 기본적으로 영역 크기의
     * 0.35배인데(정지대는 모듈 크기에 비례해야 하므로), 여기서 잘린다.
     *
     * 상한이 필요한 이유: 실측상 **크롭이 너무 커도 다시 실패한다**.
     * 여백 240px 크롭(665x660)은 읽히는데 여백 480px 크롭(1085x1140)은
     * 다시 실패한 각도가 있었다 — 애초의 실패 원인이 "탐색 면적이 커서"
     * 였으니 여백을 키우면 그 원인이 되돌아온다.
     */
    int regionRescueMaxPadPx = 160;

    // 회전까지 시도할 영역 수 상한(에너지 상위 N개). 회전은 리샘플링이
    // 들어가 자르기만 하는 것보다 비싸고, 실제로 회전이 필요한 심볼로지는
    // PDF417/1D뿐이라 상위 몇 개면 충분하다.
    int regionRescueMaxRotations = 2;


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
    /*
     * [프레임 마감(ms). 0 = 없음] — **지연을 실제로 묶는 유일한 손잡이다.**
     *
     * 실기(i.MX8MP) 로그에서 성공 프레임이 15ms인데 실패 프레임이
     * 5,000~6,500ms였다(성공의 350배). 컨베이어에서는 그 사이 프레임이
     * 큐에 쌓인다. 원인은 자기 보정 예산이 "첫 패스 x 3"이라는 **상대값**
     * 이라 느린 하드웨어에서 그대로 부푸는 것이고, 게다가 그 예산은
     * processView() 안에서만 무장돼서 2단계의 앞 계단들은 예산 밖이다.
     *
     * maxFrameMs는 프레임 진입에서 무장하므로 모든 계단이 이 마감을 본다.
     * 실측(난수 코퍼스 200장, 2stage 경로):
     *
     *   설정     검출/655   평균    p95    최대
     *   없음       360      169ms   531    989
     *   300        324       52     196    284
     *   150        324       53     180    264
     *   100        321       47     134    216
     *    60        316       43     107    130
     *
     * **설정값의 약 2.2배까지 넘친다**(100 -> 최대 216ms). 마감을 단계
     * **사이**에서만 보기 때문이고, 시작된 단계는 끝까지 간다. 즉 이건
     * 상한이 아니라 "이 값을 넘으면 다음 단계로 안 넘어간다"이다.
     * 진짜 상한이 필요하면 호출 쪽에서 오래된 프레임을 버리는 큐 정책이
     * 같이 있어야 한다.
     *
     * 검출 대가는 이 코퍼스에서 360 -> 321(-11%)이다. 현장 프레임에서의
     * 대가는 `field_diagnose`로 직접 잴 수 있다.
     */
    int maxFrameMs = 0;

    /*
     * [스스로 재는 예산] 첫 풀프레임 패스가 걸린 시간의 몇 배까지 쓸지.
     * 0이면 끔. 기본 3.0 — "이 프레임을 한 번 훑는 데 든 값"의 3배 안에
     * 끝낸다는 뜻이다.
     *
     * maxFrameMs가 좋은 장치인데 쓰기 어려웠다. 적정값이 해상도/하드웨어/
     * 프레임 내용에 다 걸려 있어서 배치마다 다시 재야 한다. 그런데 그
     * "다시 재야 하는 값"은 사실 파이프라인이 이미 알고 있다 — **첫 패스
     * 시간**이 곧 이 프레임을 이 기계에서 한 번 훑는 값이다. 그걸 기준으로
     * 삼으면 상수를 배치마다 고칠 일이 없다.
     *
     * 실측(2048x1536, x86 4코어): 열화 없는 프레임 p50 30ms, 난수 혼합
     * 프레임은 p90 196 / p95 380 / 최대 1111ms까지 갔다. 3배로 걸면
     * 꼬리가 잘리는 대신 검출을 조금 잃는다 — 얼마인지는 §3.47 표 참고.
     *
     * maxFrameMs와 함께 쓰면 **둘 중 이른 쪽**이 마감이다.
     */
    float frameBudgetXFirstPass = 3.0f;

    /*
     * [예산 하한] 배수로만 잡으면 **첫 패스가 싼 프레임이 부당하게 손해**다.
     * 배경이 거의 비어 있고 코드 하나만 있는 프레임은 첫 패스가 14ms라
     * 3배가 42ms인데, 그런 프레임의 구제(회전 45도 1D, DPM 도트각인)는
     * 110~145ms를 쓴다 — 절대값으로는 전혀 과하지 않은데 배수에 걸린다.
     * 실측(고정 40종): 하한 없이 3배만 걸면 4장을 잃는다(40 -> 36).
     *
     * 하한은 "이 정도 절대 시간은 어느 프레임에나 허용한다"는 뜻이다.
     * 꼬리는 배수가 정하므로(첫 패스가 비싼 프레임은 하한보다 훨씬 큰
     * 예산을 받는다) 하한을 둬도 최악값은 거의 안 움직인다.
     */
    int frameBudgetFloorMs = 150;

    /*
     * [자기 보정 예산의 **절대 상한**(ms). 0 = 없음]
     *
     * 자기 보정 예산은 `첫 패스 x frameBudgetXFirstPass`라는 **순수 상대값**
     * 이다. 그래서 하드웨어가 느리면 예산도 그대로 비례해서 부푼다 —
     * 실기(i.MX8MP) 로그에서 그게 그대로 나왔다:
     *
     *   성공 프레임   15ms 안팎
     *   실패 프레임   5,000 ~ 6,500ms      <- 성공의 350배
     *
     * 첫 패스가 1.7초쯤 걸리는 프레임에서 x3이 걸리면 5초가 된다. 구제
     * 사다리는 "실패 프레임에서만 도는 비싼 구간"이라고 설계했는데, 그
     * 전제가 **빠른 하드웨어에서만** 참이었다. 컨베이어에서 한 프레임에
     * 6초를 쓰면 그 사이 물건은 이미 지나갔다.
     *
     * 그래서 절대 상한을 넣어봤다. **그런데 이것만으로는 거의 안 듣는다** —
     * 자기 예산은 `processView()` 안에서만 무장되므로, 2단계 경로가 그
     * 전에 도는 계단들(coarse locate / 영역 우선 / TryHarder 단 / +Invert
     * 단)은 아예 예산 밖이다. 실측(난수 코퍼스 200장, 2stage): 상한을
     * 600/400/200/150/100 어느 값으로 줘도 검출 360/655, p95 480~500ms로
     * **사실상 변화가 없었다.**
     *
     * 지연을 실제로 묶는 것은 `maxFrameMs`다(프레임 진입에서 무장하므로
     * 모든 계단이 그 마감을 본다). 수치는 그쪽 주석에 있다.
     *
     * 이 값은 그래서 **기본 0(끔)** 이다. 자기 예산만 따로 조이고 싶을 때
     * 쓰라고 남겨두지만, 지연 보장이 목적이면 maxFrameMs를 쓸 것.
     */
    int frameBudgetMaxMs = 0;

    /*
     * [첫 영역 가산] 가장 유력한 영역 하나는 이 배수까지 봐준다.
     * 예산을 프레임 전체에 균일하게 걸면 **영역이 하나뿐인 프레임**이
     * 손해다 — 그런 프레임에서 사다리는 원래 한 벌만 돌고, 그 한 벌이
     * 깊은 구제(회전 45도 1D, DPM, 대비 0.05)를 담고 있다. 실측:
     * 균일 예산이면 고정 40종이 39/40, 축 스윕이 99.29%까지 떨어진다.
     * 반대로 첫 영역을 무제한으로 두면 한 영역이 1.9초를 쓴 프레임이
     * 나온다(총/첫패스 비율 14배).
     * 그래서 첫 영역만 별도 마감을 준다 — 무제한이 아니라 더 큰 배수로.
     */
    float frameBudgetXFirstRegion = 4.5f;

    /*
     * [첫 영역 가산의 절대 상한] 첫 패스가 비싼 프레임(클러터가 많고 코드가
     * 여럿인 프레임)은 배수만으로도 예산이 커진다 — 첫 패스 140ms면 4.5배가
     * 630ms다. 그 구간이 꼬리를 만든다. 배수와 이 상한 중 **이른 쪽**을 쓴다.
     * 0이면 상한 없음.
     */
    int frameBudgetFirstRegionCapMs = 200;

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

    /*
     * [S1 — 사전 노이즈 측정 후 선(先) 디노이즈] 1 = 켬(기본), 0 = 끔.
     *
     * 위 disableDenoiseRescue의 노이즈 구제는 **모든 게 실패한 뒤**에 돈다.
     * 그런데 노이즈가 심한 프레임에서는 그 앞의 단계들이 실패할 것이
     * 처음부터 정해져 있다. 실측(Code128 module 8, 노이즈 시그마 40,
     * 2단계 경로): zxing 호출 16회 / 652ms인데, 같은 프레임에 3x3 블러를
     * **먼저** 먹이면 호출 2회 / 24.1ms다 — fast 패스에서 바로 붙는다.
     * 35배 차이이고, 없는 기능을 만드는 게 아니라 순서만 바꾼 것이다
     * (회전에 대해 §3.24에서 이미 증명한 구조).
     *
     * 함부로 뭉개면 안 되므로 estimateNoise()로 재고 나서 정한다. 임계
     * 12는 실측에서 왔다 — 깨끗 3.5 / 모듈 2px 4.0 / 실물 3.1MP 차트 0.0
     * vs 노이즈 시그마 20에서 20.0. 5배 여유가 있다.
     * [[vscan-lite-pre-denoise]]
     */
    int autoDenoise = 1;
    float autoDenoiseNoise = 12.0f;

    /*
     * [S1b — 심한 노이즈는 선(先) 디노이즈를 **한 번에 두 겹**으로]
     *
     * 위 임계 12를 넘겨 3x3을 한 번 먹여도, 노이즈가 아주 심하면 아직
     * 모자란다. 그럴 때 두 번째 겹은 이미 있었다 — 아래쪽 구제 체인의
     * `dnAgain`(frameNoise >= 45)이다. **그런데 그건 폴백 체인을 전부
     * 돌고 실패한 뒤에 돈다.** 노이즈가 그만큼 심하면 그 앞 단계들은
     * 실패할 것이 처음부터 정해져 있는데도 값을 다 치른다.
     *
     * 그래서 순서만 바꾼다(§3.24 / autoDenoise 주석과 같은 구조 —
     * 없는 기능을 만드는 게 아니라 **이미 있는 걸 앞으로 당긴다**).
     *
     * 실측(저조도 코퍼스 24장 x 시드 5개, 2단계 경로). 1패스 대비:
     *
     *   시드   코드(1패스 -> 2패스)   평균ms          p95ms
     *   7      23 -> 24              134.8 -> 107.0  330 -> 232
     *   23     23 -> 24              136.7 -> 106.5  329 -> 226
     *   41     24 -> 24              139.3 -> 115.5  337 -> 271
     *
     * 검출은 사실상 그대로다(+2/240 = 노이즈 수준). **얻는 것은 시간이고,
     * 그게 이 변경의 이유다** — 평균 -21%, p95 -27%. 노이즈 프레임이
     * 체인을 끝까지 갈아먹는 대신 앞에서 붙거나 앞에서 접는다.
     *
     * 임계 45는 새로 고른 값이 아니라 **`dnAgain`이 쓰던 바로 그 값**이다.
     * 같은 판단을 같은 기준으로 더 일찍 할 뿐이라 동작이 바뀌는 프레임은
     * "원래 두 겹을 받았을 프레임"뿐이다.
     *
     * 0으로 두면 끈다(그러면 예전처럼 뒤쪽 dnAgain만 남는다).
     */
    float autoDenoiseStrongNoise = 45.0f;
    // 개수를 모를 때 프레임에서 기대 개수를 추정한다(vscan.h의
    // auto_expected_codes 주석 참고). [[vscan-lite-auto-expected]]
    bool autoExpectedCodes = false;

    /*
     * [2단계 경로에서도 자기 보정 예산을 무장한다] 기본 끔.
     *
     * 자기 보정 예산은 `processView()` 안에서만 무장돼서, 2단계 경로가 그
     * 전에 도는 계단들은 통째로 예산 밖이다(frameBudgetMaxMs 주석 참고 —
     * 그래서 그 절대 상한이 "거의 안 듣는다"고 적혀 있다). 지연을 묶으려면
     * maxFrameMs라는 **절대 벽시계**를 줘야 하는데, 그 값은 하드웨어마다
     * 다르다(이 저장소 기준 보드가 x86의 약 8배).
     *
     * 이 손잡이는 2단계 진입 직후의 빠른 패스를 기준으로 같은 예산을
     * 무장한다. 순수 상대값이라 **하드웨어가 바뀌어도 같은 뜻**이다.
     * [[vscan-lite-two-stage-self-budget]]
     */
    bool twoStageSelfBudget = false;

    /*
     * [S4 — 저대비 프레임은 영역 경로를 먼저]
     *
     * 풀프레임 패스(coarse + fast, 12ms)는 대비가 무너진 프레임에서
     * 성공할 수가 없다 — zxing 이진화가 8x8 블록 명암 폭 24 이하를
     * "구조 없음"으로 처리하기 때문이고, 옵션으로는 안 바뀐다(§3.25).
     * estimateLocalRange()가 이 값 미만이면 영역 경로를 먼저 돌린다.
     * 건너뛰는 게 아니라 순서만 바꾸므로 검출력 손실이 없다.
     *
     * 임계 100(블록범위 상위 1%): 대비 0.05~0.30이 24~84, 0.40이 109,
     * 정상 프레임(깨끗/회전/노이즈/블러/모듈 2px/실물 차트)이 213~255.
     */
    int lowContrastRange = 100;

    /*
     * [흑백 반전 ROI 구제] 영역 몇 개까지 반전 판본을 시도할지.
     *
     * zxing의 TryInvert는 1D와 PDF417에 대해서는 아무 일도 하지 않는다 —
     * MultiFormatReader가 반전 비트맵에서 supportsInversion이 아닌 리더를
     * 건너뛰는데, true인 건 QR/DataMatrix/Aztec 셋뿐이다. 그래서 우리가
     * 뒤집어서 넣는다(실측: 14종 중 2종만 읽히던 것이 13종으로).
     *
     * 다른 모든 시도가 실패한 뒤에만 도는 자리지만, 영역마다 디코드가
     * 한 번씩 더 붙으므로 상한을 둔다.
     */
    int invertRescueMaxRegions = 2;

    /*
     * [원근 보정 ROI 구제] 영역 몇 개까지 사각형 보정을 시도할지.
     *
     * 원근은 코드 안에서 배율이 달라져 한 스캔 행 안의 모듈 폭이 계속
     * 변한다 — 회전과 달리 각도 하나로는 못 되돌린다. 실측(module 8,
     * 원근 축이 끊기는 지점): PDF417 0.15 / UPCE 0.30 / DataBar 0.30 /
     * EAN8 0.40. 시제품 검증에서 그 구간의 8/12가 살아났다.
     *
     * 사각형이 이미 직사각형에 가까우면 perspectiveRectify()가 아무것도
     * 안 하고 false를 돌려주므로, 정상 프레임의 비용은 윤곽 추정뿐이다.
     */
    int perspRescueMaxRegions = 2;


    /*
     * [적응형 배치 프로파일] 0 = 끔(기본), 1 = 켬.
     *
     * 심볼로지 마스크를 좁히면 zxing의 포맷별 탐색 비용이 그만큼 준다
     * (§3.3 실측: 전체 포맷 대비 QR 단독 지정 시 약 2.5배). 문제는 배치마다
     * 손으로 정해줘야 한다는 것 — 대부분의 현장은 그냥 기본값(전체)으로 쓴다.
     *
     * 켜면 파이프라인이 스스로 관찰한다: 연속 N프레임 동안 나온 심볼로지가
     * 같은 집합 안에 머무르면 그 집합으로 마스크를 좁힌다. 좁힌 상태에서
     * 아무것도 못 찾은 프레임이 나오면 **그 프레임에서 즉시 전체 마스크로
     * 되돌려 다시 본다** — 새 심볼로지가 들어와도 놓치지 않는다.
     *
     * 워커당 파이프라인 인스턴스가 따로이므로 상태도 워커별로 독립이다.
     */
    int enableAdaptiveProfile = 0;
    // 좁히기 전에 관찰할 연속 프레임 수. 너무 작으면 우연히 한 종류만 나온
     // 구간에서 성급하게 좁힌다.
    int adaptiveWarmupFrames = 10;

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
    // 프레임을 호출자에게 내보내기 직전에 한 번 부른다. 믿을 만한 결과가
    // 있으면 강등된 것을 버리고, 없으면 강등된 것이라도 남긴다.
    // [[vscan-lite-low-confidence-1d]]
    static std::vector<PipelineResult> finalize(std::vector<PipelineResult>&& h);

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
    /*
     * [타일이 아니라 프레임 전체를 봐야 하는 디코더]
     *
     * 4-state 우편 바코드는 막대가 65~67개 늘어선 **길고 얇은** 심볼이다.
     * 90/270도로 세워지면 프레임 높이를 거의 다 차지하는데, 타일링은
     * 프레임을 가로 띠로 자르므로 어느 타일에도 온전히 안 들어간다.
     * 큰 코드용 풀프레임 폴백이 있긴 하지만 그건 **타일이 빈손일 때만**
     * 돈다(`[[vscan-lite-tile-large-code]]`).
     *
     * 실측으로 여기서 물렸다. IMB를 270도로 세운 프레임에서 zxing이
     * 엉뚱한 자리를 EAN-13 "6211122121212"로 읽어버린다(체크디짓까지
     * 우연히 맞는다). 그러면 타일 결과가 비지 않으니 폴백이 안 돌고
     * IMB는 통째로 사라진다. 90도는 그 유령이 안 떠서 폴백이 돌아
     * 읽혔다 — 즉 **읽히느냐가 남의 오디코딩 유무에 달려 있었다.**
     *
     * 그래서 우편 디코더는 타일 목록에서 빼고 프레임 전체에 한 번만
     * 돌린다. 비용은 실측으로 이렇다:
     *   - 우편 시험셋(길고 얇은 프레임) full 경로 평균 16.0 -> 11.1ms.
     *     타일마다 겹쳐 돌던 것이 한 번으로 줄어든 만큼이다.
     *   - 난수 코퍼스 120장은 **거의 그대로**다(켜면 +11%, 이 변경 전후
     *     같은 값). 큰 프레임에서는 타일 겹침 비율이 작아서 줄어들 것이
     *     별로 없다. "비용도 준다"고 쓸 뻔했는데 재보니 아니었다.
     * [[vscan-lite-postal]]
     */
    std::vector<std::unique_ptr<IDecoder>> fullFrameDecoders_;
    PipelineConfig cfg_;

    // processViewTracked() 상태: 직전 프레임에서 검출된 심볼들의 bounding box
    std::vector<Rect> lastPositions_;
    // 이번 프레임에서 영역 자르기/회전 패스를 이미 돌았는가 (중복 방지).
    // 첫 풀프레임 패스가 끝난 뒤 마감을 다시 잡는다(자기 보정 예산).
    void armSelfBudget(double firstPassMs);
    bool regionCropDone_ = false;
    bool regionRotDone_ = false;
    int framesSinceFullScan_ = 0;

    // 프레임 시간 예산 (maxFrameMs). 최상위 호출에서만 시작/해제한다 —
    // 폴백이 processView()를 다시 부를 때 예산이 리셋되면 안 되기 때문.
    std::chrono::steady_clock::time_point deadline_{};
    bool deadlineActive_ = false;
    // 이번 프레임의 마감을 누가 소유하는가(최상위 호출 한 곳). 자기 보정
    // 예산이 프레임 끝에서 반드시 해제되게 하려고 따로 둔다.
    bool deadlineOwned_ = false;
    // 첫 영역 전용(더 넉넉한) 마감과 그 구간 표시. 아래 armSelfBudget 주석 참고.
    std::chrono::steady_clock::time_point deadlineFirst_{};
    bool firstRectActive_ = false;
    /*
     * [maxFrameMs가 만든 **딱딱한** 마감]
     *
     * armSelfBudget()은 첫 영역에 더 넉넉한 마감(deadlineFirst_)을 주는데,
     * 그 계산이 frameBudgetMaxMs만 보고 **maxFrameMs는 안 봤다.** 그래서
     * 자기 보정 예산이 무장되는 순간 `max_frame_ms`가 상한이 아니게 된다 —
     * 실측(자동 기대 개수 + max_frame_ms=60): p95가 109ms였다. 헤더는 이
     * 옵션을 "지연을 실제로 묶는 것"이라고 문서화하고 있으니 문서가
     * 거짓말을 하고 있었던 셈이다.
     *
     * maxFrameMs로 잡힌 시각을 따로 들고 있다가 deadlineFirst_를 거기까지만
     * 늦춘다. maxFrameMs가 0이면 이 값은 안 쓴다.
     * [[vscan-lite-hard-deadline]]
     */
    std::chrono::steady_clock::time_point hardDeadline_{};
    bool hardDeadlineActive_ = false;
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

    // 적응형 배치 프로파일 상태 (enableAdaptiveProfile)
    uint32_t adaptiveObservedMask_ = 0;   // 관찰된 심볼로지 비트 합
    uint32_t adaptiveBaseMask_ = 0;       // 원래 설정값(되돌릴 때 씀)
    int adaptiveStreak_ = 0;              // 관찰 집합이 안 바뀐 연속 프레임 수
    bool adaptiveNarrowed_ = false;
    void adaptiveObserve(const std::vector<PipelineResult>& hits);
    void adaptiveWiden();
    void applyFormatMask(uint32_t mask);

    // coarse locate 적응형 스킵 상태
    int coarseMisses_ = 0;      // 연속 실패 횟수
    int coarseSkipLeft_ = 0;    // 남은 스킵 프레임 수
    GrayImage coarseBuf_;       // 절반 해상도 버퍼 (프레임마다 재사용)

    // [S1] 선 디노이즈 상태. 버퍼는 프레임마다 재사용한다.
    GrayImage denoiseBuf_;
    bool preDenoised_ = false;
    float frameNoise_ = 0.0f;   // preprocessFrame()이 잰 값 (구제 세기 결정용)
    // 저대비 경로에서 영역 패스를 먼저 돌렸을 때의 부분 결과(개수가 모자라
    // 뒤 단계로 넘어간 경우). 뒤 단계가 더 못 찾으면 이것을 쓴다.
    std::vector<PipelineResult> lowContrastPartial_;
    // 최상위 호출 깊이. 2단계 경로가 마지막에 processView()를 부르므로,
    // "이번 프레임에서 이미 뭉갰는가"를 중첩 호출이 지우지 않게 한다.
    int frameDepth_ = 0;
    struct FrameGuard {
        Pipeline* p;
        explicit FrameGuard(Pipeline* pp) : p(pp) {
            if (p->frameDepth_++ == 0) p->preDenoised_ = false;
        }
        ~FrameGuard() { --p->frameDepth_; }
    };
    // 노이즈를 재서, 필요하면 뭉갠 사본을 가리키는 뷰를 돌려준다.
    // 필요 없으면 입력을 그대로 돌려준다(복사 없음).
    // 이미 읽은 코드로 설명되지 않는 후보 영역을 세어 기대 개수를 추정한다.
    int estimateExpectedCodes(const GrayView& view,
                              const std::vector<PipelineResult>& hits);

    GrayView preprocessFrame(const GrayView& image);

    std::vector<PipelineResult> decodeTile(const GrayView& tile, int yOffset);
    // 주어진 디코더 목록을 한 뷰에 돌린다. decodeTile()의 알맹이이고,
    // fullFrameDecoders_를 따로 돌릴 때도 같은 것을 쓴다.
    static std::vector<PipelineResult> runDecoders(
        const std::vector<std::unique_ptr<IDecoder>>& ds, const GrayView& view, int yOffset);
    // need: 호출자가 아는 코드 개수(min_expected_codes). 기하만으로는
    // "한 코드의 두 밴드"와 "세로로 이웃한 같은 내용 라벨"을 못 가르는
    // 자리가 있어서, 개수를 알려준 프레임에서만 좁은 판정으로 재시도한다.
    // 기본값 1 = 예전 동작 그대로. [[vscan-lite-dedup-band-need]]
    static std::vector<PipelineResult> dedup(std::vector<PipelineResult> in, int need = 1);

    // processView()의 실제 작업(타일링/병합). 공개 processView()는 이걸
    // 호출한 뒤 빈손이면 tryDeskewRescue1D()를 마저 시도하는 얇은
    // 래퍼다 — full() 경로도 5단계 구제 혜택을 받게 하려는 것
    // (예전엔 processViewTwoStage() 안에서만 구제가 걸렸는데, 이건
    // "속도 최적화 전용 편의기능"이 아니라 "진짜로 새로 얻은 검출
    // 능력"이라 순수 풀옵션 경로에도 있어야 맞다).
    // [[vscan-lite-1d-deskew-rescue]]
    std::vector<PipelineResult> processViewCore(const GrayView& image, bool tileFallback = true);

    // [1D 바코드 회전 구제] §3.2.15~17 참고. processView()와
    // processViewTwoStage() 양쪽에서 공유하는 최종 안전망.
    // 다른 모든 방법이 실패했을 때만 호출되므로 비용은 실패한
    // 프레임에서만 발생한다.
    // [영역 구제] 큰 프레임 속 작은 코드를 잘라서 다시 본다.
    // 1D 회전 구제와 같은 자리(다른 모든 방법이 실패한 뒤)에서 돌지만,
    // 회전이 없어 더 싸고 심볼로지를 안 가린다. [[vscan-lite-region-rescue]]
    // 영역 구제는 두 가지 일을 한다: (1) 잘라서 그대로 디코드,
    // (2) 잘라서 각도만큼 되돌린 뒤 디코드. 둘의 성격이 다르다 —
    // (1)은 싸고 2D 행렬코드에 잘 듣고, (2)는 리샘플링이 들어가 비싸고
    // PDF417/1D에만 필요하다. 그래서 파이프라인의 이른 자리(빠른 패스
    // 직후)에는 (1)만, 마지막 구제 자리에는 (2)만 돌린다 — 안 나누면
    // 2단계 경로에서 (1)이 두 번 돈다(실측 p95 +50%의 주범).
    enum class RegionPass { CropOnly, RotateOnly, Both };
    std::vector<PipelineResult> tryRegionRescue(const GrayView& image, int need,
                                                 RegionPass pass = RegionPass::Both,
                                                float energyRatio = 0.20f);
    // 찾는 이미지와 읽는 이미지를 분리한 판본 (구현은 pipeline.cpp 주석 참고).
    std::vector<PipelineResult> tryRegionRescueOn(const GrayView& locateView,
                                                  const GrayView& decodeView, int need,
                                                  RegionPass pass = RegionPass::Both,
                                                  float energyRatio = 0.20f);

    // [QR 파인더 구제] 파인더 패턴으로 작은 QR을 직접 찾아 ROI 디코드로
    // 넘긴다. [[vscan-lite-qr-finder-locate]]
    std::vector<PipelineResult> tryQrFinderRescue(const GrayView& image, int need);


    std::vector<PipelineResult> tryDeskewRescue1D(const GrayView& image, int need);

    // processViewTwoStage()의 2단계와 processViewROIs()가 공유하는 실제 작업:
    // 주어진 rects를 각각 padPx만큼 여유 두고 crop -> packed 버퍼로 복사
    // (zero-copy 대신 일부러 복사하는 이유는 캐시 지역성 때문 —
    // [[vscan-lite-two-stage-decode]] 참고) -> regionCfg로 병렬 디코드 ->
    // 원본 좌표계로 보정 후 병합.
    std::vector<PipelineResult> decodeRegionsParallel(const GrayView& image, const std::vector<Rect>& rects,
                                                       int padPx, const PipelineConfig& regionCfg);

    /*
     * [고친 판본은 프레임째 다시 풀지 않는다]
     * 노이즈/조명 구제는 "고친 뷰"를 만든다. 예전에는 그 뷰를 통째로
     * 다시 디코드했는데, 그게 프레임 시간의 대부분이었다 — 실측(코퍼스
     * 140장): 평탄화본 풀디코드 78ms/회, 뭉갠본 47ms/회. 깨끗한 프레임
     * 전체가 15ms인데 구제 하나가 그 3~5배다.
     *
     * 고친 뷰가 필요한 이유는 대개 **찾기**다(그림자에서 상자가 반쪽만
     * 잡히는 것처럼). 찾고 나면 디코드는 크롭만 하면 된다. 그래서
     * 고친 뷰에서 영역을 찾고 그 크롭만 푼다.
     */
    std::vector<PipelineResult> locateAndDecode(const GrayView& view);
};

} // namespace vscan
