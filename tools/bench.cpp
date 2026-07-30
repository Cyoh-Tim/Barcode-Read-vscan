#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "vscan.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <raw_gray_file> <width> <height> [threads] [runs] [overlap_px] "
                "[symbology_mask_hex] [no_rotate 0|1] [no_invert 0|1] [no_try_harder 0|1]\n",
                argv[0]);
        return 1;
    }
    const char* path = argv[1];
    int width = atoi(argv[2]);
    int height = atoi(argv[3]);
    unsigned threads = argc > 4 ? (unsigned)atoi(argv[4]) : 0;
    int runs = argc > 5 ? atoi(argv[5]) : 5;
    int overlap = argc > 6 ? atoi(argv[6]) : 80;
    uint32_t formatMask = argc > 7 ? (uint32_t)strtoul(argv[7], nullptr, 16) : 0;
    int noRotate = argc > 8 ? atoi(argv[8]) : 0;
    int noInvert = argc > 9 ? atoi(argv[9]) : 0;
    int noTryHarder = argc > 10 ? atoi(argv[10]) : 0;

    std::vector<uint8_t> buf(static_cast<size_t>(width) * height);
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (n != buf.size()) { fprintf(stderr, "short read\n"); return 1; }

    vscan_config_t cfg{};
    cfg.tile_threads = threads;
    cfg.tile_overlap_px = overlap;
    cfg.enable_zbar_fastpath = 0;
    cfg.symbology_mask = formatMask;
    cfg.decode_flags = (noRotate ? VSCAN_FLAG_NO_ROTATE : 0u) | (noInvert ? VSCAN_FLAG_NO_INVERT : 0u) |
                        (noTryHarder ? VSCAN_FLAG_NO_TRY_HARDER : 0u);
    vscan_pipeline_t* p = vscan_create(&cfg);

    double totalMs = 0;
    for (int i = 0; i < runs; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        vscan_result_t* r = vscan_process_gray(p, buf.data(), width, height, width);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        printf("run %d: %.2f ms, symbols=%zu\n", i, ms, r ? r->count : 0);
        vscan_free_result(r);
    }
    printf("---\navg: %.2f ms over %d runs (threads=%u, overlap=%d, %dx%d = %.2fMP)\n",
           totalMs / runs, runs, threads, overlap, width, height, (width * height) / 1e6);

    vscan_destroy(p);
    return 0;
}
