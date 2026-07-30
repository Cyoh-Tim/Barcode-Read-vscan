/*
 * bench_device.cpp — 실기(i.MX8M Plus) 캘리브레이션 벤치.
 *
 * 이 프로젝트의 시간 기반 트레이드오프 결정들은 전부 x86 샌드박스에서
 * 측정된 것이다. 검출률 결론은 플랫폼 무관(zxing은 결정론적)이지만,
 * "어느 쪽이 빠른가"는 A53에서 뒤집힐 수 있다 (PROJECT_NOTES §6.5).
 * 이 도구는 그 결정 지점들을 실기에서 한 번에 재측정한다.
 *
 * 준비: 개발 PC에서 python3 tools/generate_stress_images.py --outdir stress
 *       후 stress/ 디렉토리를 보드로 복사.
 * 빌드: (SDK env source 후) $CXX -O3 -std=c++17 -Iinclude \
 *         tools/bench_device.cpp -Lbuild-aarch64 -lvscan -lpthread -o bench_device
 * 실행: ./bench_device ./stress
 *
 * 각 섹션 출력 옆의 [x86] 값과 비교해서, 결론이 뒤집힌 항목만
 * PROJECT_NOTES에 기록하고 설정을 조정하면 된다.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

#include "vscan_internal/pipeline.hpp"
#include "vscan.h"

using namespace vscan;
using Clk = std::chrono::steady_clock;
static const int W = 2048, H = 1536;

static double msSince(Clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
}

struct Img { std::string name; std::vector<uint8_t> px; };

static std::vector<Img> loadDir(const char* dir) {
    std::vector<Img> out;
    DIR* d = opendir(dir);
    if (!d) { perror("opendir"); return out; }
    std::vector<std::string> files;
    for (dirent* e; (e = readdir(d)); ) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".pgm") files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& fn : files) {
        FILE* f = fopen((std::string(dir) + "/" + fn).c_str(), "rb");
        if (!f) continue;
        char l[256];
        for (int i = 0; i < 3; ++i) if (!fgets(l, sizeof(l), f)) break;
        Img im; im.name = fn; im.px.resize((size_t)W * H);
        if (fread(im.px.data(), 1, im.px.size(), f) == im.px.size()) out.push_back(std::move(im));
        fclose(f);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <stress_image_dir>\n", argv[0]); return 1; }
    auto imgs = loadDir(argv[1]);
    if (imgs.empty()) { fprintf(stderr, "no pgm images\n"); return 1; }
    printf("이미지 %zu장 로드. HW 동시성=%u\n\n", imgs.size(), std::thread::hardware_concurrency());

    // 대표 이미지: baseline이 있으면 그것, 없으면 첫 장
    const Img* base = &imgs[0];
    for (auto& im : imgs) if (im.name.find("baseline") != std::string::npos) base = &im;
    GrayView baseView(base->px.data(), W, H, W);

    // ---------------------------------------------------------------
    printf("[A] locate 단계 이진화 — 결론이 뒤집히면 fast_locate 손익도 재평가\n");
    printf("    [x86] LocalAverage 8.1ms / GlobalHistogram 1.6ms (5배 차)\n");
    {
        struct C { const char* n; ZXingDecoder::Binarizer b; };
        C cs[] = {{"LocalAverage   ", ZXingDecoder::Binarizer::LocalAverage},
                  {"GlobalHistogram", ZXingDecoder::Binarizer::GlobalHistogram}};
        for (auto& c : cs) {
            PipelineConfig pc; pc.tileThreads = 1;
            pc.tryHarder = false; pc.tryRotate = false; pc.tryInvert = false;
            pc.binarizer = c.b;
            Pipeline p(pc);
            double best = 1e9;
            for (int i = 0; i < 5; ++i) {
                auto t0 = Clk::now();
                auto r = p.processView(baseView);
                double m = msSince(t0);
                if (m < best) best = m;
            }
            printf("    %s : %7.2f ms\n", c.n, best);
        }
    }

    // ---------------------------------------------------------------
    printf("\n[B] tracked ROI 단계 비용 — 3단 구조의 손익 확인\n");
    printf("    [x86] fast ROI 1.2ms / full ROI 5.2ms / 풀스캔 13ms+\n");
    {
        // 코드 위치 자체 캘리브레이션 (풀스캔으로 찾은 위치 사용)
        PipelineConfig full; full.tileThreads = 1;
        Pipeline pf(full);
        auto found = pf.processView(baseView);
        if (found.empty()) {
            printf("    (대표 이미지에서 코드를 못 찾아 생략)\n");
        } else {
            int minX = W, minY = H, maxX = 0, maxY = 0;
            for (auto& pt : found[0].symbol.position) {
                minX = std::min(minX, pt.first);  maxX = std::max(maxX, pt.first);
                minY = std::min(minY, pt.second); maxY = std::max(maxY, pt.second);
            }
            std::vector<Rect> roi{{minX, minY, maxX, maxY}};
            struct C { const char* n; bool fastOpts; };
            C cs[] = {{"fast ROI (H/R/I OFF)", true}, {"full ROI (풀옵션)  ", false}};
            for (auto& c : cs) {
                PipelineConfig pc; pc.tileThreads = 1;
                if (c.fastOpts) { pc.tryHarder = false; pc.tryRotate = false; pc.tryInvert = false; }
                Pipeline p(pc);
                double best = 1e9;
                for (int i = 0; i < 10; ++i) {
                    auto t0 = Clk::now();
                    auto r = p.processViewROIs(baseView, roi, 120);
                    double m = msSince(t0);
                    if (m < best) best = m;
                }
                printf("    %s : %7.2f ms\n", c.n, best);
            }

            // -----------------------------------------------------------
            printf("\n[C] packed 복사 vs zero-copy ROI — 실기에서 결론이 갈렸던 항목\n");
            printf("    [x86] packed 5.2ms vs zero-copy 5.7ms (비슷)\n");
            printf("    [8MP 이전 실측] packed가 1.7배 빨랐음 — 3워커 동시일 때 재확인 필요\n");
            {
                int pad = 120;
                int x0 = std::max(0, minX - pad), y0 = std::max(0, minY - pad);
                int x1 = std::min(W, maxX + pad), y1 = std::min(H, maxY + pad);
                int cw = x1 - x0, ch = y1 - y0;
                PipelineConfig pc; pc.tileThreads = 1;
                pc.tryHarder = false; pc.tryRotate = false; pc.tryInvert = false;
                Pipeline p(pc);
                { // packed
                    double best = 1e9;
                    for (int i = 0; i < 10; ++i) {
                        auto t0 = Clk::now();
                        GrayImage img; img.width = cw; img.height = ch;
                        img.pixels.resize((size_t)cw * ch);
                        for (int r = 0; r < ch; ++r)
                            memcpy(img.pixels.data() + (size_t)r * cw,
                                   base->px.data() + (size_t)(y0 + r) * W + x0, cw);
                        auto rr = p.processView(GrayView(img));
                        double m = msSince(t0);
                        if (m < best) best = m;
                    }
                    printf("    packed 복사+디코드 : %7.2f ms\n", best);
                }
                { // zero-copy
                    GrayView roiView = baseView.crop(x0, y0, x1, y1);
                    double best = 1e9;
                    for (int i = 0; i < 10; ++i) {
                        auto t0 = Clk::now();
                        auto rr = p.processView(roiView);
                        double m = msSince(t0);
                        if (m < best) best = m;
                    }
                    printf("    zero-copy 디코드   : %7.2f ms\n", best);
                }
            }
        }
    }

    // ---------------------------------------------------------------
    printf("\n[D] API 경로별 평균 (전체 이미지)\n");
    printf("    [x86] full 34.1ms / two_stage 11.4ms\n");
    {
        struct M { const char* n; int mode; };
        M ms[] = {{"full     ", 0}, {"two_stage", 1}};
        for (auto& m : ms) {
            vscan_config_t c{}; c.worker_mode = 1;
            vscan_pipeline_t* p = vscan_create(&c);
            double total = 0; int cnt = 0;
            for (auto& im : imgs) {
                double best = 1e9;
                for (int i = 0; i < 3; ++i) {
                    auto t0 = Clk::now();
                    vscan_result_t* r = m.mode == 0
                        ? vscan_process_gray(p, im.px.data(), W, H, W)
                        : vscan_process_gray_two_stage(p, im.px.data(), W, H, W, 50);
                    double t = msSince(t0);
                    if (t < best) best = t;
                    vscan_free_result(r);
                }
                total += best; cnt++;
            }
            printf("    %s : 평균 %7.2f ms/frame\n", m.n, total / cnt);
            vscan_destroy(p);
        }
    }

    // ---------------------------------------------------------------
    printf("\n[E] 워커 스케일링 — x86에서는 vCPU 1개라 측정 불가였던 진짜 병렬 효율\n");
    printf("    이상적: 워커 N개 = 단일 워커 fps x N. 실제로는 L2(512KB 공유)/DDR\n");
    printf("    대역폭 경쟁으로 깎인다. 3워커 효율이 2.5배 미만이면 캐시 경쟁이 심한\n");
    printf("    것 — tracked(ROI만 접근, 워킹셋 작음) 사용을 더 우선할 것.\n");
    {
        for (int nw : {1, 2, 3}) {
            std::atomic<size_t> next{0};
            std::atomic<int> done{0};
            size_t total = imgs.size() * 3;
            auto t0 = Clk::now();
            std::vector<std::thread> ws;
            for (int w = 0; w < nw; ++w) {
                ws.emplace_back([&] {
                    vscan_config_t c{}; c.worker_mode = 1;
                    vscan_pipeline_t* p = vscan_create(&c);
                    for (;;) {
                        size_t i = next.fetch_add(1);
                        if (i >= total) break;
                        vscan_result_t* r = vscan_process_gray_two_stage(
                            p, imgs[i % imgs.size()].px.data(), W, H, W, 50);
                        vscan_free_result(r);
                        done++;
                    }
                    vscan_destroy(p);
                });
            }
            for (auto& t : ws) t.join();
            double sec = msSince(t0) / 1000.0;
            printf("    워커 %d개: %.1f fps\n", nw, done / sec);
        }
    }

    printf("\n완료. 각 항목을 [x86] 기준치와 비교해 결론이 뒤집힌 것만\n");
    printf("PROJECT_NOTES §6.5 체크리스트에 기록하세요.\n");
    return 0;
}
