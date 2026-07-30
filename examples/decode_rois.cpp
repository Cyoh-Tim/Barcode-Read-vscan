/*
 * decode_rois.cpp - vscan_process_gray_rois() 사용 예제 (설계 참고용)
 *
 * 상위 검출기(객체 탐지 모델, 사람이 지정한 관심영역 등)가 이미 대략적인
 * 좌표를 알고 있을 때, 그 좌표만 크롭해서 디코드하는 흐름을 보여준다.
 * 자체 locate 단계가 없어 vscan_process_gray()/vscan_process_gray_two_stage()
 * 보다 빠르다 — 좌표를 신뢰할 수 있다는 게 전제.
 *
 * 빌드: g++ -O3 -std=c++17 -I<repo>/include decode_rois.cpp -lvscan -o decode_rois
 * 실행: ./decode_rois industrial_sample.pgm
 *
 * 이 예제는 좌표를 하드코딩했지만, 실제로는 객체 탐지 모델의 출력이나
 * UI에서 사람이 그린 사각형 등을 rois 배열에 채우면 된다.
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include "vscan.h"

namespace {

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
        fclose(f);
        return false;
    }
    fgetc(f);
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

    PgmImage img;
    if (!loadPGM(argv[1], img)) return 1;
    printf("loaded %s: %dx%d\n", argv[1], img.width, img.height);

    vscan_pipeline_t* pipeline = vscan_create(nullptr);

    // 실제로는 여기 좌표를 상위 검출기/사람이 채워준다. 이 예제는
    // examples/sample_images/industrial_sample.pgm에 맞춘 하드코딩 값.
    vscan_rect_t rois[] = {
        {650, 550, 1020, 920},   // 라벨의 QR 대략 위치
        {1050, 590, 1600, 720},  // 라벨의 Code128 대략 위치
    };
    int roiPadPx = 20; // 좌표가 살짝 타이트해도 되도록 여유

    vscan_result_t* result = vscan_process_gray_rois(
        pipeline, img.pixels.data(), img.width, img.height, img.width,
        rois, sizeof(rois) / sizeof(rois[0]), roiPadPx);

    printf("\nROI %zu개에서 검출된 심볼: %zu개\n", sizeof(rois) / sizeof(rois[0]),
           result ? result->count : 0);
    for (size_t i = 0; result && i < result->count; ++i) {
        const vscan_symbol_t& sym = result->symbols[i];
        printf("[%s] %s\n", vscan_symbology_name(sym.symbology), sym.text);
    }

    vscan_free_result(result);
    vscan_destroy(pipeline);
    return 0;
}
