#include "vscan.h"

#include <cstring>
#include <string>
#include <vector>

#include "vscan_internal/pipeline.hpp"

#ifdef VSCAN_HAVE_ZBAR
#include "vscan_internal/decoder_zbar.hpp"
#endif

namespace {

vscan_symbology_t mapSymbology(vscan::Symbology s) {
    using vscan::Symbology;
    switch (s) {
        case Symbology::QR: return VSCAN_SYM_QR;
        case Symbology::MICRO_QR: return VSCAN_SYM_MICRO_QR;
        case Symbology::DATA_MATRIX: return VSCAN_SYM_DATA_MATRIX;
        case Symbology::GS1_DATA_MATRIX: return VSCAN_SYM_GS1_DATA_MATRIX;
        case Symbology::PDF417: return VSCAN_SYM_PDF417;
        case Symbology::MICRO_PDF417: return VSCAN_SYM_MICRO_PDF417;
        case Symbology::GS1_COMPOSITE: return VSCAN_SYM_GS1_COMPOSITE;
        case Symbology::DOTCODE: return VSCAN_SYM_DOTCODE;
        case Symbology::CODE39: return VSCAN_SYM_CODE39;
        case Symbology::CODE39_FULL_ASCII: return VSCAN_SYM_CODE39_FULL_ASCII;
        case Symbology::TRIOPTIC_CODE39: return VSCAN_SYM_TRIOPTIC_CODE39;
        case Symbology::ITF: return VSCAN_SYM_ITF;
        case Symbology::INDUSTRIAL_2OF5: return VSCAN_SYM_INDUSTRIAL_2OF5;
        case Symbology::COOP_2OF5: return VSCAN_SYM_COOP_2OF5;
        case Symbology::CODABAR: return VSCAN_SYM_CODABAR;
        case Symbology::CODE128: return VSCAN_SYM_CODE128;
        case Symbology::GS1_128: return VSCAN_SYM_GS1_128;
        case Symbology::GS1_DATABAR: return VSCAN_SYM_GS1_DATABAR;
        case Symbology::CODE93: return VSCAN_SYM_CODE93;
        case Symbology::EAN_UPC: return VSCAN_SYM_EAN_UPC;
        case Symbology::PHARMACODE: return VSCAN_SYM_PHARMACODE;
        case Symbology::POSTAL_JAPAN: return VSCAN_SYM_POSTAL_JAPAN;
        case Symbology::POSTAL_IMB: return VSCAN_SYM_POSTAL_IMB;
        default: return VSCAN_SYM_UNKNOWN;
    }
}

// vscan_result_t가 가리키는 문자열/바이트의 실제 소유자.
// vscan_free_result()가 이 구조체를 delete하면 전부 같이 해제된다.
struct ResultStorage {
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> rawBytes;
    std::vector<vscan_symbol_t> symbols;
};

vscan_result_t* buildResult(std::vector<vscan::PipelineResult> results) {
    auto* storage = new ResultStorage();
    size_t n = results.size();
    storage->texts.resize(n);
    storage->rawBytes.resize(n);
    storage->symbols.resize(n);

    for (size_t i = 0; i < n; ++i) {
        const auto& sym = results[i].symbol;
        storage->texts[i] = sym.text;
        storage->rawBytes[i] = sym.rawBytes;

        vscan_symbol_t& out = storage->symbols[i];
        out.symbology = mapSymbology(sym.symbology);
        out.text = storage->texts[i].c_str();
        out.raw_bytes = storage->rawBytes[i].empty() ? nullptr : storage->rawBytes[i].data();
        out.raw_bytes_len = storage->rawBytes[i].size();
        out.is_gs1 = sym.isGS1 ? 1 : 0;
        for (int k = 0; k < 4; ++k) {
            out.corners[k].x = sym.position[k].first;
            out.corners[k].y = sym.position[k].second;
        }
    }

    auto* result = new vscan_result_t();
    result->symbols = storage->symbols.empty() ? nullptr : storage->symbols.data();
    result->count = n;
    result->_internal = storage;
    return result;
}

} // namespace

struct vscan_pipeline {
    vscan::Pipeline impl;
    explicit vscan_pipeline(vscan::PipelineConfig cfg) : impl(cfg) {}
};

extern "C" {

vscan_pipeline_t* vscan_create(const vscan_config_t* cfg) {
    vscan::PipelineConfig pcfg;
    bool wantZbar = false;
    if (cfg) {
        pcfg.tileThreads = cfg->tile_threads;
        if (cfg->tile_overlap_px) pcfg.tileOverlapPx = cfg->tile_overlap_px;
        wantZbar = cfg->enable_zbar_fastpath != 0;
        pcfg.enableZBar = wantZbar;
        pcfg.formatMask = cfg->symbology_mask;
        if (cfg->min_expected_codes > 1) pcfg.minExpectedCodes = cfg->min_expected_codes;
        if (cfg->disable_coarse_locate) pcfg.coarseLocate = false;
        if (cfg->coarse_factor == 2) pcfg.coarseFactor = 2;
        if (cfg->enable_1d_deskew_rescue) pcfg.enable1DDeskewRescue = true;
        if (cfg->disable_1d_deskew_rescue) pcfg.enable1DDeskewRescue = false;
        if (cfg->enable_dpm_rescue) pcfg.enableDPMRescue = true;
        if (cfg->max_frame_ms > 0) pcfg.maxFrameMs = cfg->max_frame_ms;
        if (cfg->disable_blank_frame_skip) pcfg.disableBlankFrameSkip = 1;
        if (cfg->disable_denoise_rescue) pcfg.disableDenoiseRescue = 1;
        if (cfg->enable_adaptive_profile) pcfg.enableAdaptiveProfile = 1;
        if (cfg->disable_region_rescue) pcfg.enableRegionRescue = false;
        if (cfg->enable_qr_finder_rescue) pcfg.enableQrFinderRescue = true;
        if (cfg->disable_auto_denoise) pcfg.autoDenoise = 0;
        if (cfg->worker_mode) {
            pcfg.tileThreads = 1;   // 내부 스레드 생성 완전 차단
        }
        if (cfg->fast_locate) {
            pcfg.locateBinarizer = vscan::ZXingDecoder::Binarizer::GlobalHistogram;
            pcfg.locateTryDownscale = false;
        } // 0 = 전체(기본)
        pcfg.tryRotate = (cfg->decode_flags & VSCAN_FLAG_NO_ROTATE) == 0;
        pcfg.tryInvert = (cfg->decode_flags & VSCAN_FLAG_NO_INVERT) == 0;
        pcfg.tryHarder = (cfg->decode_flags & VSCAN_FLAG_NO_TRY_HARDER) == 0;
    }
    // cfg==nullptr이면 PipelineConfig{}의 기본값(overlap=500px, 전체 포맷,
    // 회전/반전 탐색 켜짐)을 그대로 쓴다.

    auto* p = new vscan_pipeline(pcfg);

    // ZBar 등록은 PipelineConfig::enableZBar가 처리한다 — 내부 임시
    // 파이프라인들이 설정을 복사하므로 그래야 모든 경로에 일관되게 붙는다.
    // [[vscan-lite-zbar-in-subpipelines]]
    (void)wantZbar;

    return p;
}

void vscan_destroy(vscan_pipeline_t* pipeline) {
    delete pipeline;
}

vscan_result_t* vscan_process_gray(vscan_pipeline_t* pipeline,
                                    const uint8_t* pixels,
                                    int width, int height, int stride) {
    if (!pipeline || !pixels || width <= 0 || height <= 0) return nullptr;

    vscan::GrayView view(pixels, width, height, stride > 0 ? stride : width);
    auto results = pipeline->impl.processView(view);
    return buildResult(std::move(results));
}

vscan_result_t* vscan_process_gray_two_stage(vscan_pipeline_t* pipeline,
                                              const uint8_t* pixels,
                                              int width, int height, int stride,
                                              int crop_pad_px) {
    if (!pipeline || !pixels || width <= 0 || height <= 0) return nullptr;

    vscan::GrayView view(pixels, width, height, stride > 0 ? stride : width);
    auto results = pipeline->impl.processViewTwoStage(view, crop_pad_px);
    return buildResult(std::move(results));
}

vscan_result_t* vscan_process_gray_tracked(vscan_pipeline_t* pipeline,
                                            const uint8_t* pixels,
                                            int width, int height, int stride,
                                            int track_pad_px,
                                            int full_scan_interval) {
    if (!pipeline || !pixels || width <= 0 || height <= 0) return nullptr;
    vscan::GrayView view(pixels, width, height, stride > 0 ? stride : width);
    auto results = pipeline->impl.processViewTracked(view, track_pad_px, full_scan_interval);
    return buildResult(std::move(results));
}

vscan_result_t* vscan_process_gray_rois(vscan_pipeline_t* pipeline,
                                         const uint8_t* pixels,
                                         int width, int height, int stride,
                                         const vscan_rect_t* rois, size_t roi_count,
                                         int pad_px) {
    if (!pipeline || !pixels || width <= 0 || height <= 0) return nullptr;
    if (!rois || roi_count == 0) return buildResult({}); // 빈 결과, NULL 아님

    std::vector<vscan::Rect> rects;
    rects.reserve(roi_count);
    for (size_t i = 0; i < roi_count; ++i) {
        rects.push_back({rois[i].x0, rois[i].y0, rois[i].x1, rois[i].y1});
    }

    vscan::GrayView view(pixels, width, height, stride > 0 ? stride : width);
    auto results = pipeline->impl.processViewROIs(view, rects, pad_px);
    return buildResult(std::move(results));
}

void vscan_free_result(vscan_result_t* result) {
    if (!result) return;
    delete static_cast<ResultStorage*>(result->_internal);
    delete result;
}

const char* vscan_symbology_name(vscan_symbology_t s) {
    switch (s) {
        case VSCAN_SYM_QR: return "QR";
        case VSCAN_SYM_MICRO_QR: return "MicroQR";
        case VSCAN_SYM_DATA_MATRIX: return "DataMatrix";
        case VSCAN_SYM_GS1_DATA_MATRIX: return "GS1-DataMatrix";
        case VSCAN_SYM_PDF417: return "PDF417";
        case VSCAN_SYM_MICRO_PDF417: return "MicroPDF417";
        case VSCAN_SYM_GS1_COMPOSITE: return "GS1-Composite";
        case VSCAN_SYM_DOTCODE: return "DotCode";
        case VSCAN_SYM_CODE39: return "Code39";
        case VSCAN_SYM_CODE39_FULL_ASCII: return "Code39-FullASCII";
        case VSCAN_SYM_TRIOPTIC_CODE39: return "Trioptic-Code39";
        case VSCAN_SYM_ITF: return "ITF";
        case VSCAN_SYM_INDUSTRIAL_2OF5: return "Industrial-2of5";
        case VSCAN_SYM_COOP_2OF5: return "COOP-2of5";
        case VSCAN_SYM_CODABAR: return "Codabar";
        case VSCAN_SYM_CODE128: return "Code128";
        case VSCAN_SYM_GS1_128: return "GS1-128";
        case VSCAN_SYM_GS1_DATABAR: return "GS1-DataBar";
        case VSCAN_SYM_CODE93: return "Code93";
        case VSCAN_SYM_EAN_UPC: return "EAN/UPC";
        case VSCAN_SYM_PHARMACODE: return "Pharmacode";
        case VSCAN_SYM_POSTAL_JAPAN: return "PostalJP";
        case VSCAN_SYM_POSTAL_IMB: return "PostalIMB";
        default: return "Unknown";
    }
}

const char* vscan_version(void) {
    return "vscan-lite 0.1.0";
}

} // extern "C"
