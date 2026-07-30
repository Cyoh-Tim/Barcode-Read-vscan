/*
 * verify_accuracy.cpp — 3개 API 경로 × 악조건 이미지 32종 검출률 검증.
 *
 * 정확도에 영향을 줄 수 있는 변경(binarizer/downscale/옵션 조정 등) 후에는
 * 반드시 이걸 돌려서 회귀가 없는지 확인할 것 (PROJECT_NOTES §7).
 *
 * 이미지 준비: python3 tools/generate_stress_images.py --outdir ./stress
 * 빌드/실행:  g++ -O3 -std=c++17 -Iinclude tools/verify_accuracy.cpp -lvscan -o verify
 *             ./verify ./stress
 *
 * 기대 검출 개수는 파일명 끝의 _N에서 읽는다 (예: 02_multi_6.pgm -> 6개).
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "vscan.h"

static const int W = 2048, H = 1536;

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

struct Path { const char* name; int mode; int fast; };

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <stress_image_dir>\n", argv[0]); return 1; }
    std::string dir = argv[1];

    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) { perror("opendir"); return 1; }
    for (dirent* e; (e = readdir(d)); ) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".pgm") files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    Path paths[] = {
        {"full",        0, 0},
        {"2stage",      1, 0},
        {"2stage-fast", 1, 1},
    };
    const int NP = sizeof(paths) / sizeof(paths[0]);
    std::vector<int> pass(NP, 0);
    std::vector<double> totalMs(NP, 0.0);

    printf("%-34s %3s", "image", "exp");
    for (auto& p : paths) printf(" | %-18s", p.name);
    printf("\n");
    printf("%s\n", std::string(34 + 4 + NP * 21, '-').c_str());

    std::vector<uint8_t> buf;
    for (const auto& fn : files) {
        if (!loadPGM(dir + "/" + fn, buf)) { printf("%-34s LOAD FAIL\n", fn.c_str()); continue; }

        int expected = 1;
        size_t u = fn.rfind('_');
        if (u != std::string::npos) expected = atoi(fn.c_str() + u + 1);

        printf("%-34s %3d", fn.c_str(), expected);
        for (int i = 0; i < NP; ++i) {
            vscan_config_t cfg{};
            cfg.tile_overlap_px = 500;
            cfg.fast_locate = paths[i].fast;
            // "아이템당 코드 개수를 아는 현장" 사용법: 빠른 경로가 이 개수를
            // 못 채우면 자동으로 풀스캔 승격 -> 부분 검출 방지 (PROJECT_NOTES
            // §3.2.10). 개수를 모르는 배치라면 이 줄이 없는 조건이 현실이다.
            cfg.min_expected_codes = expected;
            vscan_pipeline_t* p = vscan_create(&cfg);

            double best = 1e9;
            size_t n = 0;
            for (int rep = 0; rep < 3; ++rep) {
                auto t0 = std::chrono::steady_clock::now();
                vscan_result_t* r = paths[i].mode == 0
                    ? vscan_process_gray(p, buf.data(), W, H, W)
                    : vscan_process_gray_two_stage(p, buf.data(), W, H, W, 50);
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (ms < best) best = ms;
                n = r ? r->count : 0;
                vscan_free_result(r);
            }
            vscan_destroy(p);

            bool ok = static_cast<int>(n) >= expected;
            if (ok) pass[i]++;
            totalMs[i] += best;
            printf(" | %s n=%-2zu %7.1fms", ok ? "OK" : "--", n, best);
        }
        printf("\n");
    }

    printf("%s\n", std::string(34 + 4 + NP * 21, '-').c_str());
    printf("%-34s %3s", "검출 성공 / 전체", "");
    for (int i = 0; i < NP; ++i) printf(" | %2d/%-2zu %11.1fms", pass[i], files.size(), totalMs[i]);
    printf("\n(시간은 3회 중 최소값의 합계)\n");
    return 0;
}
