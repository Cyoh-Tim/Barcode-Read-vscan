/*
 * chart_probe.cpp — 실물 FOV 차트 한 장을 권장 조합으로 돌려서
 * "정답 문자열이 몇 개 나왔나"만 찍는다. 게이트 [3.74]가 쓴다.
 *
 * 왜 verify_accuracy를 안 쓰나: 그쪽은 디렉터리 + labels.tsv 전제이고,
 * 이 차트는 코드 26개가 **전부 같은 문자열**이라 개수 대조가 특수하다.
 * 여기서는 "VC QR Test"가 아닌 것이 하나라도 나오면 유령으로 세고,
 * 정답 개수만 stdout 첫 줄에 낸다.
 */
#include "vscan.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool loadPGM(const char* p, std::vector<unsigned char>& px, int& w, int& h) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    char m[3] = {0};
    if (fscanf(f, "%2s", m) != 1) { fclose(f); return false; }
    auto sk = [&] { int c; while ((c = fgetc(f)) != EOF) {
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n'); }
        else if (!isspace(c)) { ungetc(c, f); break; } } };
    int mx;
    sk(); if (fscanf(f, "%d", &w) != 1) { fclose(f); return false; }
    sk(); if (fscanf(f, "%d", &h) != 1) { fclose(f); return false; }
    sk(); if (fscanf(f, "%d", &mx) != 1) { fclose(f); return false; }
    fgetc(f);
    px.resize(static_cast<size_t>(w) * h);
    size_t got = fread(px.data(), 1, px.size(), f);
    fclose(f);
    return got == px.size();
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: chart_probe <chart.pgm> [정답문자열]\n"); return 2; }
    const std::string want = argc > 2 ? argv[2] : "VC QR Test";
    std::vector<unsigned char> px; int w, h;
    if (!loadPGM(argv[1], px, w, h)) { fprintf(stderr, "load fail\n"); return 2; }

    vscan_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    // §3.103의 권장 조합 그대로.
    cfg.symbology_mask = VSCAN_FMT_QR;
    cfg.enable_qr_finder_rescue = 1;
    cfg.min_expected_codes = 26;

    vscan_pipeline_t* p = vscan_create(&cfg);
    vscan_result_t* r = vscan_process_gray_two_stage(p, px.data(), w, h, w, 0);
    int ok = 0, ghost = 0;
    for (size_t i = 0; i < (r ? r->count : 0); ++i)
        (want == r->symbols[i].text) ? ++ok : ++ghost;
    std::printf("%d\n", ghost ? -ghost : ok);   // 유령이 있으면 음수로 알린다
    std::fprintf(stderr, "정답 %d개 / 유령 %d개\n", ok, ghost);
    if (r) vscan_free_result(r);
    vscan_destroy(p);
    return 0;
}
