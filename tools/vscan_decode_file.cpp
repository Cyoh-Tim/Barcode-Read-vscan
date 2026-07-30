/*
 * vscan_decode_file.cpp - ultravc CLI 비교 테스트용 스탠드얼론 디코더 (영구 워커 모드).
 *
 * ultravc(vil/atBR)가 이미 자체 Zxing20/Zbar를 링크하고 있어서, vscan-lite의
 * (다른 버전의) Zxing/Zbar를 ultravc 실행파일에 직접 링크하면 심볼 충돌
 * 위험이 있다. 그래서 vscan-lite는 항상 별도 프로세스로 실행하고, ultravc
 * 쪽 CLI 명령(vscantest)이 이 바이너리와 파이프로 통신한다 — 링크 타임
 * 충돌이 전혀 없다.
 *
 * 2026-07-27: 매 프레임 fork+exec 하던 방식(프로세스 생성+동적 링킹 비용이
 * 프레임마다 ~29ms 고정으로 붙는 게 실측으로 확인됨)에서, 프로세스를 한 번만
 * 띄우고 살려둔 채 파이프로 프레임을 반복해서 받는 "영구 워커" 방식으로
 * 바꿨다. width/height는 프로세스 수명 내내 고정(카메라 해상도가 안 바뀌므로
 * argv로 한 번만), ROI는 매 프레임 달라질 수 있으므로 프레임 바이트 앞에
 * 텍스트 헤더 한 줄을 얹는다.
 *
 * 사용법: vscan_decode_file <width> <height> [tile_threads] [crop_pad_px]
 *
 * stdin 프로토콜 (한 프레임당):
 *   헤더 한 줄(개행으로 끝) 다음 grayscale raw 바이트(width*height, 헤더 없음):
 *     "FRAME\n"                        - 일반 two-stage 디코드 (pad=argv의 crop_pad_px)
 *     "ROI <x0> <y0> <x1> <y1> <pad>\n" - 해당 영역만 디코드 (locate 단계 없음)
 *   stdin이 EOF면(파이프가 닫히면) 정상 종료(exit 0) — 부모가 워커를 내릴 때.
 *
 * stdout 프로토콜 (매 프레임 응답, 이전과 동일한 포맷 — ultravc 쪽 파서
 * (vbr_vscantest.cpp)가 그대로 의존하므로 바꾸지 않는다):
 *   "<symbology>\t<text>\n"  (심볼당 한 줄, 0개 이상)
 *   "STAT\t<ms>\t<count>\n"  (프레임마다 마지막 줄, 다음 프레임 응답과 구분하는 경계 역할)
 * stderr: 심볼당 바운딩박스 "BBOX\t<symbology>\t<x0>\t<y0>\t<x1>\t<y1>\n" (진단용,
 *   운영 파서는 stdout만 읽으므로 wire 포맷에 영향 없음)
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vscan.h"

namespace {

void emitResult(vscan_result_t* r, double ms) {
    size_t count = r ? r->count : 0;
    for (size_t i = 0; i < count; ++i) {
        const vscan_symbol_t& sym = r->symbols[i];
        printf("%s\t%s\n", vscan_symbology_name(sym.symbology), sym.text);

        int minx = sym.corners[0].x, maxx = sym.corners[0].x;
        int miny = sym.corners[0].y, maxy = sym.corners[0].y;
        for (int c = 1; c < 4; ++c) {
            minx = std::min(minx, sym.corners[c].x); maxx = std::max(maxx, sym.corners[c].x);
            miny = std::min(miny, sym.corners[c].y); maxy = std::max(maxy, sym.corners[c].y);
        }
        fprintf(stderr, "BBOX\t%s\t%d\t%d\t%d\t%d\n", vscan_symbology_name(sym.symbology), minx, miny, maxx, maxy);
    }
    printf("STAT\t%.2f\t%zu\n", ms, count);
    fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <width> <height> [tile_threads] [crop_pad_px]\n", argv[0]);
        return 1;
    }
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    unsigned threads = argc > 3 ? (unsigned)atoi(argv[3]) : 1;
    int defaultPad = argc > 4 ? atoi(argv[4]) : 50;

    vscan_config_t cfg{};
    cfg.tile_threads = threads;
    cfg.tile_overlap_px = 500;
    // Manual-bench-only overrides (env, not argv/wire — the production caller
    // in vbr_vscantest.cpp never sets these, so this only affects a manual
    // SSH invocation). 0/unset = full/safe options, same as always.
    if (const char* df = getenv("VSCAN_DECODE_FLAGS")) cfg.decode_flags = (uint32_t)strtoul(df, nullptr, 0);
    if (const char* sm = getenv("VSCAN_SYMBOLOGY_MASK")) cfg.symbology_mask = (uint32_t)strtoul(sm, nullptr, 0);
    if (const char* zb = getenv("VSCAN_ENABLE_ZBAR")) cfg.enable_zbar_fastpath = atoi(zb);

    std::vector<uint8_t> buf(static_cast<size_t>(width) * height);
    char header[128];

    while (fgets(header, sizeof(header), stdin) != nullptr) {
        size_t n = fread(buf.data(), 1, buf.size(), stdin);
        if (n != buf.size()) {
            fprintf(stderr, "short read: got %zu, expected %zu\n", n, buf.size());
            break;
        }

        bool roiMode = strncmp(header, "ROI", 3) == 0;
        vscan_rect_t roi{};
        int roiPad = defaultPad;
        if (roiMode) {
            sscanf(header, "ROI %d %d %d %d %d", &roi.x0, &roi.y0, &roi.x1, &roi.y1, &roiPad);
        }

        // Fresh Pipeline every frame — NOT reused across the process's
        // lifetime. Pipeline carries adaptive state across calls on the same
        // instance (coarseMisses_/coarseSkipLeft_: after 3 consecutive
        // coarse-locate misses it skips coarse-locate for the next 30 frames
        // — see pipeline.hpp). That's fine for a single long-lived caller
        // that decodes a steady stream of similar frames, but this process
        // now stays alive across whatever the camera saw before (e.g. a
        // stretch with no code in view rack up 3 misses), so a *later*,
        // perfectly clear frame could silently skip coarse-locate and land
        // on a slower path for no reason visible in the image itself.
        // vscan_create()/vscan_destroy() are plain C++ object lifecycle (no
        // fork/exec/dlopen — that cost was eliminated by keeping the
        // *process* alive, not this object), so recreating per frame is
        // microseconds, not milliseconds: keeps the persistent-process win
        // while restoring the old fork-per-frame model's per-frame
        // independence.
        vscan_pipeline_t* p = vscan_create(&cfg);

        auto t0 = std::chrono::steady_clock::now();
        vscan_result_t* r = roiMode
            ? vscan_process_gray_rois(p, buf.data(), width, height, width, &roi, 1, roiPad)
            : vscan_process_gray_two_stage(p, buf.data(), width, height, width, defaultPad);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        emitResult(r, ms);
        vscan_free_result(r);
        vscan_destroy(p);
    }

    return 0;
}
