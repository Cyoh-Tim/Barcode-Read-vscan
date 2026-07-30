/*
 * vscan.h - vscan-lite 공개 C API
 *
 * libvscan.so 하나만 링크하면 되는 순수 C 인터페이스.
 * 내부적으로 zxing-cpp/zbar(C++)를 쓰지만 이 헤더에는 그 흔적이 전혀 없다.
 * 즉 이 헤더 + libvscan.so 두 개만 배포하면, 소비자는 zxing/zbar 헤더나
 * C++ ABI 걱정 없이 코드를 읽을 수 있다.
 *
 * 스레드 안전성: vscan_pipeline_t 하나를 여러 스레드에서 동시에
 * vscan_process_gray()로 호출하는 것은 안전하지 않다(내부에 이미 자체
 * 스레드풀을 쓰므로, 인스턴스당 단일 호출자를 권장).
 */
#ifndef VSCAN_H
#define VSCAN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 벤치마킹 대상 스펙 기준 심볼로지. 값은 ABI 호환을 위해 절대 재배치하지 말고
 * 새 항목은 항상 끝에 추가할 것. */
typedef enum {
    VSCAN_SYM_UNKNOWN = 0,
    VSCAN_SYM_QR,
    VSCAN_SYM_MICRO_QR,
    VSCAN_SYM_DATA_MATRIX,
    VSCAN_SYM_GS1_DATA_MATRIX,
    VSCAN_SYM_PDF417,
    VSCAN_SYM_MICRO_PDF417,
    VSCAN_SYM_GS1_COMPOSITE,   /* 미구현: 항상 검출되지 않음 */
    VSCAN_SYM_DOTCODE,         /* 미구현 */
    VSCAN_SYM_CODE39,
    VSCAN_SYM_CODE39_FULL_ASCII,
    VSCAN_SYM_TRIOPTIC_CODE39,
    VSCAN_SYM_ITF,
    VSCAN_SYM_INDUSTRIAL_2OF5,
    VSCAN_SYM_COOP_2OF5,
    VSCAN_SYM_CODABAR,
    VSCAN_SYM_CODE128,
    VSCAN_SYM_GS1_128,
    VSCAN_SYM_GS1_DATABAR,
    VSCAN_SYM_CODE93,
    VSCAN_SYM_EAN_UPC,
    VSCAN_SYM_PHARMACODE,      /* 미구현 */
    VSCAN_SYM_POSTAL_JAPAN,    /* 미구현 */
    VSCAN_SYM_POSTAL_IMB,      /* 미구현 */
} vscan_symbology_t;

typedef struct {
    int x, y;
} vscan_point_t;

typedef struct {
    vscan_symbology_t symbology;
    const char* text;          /* NUL 종료, vscan_result_t 수명 동안만 유효 */
    const uint8_t* raw_bytes;  /* GS1 등 이진 페이로드, NULL 가능 */
    size_t raw_bytes_len;
    vscan_point_t corners[4];  /* top-left, top-right, bottom-right, bottom-left */
    int is_gs1;                /* 0/1 */
} vscan_symbol_t;

typedef struct {
    vscan_symbol_t* symbols;
    size_t count;
    void* _internal;           /* vscan_free_result()가 쓰는 내부 핸들, 건드리지 말 것 */
} vscan_result_t;

/* symbology_mask 비트. 0(미설정)이면 전체 포맷 검색(기본, 안전한 값) —
 * 실제 배치에서 쓰는 심볼로지만 켜면 zxing-cpp의 포맷별 탐색 비용이
 * 그만큼 줄어든다(측정치: 전체 포맷 대비 QR 단독 지정 시 동일 프레임에서
 * 약 2.5배 추가 가속, TryRotate/TryInvert 끄기와는 별개 효과 — 2026-07-24
 * 벤치, [[vscan-lite-speed-tuning]] 참고). 값은 ABI 호환을 위해 재배치 금지,
 * 새 항목은 항상 끝에 추가할 것. */
#define VSCAN_FMT_QR                (1u << 0)
#define VSCAN_FMT_MICRO_QR          (1u << 1)
#define VSCAN_FMT_DATA_MATRIX       (1u << 2)
#define VSCAN_FMT_PDF417            (1u << 3)
#define VSCAN_FMT_CODE39            (1u << 4)
#define VSCAN_FMT_CODE93            (1u << 5)
#define VSCAN_FMT_CODE128           (1u << 6)
#define VSCAN_FMT_ITF               (1u << 7)
#define VSCAN_FMT_CODABAR           (1u << 8)
#define VSCAN_FMT_DATABAR           (1u << 9)
#define VSCAN_FMT_DATABAR_EXPANDED  (1u << 10)
#define VSCAN_FMT_EAN13             (1u << 11)
#define VSCAN_FMT_EAN8              (1u << 12)
#define VSCAN_FMT_UPCA              (1u << 13)
#define VSCAN_FMT_UPCE              (1u << 14)
#define VSCAN_FMT_ALL               0u /* symbology_mask 기본값과 동일 표기 */

/* decode_flags 비트. 0(미설정)이면 TryRotate/TryInvert 둘 다 켜짐(기본,
 * 안전한 값 — 코드가 기울어지거나 반전(흑백 반전) 인쇄될 수 있는 배치라면
 * 반드시 이 기본값을 유지할 것). 고정 마운트 + 항상 같은 방향/극성으로
 * 인쇄되는 게 보장된 배치에서만 꺼서 속도를 얻는 트레이드오프.
 * (측정치: TryRotate 끄면 약 2배, TryInvert까지 끄면 소폭 추가 — 2026-07-24
 * 벤치, [[vscan-lite-speed-tuning]] 참고) */
#define VSCAN_FLAG_NO_ROTATE      (1u << 0) /* 회전 탐색 생략 — 코드가 항상 수평일 때만 */
#define VSCAN_FLAG_NO_INVERT      (1u << 1) /* 반전(흑백 반전) 탐색 생략 */
/* zxing-cpp TryHarder 끄기 — 다중 이진화/후보 패스를 줄인다. 대비가 낮거나
 * 흐릿하거나 손상된 코드는 놓칠 수 있다(측정치: 전체 포맷 기준 추가로
 * 약 40% 더 가속, TryRotate/TryInvert와는 독립적 효과 —
 * [[vscan-lite-speed-tuning]] 참고) */
#define VSCAN_FLAG_NO_TRY_HARDER  (1u << 2)

typedef struct {
    /* 0이면 hardware_concurrency() 자동 사용 (i.MX8M Plus면 보통 4) */
    unsigned tile_threads;
    /* 타일 경계 겹침(px), 기본 500 권장 (i.MX8MP 4코어 실기 QR ~460-600px
     * 세로 크기 기준 실측 — 작업거리/해상도가 다르면 재튜닝 필요) */
    int tile_overlap_px;
    /* 1이면 ZBar를 1D fast-path로 추가 등록 (라이브러리가 VSCAN_USE_ZBAR로
     * 빌드됐을 때만 효과 있음. 꺼져있으면 무시된다) */
    int enable_zbar_fastpath;
    /* VSCAN_FMT_* 비트 OR. 0 = 전체 포맷(기본, 안전) */
    uint32_t symbology_mask;
    /* VSCAN_FLAG_NO_* 비트 OR. 0 = 회전/반전/TryHarder 다 켜짐(기본, 안전) */
    uint32_t decode_flags;

    /*
     * 워커 모드. 1이면 이 파이프라인은 내부에서 스레드를 전혀 만들지 않는다
     * (tile_threads 값과 무관하게 완전 단일 스레드로 동작).
     *
     * 애플리케이션이 "워커 N개가 각자 1코어에서 프레임 하나씩 처리"하는
     * 구조일 때 반드시 켤 것. 끄면 tile_threads=0(자동)이
     * hardware_concurrency()로 잡혀서, 워커 3개 x 내부 4스레드 = 12스레드가
     * 3코어를 놓고 경쟁한다(과다구독).
     * 실측(x86, 워커3, 32종x4라운드): 워커 모드 52.1 fps vs 과다구독 32.1 fps
     * — 62% 손해. [[vscan-lite-worker-mode]]
     *
     * 각 워커는 자기 vscan_pipeline_t를 하나씩 만들어 재사용하면 된다.
     * 서로 다른 pipeline 인스턴스는 동시 사용해도 안전하다(실측: 워커3
     * 동시 128건 처리, 단일 스레드 기준 결과와 불일치 0건).
     * 하나의 pipeline 인스턴스를 여러 스레드가 동시에 쓰는 것은 미검증.
     */
    int worker_mode;

    /*
     * vscan_process_gray_two_stage()의 1단계(위치 탐색) 전용 속도/검출력
     * 손잡이. 0 = 기본(검출력 우선). 1 = 빠른 모드.
     *
     * 빠른 모드는 GlobalHistogram 이진화 + 내장 다운스케일 패스 생략으로
     * 1단계가 2~3배 빨라지지만, 작은 코드(화면상 90px 이하)와 저대비
     * 코드를 놓친다(8종 테스트 이미지 실측: 검출 7/8 -> 5/8).
     * 배치 환경의 코드가 충분히 크고 선명하다는 게 확인된 뒤에만 켤 것.
     * 이 값은 vscan_process_gray()/vscan_process_gray_rois()에는 영향 없다.
     */
    int fast_locate;

    /*
     * 빠른 경로(two_stage의 1단계, tracked의 ROI 단계)가 이 개수 미만을
     * 찾으면 자동으로 풀스캔까지 승격한다. 0 또는 1 = 기본 동작
     * ("하나도 못 찾으면 폴백").
     *
     * 아이템당 코드 개수가 정해진 현장(모든 박스에 라벨 N장)이라면 N으로
     * 설정할 것. 품질이 섞인 다중 코드(깨끗한 코드 + 반전/저대비 코드)에서
     * 빠른 경로가 쉬운 것만 찾고 끝내는 부분 검출을 막아, 검출력이
     * vscan_process_gray()와 동일해진다.
     * 개수를 모르는 현장이라면 이 옵션으로는 부분 검출을 못 막는다 —
     * 그런 조건에서 모든 코드가 반드시 필요하면 vscan_process_gray()를 쓸 것.
     */
    int min_expected_codes;

    /*
     * [coarse locate] 빠른 경로의 위치 탐색을 축소본에서 먼저 시도한다.
     * 0 = 기본(켜짐). 1 = 끔.
     *
     * 이진화가 탐색 비용의 대부분이고 픽셀 수에 비례하므로, 1/3 축소본이면
     * 1/9이 된다. 실측(악조건 36종): two_stage 평균 11.4ms -> 4.9ms.
     * 축소본에서 못 찾으면 자동으로 풀해상도 -> 풀스캔으로 승격하므로
     * **검출률과 디코딩 텍스트는 완전히 동일하다**(36종 33/36 유지, 텍스트
     * 불일치 0건).
     *
     * 단 하나의 트레이드오프: 축소본에서 찾은 경우 반환되는 **꼭짓점 좌표의
     * 정밀도**가 약간 떨어진다(실측 96%가 2px 이내, 회전/원근 코드에서
     * 최대 14px). 좌표를 로봇 피킹 같은 정밀 용도로 쓴다면 끌 것.
     * 추적 ROI(패딩 120px 이상)나 화면 표시 용도라면 무시해도 되는 수준.
     *
     * 축소본이 연속 3회 실패하면 30프레임 동안 자동으로 건너뛴다 —
     * 작은 코드만 나오는 배치에서 매 프레임 헛수고하지 않도록.
     */
    int disable_coarse_locate;

    /*
     * coarse locate 축소 배율. 0 또는 3 = 기본(1/3). 2 = 1/2 축소.
     * 1/3은 화면상 ~90px 코드를 놓치고(1/2는 잡음) 대신 1.6배 빠르다.
     * 배치 코드가 화면에서 100px 안팎으로 작게 잡히면 2로 낮출 것.
     */
    int coarse_factor;

    /*
     * [1D 바코드 회전 구제] 20~75도 부근 회전된 1D 바코드(zxing이 풀옵션
     * 으로도 못 읽는 구간 — 실물 산업용 리더기에서는 되는 것이 스크린샷으로
     * 확인됨)를 구제하는 최종 5단계. 0 = 기본(켜짐). 1 = 끔.
     * 다른 4단계가 전부 실패했을 때만 도는 비용이라 정상 프레임에는
     * 영향이 없다. 회전 각도가 나올 수 있는 배치 환경이면 켜둘 것.
     */
    int disable_1d_deskew_rescue;

    /*
     * [DPM/점각인 구제] 레이저 도트 각인 코드의 이진화 실패를 구제하는
     * 최종 단계. 실물 비교 대상 리더기 대조 검증까지 완료됨.
     *
     * 0 = 기본(꺼짐). 1 = 켬.
     *
     * ⚠️ 이건 프레임 단위로 자동 판별하는 기능이 아니다 — DPM과
     * 무관한 실패(저대비/강한원근/1D회전 등)에서도 매번 켜져 있으면
     * 헛되이 추가 비용(~100ms)을 태운다. 신뢰할 만한 "이 프레임이
     * DPM 같다"는 사전 판별기를 만들려 했으나(그래디언트 방향
     * 일관성, 블롭 개수 등) 다른 실패 유형과 깔끔히 안 갈렸다.
     *
     * 이런 류의 상용 리더도 운영자한테 노출된 "DPM 켜기" 스위치는 없지만
     * 설치 시 그 라인의 실제 샘플로 뱅크를 튜닝하는 과정이 있다 —
     * 이 옵션도 그런 성격이다. **현장 운영자가 매번 만지는 게
     * 아니라, 상위 시스템의 설치/튜닝 단계가 그 배치 환경이 DPM을
     * 쓰는 라인인지 실제 샘플로 한 번 판단해서 고정하는 값**으로
     * 쓸 것.
     */
    int enable_dpm_rescue;

    /*
     * [프레임 시간 예산, ms] 0 = 무제한(기본, 기존 동작 그대로).
     *
     * 실측(§3.9): **전체 디코드 시간의 51%가 "결국 하나도 못 찾는" 프레임**에
     * 쓰인다(성공 프레임 평균 56ms vs 실패 프레임 150~209ms). 폴백 체인을
     * 끝까지 돌고 실패하는 비용이라, 이 값을 두면 그 꼬리를 자를 수 있다.
     *
     * 동작: 폴백 **단계 경계마다** 경과 시간을 확인해서 예산을 넘겼으면 남은
     * 단계를 생략하고 그때까지 찾은 결과를 반환한다.
     * - 빠르게 성공하는 프레임은 전혀 영향받지 않는다.
     * - 실행 중인 단계를 중간에 끊지는 않으므로 실제 소요는 예산을 한 단계
     *   만큼 넘을 수 있다. 하드 리얼타임 보장이 아니라 꼬리를 자르는 장치다.
     * - 컨베이어처럼 프레임 주기가 정해진 배치라면 주기의 60~80%가 보통 맞다.
     *   예산을 너무 낮게 잡으면 어려운 코드의 검출률이 떨어진다(트레이드오프
     *   곡선은 §3.11 참고).
     */
    int max_frame_ms;
} vscan_config_t;

/* cfg가 NULL이면 기본값(threads=auto, overlap=500, zbar=off,
 * symbology_mask=전체, decode_flags=회전/반전 탐색 켜짐) 사용 */
typedef struct vscan_pipeline vscan_pipeline_t;
vscan_pipeline_t* vscan_create(const vscan_config_t* cfg);
void vscan_destroy(vscan_pipeline_t* pipeline);

/*
 * 그레이스케일(8bit, 1 byte/px) 버퍼를 그대로(zero-copy) 디코드한다.
 * pixels는 이 함수가 리턴할 때까지만 유효하면 된다(내부에서 복사 안 함).
 * stride가 0이면 width와 같다고 간주.
 * 반환값은 malloc된 vscan_result_t*이며, 사용 후 반드시 vscan_free_result().
 * 실패 시 NULL.
 */
vscan_result_t* vscan_process_gray(vscan_pipeline_t* pipeline,
                                    const uint8_t* pixels,
                                    int width, int height, int stride);

/*
 * [선택: 속도/정확도 트레이드오프] 2단계 locate-then-refine 디코드.
 * 1단계: pipeline의 symbology_mask/tile_threads/tile_overlap_px는 유지하되
 *        TryHarder/회전/반전 탐색은 강제로 꺼서 빠르게 후보 위치만 찾는다.
 * 2단계: 각 후보 주변을 crop_pad_px만큼 여유 두고 crop(zero-copy)해서
 *        TryHarder/회전/반전을 전부 켠 채로 재디코드(후보가 여럿이면 병렬).
 *
 * 실측(i.MX8MP 3코어, 2048x1536): 순수 vscan_process_gray() 545ms ->
 * 이 함수 130-200ms.
 *
 * 주의: 1단계가 후보 위치조차 못 찾으면(예: 반전 극성 코드) 2단계는 그
 * 코드를 시도할 기회가 없다 — vscan_process_gray()(순수 풀옵션 풀프레임)와
 * 정확도가 완전히 동일하지 않다. 합성 열화 이미지 검증 결과 흑백 반전
 * 케이스에서만 차이가 확인됐다(블러/회전/저대비는 vscan_process_gray()와
 * 동일하게 실패/성공함). 실제 배치 조건 검증 전에는 vscan_process_gray()를
 * 기본으로 쓸 것.
 */
vscan_result_t* vscan_process_gray_two_stage(vscan_pipeline_t* pipeline,
                                              const uint8_t* pixels,
                                              int width, int height, int stride,
                                              int crop_pad_px);

/* 이미지 좌표계 사각 영역. x0<x1, y0<y1. */
typedef struct {
    int x0, y0, x1, y1;
} vscan_rect_t;

/*
 * 외부에서 이미 알고 있는 ROI 좌표로 디코드한다(예: 상위 검출기가 준 라벨
 * 위치, 사람이 지정한 관심영역). vscan_process_gray_two_stage()와 달리
 * 자체 locate 단계가 없다 — rois를 그대로 신뢰하고 각 영역만 crop해서
 * pipeline에 설정된 옵션(symbology_mask/decode_flags) 그대로 디코드한다.
 * rois가 여럿이면 서로 독립적이므로 병렬로 처리된다.
 *
 * pad_px만큼 각 영역 주변에 여유를 둔다 — 외부 좌표가 코드 경계에 딱
 * 맞아떨어지지 않을 때(바운딩박스가 약간 타이트한 경우 등)를 위한 안전마진.
 * 좌표가 이미 정확하다고 확신하면 0으로 줘도 된다.
 */
vscan_result_t* vscan_process_gray_rois(vscan_pipeline_t* pipeline,
                                         const uint8_t* pixels,
                                         int width, int height, int stride,
                                         const vscan_rect_t* rois, size_t roi_count,
                                         int pad_px);

void vscan_free_result(vscan_result_t* result);

/*
 * [연속 프레임 추적 모드] 직전 호출에서 검출된 위치 주변 ROI만 먼저
 * 시도하고, 비어있으면 풀스캔(vscan_process_gray_two_stage와 동일)한다.
 * 컨베이어처럼 프레임 간 코드 위치가 조금씩만 이동하는 환경에서 크게
 * 빨라진다(실측 1.9배, 검출률 동일 — PROJECT_NOTES §3.2.8).
 *
 * track_pad_px      : 프레임 간 최대 이동량보다 크게 (권장 120 이상).
 * full_scan_interval: N프레임마다 풀스캔을 강제한다. ROI 추적만 하면
 *   "새로 화면에 진입한 코드"를 놓치므로, 새 코드 검출 지연의 상한이
 *   이 값이 된다. 0 = 강제 풀스캔 없음(코드 개수가 절대 안 변하는
 *   환경에서만). 권장 기본값 10.
 *
 * 추적 상태는 pipeline 인스턴스에 저장된다(워커당 인스턴스 1개 구조와
 * 자연스럽게 맞음). 라인이 멈추거나 아이템이 바뀌면 상태가 오염될 수
 * 있는데, 그 경우에도 ROI가 빈손이 되면 자동으로 풀스캔하므로 검출은
 * 유지된다(속도만 일시 하락).
 */
vscan_result_t* vscan_process_gray_tracked(vscan_pipeline_t* pipeline,
                                            const uint8_t* pixels,
                                            int width, int height, int stride,
                                            int track_pad_px,
                                            int full_scan_interval);

/* 심볼로지 이름 문자열 (정적 문자열, free 불필요) */
const char* vscan_symbology_name(vscan_symbology_t s);

/* 라이브러리 버전 문자열 (정적 문자열, free 불필요) */
const char* vscan_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VSCAN_H */
