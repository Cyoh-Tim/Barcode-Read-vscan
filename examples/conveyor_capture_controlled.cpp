/*
 * conveyor_capture_controlled.cpp
 *
 * **노출/조명을 통제할 수 있는 컨베이어 배치**의 권장 설정을 그대로 담은
 * 예제. 2026-08-03에 §3.62~§3.74로 정리된 결론을 코드로 옮긴 것이다.
 *
 * ## 이 설정이 겨냥하는 문제
 *
 * 컨베이어에서 진짜 문제는 "어려운 코드를 못 읽는다"가 아니라
 * **"못 읽겠다는 판정에 너무 오래 걸린다"** 인 경우가 많다. 그 사이
 * 프레임이 큐에 쌓이고, 큐에 쌓인 이미지는 이미 지나간 물건의 사진이라
 * 읽어도 늦다. 실기 로그에서 성공 15ms / 실패 5,000~6,500ms였다.
 *
 * ## 실측 (1280x960, QR만 있는 저조도 프레임 24장, 2단계, reps 3)
 *
 *   설정                          코드   판정 최대   보드 환산(x8)
 *   기본                           25     366ms      2,928ms
 *   + symbology_mask (§3.62)       25     134          1,072
 *   + fast_no_read   (§3.63)       23      50            400
 *   + 이진화 기본 변경 (§3.65)      30      27            217
 *   + 최종 승격 생략  (§3.66)       30    18.6            149
 *   + NO_INVERT      (§3.68)       30     9.7             78
 *
 * 검출이 25 -> 30으로 **오르면서** 판정이 366 -> 9.7ms가 됐다.
 * 성공 프레임은 2.4ms(보드 19ms)다.
 *
 * 워커 모드(워커당 1스레드)에서도 같다 — 실측 8.9 vs 8.8ms.
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
    //     이미지 수술 단계(큰코드폴백/평탄화/영역구제/노이즈구제/최종승격)를
    //     끊는다. 한 장을 오래 쥐어짜는 대신 빨리 포기하고 다시 찍는다.
    cfg.fast_no_read = 1;

    // [3] 반전 탐색 생략 — **극성이 항상 같을 때만.**
    //     판정 경로에서 가장 비싼 단계다(실측 7.5~9.8ms).
    cfg.decode_flags = VSCAN_FLAG_NO_INVERT;

    // [4] 워커 구조라면 반드시. 워커 N개 x 내부 4스레드가 코어를 놓고
    //     경쟁하는 과다구독을 막는다(실측 62% 손해).
    cfg.worker_mode = 1;

    // [선택] 마감. 위 셋을 넣고도 꼬리가 남을 때만 쓴다 — 이건 결과를
    // 잘라서 시간을 사는 것이라 대가가 있다. 값은 자기 프레임에서 잴 것.
    // cfg.max_frame_ms = 100;

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
