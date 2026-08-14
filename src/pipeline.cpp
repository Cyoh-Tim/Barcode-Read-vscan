#include "vscan_internal/pipeline.hpp"
#include <cstdio>
#include <cstdlib>
#include "vscan_internal/decoder_zxing.hpp"
#include "vscan_internal/decoder_linear.hpp"
#include "vscan_internal/decoder_micropdf417.hpp"
#include "vscan_internal/gs1_composite.hpp"
#include "vscan_internal/decoder_dotcode.hpp"
#include "vscan_internal/decoder_postal.hpp"
#include "vscan_internal/preprocess.hpp"
#include "vscan_internal/deskew1d.hpp"
#include <cstdlib>
#include "vscan_internal/locate.hpp"
#include "vscan_internal/deskew_persp.hpp"
#include "vscan_internal/pitch_equalize.hpp"
#include "vscan_internal/locate_qr.hpp"
#ifdef VSCAN_HAVE_ZBAR
#include "vscan_internal/decoder_zbar.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <cstring>
#include <future>
#include <thread>

namespace vscan {

Pipeline::Pipeline(PipelineConfig cfg) : cfg_(cfg) {
    // 기본으로 zxing-cpp 디코더를 등록한다. Pharmacode/DotCode/ZBar 등
    // 자체/추가 디코더는 addDecoder()로 추가하면 결과가 자동 병합된다.
    decoders_.push_back(std::make_unique<ZXingDecoder>(cfg_.formatMask, cfg_.tryRotate, cfg_.tryInvert,
                                                         cfg_.tryHarder, cfg_.binarizer,
                                                         cfg_.tryDownscale, cfg_.tryCode39ExtendedMode,
                                                         cfg_.minLineCount, cfg_.validateITFCheckSum,
                                                         cfg_.downscaleThreshold));

    // zxing에 포맷이 없는 1D 심볼로지. 셋 다 꺼져 있으면 등록조차 안 한다 —
    // 디코더 목록이 하나면 파이프라인이 스레드를 안 띄우는 빠른 길을 탄다.
    if (cfg_.enableIndustrial2of5 || cfg_.enableCoop2of5 || cfg_.enablePharmacode) {
        LinearDecoderOptions lo;
        lo.industrial2of5 = cfg_.enableIndustrial2of5;
        lo.coop2of5       = cfg_.enableCoop2of5;
        lo.pharmacode     = cfg_.enablePharmacode;
        if (cfg_.pharmacodeMinBars > 0) lo.minPharmacodeBars = cfg_.pharmacodeMinBars;
        decoders_.push_back(std::make_unique<LinearDecoder>(lo));
    }
    if (cfg_.enableMicroPdf417)
        decoders_.push_back(std::make_unique<MicroPdf417Decoder>());
    if (cfg_.enablePostalJapan || cfg_.enablePostalImb) {
        PostalOptions po;
        po.japanPost = cfg_.enablePostalJapan;
        po.imb = cfg_.enablePostalImb;
        // 타일이 아니라 프레임 전체를 본다 — 이유는 pipeline.hpp의
        // fullFrameDecoders_ 주석.
        fullFrameDecoders_.push_back(std::make_unique<PostalDecoder>(po));
    }
    if (cfg_.enableDotCode) {
        DotCodeOptions dco;
        dco.enabled = true;
        // 우편과 같은 이유로 프레임 전체를 본다 — 점 격자가 타일 경계에
        // 걸리면 어느 타일에도 온전히 안 들어간다.
        fullFrameDecoders_.push_back(std::make_unique<DotCodeDecoder>(dco));
    }
#ifdef VSCAN_HAVE_ZBAR
    // 설정으로 켰으면 여기서 등록한다 — 내부에서 만드는 임시 Pipeline들이
    // cfg_를 복사하므로 자동으로 같이 따라간다.
    // [[vscan-lite-zbar-in-subpipelines]]
    // ZBar는 **디코더 목록에 안 넣는다** — 그러면 폴백 체인의 모든 패스에서
    // 매번 돌아 코퍼스 평균이 3.2배가 된다(실측 149 -> 478ms). 대신
    // tryZBarRescue()에서 실패 프레임에 한 번만 쓴다.
    // zbarAsRescue를 끄면 예전처럼 상시 디코더로 동작한다.
    // [[vscan-lite-zbar-rescue]]
    if (cfg_.enableZBar && !cfg_.zbarAsRescue)
        decoders_.push_back(std::make_unique<ZBarDecoder>());
#endif
}

// [마감은 최상위 호출이 소유한다]
// 예전에는 maxFrameMs가 켜져 있을 때만 소유권을 잡았다. 그런데 자기 보정
// 예산(armSelfBudget)이 생기면서 maxFrameMs가 꺼져 있어도 마감이 켜진다 —
// 그 경우 소유자가 없어서 프레임이 끝나도 해제되지 않았다. 파이프라인을
// 재사용하는 호출자(연속 프레임)에서는 **두 번째 프레임부터 이미 지난
// 마감을 들고 시작**해서 모든 구제가 즉시 잘렸다. 실측으로 40종이 한 장씩
// 따로 돌리면 40/40인데 한 번에 돌리면 36/40이었다.
namespace { void overArm(std::chrono::steady_clock::time_point dl, bool on); void overDump(); }

Pipeline::BudgetGuard::BudgetGuard(Pipeline* pp) : p(pp), owner(false) {
    if (p->deadlineOwned_) return;              // 중첩 호출(ROI 서브 파이프라인 등)
    p->deadlineOwned_ = true;
    owner = true;
    p->deadlineActive_ = false;
    if (p->cfg_.maxFrameMs > 0) {
        p->deadline_ = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(p->cfg_.maxFrameMs);
        p->deadlineActive_ = true;
    }
    overArm(p->deadline_, p->deadlineActive_);
}
Pipeline::BudgetGuard::~BudgetGuard() {
    if (owner) {
        overDump();
        p->deadlineActive_ = false; p->deadlineOwned_ = false; p->firstRectActive_ = false;
    }
}

// 첫 패스 시간을 재고 나서 마감을 잡는다. 이미 maxFrameMs로 잡힌 마감이
// 있으면 **더 이른 쪽**을 쓴다.
void Pipeline::armSelfBudget(double firstPassMs) {
    if (cfg_.frameBudgetXFirstPass <= 0.0f || firstPassMs <= 0.0) return;
    double total = std::max(firstPassMs * cfg_.frameBudgetXFirstPass,
                            static_cast<double>(cfg_.frameBudgetFloorMs));
    // [절대 상한] 상대값만 쓰면 느린 하드웨어에서 예산이 같이 부푼다 —
    // 근거와 실기 수치는 pipeline.hpp의 frameBudgetMaxMs 주석.
    if (cfg_.frameBudgetMaxMs > 0)
        total = std::min(total, static_cast<double>(cfg_.frameBudgetMaxMs));
    const auto self = std::chrono::steady_clock::now() +
                      std::chrono::microseconds(static_cast<long long>(
                          std::max(0.0, total - firstPassMs) * 1000.0));
    if (!deadlineActive_ || self < deadline_) { deadline_ = self; deadlineActive_ = true; }
    overArm(deadline_, deadlineActive_);
    // 첫 영역용 마감은 더 멀리 잡는다(위 frameBudgetXFirstRegion 주석).
    double totalFirst = std::max(firstPassMs * std::max(cfg_.frameBudgetXFirstRegion,
                                                        cfg_.frameBudgetXFirstPass),
                                 static_cast<double>(cfg_.frameBudgetFloorMs));
    if (cfg_.frameBudgetFirstRegionCapMs > 0)
        totalFirst = std::min(totalFirst,
                              firstPassMs + static_cast<double>(cfg_.frameBudgetFirstRegionCapMs));
    if (cfg_.frameBudgetMaxMs > 0)
        totalFirst = std::min(totalFirst, static_cast<double>(cfg_.frameBudgetMaxMs));
    deadlineFirst_ = std::chrono::steady_clock::now() +
                     std::chrono::microseconds(static_cast<long long>(
                         std::max(0.0, totalFirst - firstPassMs) * 1000.0));
}

namespace {
// [프로파일] VSPROF=1 이면 단계별 누적 시간을 프레임 끝에 stderr로 뱉는다.
// 개발용 계측이며 환경변수가 없으면 clock 호출조차 안 한다.
struct Prof {
    bool on = false;
    std::vector<std::pair<const char*, double>> rows;
    void add(const char* k, double ms) {
        if (!on) return;
        for (auto& r : rows) if (r.first == k) { r.second += ms; return; }
        rows.push_back({k, ms});
    }
    void dump(const char* tag) {
        if (!on) return;
        double t = 0; for (auto& r : rows) t += r.second;
        fprintf(stderr, "[prof] %s 합계 %.1fms |", tag, t);
        for (auto& r : rows) fprintf(stderr, " %s=%.1f", r.first, r.second);
        fprintf(stderr, "\n");
        rows.clear();
    }
};
Prof& prof() { static thread_local Prof p{getenv("VSPROF") != nullptr, {}}; return p; }

// [넘침 계측] VSOVER=1 이면 **마감을 넘긴 뒤에도 돌던 단계**를 뱉는다.
// maxFrameMs가 설정값의 2배 넘게 넘치는 이유를 찾으려면 "얼마나 넘쳤나"가
// 아니라 "누가 넘겼나"를 알아야 한다. 마감을 단계 사이에서만 보므로,
// 마감 이후에 시작했거나 마감을 걸쳐 끝난 단계가 곧 범인이다.
struct Over {
    bool on = false;
    bool active = false;
    std::chrono::steady_clock::time_point dl;
    std::vector<std::pair<const char*, double>> rows;
    void note(const char* k, std::chrono::steady_clock::time_point t0,
              std::chrono::steady_clock::time_point t1) {
        if (!on || !active || t1 <= dl) return;
        // 마감 이후 구간만 센다(마감 전에 시작했으면 걸친 부분만).
        const auto from = t0 > dl ? t0 : dl;
        const double ms = std::chrono::duration<double, std::milli>(t1 - from).count();
        for (auto& r : rows) if (r.first == k) { r.second += ms; return; }
        rows.push_back({k, ms});
    }
    void dump() {
        if (!on || rows.empty()) return;
        double t = 0; for (auto& r : rows) t += r.second;
        fprintf(stderr, "[over] 마감초과 %.1fms |", t);
        for (auto& r : rows) fprintf(stderr, " %s=%.1f", r.first, r.second);
        fprintf(stderr, "\n");
        rows.clear();
    }
};
Over& over() { static thread_local Over o{getenv("VSOVER") != nullptr, false, {}, {}}; return o; }
void overArm(std::chrono::steady_clock::time_point dl, bool on) {
    if (!over().on) return;
    over().dl = dl; over().active = on;
}
void overDump() { over().dump(); over().active = false; }

struct Stage {
    const char* key;
    std::chrono::steady_clock::time_point t0;
    explicit Stage(const char* k) : key(k), t0(std::chrono::steady_clock::now()) {}
    ~Stage() {
        if (!prof().on && !over().on) return;
        const auto t1 = std::chrono::steady_clock::now();
        over().note(key, t0, t1);
        prof().add(key, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
};
}  // namespace

bool Pipeline::budgetExceeded() const {
    if (!deadlineActive_) return false;
    const auto now = std::chrono::steady_clock::now();
    return now >= (firstRectActive_ ? deadlineFirst_ : deadline_);
}

namespace {
// Symbology -> VSCAN_FMT_* 비트. maskToFormats()(decoder_zxing.cpp)의 역방향.
uint32_t symbologyBit(Symbology s) {
    switch (s) {
        case Symbology::QR:                 return 1u << 0;
        case Symbology::MICRO_QR:           return 1u << 1;
        case Symbology::DATA_MATRIX:
        case Symbology::GS1_DATA_MATRIX:    return 1u << 2;
        case Symbology::PDF417:
        case Symbology::MICRO_PDF417:       return 1u << 3;
        case Symbology::CODE39:
        case Symbology::CODE39_FULL_ASCII:
        case Symbology::TRIOPTIC_CODE39:    return 1u << 4;
        case Symbology::CODE93:             return 1u << 5;
        case Symbology::CODE128:
        case Symbology::GS1_128:            return 1u << 6;
        case Symbology::ITF:
        case Symbology::INDUSTRIAL_2OF5:
        case Symbology::COOP_2OF5:          return 1u << 7;
        case Symbology::CODABAR:            return 1u << 8;
        case Symbology::GS1_DATABAR:        return (1u << 9) | (1u << 10);
        // EAN/UPC는 zxing이 네 포맷을 한 묶음으로 다룬다 — 하나만 봤다고
        // 나머지를 끄면 같은 계열에서 놓칠 수 있으므로 통째로 켠다.
        case Symbology::EAN_UPC:            return (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14);
        default:                            return 0;
    }
}
}

void Pipeline::applyFormatMask(uint32_t mask) {
    cfg_.formatMask = mask;                       // 하위 파이프라인이 상속받는다
    for (auto& d : decoders_) d->setFormatMask(mask);
    for (auto& d : fullFrameDecoders_) d->setFormatMask(mask);
}

void Pipeline::adaptiveObserve(const std::vector<PipelineResult>& hits) {
    if (!cfg_.enableAdaptiveProfile || hits.empty()) return;
    uint32_t seen = 0;
    for (const auto& r : hits) seen |= symbologyBit(r.symbol.symbology);
    if (seen == 0) return;                        // 매핑 없는 심볼로지는 판단 보류

    if (adaptiveNarrowed_) {
        // 좁힌 상태에서 예상 밖 심볼로지가 나오면(있을 수 없지만 방어적으로)
        // 즉시 넓힌다.
        if (seen & ~adaptiveObservedMask_) adaptiveWiden();
        return;
    }
    if (adaptiveObservedMask_ == 0 || seen == adaptiveObservedMask_) {
        adaptiveObservedMask_ |= seen;
        ++adaptiveStreak_;
    } else {
        // 집합이 바뀌었다 — 합집합으로 갱신하고 관찰을 다시 시작한다.
        adaptiveObservedMask_ |= seen;
        adaptiveStreak_ = 1;
    }
    if (adaptiveStreak_ >= std::max(1, cfg_.adaptiveWarmupFrames)) {
        adaptiveBaseMask_ = cfg_.formatMask;
        applyFormatMask(adaptiveObservedMask_);
        adaptiveNarrowed_ = true;
    }
}

void Pipeline::adaptiveWiden() {
    if (!adaptiveNarrowed_) return;
    applyFormatMask(adaptiveBaseMask_);
    adaptiveNarrowed_ = false;
    adaptiveStreak_ = 0;
}

bool Pipeline::frameHasStructure(const GrayView& image) const {
    // 축소본에서 블록별 (최대-최소)를 보고, 임계를 넘는 블록이 하나라도
    // 있으면 true. 코드가 있는 프레임은 보통 첫 몇 블록에서 바로 걸리므로
    // 조기 반환된다 — 진짜 빈 프레임에서만 전체를 훑는다.
    if (cfg_.disableBlankFrameSkip) return true;
    if (image.width < 64 || image.height < 64) return true;   // 너무 작으면 판단 안 함

    const int stride = image.stride > 0 ? image.stride : image.width;
    const int step = 4;                       // 4픽셀 건너뛰며 본다(1/16 샘플)
    const int block = 32;                     // 블록 크기(원본 좌표 기준)
    const int thresh = std::max(1, cfg_.blankFrameMinRange);

    for (int by = 0; by + block <= image.height; by += block) {
        for (int bx = 0; bx + block <= image.width; bx += block) {
            int lo = 255, hi = 0;
            for (int y = by; y < by + block; y += step) {
                const uint8_t* __restrict row = image.pixels + static_cast<size_t>(y) * stride;
                for (int x = bx; x < bx + block; x += step) {
                    int v = row[x];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            }
            if (hi - lo >= thresh) return true;
        }
    }
    return false;
}

void Pipeline::addDecoder(std::unique_ptr<IDecoder> decoder) {
    decoders_.push_back(std::move(decoder));
}

std::vector<PipelineResult> Pipeline::process(const Frame& frame) {
    if (frame.empty()) return {};

    GrayImage fallback; // YUYV/NV12 폴백일 때만 실제로 채워짐
    GrayView view = toGrayView(frame, fallback);
    return processView(view);
}

std::vector<PipelineResult> Pipeline::decodeTile(const GrayView& tile, int yOffset) {
    return runDecoders(decoders_, tile, yOffset);
}

std::vector<PipelineResult> Pipeline::runDecoders(
    const std::vector<std::unique_ptr<IDecoder>>& ds, const GrayView& tile, int yOffset) {
    // 디코더가 여러 개(예: zxing-cpp + ZBar fast-path) 등록된 경우, 서로
    // 독립적인 읽기 전용 연산이므로 순차 실행 대신 동시에 돌린다. 디코더가
    // 하나뿐이면(기본 구성) std::async 오버헤드 없이 바로 호출한다.
    std::vector<std::vector<DecodedSymbol>> perDecoder;
    if (ds.size() <= 1) {
        perDecoder.reserve(ds.size());
        for (auto& decoder : ds) perDecoder.push_back(decoder->decode(tile));
    } else {
        std::vector<std::future<std::vector<DecodedSymbol>>> futures;
        futures.reserve(ds.size());
        for (auto& decoder : ds) {
            futures.push_back(std::async(std::launch::async,
                                          [&decoder, &tile] { return decoder->decode(tile); }));
        }
        for (auto& f : futures) perDecoder.push_back(f.get());
    }

    std::vector<PipelineResult> out;
    for (auto& symbols : perDecoder) {
        for (auto& sym : symbols) {
            // 타일 좌표 -> 원본 프레임 좌표로 y 보정
            for (auto& pt : sym.position) pt.second += yOffset;

            PipelineResult r;
            r.symbol = sym;
            if (sym.isGS1) r.gs1 = parseGS1(sym);
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<PipelineResult> Pipeline::processViewCore(const GrayView& image, bool tileFallback) {
    if (image.empty()) return {};

    unsigned nThreads = cfg_.tileThreads ? cfg_.tileThreads
                                          : std::max(1u, std::thread::hardware_concurrency());
    // 이미지가 작으면 타일링 오버헤드가 더 크므로 스레드 1개(=단일 풀프레임)로 처리
    if (image.height < 200) nThreads = 1;

    if (nThreads <= 1) {
        auto r = decodeTile(image, 0);
        auto ff = runDecoders(fullFrameDecoders_, image, 0);
        r.insert(r.end(), std::make_move_iterator(ff.begin()), std::make_move_iterator(ff.end()));
        return dedup(std::move(r));
    }

    // 프레임을 수평 스트립으로 분할. 겹침(overlap)을 둬서 타일 경계에
    // 걸친 코드를 놓치지 않게 하고, 나중에 dedup()으로 중복을 제거한다.
    int tileHeight = (image.height + nThreads - 1) / nThreads;
    std::vector<std::future<std::vector<PipelineResult>>> futures;

    for (unsigned t = 0; t < nThreads; ++t) {
        int y0 = static_cast<int>(t) * tileHeight;
        int y1 = std::min(image.height, y0 + tileHeight + cfg_.tileOverlapPx);
        if (y0 >= image.height) break;

        GrayView tile = image.rowSlice(y0, y1);
        futures.push_back(std::async(std::launch::async, [this, tile, y0]() {
            return decodeTile(tile, y0);
        }));
    }
    // 프레임 전체를 봐야 하는 디코더는 타일과 나란히, 원본 뷰로 한 번 돈다.
    std::future<std::vector<PipelineResult>> fullFrameFuture;
    if (!fullFrameDecoders_.empty()) {
        fullFrameFuture = std::async(std::launch::async, [this, image]() {
            return runDecoders(fullFrameDecoders_, image, 0);
        });
    }

    std::vector<PipelineResult> merged;
    for (auto& f : futures) {
        auto part = f.get();
        merged.insert(merged.end(), part.begin(), part.end());
    }
    if (fullFrameFuture.valid()) {
        auto part = fullFrameFuture.get();
        merged.insert(merged.end(), std::make_move_iterator(part.begin()),
                      std::make_move_iterator(part.end()));
    }

    if (merged.empty() && tileFallback) {
        // 타일 병렬 스캔이 빈손이면 타일링 없이 한 번 더 본다.
        //
        // 타일링은 코드가 타일 하나 안에 온전히 들어와야 동작한다.
        // 타일 높이 = frameHeight/N + overlap 이므로, 이보다 큰 코드는
        // 어느 타일에도 온전히 안 들어가서 통째로 놓친다.
        // 실측: 1536px 프레임 4타일 + overlap 500 구성에서 1200px 코드
        // 완전 미검출(1코어=타일링 없음일 때는 정상 검출). overlap을
        // 900까지 올리면 잡히지만, 그러면 중복 스캔이 3.3배로 늘어
        // 4코어 병렬 이득이 1.2배까지 떨어진다 — 안전한 overlap과
        // 병렬 이득은 서로 상충한다.
        // 그래서 overlap을 과하게 키우는 대신 이 폴백을 둔다.
        // 비용은 "아무것도 못 찾은 프레임"에서만 발생한다.
        //
        // [최상위 호출은 이걸 끈다] 실패 프레임에서 타일 + 풀프레임을
        // 둘 다 도는 것이 첫 패스 시간을 두 배로 만든다 — 실측(난수 혼합
        // 코퍼스): core p99가 55(easy)에서 99ms(mixed)로 뛴 주범이다.
        // 자기 보정 예산은 첫 패스 시간을 기준으로 잡으므로, 첫 패스가
        // 부풀면 예산도 같이 부푼다. 그래서 processView()는 이 폴백을
        // 끄고 **구제 사다리의 한 칸으로** 따로 돌린다(예산 안에서).
        // [[vscan-lite-tile-large-code]]
        return dedup(decodeTile(image, 0));
    }

    return dedup(std::move(merged));
}

GrayView Pipeline::preprocessFrame(const GrayView& image) {
    // [[vscan-lite-pre-denoise]]
    //
    // 파이프라인에 넣기 전에 프레임 상태를 재고, 지금 상태로는 통과할 수
    // 없는 것이 확실할 때만 고쳐서 넣는다. 근거는 pipeline.hpp의
    // autoDenoise 주석(노이즈 40에서 652 -> 24.1ms).
    // 이미 이번 프레임에서 뭉갰으면 두 번 하지 않는다. 2단계 경로가
    // 마지막에 processView()를 부르는데, 거기서 또 재고 또 뭉개면 서로
    // 다른 필터(블러의 블러)가 되어 작은 모듈이 사라진다.
    if (preDenoised_) return image;
    if (!cfg_.autoDenoise || image.empty()) return image;
    // 너무 작은 프레임은 ROI 재귀에서 온 것일 수 있다. 거기서 또 뭉개면
    // 이미 뭉갠 것을 두 번 뭉개게 되므로 손대지 않는다.
    if (image.width < 256 || image.height < 256) return image;

    const float noise = estimateNoise(image);
    frameNoise_ = noise;
    if (noise < cfg_.autoDenoiseNoise) return image;

    boxBlur3x3(image, denoiseBuf_);
    // [두 번째 겹] 노이즈가 아주 심하면 한 겹으로는 모자란다. 그 두 번째
    // 겹은 원래 구제 체인 끝의 dnAgain이 하던 일인데, 거기까지 가려면
    // 실패가 예정된 단계들을 전부 치러야 한다. 같은 판단을 같은 임계로
    // 여기서 미리 한다. 근거와 실측표는 pipeline.hpp의
    // autoDenoiseStrongNoise 주석. [[vscan-lite-pre-denoise-strong]]
    if (cfg_.autoDenoiseStrongNoise > 0 && noise >= cfg_.autoDenoiseStrongNoise) {
        GrayImage second;
        boxBlur3x3(GrayView(denoiseBuf_), second);
        denoiseBuf_ = std::move(second);
    }
    preDenoised_ = true;
    return GrayView(denoiseBuf_);
}

std::vector<PipelineResult> Pipeline::processView(const GrayView& image) {
    BudgetGuard budget(this);
    // 코드가 물리적으로 존재할 수 없는 프레임(컨베이어 아이템 사이 등)은
    // 폴백 체인 전체를 건너뛴다. [[vscan-lite-blank-frame-skip]]
    if (!frameHasStructure(image)) return {};
    FrameGuard frameGuard(this);
    // [S1] 조건을 먼저 재고, 필요하면 고쳐서 넣는다. 아래 단계들은 전부
    // 이 뷰를 쓴다 — 원본이 아니라.
    GrayView view;
    { Stage st("pre"); view = preprocessFrame(image); }
    std::vector<PipelineResult> hits;
    const auto coreT0 = std::chrono::steady_clock::now();
    { Stage st("core"); hits = processViewCore(view, /*tileFallback=*/false); }
    const double coreMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - coreT0).count();
    if (!hits.empty()) { adaptiveObserve(hits); prof().dump("성공:core"); return hits; }
    // [큰 코드 폴백] 타일보다 큰 코드는 어느 타일에도 온전히 안 들어간다.
    // 예전에는 processViewCore() 안에 있었는데, 여기로 뺀 이유는 예산
    // 기준을 정직하게 만들기 위해서다 — 아래 참고.
    /*
     * [이 폴백의 값을 실측했다 — 켤지 말지는 배치가 정한다]
     *
     * 실패 프레임마다 프레임 전체를 한 번 더(단일 스레드로) 훑는 값이다.
     * 난수 코퍼스 150장 실측(2회, 값이 같았다):
     *
     *   켬  266/504 검출, 평균 89.6ms, p95 306ms
     *   끔  265/504 검출, 평균 76.9ms, p95 242ms
     *
     * **코드 1개(504분의 1)를 벌고 평균 +17%, p95 +23%를 쓴다.** 40종
     * 회귀는 켜도 꺼도 40/40이고 총합만 1408 -> 1183ms로 준다.
     *
     * 그래도 기본은 **켬**이다. 이건 `[[vscan-lite-tile-large-code]]`에
     * 실제 사고로 기록된 안전망이다 — 1536px 프레임의 1200px 코드가 4타일
     * + overlap 500 구성에서 **통째로 미검출**됐다. 난수 코퍼스에 그런
     * 큰 코드가 드물 뿐, 그 배치에서는 이 폴백이 검출의 전부다.
     * 둘째 역할도 있다: 자기 보정 예산의 기준이 core+이 폴백이라, 빼면
     * 예산이 작아져 뒤 구제가 과하게 잘린다(§3.47, 실측 67.6 -> 65.6%).
     *
     * 코드가 타일 창(프레임높이/스레드수 + overlap)보다 작다는 것을 아는
     * 배치라면 꺼서 17%를 가져가면 된다.
     */
    // [마감 검사를 여기 넣어봤고 값을 못 했다 — 2026-08-03]
    // VSOVER=1로 재면 마감 초과분의 대부분이 이 단계로 보인다(38~47ms).
    // 그런데 여기서 건너뛰어도 **프레임이 끝나지 않는다** — 빈손으로
    // 아래 구제 체인에 떨어지고 거기서 같은 시간을 쓴다. 실측(200장,
    // reps 3, maxFrameMs=100): 최대 147.5 -> 140.4ms, 평균 37.9 -> 38.2로
    // 측정 노이즈 수준이다. 넘침을 줄이려면 단계를 건너뛰는 게 아니라
    // **일 자체를 줄여야 한다**(§3.62).
    if (!cfg_.disableTileFallback && !cfg_.fastNoRead) {
        Stage st("tilefb");
        auto big = dedup(decodeTile(view, 0));
        if (!big.empty()) { adaptiveObserve(big); prof().dump("성공:tilefb"); return big; }
    }

    // [자기 보정 예산] "이 프레임을 한 번 제대로 훑는 값"을 기준으로 잡는다.
    // 실패 프레임에서 그 값은 타일 패스 + 큰 코드 폴백을 합친 것이다 —
    // 둘 다 프레임 전체를 보는 패스이고, 여기까지가 구제 이전의 정상 비용이다.
    // 타일 패스만으로 기준을 잡으면 예산이 실제보다 작아져서 구제가 과하게
    // 잘린다(실측: 검출 67.6 -> 65.6%). 폴백까지 포함하면 시간은 그대로면서
    // 검출이 돌아온다.
    armSelfBudget(std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - coreT0).count());

    // 좁힌 마스크로 빈손이면 새 심볼로지일 수 있다 — 전체 마스크로 되돌려
    // 이 프레임을 다시 본다. 되돌린 뒤에도 못 찾으면 아래 구제로 내려간다.
    if (adaptiveNarrowed_) {
        adaptiveWiden();
        hits = processViewCore(view);
        if (!hits.empty()) { adaptiveObserve(hits); return hits; }
    }

    // 여기부터는 구제 단계(DPM/1D 회전)다 — 실패 프레임에서만 도는,
    // 가장 비싼 구간이다. 예산을 넘겼으면 여기서 끊는다.
    if (budgetExceeded()) return hits;

    /*
     * [S4를 full 경로에도 — 저대비 프레임은 영역 경로를 먼저 본다]
     *
     * 2단계 경로에는 이 순서 뒤집기가 이미 있었는데(`[[vscan-lite-lowcontrast-
     * region-first]]`) full 경로에는 없었다. 그래서 full에서는 저대비 프레임이
     * 노이즈 구제 -> DPM 구제를 **각각 프레임 하나씩 다시 도는 값**으로 지나간
     * 뒤에야 영역 구제에 닿는다. 자기 보정 예산이 그 전에 끊으면 영역 구제는
     * 아예 안 돈다.
     *
     * 대비가 무너진 프레임은 원리적으로 풀프레임 패스로 못 읽는다(zxing의
     * 이진화가 8x8 블록 명암 폭 24를 "구조 없음"으로 본다 — §3.25). 그러니
     * 저대비로 판정되면 그 두 구제를 건너뛰고 영역부터 보는 게 맞다.
     *
     * **건너뛰는 게 아니라 순서만 바꾼다.** 영역이 빈손이면 아래 구제들이
     * 그대로 이어서 돈다 — 잘못 짚었을 때의 손해 상한이 "원래 순서와 같은
     * 총합"이다.
     */
    if (cfg_.enableRegionRescue && !cfg_.fastNoRead && !regionCropDone_ &&
        estimateLocalRange(view) < cfg_.lowContrastRange) {
        const auto rgT0 = std::chrono::steady_clock::now();
        auto early = tryRegionRescue(view, std::max(1, cfg_.minExpectedCodes), RegionPass::Both);
        prof().add("region-early", std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - rgT0).count());
        if ((int)early.size() >= std::max(1, cfg_.minExpectedCodes)) {
            adaptiveObserve(early);
            prof().dump("성공:region-early");
            return early;
        }
        if (early.size() > hits.size()) hits = std::move(early);

    }

    // [노이즈 구제] 노이즈가 심해 이진화가 무너진 경우를 살린다.
    // DPM/회전 구제보다 먼저 시도한다 — 필터 한 번 + 코어 패스 한 번으로
    // 가장 싸고, 실측상 가장 자주 걸린다(§3.14). [[vscan-lite-denoise-rescue]]
    //
    // [뭉개는 세기는 두 단이다] 이미 프레임 전처리에서 한 번 뭉갠
    // 프레임(= 노이즈가 임계를 넘은 프레임)이 여기까지 왔다면 3x3 한 번이
    // 모자랐다는 뜻이다. 그때는 **두 번 더** 먹인다.
    //
    // 실측(PDF417 module 8, 노이즈 축): 45/50/55/60 전부 3x3 한 번이나
    // 두 번으로는 안 열리고 세 번에서 열린다(넷 다). 앞단에서 처음부터
    // 세 번 먹이는 것은 안 된다 — 모듈이 2px인 코드가 그 자리에서 사라진다.
    // 다 실패한 뒤의 구제 자리라야 안전하다.
    // 이미 뭉갠 프레임은 **아주 시끄러울 때만** 다시 본다. 이 자리는
    // 프레임 하나를 통째로 다시 도는 값이라 p95에 그대로 실린다 —
    // 코퍼스 300장 p95: 임계 없음 525ms / 노이즈 30 이상 525ms /
    // 노이즈 45 이상 431ms(= 이 단계가 없을 때와 같다). 검출은 셋 다 같다.
    //
    // 세기는 3x3을 겹치는 대신 반경 2 박스 한 번으로 준다. 실측(PDF417
    // module 8, 노이즈 40~60 다섯 단, 앞단 3x3 뒤에):
    //   3x3 한 번 더  -> 40만
    //   3x3 두 번 더  -> 55 빼고
    //   3x3 세 번 더  -> 다섯 다 (= 반경 2와 같은 세기)
    //   반경 2 한 번  -> 다섯 다   <- 필터 통과가 한 번뿐
    //   반경 4 한 번  -> 45만 (너무 세다)
    const bool dnAgain = preDenoised_ && frameNoise_ >= 45.0f;
    // 프레임 단위 구제(뭉개기/평탄화)는 예산으로 자르지 않는다. 각각
    // 필터 한 번 + 찾기 한 번이라 비용이 정해져 있고, 자르면 노이즈/그림자
    // 축이 바로 깎인다. 예산은 **영역을 여러 개 도는 값**을 자르는 데 쓴다.
    // fastNoRead에서는 노이즈 **구제**도 뺀다. 실측(2단계, reps 3):
    //   저조도  23 -> 23코드, 판정 42.2 -> 32.4ms
    //   혼합   257 -> 256코드, 판정 107.4 -> 76.8ms (-29%)
    // 뭉개서 다시 보는 값이 재촬영보다 비싸다. 단 **선 디노이즈는 남긴다** —
    // 그것까지 빼면 저조도가 23 -> 16코드로 무너진다(§3.61).
    if (!cfg_.disableDenoiseRescue && !cfg_.fastNoRead && (!preDenoised_ || dnAgain)) {
        GrayImage smoothed;
        { Stage st("dn:filter");
          if (dnAgain) boxBlur(view, 2, smoothed);
          else boxBlur3x3(view, smoothed); }
        Stage st("dn:decode");
        // 여기서부터는 구제라 ZBar를 붙인다 — 전처리된 판본에서 zxing보다
        // 강하다(pipeline.hpp의 zbarAsRescue 주석). [[vscan-lite-zbar-rescue]]
        auto dnHits = locateAndDecode(GrayView(smoothed));
        if ((int)dnHits.size() >= std::max(1, cfg_.minExpectedCodes)) { prof().dump("성공:denoise"); return dnHits; }
    }

    // [조명 평탄화 구제] 그림자가 코드를 가로지르면 한 프레임 안에서
    // 밝기가 두 배 넘게 차이 나고, 그러면 에너지 로케이터가 어두운 쪽을
    // 코드로 안 본다 — 실측(Code128 module 8, 그림자 0.2/0.3): 972px
    // 코드에서 상자가 왼쪽 절반(465px)만 잡혔다. 그 절반만으로는 아래의
    // 어떤 ROI 구제도 못 읽는다. 그래서 **영역 구제보다 먼저** 편다.
    //
    // 큰 반경으로 뭉갠 판본을 배경으로 보고 나누면 배경이 평평해지고
    // 막대 구조만 남는다. 실측: 그림자 0.2~0.5 네 단이 전부 평탄화 뒤
    // 평범한 풀프레임 디코드로 열린다.
    //
    // 배경이 이미 평평하면 flattenIllumination()이 false를 돌려주므로
    // (뭉갠 판본의 상하위 2% 차이가 40 미만) 그림자 없는 프레임에서는
    // 뭉개기 한 번 값만 든다. [[vscan-lite-flatten-illumination]]
    if (!cfg_.fastNoRead) {
        GrayImage flat;
        bool flatOk;
        { Stage st("ff:filter"); flatOk = flattenIllumination(view, 32, flat); }
        if (flatOk) {
            Stage st("ff:decode");
            auto ffHits = locateAndDecode(GrayView(flat));
            if ((int)ffHits.size() >= std::max(1, cfg_.minExpectedCodes)) { prof().dump("성공:flatten"); return ffHits; }
        }
    }

    // [DPM/점각인 구제] processViewCore()가 풀옵션으로도 빈손이면,
    // DPM(레이저 점각인) 코드일 가능성을 본다. 실물 비교 대상 리더기 대조
    // 검증까지 완료된 구제책 — §6.4 대화 참고. 위치 탐색이 필요
    // 없는 전역 전처리라 1D 회전 구제보다 먼저 시도한다(더 싸다).
    // [[vscan-lite-dpm-rescue]]
    if (cfg_.enableDPMRescue) {
        Stage st("dpm");
        GrayImage closed;
        morphologicalCloseInverted(view, cfg_.dpmKernelSize, closed);
        auto dpmHits = processViewCore(GrayView(closed));
        if ((int)dpmHits.size() >= std::max(1, cfg_.minExpectedCodes)) {
            prof().dump("성공:dpm");
            return dpmHits;
        }
    }

    // [영역 구제 — 큰 프레임 속 작은 코드]
    // 여기까지 왔다는 건 풀프레임 풀옵션이 빈손이라는 뜻인데, 실측상
    // 그 실패의 상당수는 "코드가 프레임에 비해 너무 작아서 못 찾은 것"
    // 이다. 회전이 문제가 아니다 — 2048x1536 프레임의 128px DataMatrix는
    // 55도 이상에서 전부 실패하지만, 같은 이미지에서 코드 주변만
    // 205x200으로 잘라내면 **회전 없이** 전 각도가 읽힌다(§3.17).
    // 그래서 회전을 더 시도하는 대신 "어디를 보라"를 알려준다.
    // 1D 회전 구제보다 먼저 두는 이유: 회전/리샘플링이 없어 더 싸고,
    // 1D/2D를 가리지 않아 적용 범위가 넓다. [[vscan-lite-region-rescue]]
    // [주의] 예전에는 아래 "이미 다 했다" 분기가 **함수를 반환**했다.
    // 그러면 이 블록 뒤에 있는 QR 파인더 구제와 1D 회전 구제가 통째로
    // 건너뛰어진다 — 둘 다 opt-in 손잡이인데 **2단계 경로에서는 켜도
    // 한 번도 안 돌았다**(실측: `enable_qr_finder_rescue`를 켜고 저조도
    // 24장을 돌려도 `qrfinder` 단계 실행 0회, 시간·코드 완전 동일).
    // 2단계가 컨베이어 권장 경로라, 그 옵션을 가장 필요로 하는 쪽에서
    // 정확히 죽어 있었다. 영역 구제만 건너뛰고 나머지는 이어간다.
    // [[vscan-lite-region-skip-early-return]]
    if (cfg_.enableRegionRescue && !cfg_.fastNoRead &&
        !(regionCropDone_ && regionRotDone_)) {
        const auto rgT0 = std::chrono::steady_clock::now();
        auto regionHits = tryRegionRescue(view, std::max(1, cfg_.minExpectedCodes),
                                          regionCropDone_ ? RegionPass::RotateOnly : RegionPass::Both);
        prof().add("region", std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - rgT0).count());
        if ((int)regionHits.size() >= std::max(1, cfg_.minExpectedCodes)) { prof().dump("성공:region"); return regionHits; }
        if (regionHits.size() > hits.size()) hits = std::move(regionHits);


    }



    // [QR 파인더 구제] 영역 구제까지 실패했다면 코드가 작고 여러 개일 수
    // 있다. QR 파인더 패턴으로 직접 찾는다. [[vscan-lite-qr-finder-locate]]
    if (cfg_.enableQrFinderRescue && !budgetExceeded()) {
        Stage st("qrfinder");
        auto qrHits = tryQrFinderRescue(view, std::max(1, cfg_.minExpectedCodes));
        if ((int)qrHits.size() >= std::max(1, cfg_.minExpectedCodes)) {
            prof().dump("성공:qrfinder");
            return qrHits;
        }
        if (qrHits.size() > hits.size()) hits = std::move(qrHits);
    }

    // [1D 바코드 회전 구제] processViewCore()가 이미 풀옵션(TryHarder+
    // Rotate+Invert)으로 돌았는데도 빈손이면, 20~75도 부근 회전 1D
    // 바코드일 가능성이 있다(§3.2.15~17). 이건 "속도 최적화 편의기능"
    // 이 아니라 zxing 기본 옵션으로는 원천적으로 못 찾는 것을 추가로
    // 찾아내는 진짜 검출 능력 확장이라, two_stage뿐 아니라 순수 풀옵션
    // 경로(processView/vscan_process_gray)에도 있어야 맞다.
    // [[vscan-lite-1d-deskew-rescue]]
    if (!cfg_.enable1DDeskewRescue || budgetExceeded()) {
        prof().dump(hits.empty() ? "실패:끝" : "부분:끝");
        return hits;
    }
    int need = std::max(1, cfg_.minExpectedCodes);
    Stage st("deskew1d");
    auto rescued = tryDeskewRescue1D(view, need);
    prof().dump(rescued.empty() ? (hits.empty() ? "실패:끝" : "부분:끝") : "성공:deskew1d");
    return rescued.empty() ? hits : rescued;
}

std::vector<PipelineResult> Pipeline::tryRegionRescue(const GrayView& image, int need, RegionPass pass,
                                                       float energyRatio) {
    return tryRegionRescueOn(image, image, need, pass, energyRatio);
}

/*
 * locateView에서 영역을 찾고 decodeView에서 잘라 읽는다. 둘이 같으면
 * 기존 동작 그대로다. 다르게 주는 자리는 "평탄화본에서 찾고 원본에서 읽기".
 */
std::vector<PipelineResult> Pipeline::tryRegionRescueOn(const GrayView& locateView,
                                                         const GrayView& decodeView, int need,
                                                         RegionPass pass, float energyRatio) {
    const GrayView& image = decodeView;
    // [[vscan-lite-region-rescue]]
    //
    // 원리는 헤더(locate.hpp)에 적어둔 실측 그대로다: zxing이 못 읽는 게
    // 아니라 못 찾는 것이라, 찾아서 잘라주기만 하면 된다.
    //
    // 여백(pad)을 영역 크기에 비례해서 주되 상한을 둔다. 크기 비례인 이유는
    // 정지대(quiet zone)가 모듈 크기에 비례해야 하기 때문이고, 상한을 두는
    // 이유는 실측상 **크롭이 너무 커도 다시 실패**하기 때문이다 — 애초에
    // 실패 원인이 "프레임이 커서"였으니 당연하다(여백 240px 크롭 665x660은
    // 읽히는데 여백 480px 크롭 1085x1140은 다시 실패한 각도가 있었다).
    // 기대 코드 수가 많으면 영역 상한도 그만큼 늘린다 — 상한이 4인데
    // 코드가 12개면 영역 구제가 need를 채울 방법이 원천적으로 없다.
    const int maxRegions = std::min(16, std::max(std::max(1, cfg_.regionRescueMaxRegions), need));
    auto regions = findCodeRegions(locateView, maxRegions, 32, 4, energyRatio);

    /*
     * [로케이터 두 번째 패스 — 저대비 코드는 상대 랭킹에 묻힌다]
     *
     * findCodeRegions()는 **프레임 최대 타일 에너지 대비** 20% 이상만 후보로
     * 본다. 고대비 물체가 하나라도 있으면 저대비 코드는 그 상대 판정에 묻혀
     * 후보에 아예 안 들어온다. 실측(DataMatrix module 6, 대비 0.05): 코드는
     * (462,720)에 있는데 첫 패스가 낸 영역은 프레임 전체와 엉뚱한 우상단
     * 256x256 둘뿐이다.
     *
     * 임계를 낮추는 것으로는 안 된다 — 0.06으로 내리면 배경까지 한 덩어리가
     * 돼서 프레임 전체 상자 하나만 나온다. 문제는 임계가 아니라 **신호**다.
     * 국소 평균을 빼고 4배로 세운 사본에서 찾으면 정확한 상자
     * (384,640)-(640,896)가 나온다.
     *
     * **구제 단이 아니라 여기(후보 목록)에 넣는 것이 핵심이다.** 별도 단으로
     * 두면 자기 보정 예산(§3.47)이 벽시계 기준이라 그 자리에 닿기도 전에
     * 잘린다 — 실제로 그렇게 짰다가 프로파일에 아예 안 찍히는 것을 봤다.
     * 여기에 넣으면 로케이트 비용(약 4ms + 블러)만 내고 기존 크롭 경로가
     * 그대로 받는다. 사본은 **찾기 전용**이고 크롭은 원본에서 뜬다.
     * [[vscan-lite-locate-second-pass]]
     */
    if (cfg_.enableFlattenedLocate && &locateView == &decodeView) {
        GrayImage mu;
        boxBlur(locateView, 16, mu);
        GrayImage eq;
        eq.width = locateView.width; eq.height = locateView.height;
        eq.pixels.resize(static_cast<size_t>(eq.width) * eq.height);
        for (int y = 0; y < eq.height; ++y) {
            const uint8_t* src = locateView.pixels + static_cast<size_t>(y) * locateView.stride;
            const uint8_t* m = mu.pixels.data() + static_cast<size_t>(y) * mu.width;
            uint8_t* d = eq.pixels.data() + static_cast<size_t>(y) * eq.width;
            for (int x = 0; x < eq.width; ++x) {
                const int v = 128 + 4 * (static_cast<int>(src[x]) - static_cast<int>(m[x]));
                d[x] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        auto extra = findCodeRegions(GrayView(eq), maxRegions, 32, 4, energyRatio);
        for (const auto& e : extra) {
            const double ea = static_cast<double>(e.bbox.x1 - e.bbox.x0) * (e.bbox.y1 - e.bbox.y0);
            if (ea > 0.60 * static_cast<double>(locateView.width) * locateView.height) continue;
            bool dup = false;
            for (const auto& r : regions) {
                const int ox = std::min(r.bbox.x1, e.bbox.x1) - std::max(r.bbox.x0, e.bbox.x0);
                const int oy = std::min(r.bbox.y1, e.bbox.y1) - std::max(r.bbox.y0, e.bbox.y0);
                if (ox > 0 && oy > 0 &&
                    static_cast<double>(ox) * oy > 0.5 * ea) { dup = true; break; }
            }
            if (!dup) regions.push_back(e);
        }
    }
    /*
     * [프레임을 통째로 덮는 영역은 크롭이 아니다]
     * 임계를 낮추면 배경까지 이어져서 프레임 전체 상자가 나온다. 그걸
     * 크롭이라고 넘기면 원본을 한 번 더 도는 것과 같아서 얻는 게 없다.
     */
    regions.erase(std::remove_if(regions.begin(), regions.end(), [&](const CodeRegion& r) {
                      const double a = static_cast<double>(r.bbox.x1 - r.bbox.x0) * (r.bbox.y1 - r.bbox.y0);
                      return a > 0.60 * static_cast<double>(image.width) * image.height;
                  }), regions.end());
    if (regions.empty()) return {};

    // [헛수고 차단] 영역 하나당 코드는 많아야 하나다. 찾은 영역이 요구
    // 개수보다 적으면 이 단계는 어차피 need를 못 채우고 뒤 단계로 넘어간다 —
    // 그럴 거면 크롭 디코드를 돌 이유가 없다. locate 자체는 4ms라 여기서
    // 끊는 비용은 무시할 만하다.
    // 실측(코드 1~12개가 섞인 300장, need=기대개수): 이 검사가 없으면
    // 평균 147 -> 235ms(+60%)로 뛰는데 검출은 +1.0%p뿐이었다. 다중 코드
    // 프레임에서는 영역 4개로 12개를 채울 수 없으니 전부 헛돈 것이다.
    if ((int)regions.size() < need) return {};

    PipelineConfig roiCfg = cfg_;
    roiCfg.tileThreads = 1;          // ROI는 작아서 타일링이 손해다
    // [옵션은 항상 풀옵션] 자르기 패스가 파이프라인 이른 자리에서 도니
    // 옵션을 아껴야 할 것 같지만, 실측은 반대였다. TryRotate/TryInvert를
    // 끄면 이 패스의 성공률이 떨어져 뒤의 풀프레임 단계로 더 자주
    // 넘어가고, 그 단계들이 훨씬 비싸서 총합이 커진다
    // (단일 코드 300장 p50 59.0 -> 92.7ms, 40종 합계 1593 -> 2076ms).
    // ROI가 작아서 옵션을 다 켜도 절대 비용이 작다는 점이 핵심이다.
    roiCfg.tryHarder = true;
    roiCfg.tryRotate = true;
    roiCfg.tryInvert = true;
    roiCfg.enableRegionRescue = false;   // 재귀 방지
    roiCfg.zbarAsRescue = false;         // ROI 디코드에는 ZBar를 붙인다
    roiCfg.enable1DDeskewRescue = false;
    roiCfg.enableDPMRescue = false;

    // 영역마다 크기가 달라서 pad도 달라진다 — decodeRegionsParallel()은
    // 단일 pad만 받으므로, 여기서 미리 여백을 먹인 rect를 만들어 넘기고
    // pad 인자는 0으로 준다.
    // [S2 — 상자를 원본 해상도로 다시 재서 좁힌다]
    // 축소본 상자는 타일 격자(원본 128px) 단위라 늘 헐렁하다. 그 헐렁함이
    // 그대로 시간이 된다 — 실측(대비 0.10): 영역 크롭 985k px(프레임의
    // 31%)의 풀옵션 디코드가 13.6~27.2ms인데, 실제 코드 크기인 408k px는
    // 13.1ms다. 여기서 한 번 좁히면 자르기/대비/회전 패스가 전부 같은
    // rect를 쓰므로 이득이 모든 구제 단계에 곱해진다.
    // [[vscan-lite-region-box-refine]]
    //
    // 다만 **회전 패스는 이 상자를 쓰면 안 된다.** 기울어진 1D 코드는
    // 축정렬 상자 안에서 마름모꼴로 놓여 모서리 쪽 에너지가 옅어지는데,
    // 프로파일 임계가 그 꼬리를 잘라낸다. 실측(Code128 module 8, 45도):
    // 상자가 768 -> 699로 좁아지자 되돌린 뒤 코드의 양끝이 잘려 검출
    // 자체가 실패했다(22ms 성공 -> 190ms 실패). 되돌린 다음에는 어차피
    // tightenToContent()가 다시 좁히므로, 회전 쪽은 넉넉한 상자가 맞다.
    // 그래서 상자를 두 벌 만든다.
    std::vector<Rect> rects, rotRects, tightRects;
    rects.reserve(regions.size());
    rotRects.reserve(regions.size());
    tightRects.reserve(regions.size());
    for (auto& r : regions) {
        const int cw = r.bbox.x1 - r.bbox.x0, ch = r.bbox.y1 - r.bbox.y0;
        if (cw < 16 || ch < 16) continue;
        {   // 회전용: 예전 그대로 넉넉하게
            int pad = std::min(static_cast<int>(0.35f * std::max(cw, ch)), cfg_.regionRescueMaxPadPx);
            rotRects.push_back(Rect{std::max(0, r.bbox.x0 - pad), std::max(0, r.bbox.y0 - pad),
                                    std::min(image.width, r.bbox.x1 + pad),
                                    std::min(image.height, r.bbox.y1 + pad)});
        }
        // [2D 행렬코드는 좁히지 않는다]
        // 정밀화 + 좁은 여백은 1D/적층형에는 그대로 이득인데, QR/DataMatrix
        // 같은 2D에는 손해다. 2D는 사방에 정지대가 필요하고, 국소 이진화의
        // 구조 마스크도 코드 바깥의 "구조 없는" 영역이 있어야 경계를
        // 제대로 잡는다. 실측(QR module 8, 대비 스윕 8단): 정밀화와 좁은
        // 여백이 각각 한 단씩 깎아서 8/8 -> 6/8이 됐고, 둘 다 되돌리면
        // 8/8로 돌아온다.
        //
        // 구분은 **거친 상자의 종횡비**로 한다. 가로로(또는 세로로) 길면
        // 1D/적층형이고, 정사각형에 가까우면 2D이거나 기울어진 1D다.
        // 어느 쪽이든 넉넉한 상자가 맞다 — 기울어진 1D는 회전 패스가
        // 따로 넉넉한 상자를 쓰므로 여기서 좁힐 이유가 없다.
        //
        // angleDeg(방향성 유무)를 쓰려다 실패했다. 이론상으로는 그게
        // 맞는 신호인데(2D는 사방 엣지라 코히런스가 안 나온다),
        // **저대비에서는 1D도 코히런스가 임계 아래로 떨어진다** — 실측:
        // 같은 Code128이 대비 0.15/0.30에서는 "각도 있음"인데 0.10에서는
        // "각도 없음"으로 뒤집혀서, 정작 가장 느린 프레임이 2D 취급을
        // 받아 103ms가 됐다. 종횡비는 대비와 무관하게 안정적이다
        // (1024x512 -> 2.0, QR 256x256 -> 1.0).
        const bool elongated = std::max(cw, ch) >= 3 * std::min(cw, ch) / 2;
        if (elongated) {
            const Rect tight = refineRegionBox(image, r.bbox);
            if (tight.x1 - tight.x0 >= 16 && tight.y1 - tight.y0 >= 16) r.bbox = tight;
        }
        const int w = r.bbox.x1 - r.bbox.x0, h = r.bbox.y1 - r.bbox.y0;
        if (w < 16 || h < 16) continue;
        // [여백] 정지대는 모듈 크기에 비례해야 하는데 모듈을 모르므로
        // 상자 크기에 비례시킨다. 상자가 이제 코드에 딱 맞으므로 비율을
        // 예전(0.35)만큼 크게 줄 이유가 없다 — 그 값은 헐렁한 상자를
        // 전제로 정해진 것이었다.
        //
        // **축마다 따로 준다.** 하나의 pad를 max(w,h)에 비례시키면
        // 가로로 긴 1D 코드에서 위아래로도 그만큼 붙는데, 1D의 정지대는
        // 막대 방향(가로)에만 필요하다. 실측(Code128 module 8, 정밀화된
        // 상자 951x379): 단일 pad는 1179x607 = 715k px인데 축별 pad는
        // 1179x469 = 553k px다. 세로로 붙은 여백은 정지대가 아니라
        // 그냥 디코더가 훑을 배경일 뿐이다.
        // [하한 56px] 비율만으로 주면 작은 2D 코드에서 정지대가 모자란다.
        // QR은 규격상 정지대가 4모듈인데, 정밀화된 상자 249x249(모듈 8px)에
        // 0.12를 곱하면 29px = 3.6모듈이라 아슬아슬하게 모자랐다 — 실측으로
        // 대비 0.10 QR이 그것 때문에 안 읽혔다(하한 24/40 실패, 56/72 성공).
        // 56px는 모듈 8px 기준 7모듈이다. 더 키워도 얻는 게 없어서
        // 통과하는 가장 작은 값으로 둔다.
        int padX, padY;
        if (!elongated) {
            // 정사각형에 가까움(2D 또는 기울어진 1D): 예전 그대로 넉넉하게.
            padX = padY = std::min(static_cast<int>(0.35f * std::max(w, h)), cfg_.regionRescueMaxPadPx);
        } else {
            // 1D/적층형: 축마다 따로 준다. 하나의 pad를 max(w,h)에
            // 비례시키면 가로로 긴 코드에서 위아래로도 그만큼 붙는데,
            // 1D의 정지대는 막대 방향에만 필요하다. 실측(정밀화된 상자
            // 951x379): 단일 pad는 1179x607 = 715k px인데 축별 pad는
            // 1179x469 = 553k px다. 하한 56px은 작은 코드에서 정지대가
            // 모자라지 않게 하기 위한 것이다.
            padX = std::min(std::max(56, static_cast<int>(0.12f * w)), cfg_.regionRescueMaxPadPx);
            padY = std::min(std::max(56, static_cast<int>(0.12f * h)), cfg_.regionRescueMaxPadPx);
        }
        Rect rc{std::max(0, r.bbox.x0 - padX), std::max(0, r.bbox.y0 - padY),
                std::min(image.width, r.bbox.x1 + padX), std::min(image.height, r.bbox.y1 + padY)};
        rects.push_back(rc);

        // [저대비 전용 — 여백을 거의 안 준 판본]
        // 위 여백은 "정지대를 확보한다"가 목적이라 넉넉한데, **저대비
        // 프레임에서는 그 여백이 곧 실패 원인이 된다.**
        //
        // 실측(module 8, 대비 0.10, 정밀화된 상자 1021x385): 지금 여백
        // (padX=122)으로는 UPCE/ITF/Codabar/Code93/Code39/DataBar가 전부
        // 실패하고, 같은 상자를 여백 6~24px로 자르면 **여섯 종이 다**
        // 스트레칭 + 3x3 블러만으로 읽힌다.
        //
        // 원인을 히스토그램으로 짐작했다가 틀렸다. "종이(220)가 화소의
        // 절반을 넘어 스트레칭 범위를 가져간다"는 가설이었는데, 두 가지로
        // 기각됐다: (1) 꼬리컷을 0.5%에서 25%까지 키워도 한 종도 안 열린다,
        // (2) 넓은 크롭과 좁은 크롭의 LUT가 사실상 같고(lo/hi = 109/221 대
        // 108/220) 서로 바꿔 끼워도 결과가 안 바뀐다 — 좁은 크롭은 어느
        // LUT로도 읽히고 넓은 크롭은 어느 LUT로도 안 읽힌다.
        // 남는 설명은 **크롭 크기 자체**다(1144x497 대 1034x409). 코드가
        // 이미지에서 차지하는 비율이 zxing의 스캔라인 표본과 minLineCount=4
        // 합의에 걸리는 것으로 보인다 — 확인은 못 했고, 고치는 데 필요하지도
        // 않다. 재현되는 사실은 "여백을 걷어내면 읽힌다"이다.
        //
        // 정지대는 정밀화된 상자 자신이 이미 조금 물고 있다(프로파일
        // 임계가 코드 끝보다 20~25px 바깥에서 끊긴다). 그래서 쓰는 쪽에서
        // 그 여유에 얹는 정도만 준다. 다른 시도가 다 실패한 뒤의 추가
        // 판본이라 검출을 깎을 여지가 없다. 여기에는 **여백 없는** 상자를
        // 담는다 — 얼마를 얹을지는 쓰는 자리에서 정한다.
        // [[vscan-lite-lowcontrast-tight-crop]]
        tightRects.push_back(r.bbox);
    }
    if (rects.empty()) return {};

    std::vector<PipelineResult> hits;

    // [정밀 각도 캐시] refineRegionAngle()은 원본 해상도 구조텐서라 영역당
    // 1.2ms쯤 든다. 아래에서 "회전을 먼저 할까"를 정할 때와 실제로 회전할
    // 때 두 번 필요하므로 한 번만 재서 나눠 쓴다. 실제 각도는 (-45,45]이고
    // "방향 없음"은 +1e9라, -1e9를 "아직 안 쟀다" 센티널로 쓸 수 있다.
    constexpr float kNotMeasured = -1e9f;
    std::vector<float> refined(regions.size(), kNotMeasured);
    auto angleOf = [&](size_t i) -> float {
        if (refined[i] == kNotMeasured) {
            float a = refineRegionAngle(image, regions[i].bbox);
            // 원본 해상도에서 방향성이 안 잡히면 축소본 추정값이라도 쓴다.
            refined[i] = (a >= CodeRegion::kAngleUnknown) ? regions[i].angleDeg : a;
        }
        return refined[i];
    };

    auto take = [&](std::vector<PipelineResult>&& found) -> bool {
        if ((int)found.size() >= need) { hits = std::move(found); return true; }
        if (found.size() > hits.size()) hits = std::move(found);
        return false;
    };

    auto cropPass = [&]() -> bool {
        { Stage st("rg:crop"); if (take(decodeRegionsParallel(image, rects, 0, roiCfg))) return true; }

        // [저대비 ROI 구제] 자르기만으로 안 되면 ROI 안에서 대비를 편다.
        // 심볼로지마다 끊기는 대비가 다른데 EAN/UPC 계열만 유독 높다
        // (Code128 0.15 / QR 0.20 / EAN13·UPCA 0.30). 저대비 프레임에서
        // 코드 영역만 잘라 스트레칭하면 EAN13/UPCA가 0.20까지 내려간다.
        //
        // stretchContrast()는 이미 대비가 충분하면 false를 돌려주므로,
        // 대비가 문제가 아닌 프레임에서는 히스토그램 한 번 값만 내고
        // 디코드는 아예 안 돈다 — 비용이 사실상 0이다.
        // 프레임 전체가 아니라 반드시 ROI 안에서 해야 한다는 근거는
        // preprocess.hpp의 stretchContrast() 주석 참고.
        // [[vscan-lite-roi-contrast-stretch]]
        if (!budgetExceeded()) {
            std::vector<PipelineResult> stretched;
            std::vector<Rect> srcRect;              // stretched[i]를 만든 영역
            auto rectsTouch = [](const Rect& a, const Rect& b) {
                const int mx = std::max(a.x1 - a.x0, b.x1 - b.x0) / 4;
                const int my = std::max(a.y1 - a.y0, b.y1 - b.y0) / 4;
                return a.x0 - mx < b.x1 && b.x0 - mx < a.x1 &&
                       a.y0 - my < b.y1 && b.y0 - my < a.y1;
            };
            const int srcStride = image.stride > 0 ? image.stride : image.width;
            int rectIdx = -1;
            for (const Rect& rc : rects) {
                ++rectIdx;
                // 첫 영역 구간 표시 — budgetExceeded()가 더 넉넉한 마감을 본다.
                firstRectActive_ = (rectIdx == 0);
                if (budgetExceeded()) break;
                const int rw = rc.x1 - rc.x0, rh = rc.y1 - rc.y0;
                if (rw < 16 || rh < 16) continue;

                GrayImage crop;
                crop.width = rw;
                crop.height = rh;
                crop.pixels.resize(static_cast<size_t>(rw) * rh);
                for (int r = 0; r < rh; ++r)
                    std::memcpy(crop.pixels.data() + static_cast<size_t>(r) * rw,
                                image.pixels + static_cast<size_t>(rc.y0 + r) * srcStride + rc.x0, rw);

                Pipeline roiPipe(roiCfg);
                std::vector<PipelineResult> sHits;
                GrayImage boosted;
                Stage stC("rg:contrast");
                if (stretchContrast(GrayView(crop), boosted)) {
                    // [펴고 나서 뭉갠 판본을 **먼저** 본다]
                    // 스트레칭은 신호와 노이즈를 같이 증폭한다. 대비가
                    // 낮을수록 이득이 커지므로 양자화/센서 노이즈도 그만큼
                    // 커져서, 편 직후에는 오히려 이진화가 흔들린다.
                    // 실측(EAN13 대비 0.20): ROI를 펴기만 하면 실패,
                    // 편 뒤 3x3 블러를 한 번 먹이면 읽힌다.
                    //
                    // 예전에는 "펴기만" -> "펴고 뭉개기" 순서였는데,
                    // 실측상 저대비 프레임은 거의 항상 뭉개야 붙는다.
                    // 순서를 바꾸면 펴기만 한 판본의 디코드(실측 12.0ms,
                    // 그리고 그건 노이즈가 증폭된 이미지라 가장 비싼
                    // 판본이다)를 건너뛴다. 순서만 바꾸는 것이라 검출력에는
                    // 영향이 없다 — 뭉갠 쪽이 빈손이면 아래에서 펴기만 한
                    // 판본을 그대로 본다(모듈이 아주 작아 블러가 손해인
                    // 경우가 그쪽이다).
                    GrayImage smoothed;
                    boxBlur3x3(GrayView(boosted), smoothed);
                    if (budgetExceeded()) break;
                    sHits = roiPipe.processViewCore(GrayView(smoothed));
                    // [중괄호 없으면 성공한 결과를 덮어쓴다]
                    // 원래 이 자리가 중괄호 없이
                    //     if (sHits.empty())
                    //         if (budgetExceeded()) break;
                    //         sHits = ...(boosted);
                    // 이었다. 두 번째 줄만 if에 묶이고 세 번째 줄은
                    // **무조건** 돌아서, 뭉갠 판본이 읽어낸 결과를 펴기만 한
                    // 판본의 결과로 덮어썼다. 바로 위 주석이 설명하는
                    // "뭉갠 쪽을 먼저 본다"가 실제로는 동작하지 않고 있었다.
                    if (sHits.empty()) {
                        if (budgetExceeded()) break;
                        sHits = roiPipe.processViewCore(GrayView(boosted));
                    }
                }
                /*
                 * [그래도 빈손이면 크롭을 좁혀가며 다시 편다]
                 *
                 * 저대비 코드는 **프레임이 저대비인 게 아니라 코드만** 저대비인
                 * 경우가 많다. 그러면 영역 크롭에 딸려 들어온 고대비 장면이
                 * 스트레치의 lo/hi를 정해버려서 코드는 거의 안 펴진다.
                 * 실측(QR module 6, 대비 0.05): 코드 영역 span 34인데 크롭
                 * 전체 span 117 -> 코드의 13계조가 28계조로만 펴지고,
                 * zxing의 8x8 블록 판정(폭 24 미만 = 구조 없음)을 아슬아슬하게
                 * 지난다. 같은 이미지를 여백 0~12px로 좁혀 자르면 **보통
                 * 스트레치로도** 읽힌다(0.05와 0.10 둘 다).
                 *
                 * 영역 상자는 에너지 로케이터가 준 것이라 코드가 대체로
                 * 가운데 있다. 그래서 중앙부를 70% -> 50%로 좁혀가며 편다.
                 * 좁힐수록 장면이 빠지고 코드가 범위를 정하게 된다.
                 * [[vscan-lite-lowcontrast-inner-stretch]]
                 */
                // 70% -> 50% -> 35%. 실측(QR module 6, 대비 0.05): 로케이터가
                // 주는 상자가 256x256인데 코드는 126x126이라 여백이 65px다.
                // 파이썬 실험에서 여백 20px 이상은 실패하고 0~12px이라야
                // 읽혔으므로, 50%(여백 ~1px)까지는 내려가야 한다.
                // 70% -> 50% -> 35%. 실측(QR module 6, 대비 0.05): 로케이터가
                // 주는 상자가 256x256인데 코드는 126x126이라 여백이 65px다.
                // 여백 20px 이상은 실패하고 0~12px이라야 읽히므로 50%(여백 ~1px)
                // 까지는 내려가야 한다.
                //
                // **더 촘촘하게 하면 오히려 나빠진다.** {70,55,45,40,36}으로
                // 다섯 단을 두니 QR이 8/8 -> 7/8로 떨어졌다 — 단이 늘수록
                // 프레임 예산을 더 쓰고, 그러면 정작 되던 프레임이 뒤 단계에서
                // 잘린다. 세 단이 실측상 가장 좋다.
                /*
                 * [먼저 국소 변동으로 코드 경계를 찾아본다]
                 *
                 * 아래 중앙부 사다리는 "코드가 상자 가운데 있다"는 가정에
                 * 기대는 대용품이다. 경계를 실제로 찾을 수 있으면 그게 낫다 —
                 * tightenToLocalVariation()은 밝기가 아니라 국소 변동을 보므로
                 * 저대비 코드에도 듣는다(tightenToContent()는 전역 임계라
                 * 명암 24 미만이면 아예 false다).
                 * [[vscan-lite-lowcontrast-tighten]]
                 */
                if (sHits.empty() && !budgetExceeded()) {
                    GrayImage tightC;
                    int tcx = 0, tcy = 0;
                    if (tightenToLocalVariation(GrayView(crop), tightC, 8, 8, 8, &tcx, &tcy)) {
                        GrayImage tb;
                        if (stretchContrast(GrayView(tightC), tb)) {
                            GrayImage tsm;
                            boxBlur3x3(GrayView(tb), tsm);
                            for (const GrayImage* cand : {&tsm, &tb}) {
                                if (!sHits.empty() || budgetExceeded()) break;
                                sHits = roiPipe.processViewCore(GrayView(*cand));
                            }
                        }
                        if (!sHits.empty())
                            for (auto& r : sHits)
                                for (auto& pt : r.symbol.position) { pt.first += tcx; pt.second += tcy; }
                    }
                }

                for (int pct : {70, 50, 35}) {
                    if (!sHits.empty() || budgetExceeded()) break;
                    const int cw = rw * pct / 100, ch = rh * pct / 100;
                    if (cw < 48 || ch < 48) continue;
                    const int cx = (rw - cw) / 2, cy = (rh - ch) / 2;
                    GrayImage sub;
                    sub.width = cw; sub.height = ch;
                    sub.pixels.resize(static_cast<size_t>(cw) * ch);
                    for (int r = 0; r < ch; ++r)
                        std::memcpy(sub.pixels.data() + static_cast<size_t>(r) * cw,
                                    crop.pixels.data() + static_cast<size_t>(cy + r) * rw + cx, cw);
                    GrayImage sb;
                    if (!stretchContrast(GrayView(sub), sb)) continue;
                    GrayImage sm;
                    boxBlur3x3(GrayView(sb), sm);
                    for (const GrayImage* cand : {&sm, &sb}) {
                        if (!sHits.empty() || budgetExceeded()) break;
                        sHits = roiPipe.processViewCore(GrayView(*cand));
                    }
                    if (!sHits.empty())
                        for (auto& r : sHits)
                            for (auto& pt : r.symbol.position) { pt.first += cx; pt.second += cy; }
                }

                // [저대비일 때만] 국소 이진화는 어디까지나 대비 도구다.
                // ROI가 이미 계조를 200 이상 쓰고 있으면(stretchContrast가
                // false를 돌려준 경우) 실패 원인이 대비가 아니므로 돌 이유가
                // 없다 — 코퍼스 300장에서 이 검사 없이 돌리면 p95가 21%
                // 늘었는데 그 대부분이 대비와 무관한 프레임이었다.
                if (sHits.empty() && !boosted.pixels.empty()) {
                    // [그래도 안 되면 국소 정규화] 위 두 시도는 ROI 하나에
                    // LUT 하나를 쓰는 전역 변환이라, ROI 안에서 밝기 차가
                    // 크면 코드가 쓰는 계조 구간이 그 차이에 눌린다.
                    // 실측(Code128 module 8, 대비 0.10): 크롭의 퍼센타일이
                    // 108~220인데 코드는 그 안에서 110~140만 쓴다 — 펴봐야
                    // 코드는 4~68에 머물고, zxing의 8x8 블록 중 34%가
                    // "범위 24 미만 = 구조 없음"으로 빠진다.
                    // localAdaptiveBinarize()는 그 판정을 우회한다 —
                    // 국소 평균으로 우리가 직접 이진화한다.
                    // [[vscan-lite-roi-local-binarize]]
                    // [임계 규칙 두 가지를 다 본다] 국소 평균과 국소
                    // 중간값은 서로 다른 심볼로지를 살린다 — 실측(대비
                    // 0.05~0.40 스윕, 8단): 평균은 Code128/EAN13/DM/UPCA를
                    // 0.05까지 열지만 QR은 못 열고, 중간값은 QR/EAN8/
                    // DataBarExp를 열지만 앞의 넷을 도로 닫는다. 여기는
                    // 다른 게 다 실패한 뒤의 구제 자리이므로 둘 다 돌린다.
                    GrayImage localized;
                    for (bool mid : {false, true}) {
                    if (!sHits.empty() || budgetExceeded()) break;
                    if (localAdaptiveBinarize(GrayView(crop), localized, mid)) {
                        // [편 다음엔 다시 좁힌다] 영역 상자는 타일 격자(원본
                        // 128px) 단위라 코드보다 헐렁하다. 실측(대비 0.10):
                        // 영역 크롭이 1184x832인데 코드는 1019x377이다.
                        // 국소 정규화 뒤에는 코드가 진짜 흑백이 되고 배경은
                        // 손대지 않은 채 남으므로, 여기서 tightenToContent()가
                        // 정확히 코드만 집어낸다 — 그리고 그 차이가 결정적이다:
                        // 같은 이미지가 1184x832에서는 안 읽히고 1019x377로
                        // 좁히면 8.8ms에 읽힌다.
                        int tx = 0, ty = 0;
                        GrayImage tight;
                        const bool tightened =
                            tightenToContent(GrayView(localized), tight, 24, &tx, &ty);
                        if (budgetExceeded()) break;
                        sHits = roiPipe.processViewCore(
                            GrayView(tightened ? tight : localized));
                        if (tightened)
                            for (auto& r : sHits)
                                for (auto& pt : r.symbol.position) { pt.first += tx; pt.second += ty; }
                    }
                    }

                    // [여백을 걷어낸 판본으로 한 번 더] 위 시도들이 쓴 크롭은
                    // 정지대용 여백이 붙어 있는데, 저대비에서는 그게 실패
                    // 원인이 된다(근거와 기각된 가설은 상자 만드는 자리 주석).
                    // 이 자리는 stretchContrast가 true를 돌려준 경우 —
                    // 즉 저대비가 확인된 프레임 — 에서만 도달한다.
                    // [[vscan-lite-lowcontrast-tight-crop]]
                    // 두 벌을 본다. (1) 정밀화된 상자 + 12px, (2) 그 상자를
                    // 더 높은 에너지 임계로 한 번 더 좁힌 것 + 0px.
                    // (2)가 따로 필요한 이유는 **납작한 코드**다 — PDF417은
                    // 코드가 72px 높이인데 상자가 121px이라, 12px을 더 얹으면
                    // 종이가 크롭의 절반이 된다. 실측(PDF417 대비 0.10):
                    // (1)로는 안 열리고 (2)로 열린다. 반대로 세로가 넉넉한
                    // 1D 여섯 종은 (1)에서 다 열리므로 (2)는 그때 안 돈다.
                    if (sHits.empty() && !budgetExceeded() &&
                        rectIdx < static_cast<int>(tightRects.size())) {
                        const Rect base = tightRects[rectIdx];
                        for (int variant = 0; variant < 2 && sHits.empty(); ++variant) {
                            if (variant && budgetExceeded()) break;
                            Rect b = base;
                            int pad = 12;
                            if (variant) {
                                const Rect again = refineRegionBox(image, base, 0.60f, 8);
                                if (again.x1 - again.x0 < 16 || again.y1 - again.y0 < 16) break;
                                if (again.x1 - again.x0 >= b.x1 - b.x0 &&
                                    again.y1 - again.y0 >= b.y1 - b.y0) break;  // 안 좁아졌으면 같은 그림
                                b = again;
                                pad = 0;
                            }
                            const Rect tr{std::max(0, b.x0 - pad), std::max(0, b.y0 - pad),
                                          std::min(image.width, b.x1 + pad),
                                          std::min(image.height, b.y1 + pad)};
                            const int tw = tr.x1 - tr.x0, th = tr.y1 - tr.y0;
                            if (tw < 16 || th < 16 || (tw >= rw && th >= rh)) continue;
                            GrayImage tcrop;
                            tcrop.width = tw;
                            tcrop.height = th;
                            tcrop.pixels.resize(static_cast<size_t>(tw) * th);
                            for (int r = 0; r < th; ++r)
                                std::memcpy(tcrop.pixels.data() + static_cast<size_t>(r) * tw,
                                            image.pixels + static_cast<size_t>(tr.y0 + r) * srcStride + tr.x0, tw);
                            GrayImage tboost;
                            if (!stretchContrast(GrayView(tcrop), tboost)) continue;
                            GrayImage tsm;
                            boxBlur3x3(GrayView(tboost), tsm);
                            if (budgetExceeded()) break;
                            sHits = roiPipe.processViewCore(GrayView(tsm));
                            if (sHits.empty())
                                if (budgetExceeded()) break;
                                sHits = roiPipe.processViewCore(GrayView(tboost));
                            // [세로평균 + 행 단위 이진화]
                            // 대비 0.05에서도 **코드 행의 런 개수는 대비 1.0과
                            // 정확히 같다**(Codabar 81 / Code93 99 / ITF 49 /
                            // UPC-E 35). 진폭만 19계조로 줄었을 뿐 정보는
                            // 그대로다. 그러니 문제는 임계를 어디서 잡느냐다.
                            //
                            // 블록 단위 이진화는 여기서 무너진다 — 블록이
                            // 굵은 요소 안에 통째로 들어가면 이웃에서 빌려야
                            // 하는데, 19계조에서는 그 추정이 안 선다. 1D는
                            // 한 행이 코드 전체를 담고 그 행 안에 밝은 요소와
                            // 어두운 요소가 반드시 둘 다 있으므로, **그 행
                            // 자신의 백분위수**가 곧 임계다.
                            //
                            // 실측(대비 0.05, 다른 모든 전처리가 실패한 상태):
                            // 세로평균 반경 1 + 여백 0 + 행 10퍼센타일 한 벌로
                            // 일곱 종이 전부 열린다(Codabar/Code39/Code93/
                            // DataBar/ITF/PDF417/UPC-E). 블록 이진화 두 벌로는
                            // 셋만 열렸다. 디코드 횟수는 오히려 하나 줄었다.
                            // [[vscan-lite-row-binarize]]
                            if (sHits.empty() && !budgetExceeded()) {
                                const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
                                GrayImage z;
                                z.width = bw;
                                z.height = bh;
                                z.pixels.resize(static_cast<size_t>(bw) * bh);
                                for (int r = 0; r < bh; ++r)
                                    std::memcpy(z.pixels.data() + static_cast<size_t>(r) * bw,
                                                image.pixels + static_cast<size_t>(b.y0 + r) * srcStride + b.x0, bw);
                                // 행 단위 임계는 **막대가 세로일 때만** 맞는
                                // 가정이다. 정사각형에 가까운 상자(2D 또는
                                // 기울어진 1D)에는 블록 임계를 쓴다 — 여백을
                                // 걷은 크롭이라는 점이 여기서도 그대로 효과를
                                // 낸다. 실측(40종 18번, QR 대비 0.08): 영역
                                // 상자 그대로(870x870)는 어떤 조합으로도 안
                                // 열리는데, 여백을 걷은 512x512에서는 블록 48
                                // 평균임계로 열린다. 앞 단계가 이미 블록 24로
                                // 두 규칙을 다 돌았으므로 여기서는 더 큰 블록이
                                // 맞다 — 큰 2D 코드는 24px 블록이 한 모듈보다
                                // 작아 국소 통계가 흔들린다.
                                const bool wide = std::max(bw, bh) >= 3 * std::min(bw, bh) / 2;
                                GrayImage vbin;
                                bool ok;
                                if (wide) {
                                    GrayImage vb;
                                    verticalBlur(GrayView(z), 1, vb);
                                    ok = rowBinarize(GrayView(vb), vbin);
                                } else {
                                    GrayImage zs;
                                    if (!stretchContrast(GrayView(z), zs)) zs = z;
                                    ok = localAdaptiveBinarize(GrayView(zs), vbin, false, 48);
                                }
                                if (ok) {
                                    if (budgetExceeded()) break;
                                    sHits = roiPipe.processViewCore(GrayView(vbin));
                                    for (auto& r : sHits)
                                        for (auto& pt : r.symbol.position) {
                                            pt.first += b.x0 - tr.x0;
                                            pt.second += b.y0 - tr.y0;
                                        }
                                }
                            }
                            // 좌표는 이 크롭 기준이므로 원래 크롭 기준으로 옮긴다.
                            for (auto& r : sHits)
                                for (auto& pt : r.symbol.position) {
                                    pt.first += tr.x0 - rc.x0;
                                    pt.second += tr.y0 - rc.y0;
                                }
                        }
                    }
                }
                if (sHits.empty() && rectIdx < cfg_.perspRescueMaxRegions && !budgetExceeded()) {
                    Stage stP("rg:persp");
                    // [원근 보정] 회전은 축 하나면 되지만(§3.24) 원근은
                    // 코드 **안에서** 배율이 달라져서 한 스캔 행 안의 모듈
                    // 폭이 계속 변한다. 네 변을 직선으로 맞춰 사각형을 잡고
                    // 직사각형으로 편다. 이미 직사각형에 가까우면 함수가
                    // false를 돌려주므로 정상 프레임에서는 비용이 윤곽
                    // 추정뿐이다. [[vscan-lite-perspective-rectify]]
                    GrayImage rect;
                    RectifyMap rmap{};
                    if (perspectiveRectify(GrayView(crop), rect, 48, &rmap)) {
                        if (budgetExceeded()) break;
                        sHits = roiPipe.processViewCore(GrayView(rect));
                        // [좌표 되돌리기] 편 좌표를 그대로 두면 dedup이
                        // 엉뚱한 자리의 상자끼리 비교하게 되어 중복이
                        // 통과한다(실측: 빼먹었더니 UPCE 원근 축 중복 5건).
                        for (auto& r : sHits)
                            for (auto& pt : r.symbol.position) {
                                double sx = 0, sy = 0;
                                rectifyMapBack(rmap, pt.first, pt.second, sx, sy);
                                pt.first = static_cast<int>(sx);
                                pt.second = static_cast<int>(sy);
                            }
                    }
                }
                if (sHits.empty() && rectIdx < cfg_.perspRescueMaxRegions && !budgetExceeded()) {
                    Stage stPi("rg:pitch");
                    // [곡면(원통) 보정] 원통 라벨은 상자가 직사각형 그대로라
                    // 호모그래피로는 못 편다 — 가로 좌표만 비선형으로 밀린다.
                    // 국소 바 피치를 균등하게 다시 샘플링한다.
                    //
                    // [모수 세 벌을 차례로 본다]
                    // 피치를 재는 행 수(1/9)와 창 크기·백분위수는 어느
                    // 한 벌이 나머지를 포함하지 않는다. 곡면 축 11단계
                    // 실측에서 세 벌이 각각 자기만 여는 구간이 있다:
                    //   (1,13,10) 1행 기본 — DataBar 0.6/0.7이 여기서만 열린다
                    //   (9,21,5)  넓은 창 + 낮은 백분위수 — PDF417 0.2/0.3,
                    //             DataBar 0.1, DataBarExp 0.1
                    //   (9,13,10) 9행 기본 — DataBarExp 0.2
                    // 결과: DataBar/DataBarExp 곡면 100%, PDF417 10/11.
                    //
                    // 값은 치른다. 사다리 맨 끝이라 이미 다 실패한
                    // 프레임에서만 돌지만, 그런 프레임이 곧 p95다 —
                    // 300장 코퍼스 교차 측정(한 벌 -> 두 벌 -> 세 벌):
                    // 평균 128.9 / 139.2 / 148.0ms, p95 366 / 405 / 436ms,
                    // 검출 72.4 / 72.7 / 72.9%.
                    //
                    // 값을 깎아보려 한 것들은 전부 실패했다:
                    //  - "지도가 안 휘었으면 다음 벌을 건너뛴다"(절대 픽셀 /
                    //    코드 폭 대비 두 가지): 시간은 거의 그대로고 검출만 -1
                    //  - 뒤 벌을 첫 영역으로 제한: 시간 변화 없음(단일 코드
                    //    프레임이 대부분이라 애초에 첫 영역만 돈다)
                    //  - 뒤 벌의 디코드에서 회전/반전 끄기: 평균 -2.7%, 검출 -1
                    //  - 디코드 없이 지도를 채점해서 한 벌만 고르기: 평준화된
                    //    이미지의 런 길이가 모듈의 정수배에 얼마나 가까운가로
                    //    재봤는데, 런 길이가 정수 픽셀이라 점수가 1/8 격자에
                    //    붙어버려 아홉 개 후보가 전부 같은 값(0.1250)이 나왔다.
                    //    변별력이 없다.
                    // 이 비용은 CI 기준선에 반영돼 있다.
                    // [[vscan-lite-pitch-equalize]]
                    // 네 번째 벌은 **행 이진화를 앞에 넣는다**. 가장 강한
                    // 곡률에서는 회색조의 런 경계가 흐려 피치 추정이 흔들리는데,
                    // 행 단위로 먼저 흑백을 만들면 런이 깨끗해져서 추정이 선다.
                    // 그때는 백분위수를 30까지 올려도 된다(회색조에서는 30이면
                    // 하나도 안 열렸다 — 2모듈 런을 1모듈로 착각하기 때문인데,
                    // 이진화된 뒤에는 런 길이가 정확하다).
                    // 실측: PDF417 곡면 0.1이 여기서만 열린다.
                    // [곡면이 아니면 아예 안 든다] 이 단계는 모수 네 벌 x
                    // 영역 두 개까지 도는 자리라 실패 프레임에서 100ms 가까이
                    // 쓴다(실측 97ms/프레임). 그런데 그 대부분은 애초에 곡면이
                    // 아닌 프레임이다. 왼쪽 1/3과 오른쪽 1/3의 1모듈 픽셀 수를
                    // 비교하면(런 한 줄 훑는 값) 배율이 실제로 변하는지 바로
                    // 알 수 있다. 실측(곡면 축): 왜곡 없음 1.00~1.20,
                    // 0.6 이하 1.40~2.33. 1.25로 가른다.
                    // 0을 돌려주면 못 잰 것이므로 거르지 않는다.
                    const float pv = pitchVariation(GrayView(crop));
                    const bool curved = !(pv > 0.0f && pv < 1.25f);
                    struct PitchTry { int rows, win, pct; bool rowbin; };
                    static constexpr PitchTry kTries[] = {
                        {1, 13, 10, false}, {9, 21, 5, false}, {9, 13, 10, false},
                        {3, 31, 30, true},
                    };
                    GrayImage rbCrop;
                    bool rbDone = false, rbOk = false;
                    for (const PitchTry& t : kTries) {
                        if (!curved) break;
                        if (&t != kTries && budgetExceeded()) break;
                        const GrayImage* srcImg = &crop;
                        if (t.rowbin) {
                            // 납작한 크롭(가로가 세로의 3배 이상)에서만 돈다.
                            // 이 벌이 필요했던 것은 PDF417 곡면 0.1 하나이고,
                            // 그 크롭이 1055x201이다. 조건 없이 돌리면 코퍼스
                            // p95가 439 -> 584ms(+33%)가 된다 — 사다리 맨
                            // 끝에서 프레임마다 디코드를 한 번 더 도는 값이다.
                            if (crop.height * 3 > crop.width) break;
                            if (!rbDone) {
                                GrayImage vb;
                                verticalBlur(GrayView(crop), 1, vb);
                                rbOk = rowBinarize(GrayView(vb), rbCrop);
                                rbDone = true;
                            }
                            if (!rbOk) continue;
                            srcImg = &rbCrop;
                        }
                        GrayImage eq;
                        PitchMap pmap;
                        if (!pitchEqualize(GrayView(*srcImg), eq, &pmap, t.rows, t.win, t.pct)) continue;
                        if (budgetExceeded()) break;
                        sHits = roiPipe.processViewCore(GrayView(eq));
                        if (sHits.empty()) continue;
                        // 가로만 바뀌었으므로 x는 역사상, y는 여백만 뺀다.
                        for (auto& r : sHits)
                            for (auto& pt : r.symbol.position) {
                                const int i = pt.first - pmap.marginX;
                                if (i >= 0 && i < (int)pmap.srcX.size())
                                    pt.first = static_cast<int>(pmap.srcX[i]);
                                pt.second -= pmap.marginY;
                            }
                        break;
                    }
                }
                if (sHits.empty() && rectIdx < cfg_.invertRescueMaxRegions && !budgetExceeded()) {
                    Stage stI("rg:invert");
                    // [흑백 반전 판본] zxing의 TryInvert는 **1D와 PDF417에
                    // 대해서는 아무 일도 하지 않는다.** 소스를 보면
                    // MultiFormatReader가 반전된 비트맵에서 리더를 이렇게
                    // 거른다:
                    //
                    //   if (image.inverted() && !reader->supportsInversion) continue;
                    //
                    // 그런데 supportsInversion=true로 만들어지는 건 QR /
                    // DataMatrix / Aztec 셋뿐이다. 게다가 BinaryBitmap::
                    // invert()는 2D가 쓰는 BitMatrix만 뒤집고, 1D가 쓰는
                    // getPatternRow()는 휘도에서 직접 계산하므로 애초에
                    // 반전이 반영되지도 않는다. 즉 zxing 쪽 설정으로는
                    // 원리적으로 열리지 않는다.
                    //
                    // 실측(module 8, 반전 축): QR/DataMatrix만 읽히고
                    // **나머지 12종이 전부 0/1**이었다(그러면서 120~190ms를
                    // 썼다 — 폴백 체인을 끝까지 돌기 때문).
                    //
                    // 그래서 우리가 뒤집어서 넣는다. 극성을 미리 재보려
                    // 했지만(정지대 링의 밝기 대 내부 밝기) 심볼로지마다
                    // 값이 뒤죽박죽이라 신뢰할 수 없었다 — ITF 반전본은
                    // 링과 내부의 차이가 0이었다. 판정 대신 사다리에 한
                    // 칸 더 두는 편이 오판 위험이 없다. 다른 모든 시도가
                    // 실패한 뒤에만 도는 자리다.
                    // [[vscan-lite-roi-invert]]
                    GrayImage flipped;
                    flipped.width = rw;
                    flipped.height = rh;
                    flipped.pixels.resize(static_cast<size_t>(rw) * rh);
                    for (size_t k = 0; k < flipped.pixels.size(); ++k)
                        flipped.pixels[k] = static_cast<uint8_t>(255 - crop.pixels[k]);
                    if (budgetExceeded()) break;
                    sHits = roiPipe.processViewCore(GrayView(flipped));
                }
                for (auto& r : sHits)
                    for (auto& pt : r.symbol.position) { pt.first += rc.x0; pt.second += rc.y0; }
                // [같은 코드를 두 영역에서 각각 잡는 경우]
                // 세로로 긴 1D 코드(예: EAN13 846x544)는 타일 격자 상자
                // 여러 개에 걸친다. 각 상자를 따로 구제하면 zxing이 서로
                // 다른 스캔 밴드를 돌려주는데, 밴드는 두께 4px짜리라 서로
                // 500px 넘게 떨어질 수 있다 — 실측: (158,774)-(858,778)과
                // (164,1289)-(865,1293), 둘 다 정답 상자 762~1306 안이다.
                // dedup()의 기하 규칙(밴드/얇은조각)은 이 간격을 못 넘는다.
                //
                // 여기서는 **어느 영역에서 나왔는지**를 알고 있으므로
                // 기하로 추측할 필요가 없다. 같은 텍스트가 이미 있고 그
                // 결과를 낸 영역이 지금 영역과 맞닿아 있으면(둘을 25%씩
                // 부풀렸을 때 겹치면) 같은 코드다. 떨어져 있는 같은 내용
                // 라벨 2장은 이 조건에 안 걸린다.
                for (auto& r : sHits) {
                    bool same = false;
                    for (size_t k = 0; k < stretched.size() && !same; ++k)
                        if (stretched[k].symbol.text == r.symbol.text &&
                            k < srcRect.size() && rectsTouch(srcRect[k], rc))
                            same = true;
                    if (same) continue;
                    stretched.push_back(r);
                    srcRect.push_back(rc);
                }
            }
            if (take(dedup(std::move(stretched)))) return true;
        }
        return false;
    };

    // [2차: 영역을 각도만큼 되돌려 다시] 여기까지 오면 자르는 것만으로는
    // 부족한 심볼로지다 — 실측상 PDF417과 1D가 그렇다. 2D 행렬코드는
    // 자르기만 하면 전 각도가 읽히므로(DataMatrix 0~90도 19/19) 여기까지
    // 내려오지 않는다.
    //
    // 각도는 findCodeRegions()가 영역별로 이미 준다. 실측(PDF417 module 8,
    // 0~90도): 잘라내기만 하면 0/19였는데, 영역 추정각으로 한 번 되돌리니
    // 10/11이 살아났다(5/10/20/40/45/50/60/70/75/80도 성공, 30도만 추정
    // 오차 3도로 실패).
    //
    // 회전은 리샘플링이 들어가 영역 디코드보다 비싸므로, 방향이 뚜렷한
    // (angleDeg가 있는) 영역에만, 그리고 상위 몇 개에만 건다.
    const int rotLimit = std::min<int>(cfg_.regionRescueMaxRotations, (int)regions.size());
    auto rotatePass = [&]() -> bool {
    for (int i = 0; i < rotLimit; ++i) {
        if (budgetExceeded()) break;
        if (regions[i].angleDeg >= CodeRegion::kAngleUnknown) continue;
        if (i >= (int)rotRects.size()) break;
        const Rect& rc = rotRects[i];
        const int rw = rc.x1 - rc.x0, rh = rc.y1 - rc.y0;
        if (rw < 32 || rh < 32) continue;

        GrayImage crop;
        crop.width = rw;
        crop.height = rh;
        crop.pixels.resize(static_cast<size_t>(rw) * rh);
        const int srcStride = image.stride > 0 ? image.stride : image.width;
        for (int r = 0; r < rh; ++r)
            std::memcpy(crop.pixels.data() + static_cast<size_t>(r) * rw,
                        image.pixels + static_cast<size_t>(rc.y0 + r) * srcStride + rc.x0, rw);

        // [각도 정밀화] findCodeRegions()의 추정각은 4배 축소본에서 나온
        // 값이라 축 쪽으로 3~4도 끌려 있다. PDF417은 되돌린 뒤 읽히는
        // 각도 창이 8도 남짓이라 그 오차면 창을 벗어난다(25/30도 실패).
        // 여기서 영역 안만 원본 해상도로 다시 재면 오차가 0.7도로 줄어든다.
        // 회전할 영역(최대 2개)에만 드는 비용이다. angleOf()가 캐시하므로
        // 아래 "회전을 먼저 할까" 판정에서 이미 쟀다면 공짜다.
        const float useAngle = angleOf(static_cast<size_t>(i));

        GrayImage rotated;
        rotateAroundPoint(GrayView(crop), -useAngle,
                          static_cast<float>(rw) / 2.0f, static_cast<float>(rh) / 2.0f, rotated);

        // [되돌린 뒤 다시 좁힌다] 기울어진 코드를 담으려면 크롭이 대각선
        // 길이만큼 커야 하는데, 되돌리고 나면 코드가 축에 정렬돼서 그
        // 상자의 20% 남짓만 차지한다. 나머지는 회전으로 생긴 흰 여백이라
        // 디코더가 훑을 이유가 없다. 실측(Code128 모듈 8px, 25/40/70도):
        // 회전본 1184x1088에서 실제 코드는 882x320이고, 그만큼만 잘라
        // 디코드하면 16.8 -> 9.8ms다(검출 동일). 이 단계가 회전 구제
        // 시간의 지배항이라 효과가 그대로 총합에 남는다.
        //
        // 좌표 보정을 위해 잘라낸 원점을 기억해둔다.
        int tightX = 0, tightY = 0;
        GrayImage tight;
        if (tightenToContent(GrayView(rotated), tight)) {
            // tightenToContent()는 오프셋을 돌려주지 않으므로 폭/높이 차이로
            // 되짚는다 — 여백이 상하좌우 대칭이 아닐 수 있어 정확하지 않다.
            // 회전 좌표는 어차피 근사(아래 역회전도 크롭 중심 기준)이므로
            // 중심이 유지되는 이 근사로 충분하다.
            tightX = (rotated.width - tight.width) / 2;
            tightY = (rotated.height - tight.height) / 2;
            rotated = std::move(tight);
        }

        // [되돌린 다음엔 먼저 싸게 물어본다] 여기 들어온 이미지는 방금
        // 축에 정렬해 놓고 여백까지 잘라낸 상태라, TryHarder/TryInvert가
        // 필요 없다. 그런데 roiCfg는 풀옵션이라 성공하는 경우까지 그 값을
        // 다 낸다. 실측(Code128 module 8, 45/70도, 927x357 / 369x931):
        // 풀옵션 9.0~9.5ms인데 이 가벼운 설정은 2.5~3.0ms로 결과가 같다.
        //
        // 단 TryRotate는 켜둔다. "되돌렸으니 축에 맞았을 텐데 왜"라고
        // 생각하기 쉽지만, 되돌리는 각이 (-45,45]로 접힌 값이라 실제
        // 기울기가 50~70도였던 코드는 되돌린 뒤 **세로**로 선다(70도 ->
        // 접은 각 -20 -> 되돌리면 순수 90도). 실측으로 정확히 그 구간만
        // 이 단계가 빈손이 되어 풀옵션까지 내려갔다(50~70도 29ms,
        // 20~40도 21ms). TryRotate를 켜면 크롭이 작아서 비용은 거의 0이고
        // 그 구간이 21~23ms로 붙는다.
        PipelineConfig plainCfg = roiCfg;
        plainCfg.tryHarder = false;
        plainCfg.tryRotate = true;
        plainCfg.tryInvert = false;
        Pipeline plainPipe(plainCfg);
        auto rotHits = plainPipe.processViewCore(GrayView(rotated));
        if ((int)rotHits.size() < need) {
            Pipeline roiPipe(roiCfg);
            auto full = roiPipe.processViewCore(GrayView(rotated));
            if (full.size() > rotHits.size()) rotHits = std::move(full);
        }
        if (!rotHits.empty()) {
            // [좌표 되돌리기] zxing이 준 건 회전된 크롭의 좌표라 그대로
            // 쓰면 안 된다. rotateAroundPoint()의 목적지->원본 매핑을
            // 그대로 한 번 더 적용하면 원본 크롭 좌표가 나온다
            // (그 함수가 쓰는 각이 -degrees인 것까지 같이 맞춰야 한다).
            // 위치를 뭉개서 대표점 하나로 주면 안 되는 이유: dedup()이
            // 넓이 기반이라 4점이 한 점으로 겹치면 넓이가 0이 되어
            // 중복 판정이 통째로 무력화된다(실측: 중복 반환 2건).
            const float rad = useAngle * 3.14159265358979323846f / 180.0f;
            const float cc = std::cos(rad), ss = std::sin(rad);
            const float px = static_cast<float>(rw) / 2.0f, py = static_cast<float>(rh) / 2.0f;
            for (auto& r : rotHits)
                for (auto& pt : r.symbol.position) {
                    const float dx = static_cast<float>(pt.first + tightX) - px;
                    const float dy = static_cast<float>(pt.second + tightY) - py;
                    pt.first = rc.x0 + static_cast<int>(dx * cc - dy * ss + px);
                    pt.second = rc.y0 + static_cast<int>(dx * ss + dy * cc + py);
                }
            if (take(std::move(rotHits))) return true;
        }
    }
    return false;
    };

    // [순서 결정 — 자를까 돌릴까를 영역이 알려준다]
    // [[vscan-lite-region-rotate-first]]
    //
    // 지금까지는 항상 "자르기 -> (안 되면) 회전"이었다. 그런데 회전이
    // 필요한 프레임에서는 자르기 패스가 **반드시 실패한다** — 원인이
    // 기울기지 면적이 아니기 때문이다. 실측(Code128 module 8, 2단계 경로,
    // 각도별 총 시간 분해):
    //
    //   0도    coarse 2.1 + fast 9.6                                = 11.9ms
    //   20도   ... + locate 2.4 + **자르기 17.3** + 대비 1.0 + 회전 21.0 = 55.5ms
    //   45도   ... + locate 2.9 + **자르기 19.9** + 대비 1.1 + 회전 15.8 = 49.2ms
    //
    // 굵게 표시한 17~20ms가 통째로 헛수고다. 그래서 영역이 "방향이 뚜렷
    // 하고 그 각이 충분히 크다"고 말하면 회전을 먼저 돌린다.
    //
    // 임계 15도의 근거. zxing은 TryRotate로 90도 배수를 알아서 처리하므로
    // 실제로 문제가 되는 건 (-45,45]로 접은 각이다. 접은 각 기준으로
    // 자르기만으로 읽히는 한계를 재보면 (Code128 module 8):
    //   접은 각  0(0도) 10(10도) 20(20도) ... -20(70도) -10(80도) 0(90도)
    //   자르기    성공    성공     실패          실패      성공     성공
    // 10도는 되고 20도는 안 된다. 그 사이에 임계를 두되, 판정에 쓰는 각이
    // 원본 해상도 추정값(오차 0.7도)이므로 여유는 크게 필요 없다.
    //
    // 순서를 바꾸는 것뿐이라 검출력에는 영향이 없다 — 회전이 빈손이면
    // 자르기 패스가 그대로 뒤에서 돈다. 잘못 짚었을 때의 손해는 "원래
    // 순서로 했을 때와 같은 총합"이 상한이다.
    constexpr float kRotateFirstDeg = 15.0f;
    bool rotateFirst = false;
    if (pass == RegionPass::Both && !regions.empty() &&
        regions[0].angleDeg < CodeRegion::kAngleUnknown) {
        const float a = angleOf(0);
        rotateFirst = (a < CodeRegion::kAngleUnknown) && (std::fabs(a) >= kRotateFirstDeg);
    }

    if (pass != RegionPass::RotateOnly && !rotateFirst) { if (cropPass()) return hits; }
    if (pass != RegionPass::CropOnly) { if (rotatePass()) return hits; }
    if (pass != RegionPass::RotateOnly && rotateFirst) { if (cropPass()) return hits; }
    return hits;
}

std::vector<PipelineResult> Pipeline::tryQrFinderRescue(const GrayView& image, int need) {
    // [[vscan-lite-qr-finder-locate]]
    //
    // 에너지 로케이터(findCodeRegions)는 심볼로지를 안 가리는 대신,
    // **작은 코드가 여러 개 흩뿌려진 프레임**에서는 원리적으로 못 쓴다 —
    // 타일이 원본 128px이라 45px 코드 여러 개가 한 타일에 뭉치고, 에너지
    // 순위는 면적에 좌우돼서 큰 글자 블록이 작은 QR을 밀어낸다(실측:
    // 실물 3.1MP 해상도 차트에서 상위 2개가 글자였다).
    //
    // QR은 고유 구조가 있으니 그걸 쓴다. 파인더 패턴은 어느 방향으로
    // 잘라도 1:1:3:1:1이고 이 비율은 크기·회전 불변이라, 작아도 흐려도
    // 비율만 살아있으면 걸린다. 실측(같은 차트, 모듈 2.1px): 8.1ms에
    // QR 후보 11곳, 그중 9곳이 "잘라주면 읽히는" 지점이었다.
    //
    // 찾은 상자는 processViewROIs()로 넘긴다 — 거기엔 작은 ROI를 확대 +
    // 언샤프로 살리는 단계가 이미 있고(§3.22), 모듈 2px대 코드에는 그게
    // 필수다.
    // QR/MicroQR 비트(0,1)가 마스크에서 빠져 있으면 돌 이유가 없다.
    constexpr uint32_t kQrBits = (1u << 0) | (1u << 1);
    if (cfg_.formatMask != 0 && (cfg_.formatMask & kQrBits) == 0) return {};

    auto cands = findQrCandidates(image, std::min(16, std::max(4, need * 2)));
    if (cands.empty()) return {};

    std::vector<Rect> rects;
    rects.reserve(cands.size());
    for (const auto& c : cands) rects.push_back(c.bbox);

    // 정지대 몫으로 조금 넉넉히. 파인더 기반 상자는 코드에 딱 맞아서
    // 여백이 없으면 디코더가 경계를 못 잡는다.
    // 정지대 몫으로 조금 넉넉히. 파인더 기반 상자는 코드에 딱 맞아서
    // 여백이 없으면 디코더가 경계를 못 잡는다. 8~40px를 훑어봤는데
    // 결과가 같았다(모듈이 2px대라 어느 쪽이든 정지대가 충분하다).
    return processViewROIs(image, rects, 12);
}

std::vector<PipelineResult> Pipeline::tryDeskewRescue1D(const GrayView& image, int need) {
    // 여기까지 왔다는 건 zxing의 카디널 회전 허용범위(실측 ±21도 부근,
    // §3.2.15/16) 밖에 있는 1D 바코드일 가능성이 있다는 뜻이다.
    // 실물 산업용 리더기 실측으로 이 케이스가 실제로 발생함이
    // 확인됐다(§3.2.16).
    //
    // 접근: 정밀한 각도를 이미지 처리로 추정하지 않는다 — 대신
    // (1) "1D 바코드처럼 보이는 영역이 대략 어디인지"만 싸게 찾고
    //     (findDeskewCandidate, 타일 단위 그래디언트 일관성),
    // (2) 그 영역을 중심으로 45도/135도 두 각도만 시도해서 zxing
    //     자신에게 성공 여부를 직접 물어본다.
    // 실측(대화 중 프로토타입): 위치를 안다는 전제 하에 45도 단일
    // 시도로 24~66도 갭이 완전히 메워졌다(대칭성상 135도까지 포함하면
    // 0~180도 전 구간). 위치를 모른 채 "프레임 전체를 통째로 돌리는"
    // 순수 방식은 코드가 회전축(프레임 중심)에서 멀리 떨어져 있으면
    // 실패하거나 배경 잡음에서 엉뚱하게 오검출할 위험이 있었다(실측
    // 확인) — 그래서 반드시 후보 영역 중심을 회전축으로 써서 크롭
    // 범위 안에서만 돈다(범위가 좁아 비용도 훨씬 싸다).
    // 최종 판정은 항상 zxing의 체크섬 검증이므로, 후보 위치 추정이
    // 부정확해도(과녁을 완전히 벗어나지만 않으면) 오검출 위험은 없다.
    // [[vscan-lite-1d-deskew-rescue]]
    DeskewCandidate cand;
    if (!findDeskewCandidate(image, cand)) return {};

    int cw = cand.bbox.x1 - cand.bbox.x0, ch = cand.bbox.y1 - cand.bbox.y0;
    int pad = static_cast<int>(0.5f * std::sqrt(static_cast<float>(cw) * cw + static_cast<float>(ch) * ch));
    int x0 = std::max(0, cand.bbox.x0 - pad), y0 = std::max(0, cand.bbox.y0 - pad);
    int x1 = std::min(image.width, cand.bbox.x1 + pad), y1 = std::min(image.height, cand.bbox.y1 + pad);
    int rw = x1 - x0, rh = y1 - y0;
    if (rw < 32 || rh < 32) return {};

    GrayImage crop;
    crop.width = rw;
    crop.height = rh;
    crop.pixels.resize(static_cast<size_t>(rw) * rh);
    const int srcStride = image.stride > 0 ? image.stride : image.width;
    for (int r = 0; r < rh; ++r)
        std::memcpy(crop.pixels.data() + static_cast<size_t>(r) * rw,
                    image.pixels + static_cast<size_t>(y0 + r) * srcStride + x0, rw);

    float pivotX = static_cast<float>(rw) / 2.0f, pivotY = static_cast<float>(rh) / 2.0f;
    PipelineConfig rescueCfg = cfg_;
    rescueCfg.tileThreads = 1;
    rescueCfg.tryHarder = true;
    rescueCfg.tryRotate = true;
    rescueCfg.tryInvert = true;
    rescueCfg.enable1DDeskewRescue = false; // 재귀 방지
    rescueCfg.enableDPMRescue = false;       // 회전 크롭엔 불필요, 비용만 낭비
    Pipeline rescuePipe(rescueCfg);

    // [회전 각도 — 추정값 우선, 고정각은 폴백]
    // findDeskewCandidate()가 후보 판정용으로 어차피 계산하는 구조텐서에서
    // 지배 방향을 뽑아준다(cand.angleDeg). 그게 곧 막대가 기울어진 각이라,
    // 그 각으로 한 번만 되돌리면 끝난다.
    //
    // 예전에는 각도를 모른 채 45 -> 22.5 -> 67.5 -> 135도를 맞을 때까지
    // 순차 시도했다. 실측(§3.11): 그 비용이 프레임당 평균 23.4ms —
    // 전체 디코드 시간의 21%인데, 난수 코퍼스에서 얻는 건 500개 중 1개였다.
    //
    // [중요] 추정각이 있으면 **그것만** 시도한다. 고정각을 뒤에 덧붙이면
    // 안전해 보이지만 실제로는 반대다:
    // 구제 비용의 대부분은 "구제해도 결국 실패하는 프레임"에서 나온다.
    // 그런 프레임은 후보 영역이 애초에 바코드가 아니어서(텍스트 덩어리,
    // 박스 모서리 등) 어떤 각도로 돌려도 안 읽힌다 — 각도를 늘릴수록
    // 실패 비용만 비례해서 커진다. 실측으로 확인했다: 추정각을 앞에
    // 붙이기만 하고 고정각 4개를 남겼더니 구제 비용이 23.4 -> 26.1ms로
    // 오히려 늘었다.
    // 반대로 진짜 회전된 1D가 있으면 구조텐서가 그 방향을 가리키므로,
    // 추정각이 맞다. 즉 "추정이 되면 그것만, 안 되면 고정각 순회"가
    // 커버리지는 유지하면서 실패 비용만 잘라내는 조합이다.
    // 방향이 뚜렷하지 않은 후보(코히런스 낮음)는 **1D 바코드가 아니다** —
    // 텍스트 덩어리, 박스 모서리, 체커보드 같은 것들이다. 예전에는 그런
    // 후보에도 45/22.5/67.5/135도를 전부 돌려보고 실패했는데, 구제 비용의
    // 대부분이 정확히 거기서 나왔다(실측: 구제가 프레임당 23.4ms를 쓰면서
    // 난수 코퍼스에서 얻는 건 500개 중 1개, §3.11).
    // 바코드라면 구조텐서가 반드시 한 방향을 가리킨다 — 그게 1D의 정의다.
    if (cand.angleDeg == DeskewCandidate::kAngleUnknown) return {};

    // 추정각(부호 반전: 텐서는 "기울어진 각"을 주므로 그만큼 되돌린다)
    // + 45도 하나만 안전망으로. 45도를 남기는 이유는 후보 영역에 배경이
    // 섞이면 추정이 흔들리기 때문 — 40종 39번(45도 1D)이 정확히 그 케이스라
    // 추정만으로는 놓쳤다(36/40으로 회귀).
    const float angles[2] = {-cand.angleDeg, 45.0f};

    for (float ang : angles) {
        if (budgetExceeded()) break;
        GrayImage rotated;
        rotateAroundPoint(GrayView(crop), ang, pivotX, pivotY, rotated);
        auto hits = rescuePipe.processViewCore(GrayView(rotated));
        if ((int)hits.size() >= need) {
            for (auto& r : hits)
                for (auto& pt : r.symbol.position) { pt.first += x0; pt.second += y0; }
            prof().dump("실패");
    return hits;
        }
    }
    return {};
}

std::vector<PipelineResult> Pipeline::processViewTwoStage(const GrayView& image, int cropPadPx) {
    if (image.empty()) return {};
    BudgetGuard budget(this);
    if (!frameHasStructure(image)) return {};   // [[vscan-lite-blank-frame-skip]]
    FrameGuard frameGuard(this);
    regionCropDone_ = false;   // 프레임마다 초기화 (조기/늦은 슬롯 중복 방지)
    regionRotDone_ = false;
    (void)cropPadPx; // 하위 호환용으로 시그니처만 유지 (아래 주석 참고)

    // [S1] 조건을 먼저 재고, 지금 상태로는 통과할 수 없는 것이 확실하면
    // 고쳐서 넣는다. 아래 모든 단계가 이 뷰를 쓴다 — 원본이 아니라.
    // [[vscan-lite-pre-denoise]]
    const GrayView view = preprocessFrame(image);

    // "빠른 패스 먼저, 실패하면 풀스캔" 전략.
    //
    // 1단계: 탐색 옵션(TryHarder/Rotate/Invert)을 꺼서 빠르게 훑는다.
    //   타일링도 끈다(tileThreads=1, 단일 풀프레임 패스) — 타일로 쪼개면
    //   코드가 타일 경계에서 잘리지 않게 overlap을 코드 크기에 맞춰 미리
    //   알고 있어야 하는데, 그 크기는 작업거리/줌/해상도가 바뀔 때마다
    //   달라진다. 단일 풀프레임 패스는 애초에 타일 경계가 없다.
    //
    // 2단계: 1단계가 빈손이면 processView()(풀옵션 풀프레임)로 폴백.
    //
    // [설계 이력 — 중요]
    // 예전에는 1단계가 찾은 위치 주변을 crop해서 풀옵션으로 "재디코드"하는
    // refine 단계가 있었다. 그런데 실측 결과 **refine은 아무것도 추가하지
    // 못했다** (악조건 32종에서 locate가 찾은 45개 대비 refine 추가 검출
    // 0개, 오히려 1건은 crop에서 놓쳐서 불필요한 폴백을 유발).
    // 당연한 결과였다:
    //   - zxing의 ReadBarcodes()는 체크섬/ECC 검증을 통과한 코드만 반환한다.
    //     즉 1단계가 돌려준 결과는 이미 "정확히 디코딩된" 것이라 재디코드할
    //     이유가 없다.
    //   - refine은 1단계가 찾은 위치 주변만 보므로, 1단계가 놓친 코드를
    //     찾아낼 수도 없다.
    // 그래서 refine을 걷어냈다. cropPadPx 인자는 기존 호출부 호환을 위해
    // 남겨뒀지만 더 이상 쓰이지 않는다.
    // [[vscan-lite-refine-is-redundant]]
    PipelineConfig fastCfg = cfg_;
    fastCfg.tileThreads = 1;
    fastCfg.tryRotate = false;
    fastCfg.tryInvert = false;
    fastCfg.tryHarder = false;
    fastCfg.binarizer = cfg_.locateBinarizer;
    fastCfg.tryDownscale = cfg_.locateTryDownscale;

    const int need = std::max(1, cfg_.minExpectedCodes);

    // [S4 — 저대비 프레임은 순서를 뒤집는다]
    // 풀프레임 패스(coarse + fast, 합쳐서 12ms)는 대비가 무너진 프레임에서
    // 성공할 수가 없다. zxing의 이진화가 8x8 블록 명암 폭 24를 "구조 없음"
    // 으로 보기 때문이고, 그건 옵션을 어떻게 켜도 안 바뀐다(§3.25).
    // estimateLocalRange()로 그걸 미리 재서, 낮으면 영역 경로를 먼저 돌린다.
    //
    // **건너뛰는 게 아니라 순서만 바꾼다.** 영역 경로가 빈손이면 풀프레임
    // 패스가 그대로 뒤에서 돈다 — 잘못 짚었을 때의 손해 상한이 "원래
    // 순서로 했을 때와 같은 총합"이다. 검출력에는 영향이 없다.
    //
    // 임계 100의 근거(블록범위 상위 1%, 실측): 대비 0.05~0.30이 24~84,
    // 0.40이 109, 정상 프레임(깨끗/회전/노이즈/블러/모듈 2px/실물 차트)이
    // 213~255다. 0.40은 풀프레임 패스로 읽히므로 정상 쪽에 두는 게 맞다.
    // [[vscan-lite-lowcontrast-region-first]]
    bool regionFirstDone = false;
    const bool lowContrast = cfg_.enableRegionRescue && !cfg_.fastNoRead &&
                             estimateLocalRange(view) < cfg_.lowContrastRange;

    auto regionFirstPass = [&](std::vector<PipelineResult>& acc) -> bool {
        if (!cfg_.enableRegionRescue || cfg_.fastNoRead || budgetExceeded() || regionFirstDone) return false;
        regionFirstDone = true;
        // [회전까지 이 자리에서] 회전 구제를 체인 끝에 두면, 20~70도
        // 코드는 풀프레임 TryHarder / +Invert / 풀옵션을 전부 지나고
        // 나서야 돌려진다. 실측(14종 x 0~90도, 2단계 경로): 그 구간이
        // 150~440ms였는데 회전을 이 자리로 올리니 40~110ms가 됐다.
        // 다만 요구 개수가 회전 상한보다 많으면 이 자리에서 회전해도
        // need를 못 채우고 뒤로 넘어가므로, 그때는 자르기만 한다.
        const bool earlyRotate = need <= std::max(1, cfg_.regionRescueMaxRotations);
        regionCropDone_ = true;
        regionRotDone_ = earlyRotate;
        Stage st("ts:region");
        auto roiHits = tryRegionRescue(view, need,
                                       earlyRotate ? RegionPass::Both : RegionPass::CropOnly);
        if ((int)roiHits.size() >= need) { acc = std::move(roiHits); return true; }
        if (roiHits.size() > acc.size()) acc = std::move(roiHits);
        return false;
    };

    if (lowContrast) {
        std::vector<PipelineResult> early;
        if (regionFirstPass(early)) { adaptiveObserve(early); return early; }
        if (!early.empty()) { /* 부분 결과는 아래 단계들이 더 채울 수 있다 */ }
        lowContrastPartial_ = std::move(early);
    } else {
        lowContrastPartial_.clear();
    }

    // [0단계] coarse locate — 절반 해상도로 먼저 훑는다.
    // 이진화가 locate 비용의 대부분이고 픽셀 수에 비례하므로 1/4이 된다.
    // 실패하면 아래 풀해상도 단계로 자연스럽게 내려간다(검출력 손실 없음).
    // [[vscan-lite-coarse-locate]]
    if (cfg_.coarseLocate && view.width >= 768 && view.height >= 768) {
        if (coarseSkipLeft_ > 0) {
            --coarseSkipLeft_;
        } else {
            const int cf = (cfg_.coarseFactor == 2) ? 2 : 3;
            downsampleBox(view, coarseBuf_, cf);
            Pipeline coarse(fastCfg);
            Stage st("ts:coarse");
            auto ch = coarse.processViewCore(GrayView(coarseBuf_));
            if ((int)ch.size() >= need) {
                // 축소본 좌표 -> 원본 좌표로 환산
                for (auto& r : ch)
                    for (auto& pt : r.symbol.position) { pt.first *= cf; pt.second *= cf; }
                coarseMisses_ = 0;
                return ch;
            }
            // 연속 실패가 쌓이면 한동안 건너뛴다 — 작은 코드만 나오는
            // 배치에서 매 프레임 헛수고(약 2~3ms)하는 것을 막는다.
            if (++coarseMisses_ >= std::max(1, cfg_.coarseMaxConsecutiveMisses)) {
                coarseMisses_ = 0;
                coarseSkipLeft_ = 30;
            }
        }
    }

    Pipeline fast(fastCfg);
    std::vector<PipelineResult> hits;
    { Stage st("ts:fast"); hits = fast.processViewCore(view); }
    if ((int)hits.size() >= need) { adaptiveObserve(hits); return hits; }
    if (lowContrastPartial_.size() > hits.size()) hits = lowContrastPartial_;

    // [1.5단계 — 위치부터 찾고 그 자리만 본다]
    // 빠른 풀프레임 패스가 빈손일 때 가장 흔한 이유는 "코드가 프레임에 비해
    // 작아서 못 찾은 것"이다(§3.17). 그런데 그 다음에 오는 2/3/4단계는
    // 전부 **같은 크기의 프레임을 더 열심히** 다시 보는 것이라, 원인이
    // 면적이면 아무리 옵션을 켜도 잘 안 걸린다 — 대신 시간만 100~250ms 쓴다.
    //
    // 여기서 그래디언트 에너지로 코드 후보 영역을 먼저 찾고(수 ms) 그
    // 영역만 풀옵션으로 본다. 실측(§3.17): 2048x1536 프레임의 128px
    // DataMatrix는 55~90도 전 구간이 실패하는데, 이 단계만 넣으면 회전을
    // 한 번도 안 하고 19/19가 된다.
    //
    // 이게 산업용 리더기가 회전각과 무관하게 10~20ms로 일정한 이유이기도
    // 하다 — "찾기 + 작은 ROI 디코드"는 각도에 비례해 늘어나는 비용이 아니다.
    // 실패했을 때의 추가 비용은 locate 몇 ms + 작은 크롭 디코드뿐이고,
    // 성공하면 뒤의 100~250ms짜리 단계들을 통째로 건너뛴다.
    // [[vscan-lite-region-first]]
    {
        std::vector<PipelineResult> roiHits = std::move(lowContrastPartial_);
        lowContrastPartial_.clear();
        if (regionFirstPass(roiHits)) { adaptiveObserve(roiHits); return roiHits; }
        if (roiHits.size() > hits.size()) hits = std::move(roiHits);
    }
    // 예산을 넘겼으면 남은 단계를 생략하고 지금까지 찾은 것을 돌려준다.
    // "부분 검출이라도 제때"가 "완벽하지만 늦음"보다 나은 배치를 위한 것 —
    // 기본값(maxFrameMs=0)에서는 이 검사가 전부 무효라 동작이 동일하다.
    if (budgetExceeded()) return hits;

    // [중간 단계] TryHarder만 켠 풀해상도 재시도 (Rotate/Invert는 여전히 끔).
    //
    // 폴백이 걸리는 대다수는 "작아서/흐려서/노이즈가 많아서" 빠른 패스가
    // 놓친 경우다 — 회전되거나 반전된 게 아니라. zxing의 TryHarder는
    // 이진화/디코드를 더 끈질기게 재시도하는 옵션인데, QR 등의 파인더
    // 패턴 자체가 회전 불변이라 "회전된 코드"에도 의외로 broad하게
    // 걸린다. 실측(악조건 36종): TryHarder만으로 34/36이 풀옵션(H+R+I)과
    // 동일한 결과를 **절반 이하 비용**으로 낸다(495ms vs 1153ms, 2.3배).
    // 놓치는 2종(반전 극성 코드, 부분 검출 케이스)은 아래 최종 폴백이
    // 마저 처리하므로 검출력 손실은 없다.
    // [[vscan-lite-harder-only-tier]]
    // [호출자가 끈 것은 여기서도 꺼야 한다]
    // 예전에는 tryHarder/tryInvert를 무조건 true로 덮어써서, 공개 API의
    // VSCAN_FLAG_NO_TRY_HARDER / NO_INVERT가 **2단계 경로에서 조용히
    // 무시됐다.** 헤더는 그 플래그가 동작한다고 문서화하고 있었으니
    // 문서가 거짓말을 하고 있었던 셈이다. 이 단계들은 각각 "TryHarder를
    // 쓰는 단계", "반전을 보는 단계"라 호출자가 그걸 껐으면 단계 자체를
    // 건너뛰는 것이 맞다. [[vscan-lite-two-stage-flags]]
    if (!cfg_.tryHarder) return hits;
    PipelineConfig harderCfg = cfg_;
    harderCfg.tileThreads = 1;
    harderCfg.tryHarder = true;
    harderCfg.tryRotate = false;
    harderCfg.tryInvert = false;
    Pipeline harder(harderCfg);
    std::vector<PipelineResult> hardHits;
    { Stage st("ts:harder"); hardHits = harder.processViewCore(view); }
    if ((int)hardHits.size() >= need) return hardHits;
    if (hardHits.size() > hits.size()) hits = std::move(hardHits);
    if (budgetExceeded()) return hits;

    // [3단계] TryHarder + TryInvert (여전히 TryRotate는 끔).
    //
    // 위 단계가 놓치는 건 대부분 반전 극성 코드다. 36종 전수 검증:
    // "TryHarder+Invert(무회전)"이 "완전 풀옵션(H+R+I)"과 검출 결과가
    // 36/36 완전히 동일하면서 시간은 절반(592ms vs 1208ms, 2배)이었다.
    // 즉 이 36종 범위에서는 TryRotate가 TryHarder+Invert로 이미 못 잡는
    // 것을 추가로 잡아준 적이 한 번도 없었다 — QR/DataMatrix 등 파인더
    // 패턴이 애초에 회전 불변이라 그런 것으로 보인다.
    // 그래도 TryRotate를 완전히 없애지 않고 다음 단계로 남겨둔 이유:
    // 36종이 모든 실제 조건을 대표하진 않는다(§3.4) — 이 라이브러리가
    // 놓쳐본 적 없는 심하게 기울어진 1D 코드 같은 게 실전에 있을 수
    // 있다. 그런 안전망 역할로 최종 폴백에 TryRotate를 남겨둔다.
    // [[vscan-lite-harder-invert-tier]]
    // 이 단계의 존재 이유가 "반전 극성 코드"다(바로 위 주석). 호출자가
    // 반전을 안 본다고 했으면 이 단계는 통째로 값이 없다 — 실측상 판정
    // 경로에서 가장 비싼 항목(7.5~9.8ms)이라 지연에도 그대로 걸린다.
    if (!cfg_.tryInvert) {
        if (cfg_.fastNoRead) { prof().dump(hits.empty() ? "2단계실패:빠른불판독" : "2단계부분"); return hits; }
        auto fullNi = processView(view);
        return fullNi.size() >= hits.size() ? fullNi : hits;
    }
    PipelineConfig hiCfg = cfg_;
    hiCfg.tileThreads = 1;
    hiCfg.tryHarder = true;
    hiCfg.tryRotate = false;
    hiCfg.tryInvert = true;
    Pipeline hi(hiCfg);
    std::vector<PipelineResult> hiHits;
    { Stage st("ts:invert"); hiHits = hi.processViewCore(view); }
    if ((int)hiHits.size() >= need) return hiHits;
    if (hiHits.size() > hits.size()) hits = std::move(hiHits);
    if (budgetExceeded()) return hits;

    // [최종 폴백] TryRotate까지 포함한 완전한 풀옵션. processView()가
    // 이제 내부적으로 1D 회전 구제(5단계, [[vscan-lite-1d-deskew-rescue]])
    // 까지 자체적으로 시도하므로 여기서 따로 또 부를 필요는 없다 —
    // 그래서 이 최종 호출 하나가 사실상 4~5단계를 전부 커버한다.
    // [[vscan-lite-two-stage-fallback]]
    // [빠른 불판독은 최종 승격도 안 한다]
    // 이 자리는 "1단계도 크롭도 다 실패했다"는 뜻이라, 남은 것은 프레임
    // 전체를 풀옵션으로 한 번 더 훑는 최후 수단이다. 재촬영이 가능하면
    // 그 값을 치를 이유가 없다. 실측(reps 3, QR 마스크 + fastNoRead):
    //   저조도  30 -> 30코드(동일), 판정 최대 27.3 -> 19.4ms
    //   혼합   257 -> 232코드,      판정 최대 121.6 -> 75.3ms
    // 저조도에서는 승격이 **한 코드도 못 벌면서** 7ms를 쓴다. 혼합에서
    // 잃는 25개는 이 손잡이의 전제대로 다음 촬영에서 회수한다.
    if (cfg_.fastNoRead) { prof().dump(hits.empty() ? "2단계실패:빠른불판독" : "2단계부분"); return hits; }
    auto full = processView(view);
    return full.size() >= hits.size() ? full : hits;
}


std::vector<PipelineResult> Pipeline::processViewROIs(const GrayView& image, const std::vector<Rect>& rects,
                                                       int padPx) {
    if (image.empty() || rects.empty()) return {};

    // 자체 locate 단계 없음 — 외부에서 준 rects를 그대로 신뢰한다.
    // 이 파이프라인에 설정된 옵션(formatMask/tryRotate/tryInvert/tryHarder)을
    // 그대로 쓴다 — 호출자가 이미 원하는 트레이드오프를 cfg_에 담아뒀다고
    // 보고, processViewTwoStage처럼 "무조건 풀옵션"으로 덮어쓰지 않는다.
    PipelineConfig regionCfg = cfg_;
    regionCfg.tileThreads = 1; // 작은 crop이라 추가 타일링 불필요
    // 작은 ROI 구제를 여기서만 켠다 — 호출자가 위치를 알려준 상황이라
    // "작아서 안 읽히는" 코드에 확대를 걸 근거가 분명하다. 자동 탐지
    // 경로는 그 근거가 없어서 기본 꺼짐이다.
    // [[vscan-lite-small-code-upscale]]
    if (regionCfg.smallRoiUpscale <= 1) regionCfg.smallRoiUpscale = 3;
    regionCfg.zbarAsRescue = false;      // ROI 디코드에는 ZBar를 붙인다

    return decodeRegionsParallel(image, rects, padPx, regionCfg);
}

std::vector<PipelineResult> Pipeline::locateAndDecode(const GrayView& view) {
    auto regions = findCodeRegions(view);
    if (regions.empty()) return {};
    std::vector<Rect> rects;
    rects.reserve(regions.size());
    for (const auto& r : regions) {
        const int w = r.bbox.x1 - r.bbox.x0, h = r.bbox.y1 - r.bbox.y0;
        if (w < 16 || h < 16) continue;
        const int pad = std::min(static_cast<int>(0.35f * std::max(w, h)), cfg_.regionRescueMaxPadPx);
        rects.push_back(Rect{std::max(0, r.bbox.x0 - pad), std::max(0, r.bbox.y0 - pad),
                             std::min(view.width, r.bbox.x1 + pad),
                             std::min(view.height, r.bbox.y1 + pad)});
    }
    if (rects.empty()) return {};
    PipelineConfig roiCfg = cfg_;
    roiCfg.tileThreads = 1;
    roiCfg.enableRegionRescue = false;   // 재귀 방지
    roiCfg.zbarAsRescue = false;
    roiCfg.enable1DDeskewRescue = false;
    roiCfg.enableDPMRescue = false;
    roiCfg.autoDenoise = 0;              // 이미 고친 뷰다
    return dedup(decodeRegionsParallel(view, rects, 0, roiCfg));
}

std::vector<PipelineResult> Pipeline::processViewTracked(const GrayView& image, int trackPadPx,
                                                          int fullScanInterval) {
    if (image.empty()) return {};

    bool forceFull = lastPositions_.empty() ||
                     (fullScanInterval > 0 && framesSinceFullScan_ >= fullScanInterval);

    if (!forceFull) {
        // [1단] 빠른 ROI: 직전 프레임에서 정상적으로 읽힌 코드를 다시 찾는
        // 것이므로 TryHarder/Rotate/Invert 없이 시도한다. 실측(640x640 ROI):
        // 풀옵션 5.2ms -> 1.2ms. 이진화는 LocalAverage 유지 — GlobalHistogram
        // 으로 더 줄이면(0.6ms) 저대비 코드 추적이 매 프레임 실패해 오히려
        // 풀스캔 폴백 연발로 느려진다(§3.2.2의 저대비 한계 때문).
        // [2단] 1단이 빈손이면 풀옵션 ROI: 반전/저대비 코드를 추적 중인
        // 경우 1단은 원리적으로 못 찾는데, 풀스캔(13ms+)까지 갈 것 없이
        // 같은 ROI를 풀옵션(+5.2ms)으로 다시 보면 잡힌다.
        // [3단] 그래도 빈손이면 풀스캔 (아래 processViewTwoStage).
        // [[vscan-lite-tracked-3stage]]
        PipelineConfig fastCfg = cfg_;
        fastCfg.tileThreads = 1;
        fastCfg.tryHarder = false;
        fastCfg.tryRotate = false;
        fastCfg.tryInvert = false;
        Pipeline fastRoi(fastCfg);
        const int need = std::max(1, cfg_.minExpectedCodes);
        auto roiResults = fastRoi.processViewROIs(image, lastPositions_, trackPadPx);
        if ((int)roiResults.size() < need) {
            roiResults = processViewROIs(image, lastPositions_, trackPadPx);
        }
        if ((int)roiResults.size() >= need) {
            // ROI 적중: 위치 갱신 후 반환. 주의 — 이 경로는 "새로 화면에
            // 진입한 코드"는 못 본다. 그래서 fullScanInterval 주기로
            // 풀스캔을 강제해 진입 코드를 잡는다(놓침 최대 지연 = interval).
            ++framesSinceFullScan_;
            lastPositions_.clear();
            for (auto& r : roiResults) {
                int minX = image.width, minY = image.height, maxX = 0, maxY = 0;
                for (auto& pt : r.symbol.position) {
                    minX = std::min(minX, pt.first);  maxX = std::max(maxX, pt.first);
                    minY = std::min(minY, pt.second); maxY = std::max(maxY, pt.second);
                }
                lastPositions_.push_back({minX, minY, maxX, maxY});
            }
            return roiResults;
        }
        // ROI가 빈손 — 코드가 ROI 밖으로 나갔거나 사라짐. 풀스캔으로.
    }

    auto full = processViewTwoStage(image, 50);
    framesSinceFullScan_ = 0;
    lastPositions_.clear();
    for (auto& r : full) {
        int minX = image.width, minY = image.height, maxX = 0, maxY = 0;
        for (auto& pt : r.symbol.position) {
            minX = std::min(minX, pt.first);  maxX = std::max(maxX, pt.first);
            minY = std::min(minY, pt.second); maxY = std::max(maxY, pt.second);
        }
        lastPositions_.push_back({minX, minY, maxX, maxY});
    }
    return full;
}

std::vector<PipelineResult> Pipeline::decodeRegionsParallel(const GrayView& image, const std::vector<Rect>& rects,
                                                             int padPx, const PipelineConfig& regionCfg) {
    // 여기서는 일부러 zero-copy GrayView::crop() 대신 촘촘한(packed, stride
    // == width) 버퍼로 복사해서 넘긴다 — crop이 원본의 작은 영역이면 stride가
    // 원본 폭(예: 2048) 그대로라 한 행 한 행이 실제 필요한 데이터보다 훨씬
    // 멀리 떨어져 있어(캐시 라인 낭비) 오히려 스캔이 느려진다. 실측(i.MX8MP,
    // 700x600px 안팎 crop): zero-copy stride 유지 시 237-245ms, 촘촘한 복사
    // 시 130-143ms — crop처럼 원본 대비 훨씬 작은 영역에서는 memcpy 비용보다
    // 캐시 지역성 이득이 훨씬 크다. [[vscan-lite-two-stage-decode]] 참고.
    struct CropRegion { int x0, y0; GrayImage packed; };
    std::vector<CropRegion> crops;
    crops.reserve(rects.size());
    for (auto& r : rects) {
        int x0 = r.x0 - padPx, y0 = r.y0 - padPx;
        int x1 = r.x1 + padPx, y1 = r.y1 + padPx;
        GrayView view = image.crop(x0, y0, x1, y1);
        if (view.empty()) continue;

        GrayImage packed;
        packed.width = view.width;
        packed.height = view.height;
        packed.pixels.resize(static_cast<size_t>(view.width) * view.height);
        for (int row = 0; row < view.height; ++row) {
            std::memcpy(&packed.pixels[static_cast<size_t>(row) * view.width],
                        view.pixels + static_cast<size_t>(row) * view.stride, view.width);
        }
        crops.push_back({std::max(0, x0), std::max(0, y0), std::move(packed)});
    }

    // 워커 모드(tileThreads==1)에서는 스레드를 띄우지 않는다.
    // 워커 3개가 각자 1코어를 쓰는 구조에서 ROI마다 스레드를 추가로 만들면
    // 3코어에 워커3 x ROI수 만큼의 스레드가 올라가 서로 경쟁한다(과다구독).
    // 이 경우 순차 처리가 더 빠르다.
    // [작은 ROI 구제] 원본 크기로 빈손이면 확대 + 언샤프로 한 번 더.
    // 모듈이 2px 안팎이면 인쇄/광학 흐림이 모듈 경계를 뭉개서 이진화가
    // 어느 쪽으로도 안 떨어진다. 확대만으로는 흐림도 같이 커질 뿐이라
    // 확대한 뒤 고주파를 되살려야 한다.
    //
    // 실측(실물 3.1MP 해상도 차트, QR 21x21모듈이 약 45px = 모듈 2.1px):
    // 좌표를 정확히 알려줘도 원본 크기로는 24곳 중 0곳, 3배 확대 +
    // 언샤프를 붙이면 6곳이 읽힌다.
    //
    // 여기(ROI 디코드 공통 경로)에 두는 이유: 외부에서 위치를 주는
    // vscan_process_gray_rois()와 추적 모드가 이 함수를 쓰는데, 그 경로는
    // "어디에 있는지는 안다, 작아서 안 읽힐 뿐"인 상황 그 자체다.
    // 자동 탐지 경로에서는 영역이 보통 타일 크기(128px+여백)를 넘어서
    // 이 분기에 잘 안 걸린다 — 밀집 소형 코드는 탐지 자체가 막히는데
    // 그건 별도 문제다(§3.20).
    //
    // 비용: ROI 디코드가 빈손일 때만, 그것도 작은 ROI에만 든다.
    // [[vscan-lite-small-code-upscale]]
    auto decodeCrop = [&](Pipeline& pipe, const GrayImage& packed) {
        auto hits = pipe.processViewCore(GrayView(packed));
        if (!hits.empty()) return hits;
        const int f0 = regionCfg.smallRoiUpscale;
        if (f0 < 2 || packed.width > regionCfg.smallRoiMaxPx || packed.height > regionCfg.smallRoiMaxPx)
            return hits;
        // [배율 단계화] 먼저 싼 배율로, 안 되면 한 번 더 크게.
        // 실측(실물 해상도 차트, 파인더가 찾은 11곳): 3배는 3곳,
        // 6배는 6곳이다. 배율을 올리면 흐릿한 모듈 경계가 더 많은 픽셀에
        // 걸쳐 표현돼서 언샤프가 되살릴 여지가 생긴다. 대신 픽셀 수가
        // 배율 제곱으로 늘어 6배는 3배의 4배 비용이라, 3배로 되는
        // 코드까지 6배를 물릴 이유는 없다.
        // 대비를 편 판본도 같이 시도한다. 확대·언샤프는 **경계의 기울기**를
        // 세우는 것이고 대비 스트레칭은 **흑백의 간격**을 벌리는 것이라,
        // 저대비 + 저해상도가 겹친 코드에서는 둘 다 필요할 수 있다.
        // 이미 계조를 다 쓰고 있으면 stretchContrast()가 false를 돌려주므로
        // 그때는 시도가 하나로 줄어 비용이 안 는다.
        GrayImage boosted;
        const bool hasBoost = stretchContrast(GrayView(packed), boosted);
        for (int f : {f0, f0 * 2}) {
            if (budgetExceeded()) break;
            for (int pass = 0; pass < (hasBoost ? 2 : 1); ++pass) {
                const GrayView srcView = pass == 0 ? GrayView(packed) : GrayView(boosted);
                GrayImage big;
                upscaleSharpen(srcView, f, big, regionCfg.smallRoiSharpen);
                if (big.pixels.empty()) continue;
                auto up = pipe.processViewCore(GrayView(big));
                if (up.empty()) continue;
                for (auto& r : up)
                    for (auto& pt : r.symbol.position) { pt.first /= f; pt.second /= f; }
                return up;
            }
        }
        return hits;
    };

    std::vector<std::vector<PipelineResult>> parts(crops.size());
    if (regionCfg.tileThreads == 1 || crops.size() <= 1) {
        Pipeline refiner(regionCfg);   // crop 전체가 같은 설정이라 한 번만 만든다
        for (size_t i = 0; i < crops.size(); ++i) {
            parts[i] = decodeCrop(refiner, crops[i].packed);
        }
    } else {
        std::vector<std::future<std::vector<PipelineResult>>> futures;
        futures.reserve(crops.size());
        for (auto& c : crops) {
            futures.push_back(std::async(std::launch::async, [regionCfg, &c, &decodeCrop]() {
                Pipeline refiner(regionCfg);
                return decodeCrop(refiner, c.packed);
            }));
        }
        for (size_t i = 0; i < futures.size(); ++i) parts[i] = futures[i].get();
    }

    std::vector<PipelineResult> merged;
    for (size_t i = 0; i < parts.size(); ++i) {
        int xOff = crops[i].x0, yOff = crops[i].y0;
        for (auto& r : parts[i]) {
            // crop 좌표 -> 원본 프레임 좌표로 보정
            for (auto& p : r.symbol.position) { p.first += xOff; p.second += yOff; }
        }
        merged.insert(merged.end(), parts[i].begin(), parts[i].end());
    }

    return dedup(std::move(merged));
}

namespace {
// 심볼 중심점 (bounding quad 4점 평균)
std::pair<double,double> centerOf(const DecodedSymbol& s) {
    double cx = 0, cy = 0;
    for (auto& p : s.position) { cx += p.first; cy += p.second; }
    return {cx / 4.0, cy / 4.0};
}

// 심볼의 축 정렬 bounding box
struct BBox { double x0, y0, x1, y1; };
BBox bboxOf(const DecodedSymbol& s) {
    BBox b{1e18, 1e18, -1e18, -1e18};
    for (auto& p : s.position) {
        b.x0 = std::min(b.x0, (double)p.first);  b.y0 = std::min(b.y0, (double)p.second);
        b.x1 = std::max(b.x1, (double)p.first);  b.y1 = std::max(b.y1, (double)p.second);
    }
    return b;
}
double areaOf(const BBox& b) { return std::max(0.0, b.x1 - b.x0) * std::max(0.0, b.y1 - b.y0); }

// 위치 정보가 쓸모없는(퇴화한) 결과인가.
//
// zxing이 같은 코드를 두 번 돌려주면서 한쪽 꼭짓점 두 개를 (0,0)으로
// 채워 보내는 경우가 있다 — 실측(PDF417 회전 10/35도):
//   [0] "VSCAN-SWEEP-01" (38,263) (988,263) (988,288) (38,310)   <- 정상
//   [1] "VSCAN-SWEEP-01" (36,264) (0,0)     (0,0)     (39,309)   <- 퇴화
// 부분 검출이 살아남은 흔적으로 보이는데, 이런 결과는 bbox가 엉뚱한
// 곳까지 늘어나 중심점도 겹침비도 정상 결과와 안 맞는다. 그래서
// 아래 dedup()의 기하 규칙을 전부 빠져나가 **같은 코드가 두 번**
// 반환됐다(단일 코드 프레임에서 중복 2건).
//
// 꼭짓점이 서로 겹친 시점에서 그 결과의 위치는 신뢰할 수 없다. 위치를
// 못 믿는 결과는 "다른 자리에 있는 다른 코드"임을 주장할 근거가 없으므로,
// 같은 심볼로지 + 같은 텍스트라면 중복으로 본다.
// [[vscan-lite-dedup-degenerate-quad]]
// 한쪽 텍스트가 다른 쪽에 통째로 들어있는가 (부분 스캔의 지문).
//
// 1D 바코드를 일부만 훑은 스캔은 유효해 보이는 **짧은 코드**가 된다.
// 체크디짓이 규격상 필수가 아닌 심볼로지(ITF 등)에서 특히 잘 나온다.
// 실측(ITF 20/70도): 정답 "12345670"과 같은 자리에서 "345670"이 같이
// 나왔다. minLineCount를 올려도 남는 잔여분이 정확히 이 모양이다.
//
// 물리적으로 서로 다른 두 코드가 **겹쳐 인쇄되면서** 한쪽 내용이 다른
// 쪽의 부분 문자열이기까지 할 확률은 없다고 봐도 된다. 그래서 아래
// dedup()에서 기하 겹침 조건과 **함께** 쓸 때만 안전하다 — 텍스트
// 관계만으로 지우지 않는다. [[vscan-lite-dedup-partial-scan]]
bool textContains(const std::string& outer, const std::string& inner) {
    return !inner.empty() && inner.size() < outer.size() &&
           outer.find(inner) != std::string::npos;
}

// 한쪽이 다른 쪽의 **얇은 스캔 조각**인가.
//
// 실측(DataBar Expanded, 노이즈 15): 같은 텍스트가 두 상자로 나오는데
//   (67,612)-(956,624)   두께  12
//   (67,726)-(956,872)   두께 146
// x 범위가 67~956으로 **완전히 같고** 하나는 두께 12px짜리다. 그건 라벨이
// 아니라 스캔 밴드 한 줄이다. 그런데 세로 간격이 102px라 밴드 규칙
// ([[vscan-lite-dedup-stacked-band]], 간격 < 합친 두께의 10%)을 빠져나간다.
//
// 두께 비로 가른다: 긴 축이 90% 이상 겹치면서 얇은 쪽 두께가 두꺼운 쪽의
// 35% 미만이면, 그건 같은 심볼을 스쳐 지나간 조각이다. 나란히 붙은 같은
// 라벨 2장은 두께가 비슷하므로 이 조건에 안 걸린다.
// [[vscan-lite-dedup-thin-slice]]
bool isThinSlice(const BBox& a, const BBox& b) {
    const double aw = a.x1 - a.x0, ah = a.y1 - a.y0;
    const double bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return false;

    auto sliver = [](double aLo, double aHi, double bLo, double bHi,   // 긴 축
                     double cLo, double cHi, double dLo, double dHi) { // 두께 축
        const double ov = std::min(aHi, bHi) - std::max(aLo, bLo);
        if (ov < 0.90 * std::min(aHi - aLo, bHi - bLo)) return false;
        const double t1 = cHi - cLo, t2 = dHi - dLo;
        return std::min(t1, t2) < 0.35 * std::max(t1, t2);
    };
    return sliver(a.x0, a.x1, b.x0, b.x1, a.y0, a.y1, b.y0, b.y1) ||
           sliver(a.y0, a.y1, b.y0, b.y1, a.x0, a.x1, b.x0, b.x1);
}

// 같은 코드를 **두 줄로 스쳐 지나간** 스캔 밴드 한 쌍인가.
//
// isThinSlice()는 "한쪽만 얇을 때"를 잡는다. 그런데 둘 다 얇게 나오는
// 경우가 있다 — 실측(코퍼스 씨드 101, 90도 회전 EAN13, 모션 있음):
//   "2711660915686" (593,398)-(597,883)  두께 4
//   "2711660915686" (340,407)-(343,892)  두께 3
// 정답 상자는 x 334~604이므로 둘 다 코드 **안**이다. 두께비가 1에 가까워
// isThinSlice에 안 걸리고, 간격 250px이 결합 폭 257의 97%라 밴드 규칙도
// 못 넘는다.
//
// 판정: 둘 다 "두께가 길이의 3% 이하"인 실오라기이고, 긴 축이 90% 이상
// 겹치고, 간격이 긴 축 길이를 넘지 않으면 같은 심볼의 두 스캔이다.
// 마지막 조건이 물리적 상한이다 — 한 코드 안의 두 스캔선은 코드 높이보다
// 멀 수 없고, 1D 코드는 대개 길이가 높이보다 크다.
//
// 같은 텍스트일 때만 쓴다. 내용까지 같은 별개 라벨 2장이 이 조건에 걸릴
// 수는 있는데, 그건 isStackedBand가 이미 안고 있는 것과 같은 대가다.
// [[vscan-lite-dedup-twin-band]]
bool isTwinBand(const BBox& a, const BBox& b) {
    auto twin = [](double aLo, double aHi, double bLo, double bHi,   // 긴 축
                   double cLo, double cHi, double dLo, double dHi) { // 두께 축
        const double la = aHi - aLo, lb = bHi - bLo;
        const double ta = cHi - cLo, tb = dHi - dLo;
        if (la <= 0 || lb <= 0) return false;
        if (ta > 0.03 * la || tb > 0.03 * lb) return false;          // 실오라기가 아니다
        const double ov = std::min(aHi, bHi) - std::max(aLo, bLo);
        if (ov < 0.90 * std::min(la, lb)) return false;              // 긴 축이 안 맞는다
        const double gap = std::max(cLo, dLo) - std::min(cHi, dHi);
        return gap <= std::min(la, lb);
    };
    return twin(a.x0, a.x1, b.x0, b.x1, a.y0, a.y1, b.y0, b.y1) ||
           twin(a.y0, a.y1, b.y0, b.y1, a.x0, a.x1, b.x0, b.x1);
}

bool hasDegenerateQuad(const DecodedSymbol& s) {
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (s.position[i] == s.position[j]) return true;
    return false;
}

// 교집합 넓이 / 더 작은 쪽 넓이.
// IoU가 아니라 "작은 쪽 기준"인 이유는 아래 dedup() 주석 참고.
double containRatio(const BBox& a, const BBox& b) {
    double ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    if (ix <= 0 || iy <= 0) return 0.0;
    double small = std::min(areaOf(a), areaOf(b));
    return small > 0 ? (ix * iy) / small : 0.0;
}

// 한 심볼을 여러 밴드로 나눠 읽어 같은 결과가 여러 번 나온 것인가.
//
// 실측(DataBar Expanded 10도): 같은 텍스트가 세 상자로 나온다.
//   A (47,633)-(929,729)   두께  96
//   B (74,719)-(933,842)   두께 123   <- A와 10px 겹침
//   C (91,853)-(976,890)   두께  37   <- B와 11px 간격
// 가로 범위는 셋 다 97~100% 정렬돼 있고 세로로만 층이 진다. 한 심볼을
// 스캔 밴드별로 따로 읽은 것인데, A-B는 겹침비가 0.10이고 중심점 거리가
// 101px라 기존 두 규칙(겹침비 0.5 / 중심 64px)을 둘 다 빠져나간다.
//
// 판정: (1) 긴 축으로 70% 이상 정렬되고, (2) 짧은 축의 간격이 두 상자를
// 합친 두께의 10% 미만(겹치면 당연히 통과). 간격 기준을 "얇은 쪽 두께"가
// 아니라 "합친 두께"로 잡는 이유는 C처럼 한 조각이 아주 얇게 나올 수
// 있어서다 — 얇은 쪽 기준이면 11px 간격이 37px의 30%가 되어 놓친다.
//
// 이 규칙의 대가는 명확히 알고 있다: **박스에 같은 라벨 2장이 세로로
// 바짝 붙어 있으면** 하나로 합쳐진다(간격이 라벨 높이의 약 25% 미만일 때).
// dedup()이 원래 지키려던 케이스라 정렬 조건 70%로 좁혔지만 완전히
// 배제하지는 못한다. 같은 내용 라벨이 여러 장 붙는 배치라면 이 규칙을
// 빼는 편이 맞다. [[vscan-lite-dedup-stacked-band]]
bool isStackedBand(const BBox& a, const BBox& b) {
    const double aw = a.x1 - a.x0, ah = a.y1 - a.y0;
    const double bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return false;

    auto stacked = [](double aLo, double aHi, double bLo, double bHi,   // 층이 지는 축
                      double cLo, double cHi, double dLo, double dHi) { // 정렬돼야 하는 축
        const double gap = std::max(aLo, bLo) - std::min(aHi, bHi);
        const double combined = std::max(aHi, bHi) - std::min(aLo, bLo);
        const double ov = std::min(cHi, dHi) - std::max(cLo, dLo);
        const double minAlign = std::min(cHi - cLo, dHi - dLo);
        if (minAlign <= 0) return false;
        // 간격이 아주 작으면(결합 길이의 10% 미만) 정렬 조건을 느슨하게,
        // 간격이 그보다 벌어지면 정렬을 더 엄격히 요구한다.
        //
        // 두 번째 조건을 넣은 이유: 원근 보정을 붙이고 나서 같은 코드의
        // 서로 다른 스캔 밴드가 둘 다 반환되는 경우가 생겼다 — 실측
        // (UPCE persp 0.45): (80,725)-(831,817)과 (46,848)-(772,900).
        // 간격 31px이 결합 높이 175의 18%라 10% 문턱을 못 넘었다.
        // 그냥 문턱만 넓히면 "박스에 붙은 같은 라벨 2장"을 깨뜨리므로,
        // 넓힌 구간에서는 긴 축 정렬을 0.70 -> 0.90으로 올려서 받는다.
        if (gap < 0.10 * combined) return ov >= 0.70 * minAlign;
        if (gap < 0.25 * combined) return ov >= 0.90 * minAlign;
        return false;
    };
    return stacked(a.y0, a.y1, b.y0, b.y1, a.x0, a.x1, b.x0, b.x1) ||
           stacked(a.x0, a.x1, b.x0, b.x1, a.y0, a.y1, b.y0, b.y1);
}

// 부분 스캔 판정 전용 기하 조건: 두 결과가 **같은 바코드 위**인가.
//
// 1D는 zxing이 스캔 라인 하나를 따라 얇은 사각형을 준다 — 실측(ITF 20도):
//   "345670"   (205,718)-(950,714)   높이 4px
//   "12345670" (61,770)-(968,765)    높이 5px
// 같은 바코드의 서로 다른 두 행이라 y가 아예 안 겹치고(714~718 vs
// 765~770), 중심점 거리도 81px로 기존 64px 규칙을 아슬아슬하게 넘는다.
// 즉 2D용 겹침비도, 고정 픽셀 거리도 이 경우엔 원리적으로 안 맞는다.
//
// 그래서 두 상자를 **긴 쪽 크기에 비례하는 여유**만큼 부풀린 뒤 겹침을
// 본다. 비례로 하는 이유는 위 예처럼 바코드가 길수록 스캔 행 간격도
// 그만큼 벌어질 수 있어서다(고정 상수는 코드 크기가 바뀌면 깨진다).
//
// 여유는 50%다. 처음에는 10%였는데 그건 **평평한 코드**의 스캔 행 간격만
// 보고 정한 값이었다. 곡면/회전이 섞이면 같은 코드의 밴드가 훨씬 멀리
// 흩어진다 — 실측(난수 코퍼스 1000장, 씨드 7에서 나온 오디코딩 9건):
//   ITF 곡면: "52934743"(507,951)-(832,958) / 정답(335,1113)-(912,1119)
//             / "43425562"(421,1229)-(790,1232)   세로 간격 110~155px,
//             코드 길이 577px의 10%(58px)로는 원리적으로 못 닿는다.
//   ITF 회전: 정답(496,384)-(633,1079) / "043028"(680,531)-(685,883)
//             부분 밴드가 정답 상자의 **바깥**에 있다(x 680 > 633).
// 50%면 이 셋이 다 걸리고, 그러면서 텍스트 조건(한쪽이 다른 쪽의 부분
// 문자열)이 함께 걸려 있어야 하므로 서로 다른 코드를 지울 위험은 남지
// 않는다 — 같은 자리에서 한쪽 내용이 다른 쪽의 부분 문자열인 별개 코드는
// 실질적으로 없다.
// [[vscan-lite-dedup-partial-scan]]
bool onSameBarcodeBand(const BBox& a, const BBox& b) {
    const double span = std::max({a.x1 - a.x0, a.y1 - a.y0, b.x1 - b.x0, b.y1 - b.y0});
    const double m = 0.50 * span;
    const BBox ia{a.x0 - m, a.y0 - m, a.x1 + m, a.y1 + m};
    const BBox ib{b.x0 - m, b.y0 - m, b.x1 + m, b.y1 + m};
    return containRatio(ia, ib) >= 0.5;
}
}

namespace {
std::vector<PipelineResult> dedupOnce(std::vector<PipelineResult> in) {
    // 겹치는 타일 경계에서 같은 코드가 두 번 검출될 수 있다.
    // 같은 심볼로지 + 같은 텍스트 + (겹침이 크거나 중심점이 가까우면) 하나만 남긴다.
    //
    // [왜 중심점 거리만으로는 안 되는가 — 실측]
    // 예전에는 "중심점 64px 이내"만 봤는데, **단일 코드 프레임에서도 15.5%가
    // 중복 반환**됐다(대량 코퍼스, §3.9). 실제 좌표를 찍어보면 원인이 분명하다:
    //
    //   [0] "9IKE" (1410,640)-(1883,841)      <- 온전히 보인 타일
    //   [1] "9IKE" (1410,768)-(1883,841)      <- 다음 타일에서 위가 잘린 같은 코드
    //
    // x범위는 완전히 같고 y범위만 타일 경계(768)에서 잘린다. 잘린 쪽은 세로
    // 중심이 밀려서 중심점 거리가 64px를 넘어버린다(위 예는 정확히 64.0,
    // 다른 예는 145px). 코드가 클수록 더 많이 밀리므로 고정 상수로는 원리적
    // 으로 못 잡는다.
    //
    // [왜 IoU가 아니라 "작은 쪽 기준 겹침"인가]
    // 잘린 조각은 원본 대비 면적이 작아서 IoU가 오히려 낮게 나온다
    // (위 예 중 하나는 IoU 0.06). 반면 "교집합 / 작은 쪽 면적"은 1.0이다 —
    // 한쪽이 다른 쪽에 포함되는 관계를 정확히 잡아낸다.
    //
    // 나란히 붙은 서로 다른 같은 내용 라벨(박스에 같은 라벨 2장)은 bbox가
    // 겹치지 않으므로 병합되지 않는다.
    //
    // 남기는 쪽은 **면적이 큰 것**이다 — 잘린 조각보다 온전한 쪽의 꼭짓점
    // 좌표가 정확하다.
    // [[vscan-lite-dedup-clipped-tile]]
    constexpr double kOverlapDup = 0.5;   // 작은 쪽의 절반 이상이 겹치면 같은 코드
    constexpr double kCenterDup  = 64.0;  // 기존 규칙도 유지(작은 코드/회전 케이스)

    std::vector<PipelineResult> out;
    for (auto& cand : in) {
        auto candCenter = centerOf(cand.symbol);
        auto candBox = bboxOf(cand.symbol);
        bool isDup = false;
        for (auto& kept : out) {
            if (kept.symbol.symbology != cand.symbol.symbology) continue;

            /*
             * [같은 자리, 다른 내용 = 한쪽은 오독]
             *
             * 실측(UPC-E, 모듈 5px):
             *   "01234565" (257,670)-(766,864)   <- 정답
             *   "01244564" (252,675)-(756,855)   <- 오독, 체크디짓까지 통과
             * 두 상자가 거의 완전히 겹친다. 같은 자리에 코드 두 개가 인쇄될
             * 수는 없으므로 둘 중 하나는 틀렸다 — 둘 다 돌려주면 호출자에게
             * 모순을 넘기는 셈이고, 그건 하나만 주는 것보다 나쁘다.
             *
             * 남기는 쪽은 **상자가 큰 쪽**이다. 심볼을 더 온전히 본 결과가
             * 맞을 가능성이 높다는 기존 규칙과 같은 근거이고, 위 실측에서도
             * 정답 쪽이 509x194로 오독(504x180)보다 크다.
             *
             * 겹침 임계 0.9는 일부러 높다. 0.5(중복 판정용)로 잡으면
             * 나란히 붙은 서로 다른 코드가 조금만 겹쳐도 하나가 사라진다.
             * [[vscan-lite-dedup-conflicting-overlap]]
             */
            if (kept.symbol.text != cand.symbol.text) {
                const auto cb = bboxOf(cand.symbol), kb = bboxOf(kept.symbol);
                if (!hasDegenerateQuad(cand.symbol) && !hasDegenerateQuad(kept.symbol) &&
                    containRatio(cb, kb) >= 0.9) {
                    isDup = true;
                    if (areaOf(cb) > areaOf(kb)) kept = std::move(cand);
                    break;
                }
            }
            // 같은 텍스트이거나, 한쪽이 다른 쪽의 부분 문자열이거나.
            // 후자는 1D 부분 스캔 — 아래 기하 조건까지 만족할 때만 지운다.
            const auto keptBox = bboxOf(kept.symbol);
            const bool sameText = kept.symbol.text == cand.symbol.text;
            const bool candIsPart = textContains(kept.symbol.text, cand.symbol.text);
            const bool keptIsPart = textContains(cand.symbol.text, kept.symbol.text);

            /*
             * [내용이 아예 다른 얇은 조각 = 어긋난 스캔]
             *
             * 부분 스캔이 항상 부분 **문자열**로 나오지는 않는다. ITF는
             * 숫자를 두 개씩 엮어 넣으므로 시작 위치가 한 요소만 밀려도
             * 전혀 다른 숫자열이 나온다. EAN/UPC도 마찬가지다.
             * 실측(난수 코퍼스 씨드 7):
             *   "857212726494" (204,159)-(405,883) 201x724   <- 정답
             *   "920234"       (724,526)-(729,919)   5x393   <- 두께 5px
             *   "4936922291805"(1523,1004)-(1704,1387) 181x383
             *   "19369224"     (1489,1152)-(1504,1333) 15x181
             * 둘 다 문자열 관계가 전혀 없어서 부분 스캔 규칙을 못 탄다.
             *
             * 그런데 **두께 5px / 15px짜리 코드는 물리적으로 없다.** 긴 축이
             * 90% 이상 정렬되고 두께가 상대의 35% 미만이면 그건 라벨이
             * 아니라 심볼을 스쳐 지나간 스캔 한 줄이다(isThinSlice의 기준
             * 그대로). 같은 심볼로지 + 같은 자리(부풀린 겹침)까지 겹치면
             * 어긋난 스캔으로 보고 얇은 쪽을 버린다.
             *
             * 텍스트가 같을 때는 이미 아래에서 잡고 있었다. 여기서 넓히는
             * 것은 "내용까지 다른" 경우인데, 그건 오히려 더 위험한 쪽이다 —
             * 남겨두면 호출자가 없는 품번을 받는다.
             */
            if (!sameText && !candIsPart && !keptIsPart) {
                if (isThinSlice(candBox, keptBox) && onSameBarcodeBand(candBox, keptBox)) {
                    isDup = true;
                    if (areaOf(candBox) > areaOf(keptBox)) kept = std::move(cand);
                    break;
                }
                continue;
            }

            auto keptCenter = centerOf(kept.symbol);
            double dx = candCenter.first - keptCenter.first;
            double dy = candCenter.second - keptCenter.second;
            bool near = std::sqrt(dx * dx + dy * dy) < kCenterDup;

            const bool partial = candIsPart || keptIsPart;
            if (near || containRatio(candBox, keptBox) >= kOverlapDup ||
                hasDegenerateQuad(cand.symbol) || hasDegenerateQuad(kept.symbol) ||
                (partial && onSameBarcodeBand(candBox, keptBox)) ||
                (sameText && isStackedBand(candBox, keptBox)) ||
                (sameText && isThinSlice(candBox, keptBox)) ||
                (sameText && isTwinBand(candBox, keptBox))) {
                isDup = true;
                // 부분 스캔 관계면 **긴 쪽**을 남긴다(짧은 쪽이 잘린
                // 결과다). 같은 텍스트면 기존대로 bbox가 넓은 쪽 —
                // 타일 경계에서 잘린 조각보다 온전한 쪽의 꼭짓점이 정확하다.
                const bool takeCand = partial ? keptIsPart : (areaOf(candBox) > areaOf(keptBox));
                if (takeCand) kept = std::move(cand);
                break;
            }
        }
        if (!isDup) out.push_back(std::move(cand));
    }
    return out;
}
}  // namespace

/*
 * [고정점까지 반복한다]
 * 한 번만 돌면 **입력 순서에 결과가 달린다**. 실측(난수 코퍼스 씨드 7,
 * 회전 ITF): 입력이
 *   '04302847' / '043028' / '30284797' / '0430284797'
 * 순으로 들어오면, 먼저 자리를 잡은 '04302847'이 '043028'을 흡수하지만
 * '30284797'은 그 텍스트의 부분 문자열이 아니라(길이가 같다) 살아남는다.
 * 그 뒤 '0430284797'이 들어와 앵커를 자기로 바꾸는데, 이미 통과한
 * '30284797'은 다시 검사되지 않는다 — '0430284797'의 부분 문자열인데도.
 *
 * 한 번 더 돌리면 그때는 앵커가 긴 쪽이라 잡힌다. out이 몇 개짜리 벡터라
 * 반복 비용은 무시할 수 있다. 4회는 안전장치일 뿐 실제로는 2회에서 멈춘다.
 */
std::vector<PipelineResult> Pipeline::dedup(std::vector<PipelineResult> in) {
    for (int iter = 0; iter < 4; ++iter) {
        const size_t before = in.size();
        in = dedupOnce(std::move(in));
        if (in.size() == before) break;
    }

    /*
     * [서로 어긋나는 쌍둥이 밴드는 **둘 다** 버린다]
     *
     * 같은 심볼을 두 줄로 스쳐 지나간 밴드 한 쌍인데(isTwinBand) 내용이
     * 다르고 한쪽이 다른 쪽의 부분 문자열도 아니면, 둘 다 어긋난 시작
     * 위치에서 읽힌 것이다 — 적어도 하나는 틀렸고 어느 쪽인지 알 방법이
     * 없다. 실측(난수 코퍼스, 곡면 ITF): 정답 "52934743425562"인데
     *   "52934743"   (507,952)-(833,960)  두께 8
     *   "4743425562" (400,1199)-(826,1202) 두께 3
     * 이 나왔다. 둘 다 정답의 부분 문자열이지만 서로는 포함 관계가 아니다.
     *
     * 하나를 골라 돌려주면 **없는 품번을 만들어내는 것**이고, 그건 이
     * 프로젝트가 일관되게 미검출보다 나쁘다고 본 것이다(§3.18). 그래서
     * 둘 다 버린다 — 이 프레임은 미검출로 끝난다.
     * [[vscan-lite-dedup-conflicting-twins]]
     */
    std::vector<char> drop(in.size(), 0);
    for (size_t i = 0; i < in.size(); ++i) {
        if (drop[i]) continue;
        for (size_t j = i + 1; j < in.size(); ++j) {
            if (drop[j]) continue;
            if (in[i].symbol.symbology != in[j].symbol.symbology) continue;
            const std::string& a = in[i].symbol.text;
            const std::string& b = in[j].symbol.text;
            if (a == b || textContains(a, b) || textContains(b, a)) continue;
            const auto ba = bboxOf(in[i].symbol), bb = bboxOf(in[j].symbol);
            if (!isTwinBand(ba, bb)) continue;
            drop[i] = drop[j] = 1;
        }
    }
    /*
     * [증명할 것이 없는 심볼로지는 겹치면 진다]
     *
     * Pharmacode는 시작/정지 패턴도 체크디짓도 없다 — "가는/굵은 막대의
     * 나열"이 곧 값이라 어떤 막대열도 유효한 값으로 읽힌다. 그래서 다른
     * 코드의 막대열 위에서 유령이 뜬다. 실측(코드 693개 500장 난수 코퍼스,
     * Pharmacode는 한 장도 없음): 원근+글레어로 깨진 **ITF** 위에서
     * Pharmacode 95가 나왔다.
     *
     * 디코더 안에서는 못 막는다. LinearDecoder는 자기가 낸 2of5만 볼 수
     * 있고 zxing이 낸 ITF는 못 본다. 모든 디코더의 결과가 처음 만나는
     * 자리가 여기라서 여기서 건다 — **같은 자리에서 다른 심볼로지가
     * 나왔으면 Pharmacode를 버린다.** 반대 방향(진짜 Pharmacode 위에
     * 다른 코드가 뜨는 것)은 그 코드들이 시작/정지 패턴으로 자기를
     * 증명하므로 대칭이 아니다.
     * [[vscan-lite-pharmacode-shadowed]]
     */
    for (size_t i = 0; i < in.size(); ++i) {
        if (drop[i] || in[i].symbol.symbology != Symbology::PHARMACODE) continue;
        const auto bi = bboxOf(in[i].symbol);
        for (size_t j = 0; j < in.size(); ++j) {
            if (i == j || drop[j] || in[j].symbol.symbology == Symbology::PHARMACODE) continue;
            const auto bj = bboxOf(in[j].symbol);
            const bool apart = bi.x0 > bj.x1 || bj.x0 > bi.x1 || bi.y0 > bj.y1 || bj.y0 > bi.y1;
            if (!apart) { drop[i] = 1; break; }
        }
    }

    /*
     * [4-state 우편 심볼 안에서 나온 다른 코드는 그 막대들을 잘못 읽은 것이다]
     *
     * 우편 바코드는 굵기가 다 같은 막대가 65~67개 늘어선 것이라, 1D 리더가
     * 그 위에서 유효한 코드를 만들어내기 쉽다. 실측: IMB를 270도로 세운
     * 프레임에서 zxing이 EAN-13 "6211122121212"를 냈고 **체크디짓까지
     * 우연히 맞았다** — 체크디짓으로는 못 거른다.
     *
     * 우편 쪽은 CRC-11(IMB) 또는 mod 19 검사 심볼(일본우편)을 통과한
     * 것이라 증명 강도가 비교가 안 된다. 그래서 우편 심볼의 사각형에
     * **완전히 들어가는** 다른 심볼은 버린다. "겹치면"이 아니라 "들어가면"인
     * 이유는, 회전 프레임에서 사각형이 스치듯 겹치는 진짜 코드를 안 죽이려는
     * 것이다. 우편이 꺼져 있으면(기본) 이 규칙은 걸릴 상대가 없다 —
     * 기본 설정에서 저 EAN 유령이 남는 것은 §8에 알려진 이슈로 적었다.
     * [[vscan-lite-postal]]
     */
    for (size_t j = 0; j < in.size(); ++j) {
        if (drop[j]) continue;
        const Symbology sj = in[j].symbol.symbology;
        if (sj != Symbology::POSTAL_JAPAN && sj != Symbology::POSTAL_IMB) continue;
        const auto bj = bboxOf(in[j].symbol);
        for (size_t i = 0; i < in.size(); ++i) {
            if (i == j || drop[i]) continue;
            const Symbology si = in[i].symbol.symbology;
            if (si == Symbology::POSTAL_JAPAN || si == Symbology::POSTAL_IMB) continue;
            const auto bi = bboxOf(in[i].symbol);
            if (bi.x0 >= bj.x0 && bi.x1 <= bj.x1 && bi.y0 >= bj.y0 && bi.y1 <= bj.y1)
                drop[i] = 1;
        }
    }

    /*
     * [GS1 Composite의 CC-C는 zxing이 깨진 글자로 읽는다 — 그건 버린다]
     *
     * CC-C는 PDF417 심볼이라 zxing이 기본 경로에서 읽어버리는데, 데이터 계층이
     * ISO 24723이라 PDF417 압축해제로는 못 푼다. 결과는 valid=1인 쓰레기
     * 바이트열이다(실측: `[PDF417] l\x08!\x8a9%...`). 기본 설정에서 나오는
     * 오디코딩이라 opt-in과 무관하게 막아야 한다.
     *
     * **내용만으로는 못 가른다.** 실측으로 확인했다 — 정상 PDF417 페이로드
     * 19장이 19장 모두 ISO 24723 스트림으로 "성공" 파싱된다. 그래서
     * 재해석(다시 라벨 붙이기)은 하지 않고, 세 조건이 다 맞을 때만 **버린다**:
     *   1) 내용이 대부분 인쇄 불가 바이트다 (정상 PDF417 텍스트는 여기서 걸린다)
     *   2) ISO 24723 스트림으로 파싱된다
     *   3) 같은 프레임에 GS1 선형 심볼이 있다 (Composite의 정의가 그것이다)
     * 버리면 미검출이 되는데, 그게 없는 문자열을 만들어내는 것보다 낫다(§3.18).
     *
     * 제대로 읽으려면 PDF417 코드워드를 직접 뽑아 첫 코드워드가 920인지
     * 봐야 한다 — zxing은 코드워드를 밖으로 안 준다. §4의 남은 항목.
     * [[vscan-lite-gs1-composite]]
     */
    bool hasGs1Linear = false;
    for (size_t i = 0; i < in.size(); ++i) {
        if (drop[i]) continue;
        const Symbology sy = in[i].symbol.symbology;
        if (sy == Symbology::GS1_128 || sy == Symbology::GS1_DATABAR ||
            sy == Symbology::EAN_UPC || sy == Symbology::CODE128)
            hasGs1Linear = true;
    }
    if (hasGs1Linear) {
        for (size_t i = 0; i < in.size(); ++i) {
            if (drop[i] || in[i].symbol.symbology != Symbology::PDF417) continue;
            // **text가 아니라 rawBytes를 본다** — zxing이 text를 UTF-8로 인코딩해서
            // 0x80 이상 바이트가 두 배로 늘어나 있다(실측으로 여기서 한 번 틀렸다).
            const std::vector<uint8_t>& t = in[i].symbol.rawBytes;
            if (t.size() < 8) continue;
            size_t odd = 0;
            for (uint8_t ch : t)
                if (ch < 0x20 || ch >= 0x7f) odd++;
            if (odd * 2 < t.size()) continue;          // 절반 미만이면 평범한 텍스트다
            std::vector<uint8_t> bits;
            bits.reserve(t.size() * 8);
            for (uint8_t ch : t)
                for (int k = 7; k >= 0; --k) bits.push_back(static_cast<uint8_t>((ch >> k) & 1));
            std::string dummy;
            if (decodeGs1CompositeBits(bits, dummy)) drop[i] = 1;
        }
    }

    /*
     * [CC-C를 우리가 제대로 읽었으면, 같은 자리의 PDF417은 그것의 깨진 판이다]
     *
     * 위의 내용 기반 규칙은 "인쇄 불가 바이트가 절반 넘게"를 요구하는데
     * 실측으로 그게 자주 모자란다. CC-C 시험셋 7장 중 4장에서 zxing이 낸
     * 쓰레기가 절반을 못 넘겨서 그대로 살아남았다:
     *   `t\x08!\x8a9%!\x08B\x10` -> 인쇄 불가 4 / 전체 10
     * 임계를 낮추면 정상 PDF417까지 물리므로 그 방향은 막다른 길이다.
     *
     * 여기서는 내용을 안 본다. **같은 자리에서 GS1_COMPOSITE가 이미 나왔다면**
     * 그 PDF417은 같은 물리 심볼을 잘못 푼 것이다 — CC-C의 2D 성분이 곧
     * PDF417 심볼이기 때문이다. 우리 CC-C 경로는 코드워드[1]==920을 확인하고
     * 내므로(§3.50) 근거가 내용 추측이 아니라 구조다. 위 규칙은 우리가 CC-C를
     * 못 읽은 경우를 위해 남겨둔다(그때는 겹칠 상대가 없다).
     * [[vscan-lite-gs1-composite]]
     */
    for (size_t i = 0; i < in.size(); ++i) {
        if (drop[i] || in[i].symbol.symbology != Symbology::PDF417) continue;
        const auto bi = bboxOf(in[i].symbol);
        for (size_t j = 0; j < in.size(); ++j) {
            if (drop[j] || in[j].symbol.symbology != Symbology::GS1_COMPOSITE) continue;
            const auto bj = bboxOf(in[j].symbol);
            const bool apart = bi.x0 > bj.x1 || bj.x0 > bi.x1 || bi.y0 > bj.y1 || bj.y0 > bi.y1;
            if (!apart) { drop[i] = 1; break; }
        }
    }

    std::vector<PipelineResult> out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        if (!drop[i]) out.push_back(std::move(in[i]));
    return out;
}

} // namespace vscan
