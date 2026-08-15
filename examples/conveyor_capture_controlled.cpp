/*
 * conveyor_capture_controlled.cpp
 *
 * **노출/조명을 통제할 수 있는 컨베이어 배치**의 권장 설정을 그대로 담은
 * 예제. 2026-08-03에 §3.62~§3.79로 정리된 결론을 코드로 옮긴 것이다.
 *
 * ## 이 설정이 겨냥하는 문제
 *
 * 컨베이어에서 진짜 문제는 "어려운 코드를 못 읽는다"가 아니라
 * **"못 읽겠다는 판정에 너무 오래 걸린다"** 인 경우가 많다. 그 사이
 * 프레임이 큐에 쌓이고, 큐에 쌓인 이미지는 이미 지나간 물건의 사진이라
 * 읽어도 늦다. 실기 로그에서 성공 15ms / 실패 5,000~6,500ms였다.
 *
 * ## 실측 (1280x960, QR만 있는 저조도 프레임, 2단계, reps 3)
 *
 * 한 시드의 좋은 실행을 인용하지 않도록 **독립 시드 다섯**으로 쟀다(§3.79).
 * 아래는 이 설정 그대로의 값이다.
 *
 *   시드   코드    판정 평균   판정 최대   보드 환산(x8)
 *    7      30      11.2ms      15.4        123ms
 *   23      30      10.7        12.3         99
 *   41      30      10.6        11.2         90
 *   59      30      10.9        13.7        110
 *   77      29      10.4        10.8         86
 *
 * **평균은 보드 83~90ms, 최대는 86~123ms다.** 오디코딩은 전 시드 0이고
 * 성공 프레임은 보드 18~38ms다. 출발점(기본 설정)이 판정 최대 보드
 * 2,928ms였으므로 20배 남짓 줄었고, 그러면서 검출은 25 -> 30으로 올랐다.
 *
 * 워커 모드(워커당 1스레드)와 4스레드가 사실상 같다 — 실측 8.9 vs 8.8ms.
 *
 * 40종 회귀셋 기준으로는: 기본 40/40, QR 마스크만 36/40, **여기에
 * fast_no_read와 NO_INVERT를 얹어도 36/40**이다(마스크에서 빠지는 4개는
 * 비-QR 코드다). 즉 이 설정의 뒤 두 줄은 마스크 대비 **검출 손실이 없다.**
 *
 * ## 목표 100ms를 완전히 지키지는 못한다
 *
 * 평균은 지키지만 **꼬리(최대 123ms)는 못 지킨다.** 마감(max_frame_ms)으로
 * 자를 수 있으나 40종이 36 -> 34/40이 되어 뺐다(아래 주석 참고).
 * 남은 길은 호출 쪽의 **오래된 프레임을 버리는 큐 정책**이다 — 이 파일
 * 맨 아래 주석.
 *
 * ## 전제를 반드시 확인할 것
 *
 * 이 설정은 셋을 가정한다. **하나라도 아니면 그 줄을 지워야 한다.**
 *
 *   1. 재촬영이 가능하다 (노출/조명을 통제한다)   -> fast_no_read
 *   2. 이 라인에 QR/MicroQR만 들어온다            -> symbology_mask
 *   3. 라벨 극성이 항상 같다 (밝은 바탕에 검은 코드) -> NO_INVERT
 *
 * 전제가 틀리면 그건 **우리가 만든 미검출**이다. 자기 프레임에서 확인하는
 * 방법은 `tools/field_diagnose <프레임디렉터리> --target-ms 100`이다.
 *
 * ## 빌드
 *
 *   g++ -O3 -std=c++17 -Iinclude examples/conveyor_capture_controlled.cpp \
 *       -Lbuild -lvscan -o conveyor
 */
#include "vscan.h"

#include <cstdio>
#include <cstring>
#include <vector>

// 호출자가 채워 넣을 자리: 카메라에서 프레임 한 장을 받는다.
// 반환 false면 스트림 끝.
static bool grabFrame(std::vector<uint8_t>& gray, int& w, int& h);

// 호출자가 채워 넣을 자리: 노출/게인을 바꾼다.
// **이 함수가 있다는 것이 이 설정 전체의 전제다.**
static void setExposure(int step);

int main() {
    vscan_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));   // 0 초기화 = 전부 기본값

    // [1] 심볼로지를 좁힌다 — 대가가 없는 유일한 손잡이.
    //     안 쓰는 심볼로지를 찾던 시간이 통째로 없어진다. MicroQR을 같이
    //     켜는 비용은 실측상 0에 가까우니 안전 여유로 넣는다.
    cfg.symbology_mask = VSCAN_FMT_QR | VSCAN_FMT_MICRO_QR;

    // [2] 빠른 불판독 — **재촬영이 가능할 때만.**
    //     이미지 수술 단계(큰코드폴백/평탄화/노이즈구제/최종승격, 그리고
    //     영역 구제의 대비·원근·곡면·반전 단)를 끊는다. 한 장을 오래
    //     쥐어짜는 대신 빨리 포기하고 다시 찍는다.
    //     **영역 구제의 '자르기'는 남는다** — 그게 회전을 담당하기 때문이고,
    //     통째로 끄면 회전된 1D를 통째로 잃는다(§3.78).
    cfg.fast_no_read = 1;

    // [3] 반전 탐색 생략 — **극성이 항상 같을 때만.**
    //     판정 경로에서 가장 비싼 단계다(실측 7.5~9.8ms).
    cfg.decode_flags = VSCAN_FLAG_NO_INVERT;

    // [4] 워커 구조라면 반드시. 워커 N개 x 내부 4스레드가 코어를 놓고
    //     경쟁하는 과다구독을 막는다(실측 62% 손해).
    cfg.worker_mode = 1;

    // [선택] 마감. **기본 권장에서는 뺐다**(§3.79) — 저조도에서는 공짜로
    // 보이지만 40종에서 36 -> 34/40이다. 얻는 것은 평균이 아니라 꼬리
    // 20% 남짓이라 그 값으로 두 장을 줄 이유가 없다.
    //
    // 넣는다면 **보드 시간으로** 줄 것. 마감은 절대 벽시계라, x86에서
    // 잰 값을 그대로 넣으면 안 된다(이 저장소 기준 보드가 약 8배 느리므로
    // x86 6ms에 해당하는 값은 보드에서 약 50ms다).
    // cfg.max_frame_ms = 50;

    vscan_pipeline_t* p = vscan_create(&cfg);
    if (!p) { std::fprintf(stderr, "파이프라인 생성 실패\n"); return 1; }

    std::vector<uint8_t> gray;
    int w = 0, h = 0;
    int exposureStep = 0;

    while (grabFrame(gray, w, h)) {
        vscan_result_t* r =
            vscan_process_gray_two_stage(p, gray.data(), w, h, w, /*crop_pad*/ 0);

        const size_t n = r ? r->count : 0;
        for (size_t i = 0; i < n; ++i)
            std::printf("%s\n", r->symbols[i].text);
        if (r) vscan_free_result(r);

        // [핵심] 못 읽었으면 **다시 찍는다.** 이 라이브러리를 더 오래
        // 돌리는 것보다 더 좋은 사진을 얻는 쪽이 이긴다 — 검출을 막는 것이
        // 알고리즘이 아니라 SNR인 구간이 실제로 있고(§3.61: 모듈 4px 이상은
        // SNR 3까지, 그 아래는 화소가 포화해 정보 자체가 없다), 그건
        // 디코더로 넘을 수 없다.
        if (n == 0) {
            exposureStep = (exposureStep + 1) % 3;   // 예: 기준 / 밝게 / 어둡게
            setExposure(exposureStep);
        } else if (exposureStep != 0) {
            exposureStep = 0;                        // 성공했으면 기준으로 복귀
            setExposure(0);
        }

        // [주의] 진짜 지연 상한은 여기서 나온다.
        // 디코더가 도는 동안 들어온 프레임은 **버리고 최신 것만** 넣을 것.
        // 라이브러리는 자기가 받은 프레임을 빨리 끝낼 수 있을 뿐,
        // 큐에 쌓인 과거 프레임을 대신 버려줄 수는 없다.
    }

    vscan_destroy(p);
    return 0;
}

// ---- 아래 둘은 배치마다 다르므로 비워 둔다 -------------------------------
static bool grabFrame(std::vector<uint8_t>&, int&, int&) { return false; }
static void setExposure(int) {}
