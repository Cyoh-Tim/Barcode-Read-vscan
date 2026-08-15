/*
 * audit_config.cpp — 공개 설정이 **실제로 동작하는지** 전수로 확인한다.
 *
 * ## 왜 필요한가
 *
 * §3.68에서 `VSCAN_FLAG_NO_INVERT`와 `NO_TRY_HARDER`가 2단계 경로에서
 * **조용히 무시되고 있던 것**을 찾았다. 원인은 파이프라인이 `cfg_`를
 * 복사해 내부 단계용 설정을 만들면서 몇 필드를 무조건 덮어쓴 것이었다.
 *
 *     PipelineConfig hiCfg = cfg_;
 *     hiCfg.tryInvert = true;      // 호출자가 껐어도 켠다
 *
 * 이 관용구는 이 저장소에 여러 곳 있다. 즉 **같은 종류의 침묵이 더 있을
 * 수 있다.** 헤더가 "이 옵션은 이런 일을 한다"고 적어놨는데 실제로는
 * 아무 일도 안 하는 것이 가장 나쁘다 — 배치가 그 값을 믿고 튜닝한다.
 *
 * ## 어떻게 보나
 *
 * 옵션마다 켠 판본과 끈 판본을 같은 프레임에 돌려서 **결과(코드 수/텍스트)
 * 또는 시간이 바뀌는지** 본다. 아무것도 안 바뀌면 셋 중 하나다:
 *
 *   (a) 그 옵션이 배선이 끊겨 있다            <- 버그. 찾으려는 것.
 *   (b) 이 프레임들이 그 옵션과 무관하다      <- 프레임을 바꿔야 한다
 *   (c) 그 옵션이 원래 no-op다(호환용 등)     <- 알려진 것은 아래 표에 적는다
 *
 * 도구는 (a)와 (b)를 구별하지 못한다. 그래서 **"무시됨"이라고 단정하지
 * 않고 "이 프레임에서는 변화 없음"이라고만 보고한다.** 그 구별은 사람이
 * 프레임을 바꿔가며 해야 한다 — 이 도구의 일은 **어디를 볼지** 좁히는 것이다.
 *
 * ## 쓰는 법
 *
 *     audit_config <프레임_디렉터리> [--reps 2]
 */
#include "vscan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace {

struct Frame { std::vector<uint8_t> px; int w = 0, h = 0; std::string name; };

bool loadPGM(const std::string& path, Frame& f) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[3] = {0};
    if (fscanf(fp, "%2s", magic) != 1 || strcmp(magic, "P5") != 0) { fclose(fp); return false; }
    int maxv = 0;
    if (fscanf(fp, "%d %d %d", &f.w, &f.h, &maxv) != 3 || maxv != 255) { fclose(fp); return false; }
    fgetc(fp);
    f.px.resize(static_cast<size_t>(f.w) * f.h);
    const size_t rd = fread(f.px.data(), 1, f.px.size(), fp);
    fclose(fp);
    return rd == f.px.size();
}

struct Opt {
    const char* name;
    const char* note;               // 알려진 no-op이면 이유를 적는다
    void (*apply)(vscan_config_t&);
};

// 옵션 적용 함수들. 기본값에서 **의미 있게 다른 값**으로 바꾼다.
void oTileOverlap(vscan_config_t& c)   { c.tile_overlap_px = 80; }
void oSymMask(vscan_config_t& c)       { c.symbology_mask = VSCAN_FMT_QR | VSCAN_FMT_MICRO_QR; }
void oNoRotate(vscan_config_t& c)      { c.decode_flags |= VSCAN_FLAG_NO_ROTATE; }
void oNoInvert(vscan_config_t& c)      { c.decode_flags |= VSCAN_FLAG_NO_INVERT; }
void oNoHarder(vscan_config_t& c)      { c.decode_flags |= VSCAN_FLAG_NO_TRY_HARDER; }
void oWorker(vscan_config_t& c)        { c.worker_mode = 1; }
void oFastLocate(vscan_config_t& c)    { c.fast_locate = 1; }
void oAccurateLocate(vscan_config_t& c){ c.accurate_locate = 1; }
void oMinExpected(vscan_config_t& c)   { c.min_expected_codes = 4; }
void oNoCoarse(vscan_config_t& c)      { c.disable_coarse_locate = 1; }
void oCoarse2(vscan_config_t& c)       { c.coarse_factor = 2; }
void oDeskew(vscan_config_t& c)        { c.enable_1d_deskew_rescue = 1; }
void oDpm(vscan_config_t& c)           { c.enable_dpm_rescue = 1; }
void oMaxFrame(vscan_config_t& c)      { c.max_frame_ms = 20; }
void oNoBlankSkip(vscan_config_t& c)   { c.disable_blank_frame_skip = 1; }
void oNoDenoiseRescue(vscan_config_t& c){ c.disable_denoise_rescue = 1; }
void oNoRegion(vscan_config_t& c)      { c.disable_region_rescue = 1; }
void oQrFinder(vscan_config_t& c)      { c.enable_qr_finder_rescue = 1; }
void oNoAutoDenoise(vscan_config_t& c) { c.disable_auto_denoise = 1; }
void oStrongDn(vscan_config_t& c)      { c.auto_denoise_strong_noise = 12.0f; }
void oNoTileFb(vscan_config_t& c)      { c.disable_tile_fallback = 1; }
void oFastNoRead(vscan_config_t& c)    { c.fast_no_read = 1; }
void oItfSum(vscan_config_t& c)        { c.validate_itf_checksum = 1; }
void oMinLines(vscan_config_t& c)      { c.min_line_count = 8; }
void oInd25(vscan_config_t& c)         { c.enable_industrial_2of5 = 1; }
void oPharma(vscan_config_t& c)        { c.enable_pharmacode = 1; }
void oMicroPdf(vscan_config_t& c)      { c.enable_micro_pdf417 = 1; }
void oDotCode(vscan_config_t& c)       { c.enable_dotcode = 1; }
void oAdaptive(vscan_config_t& c)      { c.enable_adaptive_profile = 1; }
void oAutoExp(vscan_config_t& c)       { c.auto_expected_codes = 1; }
void oTsBudget(vscan_config_t& c)      { c.two_stage_self_budget = 1; }

const Opt kOpts[] = {
    {"tile_overlap_px=80",        nullptr, oTileOverlap},
    {"symbology_mask=QR",         nullptr, oSymMask},
    {"NO_ROTATE",                 nullptr, oNoRotate},
    {"NO_INVERT",                 nullptr, oNoInvert},
    {"NO_TRY_HARDER",             nullptr, oNoHarder},
    {"worker_mode=1",             "스레드만 바꾼다 — 결과는 같아야 정상", oWorker},
    {"fast_locate=1",             "2026-08-03부터 기본과 같다(ABI 호환 no-op)", oFastLocate},
    {"accurate_locate=1",         nullptr, oAccurateLocate},
    {"min_expected_codes=4",      nullptr, oMinExpected},
    {"disable_coarse_locate",     nullptr, oNoCoarse},
    {"coarse_factor=2",           nullptr, oCoarse2},
    {"enable_1d_deskew_rescue",   nullptr, oDeskew},
    {"enable_dpm_rescue",         nullptr, oDpm},
    {"max_frame_ms=20",           nullptr, oMaxFrame},
    {"disable_blank_frame_skip",  nullptr, oNoBlankSkip},
    {"disable_denoise_rescue",    nullptr, oNoDenoiseRescue},
    {"disable_region_rescue",     nullptr, oNoRegion},
    {"enable_qr_finder_rescue",   nullptr, oQrFinder},
    {"disable_auto_denoise",      nullptr, oNoAutoDenoise},
    {"auto_denoise_strong=12",    nullptr, oStrongDn},
    {"disable_tile_fallback",     nullptr, oNoTileFb},
    {"fast_no_read=1",            nullptr, oFastNoRead},
    {"validate_itf_checksum",     "ITF가 없는 프레임이면 변화 없음이 정상", oItfSum},
    {"min_line_count=8",          nullptr, oMinLines},
    {"enable_industrial_2of5",    "해당 심볼로지가 없으면 변화 없음이 정상", oInd25},
    {"enable_pharmacode",         "해당 심볼로지가 없으면 변화 없음이 정상", oPharma},
    {"enable_micro_pdf417",       "해당 심볼로지가 없으면 변화 없음이 정상", oMicroPdf},
    {"enable_dotcode",            "해당 심볼로지가 없으면 변화 없음이 정상", oDotCode},
    {"enable_adaptive_profile",   "프레임 간 학습이라 1장 반복으로는 안 보일 수 있다", oAdaptive},
    {"auto_expected_codes=1",     "코드가 하나뿐인 프레임에서는 변화 없음이 정상", oAutoExp},
    {"two_stage_self_budget=1",   "쉬운 프레임은 마감에 안 닿으므로 변화 없음이 정상", oTsBudget},
};
constexpr int kNumOpts = static_cast<int>(sizeof(kOpts) / sizeof(kOpts[0]));

struct Res { std::string sig; double ms = 0; };

// 결과 서명 = 디코드된 텍스트를 이어붙인 것. 개수만 보면 "다른 코드를 같은
// 개수만큼 찾은" 변화를 놓친다.
Res run(void (*apply)(vscan_config_t&), const std::vector<Frame>& frames, int reps, bool twoStage) {
    vscan_config_t c;
    memset(&c, 0, sizeof(c));
    c.tile_threads = 4;
    if (apply) apply(c);
    vscan_pipeline_t* p = vscan_create(&c);
    Res out;
    double best = 0;
    for (int k = 0; k < reps; ++k) {
        std::string sig;
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& f : frames) {
            vscan_result_t* r = twoStage
                ? vscan_process_gray_two_stage(p, f.px.data(), f.w, f.h, f.w, 0)
                : vscan_process_gray(p, f.px.data(), f.w, f.h, f.w);
            if (r) {
                for (size_t i = 0; i < r->count; ++i) { sig += r->symbols[i].text; sig += '\x1f'; }
                vscan_free_result(r);
            }
            sig += '\x1e';
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        if (k == 0 || ms < best) best = ms;
        out.sig = std::move(sig);
    }
    out.ms = best;
    vscan_destroy(p);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "사용: %s <프레임_디렉터리> [--reps N] [--path full|2stage] [--require a,b,c]\n"
            "  --require  적은 옵션이 이 프레임에서 **변화를 만들지 못하면 실패**한다.\n"
            "             배선이 끊기는 회귀를 잡는 용도(§3.68/§3.69가 그 사고였다).\n", argv[0]);
        return 2;
    }
    int reps = 2;
    std::string onlyPath, require;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--path") == 0 && i + 1 < argc) onlyPath = argv[++i];
        else if (std::strcmp(argv[i], "--require") == 0 && i + 1 < argc) require = argv[++i];
    }
    auto required = [&](const char* nm) {
        if (require.empty()) return false;
        std::string hay = "," + require + ",";
        return hay.find("," + std::string(nm) + ",") != std::string::npos;
    };
    int missing = 0;

    std::vector<Frame> frames;
    DIR* d = opendir(argv[1]);
    if (!d) { std::fprintf(stderr, "디렉터리를 못 연다: %s\n", argv[1]); return 2; }
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) {
            Frame f; f.name = n;
            if (loadPGM(std::string(argv[1]) + "/" + n, f)) frames.push_back(std::move(f));
        }
    }
    closedir(d);
    if (frames.empty()) { std::fprintf(stderr, "프레임이 없다\n"); return 2; }
    std::printf("프레임 %zu장 / reps %d\n\n", frames.size(), reps);

    for (int ts = 0; ts < 2; ++ts) {
        const char* pathName = ts ? "2stage" : "full";
        if (!onlyPath.empty() && onlyPath != pathName) continue;
        const Res base = run(nullptr, frames, reps, ts != 0);
        std::printf("== 경로 %s (기준 %.1fms) ==\n", pathName, base.ms);
        int silent = 0;
        for (int i = 0; i < kNumOpts; ++i) {
            const Res r = run(kOpts[i].apply, frames, reps, ts != 0);
            const bool sameResult = (r.sig == base.sig);
            // 시간은 흔들리므로 넉넉히 본다. 8% 안쪽이면 "같다"로 친다.
            const double rel = base.ms > 0 ? (r.ms - base.ms) / base.ms : 0;
            const bool sameTime = rel > -0.08 && rel < 0.08;
            if (sameResult && sameTime) {
                ++silent;
                const bool req = required(kOpts[i].name);
                if (req) ++missing;
                std::printf("  [변화없음%s] %-26s %+5.0f%%   %s\n", req ? "!!" : "  ",
                            kOpts[i].name, rel * 100,
                            req ? "<- **필수인데 동작하지 않는다**"
                                : (kOpts[i].note ? kOpts[i].note : "<- 볼 것"));
            } else {
                std::printf("  [동작함  ] %-26s %+5.0f%%   %s\n", kOpts[i].name, rel * 100,
                            sameResult ? "시간만 바뀜" : "결과가 바뀜");
            }
        }
        std::printf("  -> %s 경로에서 변화 없는 옵션 %d개\n\n", pathName, silent);
    }

    if (!require.empty()) {
        if (missing > 0) {
            std::printf("\n!! 필수 옵션 %d개가 이 프레임에서 아무 변화도 만들지 못했다.\n"
                        "   배선이 끊겼거나(§3.68/§3.69 같은 사고) 이 코퍼스가 바뀐 것이다.\n", missing);
            return 1;
        }
        std::printf("\n필수 옵션 전부 정상 동작\n");
        return 0;
    }
    std::printf("주의: \"변화없음\"은 배선이 끊겼다는 **증거가 아니다.**\n"
                "이 프레임들이 그 옵션과 무관해서일 수도 있다. 이 도구는\n"
                "어디를 볼지 좁혀줄 뿐이고, 구별은 프레임을 바꿔가며 해야 한다.\n");
    return 0;
}
