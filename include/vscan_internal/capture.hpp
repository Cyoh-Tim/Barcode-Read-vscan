#pragma once
#include <optional>
#include <string>
#include "vscan_internal/frame.hpp"

namespace vscan {

struct CaptureConfig {
    std::string device = "/dev/video0";
    int width = 1920;
    int height = 1080;
    // ISP(IMX900 → i.MX8MP ISP 파이프라인)가 이미 그레이스케일로 뽑아준다는
    // 전제. YUYV/NV12는 ISP 설정이 잘못됐을 때를 위한 폴백으로만 남겨둔다.
    PixelFormat format = PixelFormat::GREY;
    int buffer_count = 4; // mmap buffer 개수
};

// mmap된 V4L2 버퍼를 직접 가리키는 뷰. 복사가 전혀 없다.
// 사용이 끝나면 반드시 release()로 버퍼를 커널에 반납해야 한다
// (반납 전까지는 그 버퍼 슬롯에 새 프레임이 캡처되지 않는다).
struct FrameView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::GREY;
    int bufferIndex = -1; // release()에 그대로 넘길 내부 핸들
};

// mmap 기반 V4L2 스트리밍 캡처. 이미 다른 곳에서 V4L2로 캡처 중이라면
// 이 클래스 대신 Frame을 직접 채워서 Pipeline에 넣어도 된다.
class V4L2Capture {
public:
    explicit V4L2Capture(CaptureConfig cfg);
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // 디바이스 open, 포맷 설정, 버퍼 mmap, streaming 시작
    bool open();
    void close();

    // [권장/제로카피] mmap 버퍼를 가리키는 뷰만 반환한다 (memcpy 없음).
    // 반드시 사용 후 release(view)를 호출해서 버퍼를 반납할 것.
    // Pipeline::processView()에 view.data를 그대로 넘기면 캡처부터
    // 디코드까지 복사가 한 번도 발생하지 않는다.
    std::optional<FrameView> grabView();
    void release(const FrameView& view);

    // [레거시] 버퍼 내용을 Frame으로 복사해서 반환한다. 버퍼는 즉시
    // 반납되므로 수명 관리가 필요 없지만, 프레임마다 memcpy 1회가 발생한다.
    std::optional<Frame> grab();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vscan
