/*
 * decode_image.cpp - vscan.h 공개 API 사용 예제 (설계 참고용)
 *
 * 목적: libvscan.so를 링크하는 애플리케이션이 어떻게 파이프라인을
 * 초기화하고, 프레임을 넣고, 결과를 순회하고, 리소스를 정리해야 하는지
 * 보여주는 최소 레퍼런스. 실제 V4L2 캡처 대신 PGM(P5) 정지 이미지를
 * 읽어서 처리한다 — 이미지 코덱 의존성 없이 순수 C++만으로 동작.
 *
 * 빌드: g++ -O3 -std=c++17 -I<repo>/include decode_image.cpp -lvscan -o decode_image
 * 실행: ./decode_image industrial_sample.pgm
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vscan.h"

namespace {

// 아주 단순한 PGM(P5, binary) 로더. 주석(#) 라인은 처리 안 함 —
// 데모용이라 실제 제품에선 더 견고한 로더를 쓸 것.
struct PgmImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

bool loadPGM(const char* path, PgmImage& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return false; }

    char magic[3] = {0};
    int maxval = 0;
    if (fscanf(f, "%2s", magic) != 1 || std::strcmp(magic, "P5") != 0) {
        fprintf(stderr, "지원하지 않는 포맷 (P5 PGM만 지원)\n");
        fclose(f);
        return false;
    }
    if (fscanf(f, "%d %d %d", &out.width, &out.height, &maxval) != 3) {
        fprintf(stderr, "PGM 헤더 파싱 실패\n");
        fclose(f);
        return false;
    }
    fgetc(f); // 헤더 뒤 개행 하나 소비

    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    size_t n = fread(out.pixels.data(), 1, out.pixels.size(), f);
    fclose(f);
    return n == out.pixels.size();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.pgm>\n", argv[0]);
        return 1;
    }

    printf("vscan version: %s\n\n", vscan_version());

    // 1) 이미지 로드 (실제 장비에서는 V4L2 캡처의 GrayView가 이 자리를 대체)
    PgmImage img;
    if (!loadPGM(argv[1], img)) return 1;
    printf("loaded %s: %dx%d (%.2f MP)\n", argv[1], img.width, img.height,
           (img.width * img.height) / 1e6);

    // 2) 파이프라인 생성. cfg를 nullptr로 넘기면 기본값(스레드 자동, overlap 500px).
    vscan_config_t cfg{};
    cfg.tile_threads = 0;            // 0 = hardware_concurrency() 자동
    cfg.tile_overlap_px = 500;
    cfg.enable_zbar_fastpath = 1;    // 1D fast-path 켜기 (VSCAN_USE_ZBAR로 빌드된 경우)
    // opt-in 심볼로지는 환경변수로 켠다 — 이 예제를 그 심볼로지 확인에도
    // 쓸 수 있게 하려는 것뿐이고, 실제 앱은 cfg 필드를 직접 채우면 된다.
    cfg.enable_industrial_2of5 = getenv("VSCAN_IND25") != nullptr;
    cfg.enable_coop_2of5       = getenv("VSCAN_COOP25") != nullptr;
    cfg.enable_pharmacode      = getenv("VSCAN_PHARMA") != nullptr;
    if (const char* mb = getenv("VSCAN_PHARMA_MINBARS")) cfg.pharmacode_min_bars = atoi(mb);
    cfg.enable_micro_pdf417    = getenv("VSCAN_MPDF") != nullptr;
    vscan_pipeline_t* pipeline = vscan_create(&cfg);
    if (!pipeline) { fprintf(stderr, "vscan_create 실패\n"); return 1; }

    // 3) 디코드 (zero-copy: img.pixels 버퍼를 그대로 넘김, 복사 없음)
    vscan_result_t* result = vscan_process_gray(
        pipeline, img.pixels.data(), img.width, img.height, /*stride=*/img.width);

    if (!result) {
        fprintf(stderr, "디코드 실패\n");
        vscan_destroy(pipeline);
        return 1;
    }

    // 4) 결과 순회
    printf("\n검출된 심볼: %zu개\n", result->count);
    printf("----------------------------------------\n");
    for (size_t i = 0; i < result->count; ++i) {
        const vscan_symbol_t& sym = result->symbols[i];
        printf("[%s] %s\n", vscan_symbology_name(sym.symbology), sym.text);
        printf("  위치(꼭짓점): (%d,%d) (%d,%d) (%d,%d) (%d,%d)\n",
               sym.corners[0].x, sym.corners[0].y, sym.corners[1].x, sym.corners[1].y,
               sym.corners[2].x, sym.corners[2].y, sym.corners[3].x, sym.corners[3].y);
        if (sym.is_gs1) {
            printf("  GS1 페이로드로 판단됨 (raw_bytes_len=%zu)\n", sym.raw_bytes_len);
        }
        printf("----------------------------------------\n");
    }

    // 5) 정리 - 결과와 파이프라인은 각각 반드시 해제
    vscan_free_result(result);
    vscan_destroy(pipeline);
    return 0;
}
