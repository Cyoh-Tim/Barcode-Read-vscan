/*
 * verify_accuracy.cpp — 3개 API 경로 × 악조건 이미지 검출률/시간 검증.
 *
 * 정확도에 영향을 줄 수 있는 변경(binarizer/downscale/옵션 조정 등) 후에는
 * 반드시 이걸 돌려서 회귀가 없는지 확인할 것 (PROJECT_NOTES §7).
 *
 * 두 가지 규모를 같은 도구로 본다:
 *
 *  1. 고정 40종 (회귀 게이트)
 *       python3 tools/generate_stress_images.py --outdir ./stress
 *       ./verify_accuracy ./stress
 *     -> 기존과 동일하게 이미지별 표 + "검출 성공 / 전체 37/40" 출력.
 *
 *  2. 대량 코퍼스 (성능/정확도 튜닝용, 수백~수만 장)
 *       python3 tools/generate_corpus.py -o ./corpus -n 2000 --jobs 4
 *       ./verify_accuracy ./corpus --reps 1 --csv run-before.csv
 *     -> labels.tsv를 자동으로 읽어서
 *        (a) 코드 단위 검출률(이미지 합격/불합격보다 해상도가 높다)
 *        (b) 디코딩 텍스트 정답 대조 — 오디코딩(misdecode)까지 잡는다
 *        (c) **조건 태그별 검출률/시간 집계** — 무엇이 느리고 무엇을
 *            놓치는지 축 단위로 보인다
 *        (d) 평균/중앙값/p95 — 40장에서는 의미 없던 분포 통계
 *     대량 코퍼스는 절대 목표치가 아니라 A/B 비교용이다: 같은 --seed로
 *     만든 같은 코퍼스를 변경 전/후에 돌려 CSV를 비교한다.
 *
 * 빌드: g++ -O3 -std=c++17 -Iinclude tools/verify_accuracy.cpp -lvscan -o verify_accuracy
 *
 * 기대 검출 개수는 labels.tsv가 있으면 그걸 쓰고, 없으면 파일명 끝의 _N에서
 * 읽는다 (예: 02_multi_6.pgm -> 6개).
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "vscan.h"

// ---------------------------------------------------------------- PGM 로더
struct Gray {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
};

// P5 헤더를 실제로 파싱한다(주석/임의 공백/임의 해상도 허용). 예전 버전은
// 2048x1536을 하드코딩했는데, 대량 코퍼스는 --width/--height로 절반
// 해상도를 뽑을 수 있어야 디스크/시간이 감당된다.
static bool loadPGM(const std::string& path, Gray& g) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    auto tok = [&](long& out) -> bool {
        int c;
        for (;;) {
            do { c = fgetc(f); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
            break;
        }
        if (c == EOF) return false;
        long v = 0;
        bool any = false;
        while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); any = true; c = fgetc(f); }
        out = v;
        return any;
    };
    char magic[3] = {0};
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '5') { fclose(f); return false; }
    long w = 0, h = 0, maxv = 0;
    if (!tok(w) || !tok(h) || !tok(maxv) || w <= 0 || h <= 0 || maxv != 255) { fclose(f); return false; }
    g.w = static_cast<int>(w);
    g.h = static_cast<int>(h);
    g.px.resize(static_cast<size_t>(g.w) * g.h);
    bool ok = fread(g.px.data(), 1, g.px.size(), f) == g.px.size();
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- 정답 라벨
struct Label {
    int expected = 1;
    std::vector<std::string> tags;        // 이미지 단위 (블러/노이즈/클러터...)
    std::vector<std::string> texts;       // 코드별 정답 텍스트 (순서 = 코드 순서)
    std::vector<std::string> codeTags;    // 코드별 태그, '&' 로 묶임 (모듈크기/반전/DPM...)
};

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string::npos ? q : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

// labels.tsv: file \t expected \t tags(,) \t texts(|) \t symbologies(|)
static bool loadLabels(const std::string& path, std::unordered_map<std::string, Label>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string line;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c != '\n') { line.push_back(static_cast<char>(c)); continue; }
        if (!line.empty() && line[0] != '#') {
            auto col = split(line, '\t');
            if (col.size() >= 2) {
                Label l;
                l.expected = atoi(col[1].c_str());
                if (col.size() >= 3) l.tags = split(col[2], ',');
                if (col.size() >= 4) l.texts = split(col[3], '|');
                if (col.size() >= 6) l.codeTags = split(col[5], '|');
                out[col[0]] = std::move(l);
            }
        }
        line.clear();
    }
    if (!line.empty() && line[0] != '#') {
        auto col = split(line, '\t');
        if (col.size() >= 2) {
            Label l;
            l.expected = atoi(col[1].c_str());
            if (col.size() >= 3) l.tags = split(col[2], ',');
            if (col.size() >= 4) l.texts = split(col[3], '|');
            if (col.size() >= 6) l.codeTags = split(col[5], '|');
            out[col[0]] = std::move(l);
        }
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- 통계
struct Stat {
    int images = 0, pass = 0;
    long expected = 0, found = 0;      // 코드 단위
    long textOk = 0, misdecode = 0, dup = 0;   // 텍스트 대조
    double totalMs = 0;
    std::vector<double> ms;

    void add(bool ok, int exp, int n, int tok, int bad, double t, int dp = 0) {
        images++;
        pass += ok ? 1 : 0;
        expected += exp;
        found += n;
        textOk += tok;
        misdecode += bad;
        dup += dp;
        totalMs += t;
        ms.push_back(t);
    }
    double pct(long a, long b) const { return b ? 100.0 * a / b : 0.0; }
    double mean() const { return images ? totalMs / images : 0.0; }
    double quant(double q) {
        if (ms.empty()) return 0.0;
        std::sort(ms.begin(), ms.end());
        size_t i = static_cast<size_t>(q * (ms.size() - 1) + 0.5);
        return ms[i];
    }
};

// 태그 집계는 **코드 단위**다. 이미지 단위로 세면 "2px 코드 하나가 섞인
// 이미지"의 큰 코드들까지 mod-2px 행에 들어가서 축이 뭉개진다.
// 시간(ms)만은 이미지당 한 번 더한다 — 코드 수로 가중되면 다중 코드
// 이미지가 평균을 끌어당긴다.
struct TagStat {
    int images = 0;
    long expected = 0, found = 0;
    double totalMs = 0;
    double rate() const { return expected ? 100.0 * found / expected : 0.0; }
    double mean() const { return images ? totalMs / images : 0.0; }
};

struct Path { const char* name; int mode; int fast; };

static void usage(const char* argv0) {
    fprintf(stderr,
        "usage: %s <image_dir> [options]   (또는 %s --stdin)\n"
        "  --labels <f>     정답 파일 (기본: <dir>/labels.tsv 있으면 자동)\n"
        "  --reps N         이미지당 반복 측정 횟수, 최소값 채택 (기본: 40장 이하 3, 그 이상 1)\n"
        "  --paths a,b,c    full,2stage,2stage-fast 중 선택 (기본: 전부)\n"
        "  --limit N        앞의 N장만\n"
        "  --stride N       N장마다 하나씩 (코퍼스 표본 추출)\n"
        "  --verbose        이미지별 줄 강제 출력 (기본: 64장 이하만)\n"
        "  --quiet          이미지별 줄 생략\n"
        "  --csv <f>        이미지×경로 결과를 CSV로 저장 (A/B 비교용)\n"
        "  --tags           조건 태그별 집계표 출력 (labels.tsv 필요, 기본 켜짐)\n"
        "  --no-tags        태그 집계 생략\n"
        "  --no-min-expected  min_expected_codes를 0으로 (개수를 모르는 현장 조건)\n"
        "  --no-text        텍스트 대조 생략\n"
        "  --overlap N      tile_overlap_px (기본 500)\n"
        "  --dpm-rescue     enable_dpm_rescue=1 (기본 꺼짐 — 40종 기준선은 꺼진 값)\n"
        "  --max-frame-ms N 프레임 시간 예산(0=무제한). 폴백 꼬리를 자른다\n"
        "  --no-deskew      1D 회전 구제 끔 (구제 비용 분리 측정용)\n"
        "  --no-blank-skip  빈 프레임 조기 종료 끔 (효과 분리 측정용)\n"
        "  --no-denoise     노이즈 구제 끔 (효과 분리 측정용)\n"
        "  --no-region      영역(ROI) 구제 끔 (효과/비용 분리 측정용)\n"
        "  --zbar           ZBar 보조 디코더 켬\n"        "  --qr-finder      QR 파인더 구제 켬 (작고 밀집한 QR용)\n"
        "  --adaptive       적응형 배치 프로파일 켜고 파이프라인을 프레임 간 재사용\n"
        "                   (실제 워커 구조와 같은 조건. min_expected_codes=0 강제)\n"
        "  --dump-mismatch  정답과 다른 디코딩 결과를 기대/실제로 출력 (오디코딩 추적)\n"
        "  --stdin          디렉토리 대신 stdin 스트림을 읽는다 (디스크 0). 아래 참고\n"
        "  --tag-sort n|i   태그표 정렬: name(스윕용) / imgs(기본, 이미지 수)\n"
        "\n"
        "스트리밍 (수십만 장을 디스크 없이):\n"
        "  python3 tools/generate_corpus.py --sweep angle:0:359:1 --stream \\\n"
        "    | %s --stdin\n",
        argv0, argv0, argv0);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    std::string dir = (argv[1][0] == '-') ? std::string() : argv[1];
    std::string labelPath, csvPath, pathSel = "full,2stage,2stage-fast";
    int reps = 0, limit = 0, stride = 1, overlap = 500;
    int verbose = -1, useTags = 1, useText = 1, useMinExp = 1, dpmRescue = 0, dumpMismatch = 0;
    int stdinMode = 0, tagSortName = 0, maxFrameMs = 0, noDeskew = 0, noBlankSkip = 0, noDenoise = 0, adaptive = 0, noRegion = 0, useZbar = 0, qrFinder = 0;

    for (int i = dir.empty() ? 1 : 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--labels") labelPath = next();
        else if (a == "--reps") reps = atoi(next());
        else if (a == "--paths") pathSel = next();
        else if (a == "--limit") limit = atoi(next());
        else if (a == "--stride") stride = std::max(1, atoi(next()));
        else if (a == "--csv") csvPath = next();
        else if (a == "--overlap") overlap = atoi(next());
        else if (a == "--verbose") verbose = 1;
        else if (a == "--quiet") verbose = 0;
        else if (a == "--tags") useTags = 1;
        else if (a == "--no-tags") useTags = 0;
        else if (a == "--tag-sort") tagSortName = (next()[0] == 'n');
        else if (a == "--no-text") useText = 0;
        else if (a == "--no-min-expected") useMinExp = 0;
        else if (a == "--dpm-rescue") dpmRescue = 1;
        else if (a == "--max-frame-ms") maxFrameMs = atoi(next());
        else if (a == "--no-deskew") noDeskew = 1;
        else if (a == "--no-blank-skip") noBlankSkip = 1;
        else if (a == "--no-denoise") noDenoise = 1;
        else if (a == "--no-region") noRegion = 1;
        else if (a == "--zbar") useZbar = 1;
        else if (a == "--qr-finder") qrFinder = 1;
        else if (a == "--adaptive") { adaptive = 1; useMinExp = 0; }
        else if (a == "--dump-mismatch") dumpMismatch = 1;
        else if (a == "--stdin") stdinMode = 1;
        else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }

    // ---- 파일 목록 (stdin 모드에서는 프레임이 파이프로 들어오므로 생략)
    std::vector<std::string> files;
    std::unordered_map<std::string, Label> labels;
    bool haveLabels = false;
    if (!stdinMode) {
        if (dir.empty()) { fprintf(stderr, "이미지 디렉토리가 필요합니다\n"); return 1; }
        // 하위 폴더도 한 단계 훑는다. generate_corpus.py가 판독 가능성
        // 버킷별로(ok/ borderline/ mixed/ impossible/) 나눠 떨어뜨리므로,
        // 상위 폴더를 주면 전체, 'corpus/ok'를 주면 "읽혀야 정상인 것"만 채점된다.
        std::vector<std::string> all;
        auto scan = [&](const std::string& sub) {
            DIR* d = opendir((sub.empty() ? dir : dir + "/" + sub).c_str());
            if (!d) return;
            for (dirent* e; (e = readdir(d)); ) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.substr(n.size() - 4) == ".pgm")
                    all.push_back(sub.empty() ? n : sub + "/" + n);
            }
            closedir(d);
        };
        scan("");
        for (const char* b : {"ok", "borderline", "mixed", "impossible"}) scan(b);
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i < all.size(); i += stride) {
            files.push_back(all[i]);
            if (limit && static_cast<int>(files.size()) >= limit) break;
        }
        if (files.empty()) { fprintf(stderr, "%s: .pgm 파일이 없습니다\n", dir.c_str()); return 1; }

        // ---- 정답 라벨 (있으면)
        if (labelPath.empty()) labelPath = dir + "/labels.tsv";
        haveLabels = loadLabels(labelPath, labels) && !labels.empty();
    } else {
        haveLabels = true;              // 정답이 프레임 헤더에 실려 온다
    }
    if (!haveLabels) { useTags = 0; useText = 0; }

    // ---- 경로 선택
    std::vector<Path> paths;
    for (const auto& s : split(pathSel, ',')) {
        if (s == "full") paths.push_back({"full", 0, 0});
        else if (s == "2stage") paths.push_back({"2stage", 1, 0});
        else if (s == "2stage-fast") paths.push_back({"2stage-fast", 1, 1});
        else { fprintf(stderr, "unknown path: %s\n", s.c_str()); return 1; }
    }
    if (paths.empty()) { fprintf(stderr, "no paths selected\n"); return 1; }
    const int NP = static_cast<int>(paths.size());

    // 대량 코퍼스에서 3회 반복은 그냥 3배 시간이다. 표본이 크면 반복 대신
    // 이미지 수로 분산이 잡히므로 기본 1회.
    if (reps <= 0) reps = (!stdinMode && files.size() <= 40) ? 3 : 1;
    if (verbose < 0) verbose = (!stdinMode && files.size() <= 64) ? 1 : 0;

    if (stdinMode)
        printf("stdin 스트림 / 경로 %d / reps %d / min_expected_codes %s%s\n",
               NP, reps, useMinExp ? "= 기대개수" : "= 0", dpmRescue ? " / dpm_rescue ON" : "");
    else
        printf("이미지 %zu장 (%s%s) / 경로 %d / reps %d / min_expected_codes %s%s\n",
               files.size(), dir.c_str(), haveLabels ? " + labels.tsv" : "", NP, reps,
               useMinExp ? "= 기대개수" : "= 0", dpmRescue ? " / dpm_rescue ON" : "");

    // ---- 파이프라인 (이미지마다 새로 만들지 않고 경로별로 재사용.
    //      단 min_expected_codes가 이미지마다 달라서 그때만 재생성한다.)
    std::vector<Stat> st(NP);
    std::map<std::string, std::vector<TagStat>> byTag;
    FILE* csv = nullptr;
    if (!csvPath.empty()) {
        csv = fopen(csvPath.c_str(), "wb");
        if (csv) fprintf(csv, "file,path,expected,found,text_ok,misdecode,dup,ms,tags\n");
        else perror("csv fopen");
    }

    const int lineW = 34;
    if (verbose) {
        printf("%-*s %3s", lineW, "image", "exp");
        for (auto& p : paths) printf(" | %-18s", p.name);
        printf("\n%s\n", std::string(lineW + 4 + NP * 21, '-').c_str());
    }

    // --adaptive는 프레임 간 상태(관찰된 심볼로지)가 쌓여야 의미가 있으므로
    // 실제 워커처럼 파이프라인을 재사용한다. 그 외 모드는 기존대로 이미지마다
    // 새로 만든다(min_expected_codes가 이미지마다 다르기 때문).
    std::vector<vscan_pipeline_t*> shared(NP, nullptr);
    if (adaptive) {
        for (int i = 0; i < NP; ++i) {
            vscan_config_t c{};
            c.tile_overlap_px = overlap;
            c.fast_locate = paths[i].fast;
            c.enable_dpm_rescue = dpmRescue;
            c.max_frame_ms = maxFrameMs;
            // opt-in 심볼로지(Industrial 2of5 / COOP 2of5 / Pharmacode).
            // 기본이 꺼져 있는 것들이라 회귀에는 안 걸리고, 유령 검출을
            // 재려고 환경변수로 켠다.
            c.enable_industrial_2of5 = getenv("VSCAN_IND25") != nullptr;
            c.enable_coop_2of5       = getenv("VSCAN_COOP25") != nullptr;
            c.enable_pharmacode      = getenv("VSCAN_PHARMA") != nullptr;
            c.enable_micro_pdf417   = getenv("VSCAN_MPDF") != nullptr;
            c.enable_postal_japan   = getenv("VSCAN_JPPOST") != nullptr;
            c.enable_postal_imb     = getenv("VSCAN_IMB") != nullptr;
            c.enable_dotcode       = getenv("VSCAN_DOTCODE") != nullptr;
            c.validate_itf_checksum = getenv("VSCAN_ITFSUM") != nullptr;
            c.frame_budget_max_ms = getenv("VSCAN_BUDGETMAX") ? atoi(getenv("VSCAN_BUDGETMAX")) : 0;
            c.max_frame_ms = getenv("VSCAN_MAXFRAME") ? atoi(getenv("VSCAN_MAXFRAME")) : 0;
            c.min_line_count = getenv("VSCAN_MINLINES") ? atoi(getenv("VSCAN_MINLINES")) : 0;
            // 선(先) 디노이즈를 떼어내고 재기 위한 손잡이. 저조도에서
            // 이게 실제로 일하고 있는지는 껐을 때의 차이로만 알 수 있다.
            c.disable_auto_denoise = getenv("VSCAN_NOAUTODN") != nullptr;
            c.fast_no_read = getenv("VSCAN_FASTNR") != nullptr;
            c.auto_expected_codes = getenv("VSCAN_AUTOEXP") != nullptr;
            c.accurate_locate = getenv("VSCAN_ACCLOC") != nullptr;
            if (const char* me = getenv("VSCAN_MINEXP")) c.min_expected_codes = atoi(me);
            if (const char* df = getenv("VSCAN_FLAGS")) c.decode_flags = strtoul(df, nullptr, 0);
            // 배치가 심볼로지를 아는 경우의 값을 재기 위한 손잡이.
            // 예: VSCAN_FMTMASK=1 이면 QR만.
            if (const char* fm = getenv("VSCAN_FMTMASK")) c.symbology_mask = strtoul(fm, nullptr, 0);
            c.auto_denoise_strong_noise = getenv("VSCAN_STRONGDN") ? atof(getenv("VSCAN_STRONGDN")) : -1;
            if (const char* mb = getenv("VSCAN_PHARMA_MINBARS")) c.pharmacode_min_bars = atoi(mb);
            c.disable_1d_deskew_rescue = noDeskew;
            c.disable_blank_frame_skip = noBlankSkip;
            c.disable_denoise_rescue = noDenoise;
            c.disable_region_rescue = noRegion;
            c.enable_zbar_fastpath = useZbar;
            c.enable_qr_finder_rescue = qrFinder;
            c.enable_adaptive_profile = 1;
            c.min_expected_codes = 0;
            if (const char* me = getenv("VSCAN_MINEXP")) c.min_expected_codes = atoi(me);
            shared[i] = vscan_create(&c);
        }
    }

    Gray g;
    size_t done = 0, total = files.size();

    // 한 프레임 처리. 디렉토리 모드와 stdin 스트림 모드가 이걸 공유한다 —
    // 통계/태그 집계/CSV가 두 경로에서 완전히 같은 코드로 나오게 하려고.
    auto handle = [&](const std::string& fn, const Gray& g, const Label* lab, int expected) {
        if (verbose) printf("%-*s %3d", lineW, fn.c_str(), expected);

        for (int i = 0; i < NP; ++i) {
            vscan_config_t cfg{};
            cfg.tile_overlap_px = overlap;
            cfg.fast_locate = paths[i].fast;
            cfg.enable_dpm_rescue = dpmRescue;
            cfg.max_frame_ms = maxFrameMs;
            // opt-in 심볼로지(Industrial 2of5 / COOP 2of5 / Pharmacode).
            // 기본이 꺼져 있는 것들이라 회귀에는 안 걸리고, 유령 검출을
            // 재려고 환경변수로 켠다.
            cfg.enable_industrial_2of5 = getenv("VSCAN_IND25") != nullptr;
            cfg.enable_coop_2of5       = getenv("VSCAN_COOP25") != nullptr;
            cfg.enable_pharmacode      = getenv("VSCAN_PHARMA") != nullptr;
            cfg.enable_micro_pdf417   = getenv("VSCAN_MPDF") != nullptr;
            cfg.enable_postal_japan   = getenv("VSCAN_JPPOST") != nullptr;
            cfg.enable_postal_imb     = getenv("VSCAN_IMB") != nullptr;
            cfg.enable_dotcode       = getenv("VSCAN_DOTCODE") != nullptr;
            cfg.validate_itf_checksum = getenv("VSCAN_ITFSUM") != nullptr;
            cfg.disable_auto_denoise = getenv("VSCAN_NOAUTODN") != nullptr;
            cfg.fast_no_read = getenv("VSCAN_FASTNR") != nullptr;
            cfg.auto_expected_codes = getenv("VSCAN_AUTOEXP") != nullptr;
            cfg.accurate_locate = getenv("VSCAN_ACCLOC") != nullptr;
            if (const char* me = getenv("VSCAN_MINEXP")) cfg.min_expected_codes = atoi(me);
            if (const char* df = getenv("VSCAN_FLAGS")) cfg.decode_flags = strtoul(df, nullptr, 0);
            if (const char* fm = getenv("VSCAN_FMTMASK")) cfg.symbology_mask = strtoul(fm, nullptr, 0);
            cfg.auto_denoise_strong_noise = getenv("VSCAN_STRONGDN") ? atof(getenv("VSCAN_STRONGDN")) : -1;
            cfg.frame_budget_max_ms = getenv("VSCAN_BUDGETMAX") ? atoi(getenv("VSCAN_BUDGETMAX")) : 0;
            cfg.max_frame_ms = getenv("VSCAN_MAXFRAME") ? atoi(getenv("VSCAN_MAXFRAME")) : 0;
            cfg.min_line_count = getenv("VSCAN_MINLINES") ? atoi(getenv("VSCAN_MINLINES")) : 0;
            if (const char* mb = getenv("VSCAN_PHARMA_MINBARS")) cfg.pharmacode_min_bars = atoi(mb);
            cfg.disable_1d_deskew_rescue = noDeskew;
            cfg.disable_blank_frame_skip = noBlankSkip;
            cfg.disable_denoise_rescue = noDenoise;
            cfg.disable_region_rescue = noRegion;
            cfg.enable_zbar_fastpath = useZbar;
            cfg.enable_qr_finder_rescue = qrFinder;
            // "아이템당 코드 개수를 아는 현장" 사용법: 빠른 경로가 이 개수를
            // 못 채우면 자동으로 풀스캔 승격 -> 부분 검출 방지 (PROJECT_NOTES
            // §3.2.10). 개수를 모르는 배치가 현실이라면 --no-min-expected.
            cfg.min_expected_codes = useMinExp ? expected : 0;
            // 개수를 모르는 현장 조건을 흉내내되 "많다고 가정"하는 판본을
            // 재기 위한 손잡이. 위 줄 뒤에 와야 이긴다.
            if (const char* me = getenv("VSCAN_MINEXP")) cfg.min_expected_codes = atoi(me);
            vscan_pipeline_t* p = adaptive ? shared[i] : vscan_create(&cfg);

            double best = 1e9;
            size_t n = 0;
            int textOk = 0, bad = 0, dups = 0;
            std::vector<bool> matched;     // 기대 텍스트별 검출 여부 (태그 귀속용)
            for (int rep = 0; rep < reps; ++rep) {
                auto t0 = std::chrono::steady_clock::now();
                vscan_result_t* r = paths[i].mode == 0
                    ? vscan_process_gray(p, g.px.data(), g.w, g.h, g.w)
                    : vscan_process_gray_two_stage(p, g.px.data(), g.w, g.h, g.w, 50);
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (ms < best) best = ms;
                n = r ? r->count : 0;
                if (rep == 0 && useText && lab && r) {
                    if (dumpMismatch) {
                        // 정답과 안 맞는 건이 나오면 기대/실제를 그대로 찍는다.
                        // 코퍼스 정답 자체가 틀린 경우(체크디짓 유무, UPC-A 축약
                        // 같은 함정)와 라이브러리 오디코딩을 구분하려면 필수.
                        std::vector<std::string> got;
                        for (size_t k = 0; k < r->count; ++k)
                            got.push_back(r->symbols[k].text ? r->symbols[k].text : "");
                        std::vector<std::string> exp = lab->texts;
                        std::sort(got.begin(), got.end());
                        std::sort(exp.begin(), exp.end());
                        if (got != exp) {
                            printf("  ! %s [%s]\n      exp:", fn.c_str(), paths[i].name);
                            for (auto& t : exp) printf(" \"%s\"", t.c_str());
                            printf("\n      got:");
                            for (size_t k = 0; k < r->count; ++k)
                                printf(" \"%s\"(sym=%d)", r->symbols[k].text ? r->symbols[k].text : "",
                                       static_cast<int>(r->symbols[k].symbology));
                            printf("\n");
                        }
                    }
                    // 기대 텍스트 다중집합 대조. 놓친 것(miss)과 잘못 읽은
                    // 것(misdecode)은 현장에서 심각도가 전혀 다르다 —
                    // 개수만 세면 둘이 상쇄돼서 안 보인다.
                    // 같은 코드를 두 번 돌려준 경우(중복)와 없는 코드를 만들어낸
                    // 경우(오디코딩)는 완전히 다른 문제다. 섞어 세면 안 된다 —
                    // 중복은 dedup 문제, 오디코딩은 안전성 문제.
                    std::vector<bool> used(lab->texts.size(), false);
                    for (size_t k = 0; k < r->count; ++k) {
                        const char* t = r->symbols[k].text ? r->symbols[k].text : "";
                        bool hit = false, known = false;
                        for (size_t j = 0; j < lab->texts.size(); ++j) {
                            if (lab->texts[j] == t) {
                                known = true;
                                if (!used[j]) { used[j] = true; hit = true; break; }
                            }
                        }
                        if (hit) textOk++;
                        else if (known) dups++;
                        else bad++;
                    }
                    matched = used;
                }
                vscan_free_result(r);
            }
            if (!adaptive) vscan_destroy(p);

            bool ok = static_cast<int>(n) >= expected;
            st[i].add(ok, expected, static_cast<int>(n), textOk, bad, best, dups);
            if (csv) {
                std::string tg;
                if (lab) for (size_t k = 0; k < lab->tags.size(); ++k)
                    tg += (k ? ";" : "") + lab->tags[k];
                fprintf(csv, "%s,%s,%d,%zu,%d,%d,%d,%.3f,%s\n", fn.c_str(), paths[i].name,
                        expected, n, textOk, bad, dups, best, tg.c_str());
            }
            if (useTags && lab) {
                auto slot = [&](const std::string& t) -> TagStat& {
                    auto& v = byTag[t];
                    if (v.empty()) v.resize(NP);
                    return v[i];
                };
                if (useText && !matched.empty()) {
                    // 코드별 귀속: 그 코드의 정답 텍스트가 나왔는지로 판정
                    for (size_t j = 0; j < lab->texts.size(); ++j) {
                        bool hit = j < matched.size() && matched[j];
                        for (const auto& t : lab->tags) {
                            TagStat& ts = slot(t); ts.expected++; ts.found += hit ? 1 : 0;
                        }
                        if (j < lab->codeTags.size()) {
                            for (const auto& t : split(lab->codeTags[j], '&')) {
                                if (t.empty() || t == "-") continue;
                                TagStat& ts = slot(t); ts.expected++; ts.found += hit ? 1 : 0;
                            }
                        }
                    }
                } else {
                    // 텍스트 대조가 없으면 이미지 단위로만 (개수 기준)
                    for (const auto& t : lab->tags) {
                        TagStat& ts = slot(t);
                        ts.expected += expected;
                        ts.found += std::min<long>(n, expected);
                    }
                }
                // 시간/이미지 수는 이미지당 한 번
                std::vector<std::string> seenTags = lab->tags;
                for (const auto& ct : lab->codeTags)
                    for (const auto& t : split(ct, '&'))
                        if (!t.empty() && t != "-") seenTags.push_back(t);
                std::sort(seenTags.begin(), seenTags.end());
                seenTags.erase(std::unique(seenTags.begin(), seenTags.end()), seenTags.end());
                for (const auto& t : seenTags) { TagStat& ts = slot(t); ts.images++; ts.totalMs += best; }
            }
            if (verbose) printf(" | %s n=%-2zu %7.1fms", ok ? "OK" : "--", n, best);
        }
        if (verbose) printf("\n");

        if (!verbose && (++done % 50 == 0 || done == total)) {
            if (total) fprintf(stderr, "\r  %zu/%zu ...", done, total);
            else       fprintf(stderr, "\r  %zu ...", done);
            fflush(stderr);
        }
    };

    if (!stdinMode) {
        for (const auto& fn : files) {
            if (!loadPGM(dir + "/" + fn, g)) { printf("%-*s LOAD FAIL\n", lineW, fn.c_str()); continue; }
            int expected = 1;
            const Label* lab = nullptr;
            size_t sl = fn.rfind('/');
            std::string base = (sl == std::string::npos) ? fn : fn.substr(sl + 1);
            auto it = labels.find(base);
            if (it != labels.end()) { lab = &it->second; expected = lab->expected; }
            else {
                size_t u = base.rfind('_');
                if (u != std::string::npos) expected = atoi(base.c_str() + u + 1);
            }
            handle(fn, g, lab, expected);
        }
    } else {
        // 스트림 프로토콜 (generate_corpus.py --stream 이 내보내는 것):
        //   "FRAME \t w \t h \t name \t expected \t tags(,) \t texts(|)\n"
        //   그 다음 raw grayscale w*h 바이트 (헤더 없음)
        // 이미지가 디스크에 전혀 안 닿기 때문에 장수 제한이 사라진다.
        std::string hdr;
        for (;;) {
            int c;
            hdr.clear();
            while ((c = fgetc(stdin)) != EOF && c != '\n') hdr.push_back(static_cast<char>(c));
            if (hdr.empty()) { if (c == EOF) break; else continue; }
            auto col = split(hdr, '\t');
            if (col.size() < 5 || col[0] != "FRAME") {
                fprintf(stderr, "프레임 헤더가 이상합니다: %s\n", hdr.c_str());
                break;
            }
            int w = atoi(col[1].c_str()), h = atoi(col[2].c_str());
            if (w <= 0 || h <= 0) { fprintf(stderr, "프레임 크기 오류\n"); break; }
            Label lab;
            lab.expected = atoi(col[4].c_str());
            if (col.size() >= 6) lab.tags = split(col[5], ',');
            if (col.size() >= 7) lab.texts = split(col[6], '|');
            if (col.size() >= 8) lab.codeTags = split(col[7], '|');
            g.w = w; g.h = h;
            g.px.resize(static_cast<size_t>(w) * h);
            if (fread(g.px.data(), 1, g.px.size(), stdin) != g.px.size()) {
                fprintf(stderr, "프레임이 잘렸습니다 — 스트림 종료\n");
                break;
            }
            handle(col[3], g, &lab, lab.expected);
            if (limit && static_cast<int>(done) >= limit) break;
        }
        total = done;
    }
    if (!verbose) fprintf(stderr, "\r%*s\r", 24, "");
    for (auto* p : shared) if (p) vscan_destroy(p);
    if (csv) fclose(csv);

    // ---- 요약 (기존 포맷 유지: run-tests.sh가 이 줄을 grep한다)
    printf("%s\n", std::string(lineW + 4 + NP * 21, '-').c_str());
    printf("%-*s %3s", lineW, "검출 성공 / 전체", "");
    for (int i = 0; i < NP; ++i)
        printf(" | %2d/%-2zu %11.1fms", st[i].pass, total, st[i].totalMs);
    printf("\n(시간은 %d회 중 최소값의 합계)\n", reps);

    // ---- 대량 코퍼스용 상세 통계
    // 헤더는 ASCII로 — 한글 폭 때문에 열이 어긋나면 대량 출력에서 읽기 힘들다.
    printf("\n(img_pass=이미지 합격률, codes=코드 단위 검출, text_ok=텍스트 일치,"
           " misdec=없는 코드를 만들어냄, dup=같은 코드 중복 반환)\n");
    printf("%-14s%9s %13s %9s %7s %6s %9s %9s %9s\n", "path", "img_pass", "codes",
           "text_ok", "misdec", "dup", "mean_ms", "p50_ms", "p95_ms");
    for (int i = 0; i < NP; ++i) {
        Stat& s = st[i];
        printf("%-14s", paths[i].name);
        printf("%8.1f%% %6ld/%-6ld %8.1f%% %7ld %6ld %9.2f %9.2f %9.2f\n",
               s.pct(s.pass, s.images), s.found, s.expected,
               useText ? s.pct(s.textOk, s.expected) : 0.0, s.misdecode, s.dup,
               s.mean(), s.quant(0.5), s.quant(0.95));
    }
    if (!useText && haveLabels) printf("(텍스트 대조 생략)\n");

    // ---- 판독 가능성 버킷별: 이게 헤드라인 숫자다
    // 난수 코퍼스에는 물리적으로 못 읽는 이미지가 섞인다. 그걸 포함한
    // 전체 검출률은 "우리 성능"이 아니라 "우리 성능 + 물리 한계"의 혼합이다.
    // dec-ok 행만 보면 개선 여지가 있는 부분의 진짜 성공률이 나온다.
    {
        const char* names[3] = {"dec-ok", "dec-borderline", "dec-impossible"};
        // 라벨은 ASCII — 한글 폭 때문에 열이 어긋난다
        const char* label[3] = {"ok  (읽혀야 정상)", "borderline (경계)", "impossible (난독)"};
        bool any = false;
        for (int b = 0; b < 3; ++b) if (byTag.count(names[b])) any = true;
        if (any) {
            printf("\n판독 가능성 버킷별 (코드 단위) — 물리적으로 읽을 수 있는가로 분류\n");
            printf("%-18s %7s", "bucket", "codes");
            for (auto& p : paths) printf(" | %-16s", p.name);
            printf("\n%s\n", std::string(30 + NP * 19, '-').c_str());
            for (int b = 0; b < 3; ++b) {
                auto it2 = byTag.find(names[b]);
                if (it2 == byTag.end()) continue;
                printf("%-26s %7ld", label[b], it2->second[0].expected);
                for (int i = 0; i < NP; ++i) {
                    TagStat& t = it2->second[i];
                    printf(" | %5.1f%% %8.2f", t.rate(), t.mean());
                }
                printf("\n");
            }
            printf("★ 개선 지표는 'ok' 행이다. 'impossible'은 실패가 정상이고,\n"
                   "  100%%에 가까우면 오히려 분류 기준이 느슨하다는 뜻이다.\n");
        }
    }

    // ---- 조건 태그별 집계: "무엇이 느리고 무엇을 놓치는가"
    if (useTags && !byTag.empty()) {
        printf("\n조건 태그별 (코드 단위 검출률 %% / 이미지당 평균 ms)\n");
        printf("%-22s %6s", "tag", "imgs");
        for (auto& p : paths) printf(" | %-16s", p.name);
        printf("\n%s\n", std::string(29 + NP * 19, '-').c_str());
        std::vector<std::pair<std::string, std::vector<TagStat>*>> rows;
        for (auto& kv : byTag) rows.push_back({kv.first, &kv.second});
        // 스윕(angle=0..359 같은 k=v 태그)은 이름순이어야 그래프처럼 읽힌다.
        // 난수 코퍼스는 표본이 많은 태그부터 보는 게 유용하다.
        bool sweepish = tagSortName || (!rows.empty() &&
            rows.front().first.find('=') != std::string::npos);
        if (sweepish)
            std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
        else
            std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
                return (*a.second)[0].images > (*b.second)[0].images;
            });
        for (auto& r : rows) {
            printf("%-22s %6d", r.first.c_str(), (*r.second)[0].images);
            for (int i = 0; i < NP; ++i) {
                TagStat& s = (*r.second)[i];
                printf(" | %5.1f%% %8.2f", s.rate(), s.mean());
            }
            printf("\n");
        }
        printf("\n태그는 한 이미지/코드에 여러 개 붙으므로 행 합계는 전체와 다르다.\n"
               "절대 수치보다 **변경 전/후 같은 태그 행의 차이**를 볼 것.\n");
    }
    if (!csvPath.empty()) printf("CSV: %s\n", csvPath.c_str());
    return 0;
}
