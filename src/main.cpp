#include <iostream>
#include <string>

#include "vscan_internal/capture.hpp"
#include "vscan_internal/pipeline.hpp"

namespace {

const char* symbologyName(vscan::Symbology s) {
    using vscan::Symbology;
    switch (s) {
        case Symbology::QR: return "QR";
        case Symbology::MICRO_QR: return "MicroQR";
        case Symbology::DATA_MATRIX: return "DataMatrix";
        case Symbology::GS1_DATA_MATRIX: return "GS1-DataMatrix";
        case Symbology::PDF417: return "PDF417";
        case Symbology::MICRO_PDF417: return "MicroPDF417";
        case Symbology::GS1_COMPOSITE: return "GS1-Composite(TODO)";
        case Symbology::DOTCODE: return "DotCode(TODO)";
        case Symbology::CODE39: return "Code39";
        case Symbology::CODE39_FULL_ASCII: return "Code39-FullASCII";
        case Symbology::TRIOPTIC_CODE39: return "Trioptic-Code39";
        case Symbology::ITF: return "ITF";
        case Symbology::INDUSTRIAL_2OF5: return "Industrial-2of5";
        case Symbology::COOP_2OF5: return "COOP-2of5";
        case Symbology::CODABAR: return "Codabar";
        case Symbology::CODE128: return "Code128";
        case Symbology::GS1_128: return "GS1-128";
        case Symbology::GS1_DATABAR: return "GS1-DataBar";
        case Symbology::CODE93: return "Code93";
        case Symbology::EAN_UPC: return "EAN/UPC";
        case Symbology::PHARMACODE: return "Pharmacode";
        case Symbology::POSTAL_JAPAN: return "PostalJP";
        case Symbology::POSTAL_IMB: return "PostalIMB";
        default: return "Unknown";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string device = argc > 1 ? argv[1] : "/dev/video0";

    vscan::CaptureConfig cfg;
    cfg.device = device;
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.format = vscan::PixelFormat::YUYV;

    vscan::V4L2Capture capture(cfg);
    if (!capture.open()) {
        std::cerr << "Failed to open " << device << "\n";
        return 1;
    }

    vscan::Pipeline pipeline;

    std::cout << "Streaming from " << device << " ... Ctrl+C to stop\n";
    while (true) {
        auto view = capture.grabView(); // 복사 없음
        if (!view) continue;

        vscan::GrayView gray(view->data, view->width, view->height, view->width);
        auto results = pipeline.processView(gray); // 복사 없음, 타일 병렬 디코드

        for (const auto& r : results) {
            std::cout << "[" << symbologyName(r.symbol.symbology) << "] "
                      << r.symbol.text;
            if (r.symbol.isGS1 && r.gs1.parsedOk) {
                std::cout << "  GS1{";
                for (const auto& [ai, val] : r.gs1.fields) {
                    std::cout << "(" << ai << ")" << val << " ";
                }
                std::cout << "}";
            }
            std::cout << "\n";
        }

        capture.release(*view); // 여기서 비로소 버퍼 반납 -> 다음 프레임 캡처 가능
    }
}
