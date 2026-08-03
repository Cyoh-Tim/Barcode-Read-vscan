/*
 * field_diagnose.cpp — 현장 자가진단.
 *
 * ## 왜 이게 필요한가
 *
 * 이 저장소의 정확도 수치는 전부 **합성 이미지**에서 나온 것이다. 그런데
 * 실기 프레임을 모은다고 해결되지도 않는다 — 렌즈/작업거리/조명/기재가
 * 바뀌면 그 숫자도 같이 무너지기 때문이다. 실제로 이 저장소는 그 함정에
 * 한 번 빠진 적이 있다(§6.4.5b: 실기에서 안 읽힌다고 판단했던 것이
 * 라이브러리가 아니라 **캡처 단계** 문제였다).
 *
 * 그래서 방향을 바꾼다. **우리가 모든 환경을 재는 대신, 각 환경이 스스로
 * 재게 한다.** 환경이 바뀌어도 그대로 쓰이는 것은 숫자가 아니라
 *   (1) 실패가 어느 단계에서 나는가
 *   (2) 어떤 손잡이가 어떤 대가로 무엇을 바꾸는가
 * 이 둘이고, 이 도구는 그 둘을 **현장의 프레임으로** 뽑아준다.
 *
 * ## 정답 라벨이 없다는 것의 뜻 — 반드시 읽을 것
 *
 * 현장에는 정답표가 없다. 그래서 이 도구는 **오디코딩을 잴 수 없다.**
 * 코드가 더 많이 나오는 설정이 더 좋은 설정이라고 말할 수 없다는 뜻이다 —
 * 늘어난 것이 유령일 수도 있다(이 저장소 실측으로 난수 코퍼스에서
 * 571코드당 1건의 유령이 나온다, §7).
 *
 * 그래서 대신 **설정 간 합의**를 본다. 여러 설정이 같은 자리에서 같은
 * 문자열을 내면 그건 실제 코드일 가능성이 높고, 한 설정에서만 나오면
 * 표시해서 사람이 확인하게 한다. 이 도구의 출력은 "정답"이 아니라
 * **"어디를 봐야 하는지"** 다.
 *
 * ## 쓰는 법
 *
 *     field_diagnose <프레임_디렉터리> [--reps 3]
 *
 * 디렉터리 안의 .pgm(P5) 파일을 전부 읽는다. 정답 파일은 필요 없다.
 * 현장에서 실제로 들어오는 프레임을 그대로 몇 장~수십 장 넣으면 된다.
 * [[vscan-lite-field-diagnose]]
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "vscan.h"

namespace {

struct Frame {
    std::string name;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
};

bool loadPGM(const std::string& path, Frame& f) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[3] = {0};
    int maxv = 0;
    if (fscanf(fp, "%2s %d %d %d", magic, &f.w, &f.h, &maxv) != 4 ||
        strcmp(magic, "P5") != 0 || f.w <= 0 || f.h <= 0) {
        fclose(fp);
        return false;
    }
    fgetc(fp);
    f.px.resize(static_cast<size_t>(f.w) * f.h);
    const bool ok = fread(f.px.data(), 1, f.px.size(), fp) == f.px.size();
    fclose(fp);
    return ok;
}

// ---------------------------------------------------------------------------
// 프레임 통계 — "이 프레임이 어떤 종류의 어려움을 갖고 있나"
// ---------------------------------------------------------------------------
struct Stats {
    double mean = 0;       // 평균 밝기. 낮으면 노출 부족.
    int localRange = 0;    // 8x8 블록 명암 폭의 상위 1% 값.
    double noise = 0;      // 3x3 평균과의 차이로 잰 노이즈 시그마 추정.
    /*
     * [흐림 지표는 넣었다가 뺐다 — 실측으로 못 가른다]
     * 이웃 화소 차이의 평균을 흐림 척도로 쓰려 했는데, 깨끗한 프레임이
     * 0.83~5.05, 일부러 블러를 먹인 프레임이 3.45~4.59로 나왔다. 겹친다.
     * 이 값은 초점이 아니라 **프레임에서 코드가 차지하는 면적**을 재고
     * 있었다(빈 배경이 넓으면 낮게 나온다). 코드 영역에서만 재야 뜻이
     * 생기는데 그러려면 로케이터가 필요하고, 그건 이 도구의 범위를 넘는다.
     * 방어할 수 없는 숫자는 안 내보내는 편이 낫다.
     */
};

Stats measure(const Frame& f) {
    Stats s;
    long long sum = 0;
    for (uint8_t v : f.px) sum += v;
    s.mean = static_cast<double>(sum) / f.px.size();

    // 블록 명암 폭 — zxing 이진화가 "구조 없음"으로 보는 기준(24)과 같은 척도.
    std::vector<int> ranges;
    for (int by = 0; by + 8 <= f.h; by += 8) {
        for (int bx = 0; bx + 8 <= f.w; bx += 8) {
            int lo = 255, hi = 0;
            for (int y = by; y < by + 8; ++y)
                for (int x = bx; x < bx + 8; ++x) {
                    const int v = f.px[static_cast<size_t>(y) * f.w + x];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            ranges.push_back(hi - lo);
        }
    }
    if (!ranges.empty()) {
        const size_t k = ranges.size() * 99 / 100;
        std::nth_element(ranges.begin(), ranges.begin() + k, ranges.end());
        s.localRange = ranges[k];
    }

    // 노이즈: 3x3 평균과의 차이. 코드의 모듈 신호는 3x3에서 크게 안 죽지만
    // 화소 단위 노이즈는 죽으므로, 차이의 표준편차가 노이즈 쪽에 가깝다.
    double acc = 0, acc2 = 0;
    long n = 0;
    for (int y = 1; y + 1 < f.h; y += 3) {
        for (int x = 1; x + 1 < f.w; x += 3) {
            int m = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    m += f.px[static_cast<size_t>(y + dy) * f.w + (x + dx)];
            const double d = f.px[static_cast<size_t>(y) * f.w + x] - m / 9.0;
            acc += d;
            acc2 += d * d;
            ++n;
        }
    }
    if (n > 1) {
        const double mu = acc / n;
        s.noise = std::sqrt(std::max(0.0, acc2 / n - mu * mu));
    }
    return s;
}

// ---------------------------------------------------------------------------
// 설정 조합
// ---------------------------------------------------------------------------
struct Variant {
    const char* name;
    const char* why;          // 왜 이걸 시도하는가 (사람이 읽는 설명)
    bool twoStage = false;
    void (*apply)(vscan_config_t&) = nullptr;
};

void cfgBase(vscan_config_t&) {}
void cfgNoTileFb(vscan_config_t& c) { c.disable_tile_fallback = 1; }
void cfgQrFinder(vscan_config_t& c) { c.enable_qr_finder_rescue = 1; }
void cfgItfSum(vscan_config_t& c) { c.validate_itf_checksum = 1; }
void cfgLines6(vscan_config_t& c) { c.min_line_count = 6; }

const Variant kVariants[] = {
    {"기본(full)", "vscan_process_gray() 그대로", false, cfgBase},
    {"2단계", "vscan_process_gray_two_stage(). 훨씬 싸다. 단 기대 코드 수를 안 주면"
  " 빠른 패스가 찾은 만큼에서 멈추므로 코드가 여럿인 프레임에서는 덜 나올 수 있다"
  " — 그때는 min_expected_codes를 주면 된다", true, cfgBase},
    {"타일폴백 끔", "실패 프레임마다 프레임 전체를 한 번 더 훑는 단계를 끈다", false, cfgNoTileFb},
    {"QR파인더 구제", "작은 QR이 여럿 흩뿌려진 프레임용 (기본 OFF)", false, cfgQrFinder},
    {"ITF 체크섬 강제", "ITF 유령을 막는다. 체크디짓 없는 ITF는 버려진다", false, cfgItfSum},
    {"스캔라인 6", "1D 부분 스캔 유령을 줄인다. 얇게 잡히는 정상 코드도 같이 잃는다", false, cfgLines6},
};
constexpr int kNumVariants = static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]));

struct Found {
    std::string text;
    int sym = 0;
    int cx = 0, cy = 0;
};

struct Run {
    std::vector<std::vector<Found>> perFrame;
    double totalMs = 0;
    int codes = 0;
    int framesWithCode = 0;
};

Run runVariant(const Variant& v, const std::vector<Frame>& frames, int reps) {
    vscan_config_t cfg{};
    cfg.tile_overlap_px = 500;
    if (v.apply) v.apply(cfg);
    vscan_pipeline_t* p = vscan_create(&cfg);
    Run r;
    r.perFrame.resize(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame& f = frames[i];
        double best = 1e18;
        for (int k = 0; k < reps; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            vscan_result_t* res = v.twoStage
                ? vscan_process_gray_two_stage(p, f.px.data(), f.w, f.h, f.w, 50)
                : vscan_process_gray(p, f.px.data(), f.w, f.h, f.w);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            if (ms < best) best = ms;
            if (k == 0 && res) {
                for (size_t j = 0; j < res->count; ++j) {
                    Found fd;
                    fd.text = res->symbols[j].text ? res->symbols[j].text : "";
                    fd.sym = static_cast<int>(res->symbols[j].symbology);
                    int sx = 0, sy = 0;
                    for (int c = 0; c < 4; ++c) {
                        sx += res->symbols[j].corners[c].x;
                        sy += res->symbols[j].corners[c].y;
                    }
                    fd.cx = sx / 4;
                    fd.cy = sy / 4;
                    r.perFrame[i].push_back(std::move(fd));
                }
            }
            vscan_free_result(res);
        }
        r.totalMs += best;
        r.codes += static_cast<int>(r.perFrame[i].size());
        if (!r.perFrame[i].empty()) ++r.framesWithCode;
    }
    vscan_destroy(p);
    return r;
}

const char* symName(int s) {
    switch (s) {
        case 1: return "QR"; case 2: return "MicroQR";
        case 3: return "DataMatrix"; case 4: return "GS1-DataMatrix";
        case 5: return "PDF417"; case 6: return "MicroPDF417";
        case 9: return "Code39"; case 12: return "ITF";
        case 16: return "Code128"; case 17: return "GS1-128";
        case 18: return "GS1-DataBar"; case 19: return "Code93";
        case 20: return "EAN/UPC"; default: return "기타";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "사용: %s <프레임_디렉터리> [--reps N]\n"
                     "  디렉터리 안의 .pgm(P5)을 전부 읽는다. 정답 파일은 필요 없다.\n",
                     argv[0]);
        return 2;
    }
    int reps = 3;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) reps = std::atoi(argv[++i]);

    std::vector<Frame> frames;
    {
        DIR* d = opendir(argv[1]);
        if (!d) { std::fprintf(stderr, "디렉터리를 못 연다: %s\n", argv[1]); return 2; }
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0) names.push_back(n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            Frame f;
            f.name = n;
            if (loadPGM(std::string(argv[1]) + "/" + n, f)) frames.push_back(std::move(f));
        }
    }
    if (frames.empty()) { std::fprintf(stderr, "읽을 수 있는 .pgm이 없다\n"); return 2; }

    std::printf("vscan 현장 자가진단 — 프레임 %zu장 (reps %d)\n", frames.size(), reps);
    std::printf("%s\n", std::string(78, '=').c_str());

    // ---- 1) 프레임이 어떤 어려움을 갖고 있나 --------------------------------
    std::printf("\n[1] 프레임 통계\n");
    int darkN = 0, lowContrastN = 0, noisyN = 0;
    double meanLo = 1e9, meanHi = -1e9;
    int lrLo = 1 << 30, lrHi = 0;
    for (const auto& f : frames) {
        const Stats s = measure(f);
        meanLo = std::min(meanLo, s.mean);
        meanHi = std::max(meanHi, s.mean);
        lrLo = std::min(lrLo, s.localRange);
        lrHi = std::max(lrHi, s.localRange);
        if (s.mean < 96) ++darkN;
        if (s.localRange < 100) ++lowContrastN;
        if (s.noise > 8.0) ++noisyN;
    }
    std::printf("  해상도        %dx%d\n", frames[0].w, frames[0].h);
    std::printf("  평균 밝기     %.0f ~ %.0f%s\n", meanLo, meanHi,
                darkN ? "" : "  (정상 범위)");
    if (darkN) std::printf("     -> 노출 부족 의심 %d장 (평균 밝기 96 미만)\n", darkN);
    std::printf("  국소 명암폭   %d ~ %d  (zxing 이진화 기준선은 24)\n", lrLo, lrHi);
    if (lowContrastN) std::printf("     -> 저대비 의심 %d장 (상위 1%% 블록조차 100 미만)\n", lowContrastN);
    if (noisyN) std::printf("  노이즈 큼     %d장\n", noisyN);

    // ---- 2) 설정별 결과 ----------------------------------------------------
    std::printf("\n[2] 설정별 결과\n");
    // 한글은 printf의 폭 지정이 바이트 기준이라 정렬이 깨진다. 이름을 줄
    // 머리에 따로 찍고 수치만 정렬한다.
    std::printf("  (검출프레임 / 코드 / 평균ms)\n");
    std::vector<Run> runs;
    runs.reserve(kNumVariants);
    for (int v = 0; v < kNumVariants; ++v) {
        runs.push_back(runVariant(kVariants[v], frames, reps));
        const Run& r = runs.back();
        std::printf("  %s\n      %d/%zu 프레임, 코드 %d개, 평균 %.1fms\n      %s\n",
                    kVariants[v].name, r.framesWithCode, frames.size(), r.codes,
                    r.totalMs / frames.size(), kVariants[v].why);
    }

    // ---- 3) 설정 간 합의 ---------------------------------------------------
    //
    // 정답표가 없으므로 "많이 나오면 좋다"고 말할 수 없다. 대신 여러 설정이
    // 같은 문자열을 내면 실제 코드일 가능성이 높다고 보고, 한 설정에서만
    // 나온 것은 **사람이 확인할 대상**으로 표시한다.
    std::printf("\n[3] 설정 간 합의 — 정답표가 없으므로 이것이 신뢰도의 대용품이다\n");
    int confirmed = 0, single = 0;
    std::vector<std::string> singles;
    for (size_t i = 0; i < frames.size(); ++i) {
        std::map<std::string, int> votes;
        for (const auto& r : runs)
            for (const auto& f : r.perFrame[i]) ++votes[f.text];
        for (const auto& kv : votes) {
            if (kv.second >= 2) ++confirmed;
            else {
                ++single;
                if (singles.size() < 10) {
                    // 어느 프레임의 무엇인지 같이 남긴다.
                    for (size_t v = 0; v < runs.size(); ++v)
                        for (const auto& f : runs[v].perFrame[i])
                            if (f.text == kv.first) {
                                singles.push_back(frames[i].name + "  \"" + kv.first +
                                                  "\" (" + symName(f.sym) + ", " +
                                                  kVariants[v].name + "에서만)");
                                break;
                            }
                }
            }
        }
    }
    std::printf("  두 설정 이상에서 같이 나온 코드   %d개\n", confirmed);
    std::printf("  한 설정에서만 나온 코드           %d개%s\n", single,
                single ? "   <- 눈으로 확인할 것" : "");
    for (const auto& s : singles) std::printf("     %s\n", s.c_str());

    // ---- 4) 권고 -----------------------------------------------------------
    std::printf("\n[4] 권고\n");
    const Run& base = runs[0];
    const Run& two = runs[1];
    int n = 0;

    if (two.codes >= base.codes && two.totalMs < base.totalMs * 0.8)
        std::printf("  %d) **2단계 경로를 쓸 것.** 검출은 %d -> %d개로 안 줄면서\n"
                    "     평균이 %.1f -> %.1fms다.\n",
                    ++n, base.codes, two.codes, base.totalMs / frames.size(),
                    two.totalMs / frames.size());
    else if (two.codes < base.codes) {
        // [값이 실제로 싼지 확인하고 말한다] 2단계가 항상 싼 것은 아니다 —
        // 코드가 여럿이면 승격이 걸려서 오히려 비싸질 수 있다. 실측으로
        // 그런 프레임 묶음을 봤다(106 -> 114ms). 사실과 다른 문구를
        // 찍지 않도록 여기서 갈라 쓴다.
        const double bMs = base.totalMs / frames.size();
        const double tMs = two.totalMs / frames.size();
        std::printf("  %d) 2단계가 코드 %d -> %d개로 덜 찾는다. 한 프레임에 코드가\n"
                    "     여럿인 배치다 — **min_expected_codes에 기대 개수를 주면**\n"
                    "     2단계가 그 수를 채울 때까지 승격해서 검출을 회복한다.\n",
                    ++n, base.codes, two.codes);
        if (tMs < bMs * 0.9)
            std::printf("     값도 %.1f -> %.1fms로 싸다.\n", bMs, tMs);
        else
            std::printf("     다만 이 프레임들에서는 값이 %.1f -> %.1fms로 **더 비싸다** —\n"
                        "     승격이 자주 걸린다는 뜻이다. 검출이 같다면 기본 경로가 낫다.\n",
                        bMs, tMs);
    }

    // 심볼로지가 한두 종뿐이면 마스크를 좁히라고 권한다 — 실측 2.3~2.9배.
    std::set<int> syms;
    for (const auto& r : runs)
        for (const auto& pf : r.perFrame)
            for (const auto& f : pf) syms.insert(f.sym);
    if (!syms.empty() && syms.size() <= 3) {
        std::printf("  %d) **symbology_mask를 좁힐 것.** 이 프레임들에는 ", ++n);
        for (int s : syms) std::printf("%s ", symName(s));
        std::printf("만 나온다.\n     실측상 2.3~2.9배 빨라지고 검출 손실은 0이었다.\n");
    }

    if (syms.count(12))
        std::printf("  %d) ITF가 있다. 체크디짓이 규격상 필수인 배치(ITF-14 등)라면\n"
                    "     **validate_itf_checksum=1**을 켤 것 — 실측상 1도 간격 각도\n"
                    "     스윕에서 ITF 유령 13건이 0이 됐고 검출 손실은 없었다.\n"
                    "     체크디짓 없는 ITF를 쓴다면 켜면 안 된다(전부 잃는다).\n", ++n);

    if (lowContrastN * 2 >= static_cast<int>(frames.size()))
        std::printf("  %d) 저대비 프레임이 절반 이상이다. 조명을 먼저 볼 것 —\n"
                    "     라이브러리로 여는 것보다 대비를 올리는 쪽이 훨씬 싸다.\n"
                    "     (실측: 2D 심볼은 대비 0.10 아래에서 원리적으로 어렵다)\n", ++n);
    if (darkN * 2 >= static_cast<int>(frames.size()))
        std::printf("  %d) 노출 부족 프레임이 절반 이상이다. 게인보다 **노출/조리개**를\n"
                    "     먼저 볼 것 — 게인은 노이즈를 같이 올린다.\n", ++n);

    const Run& noFb = runs[2];
    if (noFb.codes == base.codes && noFb.totalMs < base.totalMs * 0.9)
        std::printf("  %d) **disable_tile_fallback=1**을 검토할 것. 이 프레임들에서는\n"
                    "     검출이 같은데 평균이 %.0f -> %.0fms다. 단, 타일 창보다 큰\n"
                    "     코드가 들어오는 배치에서는 그 코드를 통째로 잃는다.\n",
                    ++n, base.totalMs / frames.size(), noFb.totalMs / frames.size());

    if (base.framesWithCode < static_cast<int>(frames.size()))
        std::printf("  %d) 검출 실패 프레임이 %zu장 있다. `VSPROF=1`을 주고 그 프레임만\n"
                    "     다시 돌리면 **어느 단계에서 끊겼는지** stderr에 찍힌다.\n"
                    "     (core / tilefb / region / denoise / dpm / deskew1d)\n",
                    ++n, frames.size() - base.framesWithCode);

    if (n == 0) std::printf("  특별히 바꿀 것이 안 보인다. 기본 설정으로 충분하다.\n");

    std::printf("\n주의: 정답표가 없으므로 이 도구는 **오디코딩을 측정하지 못한다.**\n"
                "코드 개수가 늘었다고 좋아진 것이 아닐 수 있다 — [3]의 \"한 설정에서만\n"
                "나온 코드\"를 반드시 눈으로 확인할 것.\n");
    return 0;
}
