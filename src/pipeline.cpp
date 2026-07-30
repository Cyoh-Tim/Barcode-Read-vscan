#include "vscan_internal/pipeline.hpp"
#include "vscan_internal/decoder_zxing.hpp"
#include "vscan_internal/preprocess.hpp"
#include "vscan_internal/deskew1d.hpp"

#include <algorithm>
#include <cmath>
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
                                                         cfg_.downscaleThreshold));
}

Pipeline::BudgetGuard::BudgetGuard(Pipeline* pp) : p(pp), owner(false) {
    if (p->cfg_.maxFrameMs > 0 && !p->deadlineActive_) {
        p->deadline_ = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(p->cfg_.maxFrameMs);
        p->deadlineActive_ = true;
        owner = true;
    }
}
Pipeline::BudgetGuard::~BudgetGuard() { if (owner) p->deadlineActive_ = false; }

bool Pipeline::budgetExceeded() const {
    return deadlineActive_ && std::chrono::steady_clock::now() >= deadline_;
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
    // 디코더가 여러 개(예: zxing-cpp + ZBar fast-path) 등록된 경우, 서로
    // 독립적인 읽기 전용 연산이므로 순차 실행 대신 동시에 돌린다. 디코더가
    // 하나뿐이면(기본 구성) std::async 오버헤드 없이 바로 호출한다.
    std::vector<std::vector<DecodedSymbol>> perDecoder;
    if (decoders_.size() <= 1) {
        perDecoder.reserve(decoders_.size());
        for (auto& decoder : decoders_) perDecoder.push_back(decoder->decode(tile));
    } else {
        std::vector<std::future<std::vector<DecodedSymbol>>> futures;
        futures.reserve(decoders_.size());
        for (auto& decoder : decoders_) {
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

std::vector<PipelineResult> Pipeline::processViewCore(const GrayView& image) {
    if (image.empty()) return {};

    unsigned nThreads = cfg_.tileThreads ? cfg_.tileThreads
                                          : std::max(1u, std::thread::hardware_concurrency());
    // 이미지가 작으면 타일링 오버헤드가 더 크므로 스레드 1개(=단일 풀프레임)로 처리
    if (image.height < 200) nThreads = 1;

    if (nThreads <= 1) {
        return dedup(decodeTile(image, 0));
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

    std::vector<PipelineResult> merged;
    for (auto& f : futures) {
        auto part = f.get();
        merged.insert(merged.end(), part.begin(), part.end());
    }

    if (merged.empty()) {
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
        // [[vscan-lite-tile-large-code]]
        return dedup(decodeTile(image, 0));
    }

    return dedup(std::move(merged));
}

std::vector<PipelineResult> Pipeline::processView(const GrayView& image) {
    BudgetGuard budget(this);
    // 코드가 물리적으로 존재할 수 없는 프레임(컨베이어 아이템 사이 등)은
    // 폴백 체인 전체를 건너뛴다. [[vscan-lite-blank-frame-skip]]
    if (!frameHasStructure(image)) return {};
    auto hits = processViewCore(image);
    if (!hits.empty()) { adaptiveObserve(hits); return hits; }

    // 좁힌 마스크로 빈손이면 새 심볼로지일 수 있다 — 전체 마스크로 되돌려
    // 이 프레임을 다시 본다. 되돌린 뒤에도 못 찾으면 아래 구제로 내려간다.
    if (adaptiveNarrowed_) {
        adaptiveWiden();
        hits = processViewCore(image);
        if (!hits.empty()) { adaptiveObserve(hits); return hits; }
    }

    // 여기부터는 구제 단계(DPM/1D 회전)다 — 실패 프레임에서만 도는,
    // 가장 비싼 구간이다. 예산을 넘겼으면 여기서 끊는다.
    if (budgetExceeded()) return hits;

    // [노이즈 구제] 노이즈가 심해 이진화가 무너진 경우를 살린다.
    // DPM/회전 구제보다 먼저 시도한다 — 필터 한 번 + 코어 패스 한 번으로
    // 가장 싸고, 실측상 가장 자주 걸린다(§3.14). [[vscan-lite-denoise-rescue]]
    if (!cfg_.disableDenoiseRescue && !budgetExceeded()) {
        GrayImage smoothed;
        boxBlur3x3(image, smoothed);
        auto dnHits = processViewCore(GrayView(smoothed));
        if ((int)dnHits.size() >= std::max(1, cfg_.minExpectedCodes)) return dnHits;
    }

    // [DPM/점각인 구제] processViewCore()가 풀옵션으로도 빈손이면,
    // DPM(레이저 점각인) 코드일 가능성을 본다. 실물 비교 대상 리더기 대조
    // 검증까지 완료된 구제책 — §6.4 대화 참고. 위치 탐색이 필요
    // 없는 전역 전처리라 1D 회전 구제보다 먼저 시도한다(더 싸다).
    // [[vscan-lite-dpm-rescue]]
    if (cfg_.enableDPMRescue) {
        GrayImage closed;
        morphologicalCloseInverted(image, cfg_.dpmKernelSize, closed);
        auto dpmHits = processViewCore(GrayView(closed));
        if ((int)dpmHits.size() >= std::max(1, cfg_.minExpectedCodes)) return dpmHits;
    }

    // [1D 바코드 회전 구제] processViewCore()가 이미 풀옵션(TryHarder+
    // Rotate+Invert)으로 돌았는데도 빈손이면, 20~75도 부근 회전 1D
    // 바코드일 가능성이 있다(§3.2.15~17). 이건 "속도 최적화 편의기능"
    // 이 아니라 zxing 기본 옵션으로는 원천적으로 못 찾는 것을 추가로
    // 찾아내는 진짜 검출 능력 확장이라, two_stage뿐 아니라 순수 풀옵션
    // 경로(processView/vscan_process_gray)에도 있어야 맞다.
    // [[vscan-lite-1d-deskew-rescue]]
    if (!cfg_.enable1DDeskewRescue || budgetExceeded()) return hits;
    int need = std::max(1, cfg_.minExpectedCodes);
    auto rescued = tryDeskewRescue1D(image, need);
    return rescued.empty() ? hits : rescued;
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
            return hits;
        }
    }
    return {};
}

std::vector<PipelineResult> Pipeline::processViewTwoStage(const GrayView& image, int cropPadPx) {
    if (image.empty()) return {};
    BudgetGuard budget(this);
    if (!frameHasStructure(image)) return {};   // [[vscan-lite-blank-frame-skip]]
    (void)cropPadPx; // 하위 호환용으로 시그니처만 유지 (아래 주석 참고)

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

    // [0단계] coarse locate — 절반 해상도로 먼저 훑는다.
    // 이진화가 locate 비용의 대부분이고 픽셀 수에 비례하므로 1/4이 된다.
    // 실패하면 아래 풀해상도 단계로 자연스럽게 내려간다(검출력 손실 없음).
    // [[vscan-lite-coarse-locate]]
    if (cfg_.coarseLocate && image.width >= 768 && image.height >= 768) {
        if (coarseSkipLeft_ > 0) {
            --coarseSkipLeft_;
        } else {
            const int cf = (cfg_.coarseFactor == 2) ? 2 : 3;
            downsampleBox(image, coarseBuf_, cf);
            Pipeline coarse(fastCfg);
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
    auto hits = fast.processViewCore(image);
    if ((int)hits.size() >= need) { adaptiveObserve(hits); return hits; }
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
    PipelineConfig harderCfg = cfg_;
    harderCfg.tileThreads = 1;
    harderCfg.tryHarder = true;
    harderCfg.tryRotate = false;
    harderCfg.tryInvert = false;
    Pipeline harder(harderCfg);
    auto hardHits = harder.processViewCore(image);
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
    PipelineConfig hiCfg = cfg_;
    hiCfg.tileThreads = 1;
    hiCfg.tryHarder = true;
    hiCfg.tryRotate = false;
    hiCfg.tryInvert = true;
    Pipeline hi(hiCfg);
    auto hiHits = hi.processViewCore(image);
    if ((int)hiHits.size() >= need) return hiHits;
    if (hiHits.size() > hits.size()) hits = std::move(hiHits);
    if (budgetExceeded()) return hits;

    // [최종 폴백] TryRotate까지 포함한 완전한 풀옵션. processView()가
    // 이제 내부적으로 1D 회전 구제(5단계, [[vscan-lite-1d-deskew-rescue]])
    // 까지 자체적으로 시도하므로 여기서 따로 또 부를 필요는 없다 —
    // 그래서 이 최종 호출 하나가 사실상 4~5단계를 전부 커버한다.
    // [[vscan-lite-two-stage-fallback]]
    auto full = processView(image);
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

    return decodeRegionsParallel(image, rects, padPx, regionCfg);
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
    std::vector<std::vector<PipelineResult>> parts(crops.size());
    if (regionCfg.tileThreads == 1 || crops.size() <= 1) {
        Pipeline refiner(regionCfg);   // crop 전체가 같은 설정이라 한 번만 만든다
        for (size_t i = 0; i < crops.size(); ++i) {
            parts[i] = refiner.processViewCore(GrayView(crops[i].packed));
        }
    } else {
        std::vector<std::future<std::vector<PipelineResult>>> futures;
        futures.reserve(crops.size());
        for (auto& c : crops) {
            futures.push_back(std::async(std::launch::async, [regionCfg, &c]() {
                Pipeline refiner(regionCfg);
                return refiner.processViewCore(GrayView(c.packed));
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

// 교집합 넓이 / 더 작은 쪽 넓이.
// IoU가 아니라 "작은 쪽 기준"인 이유는 아래 dedup() 주석 참고.
double containRatio(const BBox& a, const BBox& b) {
    double ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    if (ix <= 0 || iy <= 0) return 0.0;
    double small = std::min(areaOf(a), areaOf(b));
    return small > 0 ? (ix * iy) / small : 0.0;
}
}

std::vector<PipelineResult> Pipeline::dedup(std::vector<PipelineResult> in) {
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
            if (kept.symbol.text != cand.symbol.text) continue;

            auto keptBox = bboxOf(kept.symbol);
            auto keptCenter = centerOf(kept.symbol);
            double dx = candCenter.first - keptCenter.first;
            double dy = candCenter.second - keptCenter.second;
            bool near = std::sqrt(dx * dx + dy * dy) < kCenterDup;

            if (near || containRatio(candBox, keptBox) >= kOverlapDup) {
                isDup = true;
                if (areaOf(candBox) > areaOf(keptBox)) kept = std::move(cand);
                break;
            }
        }
        if (!isDup) out.push_back(std::move(cand));
    }
    return out;
}

} // namespace vscan
