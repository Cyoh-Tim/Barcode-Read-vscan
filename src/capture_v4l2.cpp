#include "vscan_internal/capture.hpp"

#ifdef VSCAN_HAVE_V4L2
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <cerrno>
#include <vector>
#include <stdexcept>
#endif

namespace vscan {

#ifdef VSCAN_HAVE_V4L2

namespace {

uint32_t toV4L2FourCC(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::YUYV: return V4L2_PIX_FMT_YUYV;
        case PixelFormat::NV12: return V4L2_PIX_FMT_NV12;
        case PixelFormat::GREY: return V4L2_PIX_FMT_GREY;
    }
    return V4L2_PIX_FMT_YUYV;
}

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

} // namespace

struct V4L2Capture::Impl {
    CaptureConfig cfg;
    int fd = -1;
    struct MappedBuf { void* start = nullptr; size_t length = 0; };
    std::vector<MappedBuf> buffers;
    bool streaming = false;
};

V4L2Capture::V4L2Capture(CaptureConfig cfg) : impl_(new Impl{std::move(cfg)}) {}

V4L2Capture::~V4L2Capture() {
    close();
    delete impl_;
}

bool V4L2Capture::open() {
    auto& d = *impl_;
    d.fd = ::open(d.cfg.device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (d.fd < 0) return false;

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = d.cfg.width;
    fmt.fmt.pix.height = d.cfg.height;
    fmt.fmt.pix.pixelformat = toV4L2FourCC(d.cfg.format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(d.fd, VIDIOC_S_FMT, &fmt) < 0) return false;

    v4l2_requestbuffers req{};
    req.count = d.cfg.buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(d.fd, VIDIOC_REQBUFS, &req) < 0) return false;
    if (req.count < 2) return false;

    d.buffers.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(d.fd, VIDIOC_QUERYBUF, &buf) < 0) return false;

        d.buffers[i].length = buf.length;
        d.buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, d.fd, buf.m.offset);
        if (d.buffers[i].start == MAP_FAILED) return false;

        if (xioctl(d.fd, VIDIOC_QBUF, &buf) < 0) return false;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(d.fd, VIDIOC_STREAMON, &type) < 0) return false;
    d.streaming = true;
    return true;
}

void V4L2Capture::close() {
    auto& d = *impl_;
    if (d.fd < 0) return;
    if (d.streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(d.fd, VIDIOC_STREAMOFF, &type);
        d.streaming = false;
    }
    for (auto& b : d.buffers) {
        if (b.start) munmap(b.start, b.length);
    }
    d.buffers.clear();
    ::close(d.fd);
    d.fd = -1;
}

std::optional<FrameView> V4L2Capture::grabView() {
    auto& d = *impl_;
    if (d.fd < 0 || !d.streaming) return std::nullopt;

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(d.fd, VIDIOC_DQBUF, &buf) < 0) return std::nullopt;

    FrameView view;
    view.data = static_cast<const uint8_t*>(d.buffers[buf.index].start);
    view.width = d.cfg.width;
    view.height = d.cfg.height;
    view.format = d.cfg.format;
    view.bufferIndex = static_cast<int>(buf.index);
    // 주의: 여기서는 QBUF(반납)를 하지 않는다. release()가 호출되기 전까지
    // 이 mmap 영역은 유효하다 — 이게 zero-copy의 핵심이다.
    return view;
}

void V4L2Capture::release(const FrameView& view) {
    auto& d = *impl_;
    if (d.fd < 0 || view.bufferIndex < 0) return;

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = static_cast<unsigned>(view.bufferIndex);
    xioctl(d.fd, VIDIOC_QBUF, &buf);
}

std::optional<Frame> V4L2Capture::grab() {
    // 레거시 경로: view를 받아 즉시 복사하고 반납한다.
    auto view = grabView();
    if (!view) return std::nullopt;

    Frame frame;
    frame.width = view->width;
    frame.height = view->height;
    frame.format = view->format;
    size_t bytes = static_cast<size_t>(view->width) * view->height *
                   (view->format == PixelFormat::GREY ? 1 : 2); // YUYV 등은 대략치
    frame.data.assign(view->data, view->data + bytes);

    release(*view);
    return frame;
}

#else // !VSCAN_HAVE_V4L2

struct V4L2Capture::Impl {};
V4L2Capture::V4L2Capture(CaptureConfig) : impl_(nullptr) {}
V4L2Capture::~V4L2Capture() {}
bool V4L2Capture::open() { return false; }
void V4L2Capture::close() {}
std::optional<FrameView> V4L2Capture::grabView() { return std::nullopt; }
void V4L2Capture::release(const FrameView&) {}
std::optional<Frame> V4L2Capture::grab() { return std::nullopt; }

#endif

} // namespace vscan
