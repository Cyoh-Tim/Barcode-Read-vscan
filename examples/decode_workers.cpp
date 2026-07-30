/*
 * decode_workers.cpp - 워커 모드 사용 예제 (설계 참고용)
 *
 * 애플리케이션 구조 가정 (i.MX8M Plus 4코어 기준):
 *   - 코어 1개: 애플리케이션(캡처/통신/UI)
 *   - 코어 3개: 워커 3개가 각자 1코어에서 프레임 하나를 통째로 처리
 *     (탐색 + 디코드 + 결과 생성까지 전부 그 코어 안에서)
 *
 * 핵심 규칙:
 *   1. vscan_config_t.worker_mode = 1  -> 라이브러리가 내부 스레드를 안 만든다.
 *      이걸 안 켜면 워커 3개 x 내부 스레드 4개 = 12스레드가 3코어를 놓고
 *      경쟁해서 처리량이 크게 떨어진다(실측 62% 손해).
 *   2. 워커마다 자기 vscan_pipeline_t를 하나 만들어 재사용한다.
 *      서로 다른 인스턴스는 동시 사용해도 안전하다.
 *      하나의 인스턴스를 여러 스레드가 공유하는 것은 하지 말 것.
 *   3. 워커를 코어에 고정(affinity)한다. A53은 코어당 L1이 32KB로 작아서
 *      스케줄러가 워커를 다른 코어로 옮기면 캐시를 통째로 다시 채운다.
 *      코어0 = 애플리케이션, 코어1~3 = 워커0~2 로 고정하는 게 정석.
 *      (앱 쪽도 taskset/cgroup으로 코어0에 묶으면 더 확실하다)
 *
 * 빌드: g++ -O3 -std=c++17 -I<repo>/include decode_workers.cpp -lvscan -lpthread -o decode_workers
 * 실행: ./decode_workers <pgm이 들어있는 디렉토리>
 */
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vscan.h"

static const int W = 2048, H = 1536;
static const int NUM_WORKERS = 3;

struct Frame { std::string name; std::vector<uint8_t> px; };

static bool loadPGM(const std::string& path, std::vector<uint8_t>& buf) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[256];
    for (int i = 0; i < 3; ++i) if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    buf.resize(static_cast<size_t>(W) * H);
    bool ok = fread(buf.data(), 1, buf.size(), f) == buf.size();
    fclose(f);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <pgm_dir>\n", argv[0]); return 1; }

    // 실제 장비에서는 V4L2 캡처 큐가 이 자리를 대체한다.
    // 여기서는 디렉토리의 PGM들을 "들어오는 프레임"으로 사용.
    std::vector<Frame> frames;
    DIR* d = opendir(argv[1]);
    if (!d) { perror("opendir"); return 1; }
    for (dirent* e; (e = readdir(d)); ) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".pgm") {
            Frame fr; fr.name = n;
            if (loadPGM(std::string(argv[1]) + "/" + n, fr.px)) frames.push_back(std::move(fr));
        }
    }
    closedir(d);
    printf("프레임 %zu장, 워커 %d개\n\n", frames.size(), NUM_WORKERS);

    std::atomic<size_t> nextFrame{0};
    std::atomic<int> totalSymbols{0};
    std::mutex printMu;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (int w = 0; w < NUM_WORKERS; ++w) {
        workers.emplace_back([&, w] {
            // 워커 w를 코어 (w+1)에 고정 — 코어0은 애플리케이션 몫.
            // A53에서 코어 이동은 L1(32KB) 전체 재적재를 의미하므로 필수.
            cpu_set_t cpus;
            CPU_ZERO(&cpus);
            CPU_SET(w + 1, &cpus);
            pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);

            // 워커당 파이프라인 1개, 루프 밖에서 만들어 재사용
            vscan_config_t cfg{};
            cfg.worker_mode = 1;        // 필수: 내부 스레드 생성 차단
            cfg.tile_overlap_px = 500;
            vscan_pipeline_t* pipe = vscan_create(&cfg);

            for (;;) {
                size_t idx = nextFrame.fetch_add(1);
                if (idx >= frames.size()) break;
                const Frame& fr = frames[idx];

                vscan_result_t* r = vscan_process_gray_two_stage(
                    pipe, fr.px.data(), W, H, W, /*crop_pad_px(미사용)*/ 50);

                {
                    std::lock_guard<std::mutex> lk(printMu);
                    printf("[worker%d] %-34s -> %zu개", w, fr.name.c_str(), r ? r->count : 0);
                    for (size_t i = 0; r && i < r->count && i < 2; ++i)
                        printf("  [%s]", r->symbols[i].text);
                    printf("\n");
                }
                if (r) totalSymbols += static_cast<int>(r->count);
                vscan_free_result(r);
            }
            vscan_destroy(pipe);
        });
    }
    for (auto& t : workers) t.join();
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("\n총 %zu프레임 / 심볼 %d개 / %.1f ms (프레임당 %.2f ms, 초당 %.1f 프레임)\n",
           frames.size(), totalSymbols.load(), ms, ms / frames.size(),
           frames.size() * 1000.0 / ms);
    return 0;
}
