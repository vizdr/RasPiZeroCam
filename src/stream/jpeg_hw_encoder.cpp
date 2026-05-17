// src/stream/jpeg_hw_encoder.cpp
// Adapted from h264_hw_encoder.cpp — same V4L2 M2M pattern, JPEG variant.

#include "jpeg_hw_encoder.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

// ── Helper ─────────────────────────────────────────────────────────────────

static int xioctl(int fd, unsigned long req, void* arg)
{
    int ret;
    do { ret = ioctl(fd, req, arg); }
    while (ret == -1 && errno == EINTR);
    return ret;
}

// ── Constructor ────────────────────────────────────────────────────────────

JpegHwEncoder::JpegHwEncoder(uint32_t    width,
                              uint32_t    height,
                              uint32_t    stride,
                              int         quality,
                              const char* device)
    : width_(width), height_(height), stride_(stride)
{
    // ── 2a: Open device ───────────────────────────────────────────────────
    fd_ = open(device, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
        throw std::runtime_error(
            std::string("JpegHwEncoder: cannot open ") + device + ": " + strerror(errno));

    std::cout << "JpegHwEncoder: opened " << device
              << " (" << width_ << "x" << height_ << " q=" << quality << ")\n";

    // ── Quality control ───────────────────────────────────────────────────
    {
        v4l2_control ctrl{};
        ctrl.id    = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        ctrl.value = quality;
        if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0)
            std::cerr << "JpegHwEncoder: failed to set quality\n";
    }

    // ── 2b: OUTPUT format — YU12 (I420 planar 4:2:0) ─────────────────────
    // /dev/video31 does NOT accept NV12; YU12 is the closest planar format.
    // Our encode() de-interleaves NV12→YU12 before QBUF.
    {
        v4l2_format fmt{};
        fmt.type                                  = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fmt.fmt.pix_mp.width                      = width_;
        fmt.fmt.pix_mp.height                     = height_;
        fmt.fmt.pix_mp.pixelformat                = V4L2_PIX_FMT_YUV420; // YU12 / I420
        fmt.fmt.pix_mp.num_planes                 = 1;
        fmt.fmt.pix_mp.field                      = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.colorspace                 = V4L2_COLORSPACE_REC709;
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage     = width_ * height_ * 3 / 2;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline  = width_;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
            throw std::runtime_error("JpegHwEncoder: failed to set OUTPUT format");
    }

    // ── 2c: CAPTURE format — JPEG ─────────────────────────────────────────
    {
        v4l2_format fmt{};
        fmt.type                               = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width                   = width_;
        fmt.fmt.pix_mp.height                  = height_;
        fmt.fmt.pix_mp.pixelformat             = V4L2_PIX_FMT_JPEG;
        fmt.fmt.pix_mp.num_planes              = 1;
        fmt.fmt.pix_mp.field                   = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage  = 512 * 1024; // 512 KB — ample for any quality
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
            throw std::runtime_error("JpegHwEncoder: failed to set CAPTURE format");
    }

    // ── 2d: REQUEST OUTPUT buffers (MMAP), query and mmap each ───────────
    {
        v4l2_requestbuffers req{};
        req.count  = NUM_OUTPUT_BUFFERS;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
            throw std::runtime_error("JpegHwEncoder: REQBUFS OUTPUT failed");

        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_plane  plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.index    = i;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
                throw std::runtime_error("JpegHwEncoder: QUERYBUF OUTPUT failed");

            output_bufs_[i].size = buf.m.planes[0].length;
            output_bufs_[i].mem  = mmap(nullptr, buf.m.planes[0].length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         fd_, buf.m.planes[0].m.mem_offset);
            if (output_bufs_[i].mem == MAP_FAILED)
                throw std::runtime_error("JpegHwEncoder: mmap OUTPUT failed");

            free_output_slots_.push(static_cast<int>(i));
        }
        std::cout << "JpegHwEncoder: " << req.count << " OUTPUT slots\n";
    }

    // ── 2e: REQUEST CAPTURE buffers (MMAP), query, mmap, pre-queue all ───
    {
        v4l2_requestbuffers req{};
        req.count  = NUM_CAPTURE_BUFFERS;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
            throw std::runtime_error("JpegHwEncoder: REQBUFS CAPTURE failed");
        num_capture_bufs_ = static_cast<int>(req.count);

        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_plane  plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.index    = i;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
                throw std::runtime_error("JpegHwEncoder: QUERYBUF CAPTURE failed");

            capture_bufs_[i].size = buf.m.planes[0].length;
            capture_bufs_[i].mem  = mmap(nullptr, buf.m.planes[0].length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          fd_, buf.m.planes[0].m.mem_offset);
            if (capture_bufs_[i].mem == MAP_FAILED)
                throw std::runtime_error("JpegHwEncoder: mmap CAPTURE failed");

            // Pre-queue so encoder can write immediately
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
                throw std::runtime_error("JpegHwEncoder: QBUF CAPTURE pre-queue failed");
        }
        std::cout << "JpegHwEncoder: " << req.count << " CAPTURE buffers\n";
    }

    // ── 2f: STREAMON both queues ──────────────────────────────────────────
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throw std::runtime_error("JpegHwEncoder: STREAMON OUTPUT failed");
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throw std::runtime_error("JpegHwEncoder: STREAMON CAPTURE failed");
    }

    poll_thread_ = std::thread(&JpegHwEncoder::poll_thread_fn, this);
    std::cout << "JpegHwEncoder: streaming started\n";
}

// ── Destructor ─────────────────────────────────────────────────────────────

JpegHwEncoder::~JpegHwEncoder()
{
    abort_ = true;
    if (poll_thread_.joinable())
        poll_thread_.join();

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);

    // Free OUTPUT buffers
    {
        v4l2_requestbuffers req{};
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        for (auto& b : output_bufs_)
            if (b.mem) munmap(b.mem, b.size);
        xioctl(fd_, VIDIOC_REQBUFS, &req);
    }

    // Free CAPTURE buffers
    {
        v4l2_requestbuffers req{};
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        for (int i = 0; i < num_capture_bufs_; ++i)
            if (capture_bufs_[i].mem) munmap(capture_bufs_[i].mem, capture_bufs_[i].size);
        xioctl(fd_, VIDIOC_REQBUFS, &req);
    }

    close(fd_);
    fd_ = -1;
    std::cout << "JpegHwEncoder: closed\n";
}

// ── encode() ───────────────────────────────────────────────────────────────
// ── 2g: NV12 → YU12 de-interleave into MMAP slot, then VIDIOC_QBUF ───────

bool JpegHwEncoder::encode(const FramePtr& frame)
{
    int slot;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (free_output_slots_.empty())
            return false;   // all slots busy — drop this frame
        slot = free_output_slots_.front();
        free_output_slots_.pop();
    }

    uint8_t* dst    = static_cast<uint8_t*>(output_bufs_[slot].mem);
    const uint8_t* Y  = frame->data;
    const uint8_t* UV = frame->data + stride_ * height_;

    // Y plane — strip stride padding: copy width_ bytes per row
    for (uint32_t r = 0; r < height_; ++r)
        std::memcpy(dst + r * width_, Y + r * stride_, width_);
    dst += width_ * height_;

    // UV de-interleave: NV12 [Cb Cr Cb Cr ...] → YU12 [Cb...][Cr...]
    const uint32_t uv_w = width_ / 2;
    const uint32_t uv_h = height_ / 2;
    uint8_t* cb = dst;
    uint8_t* cr = dst + uv_w * uv_h;

    for (uint32_t r = 0; r < uv_h; ++r) {
        const uint8_t* uv_row = UV + r * stride_;   // UV row stride = Y stride
        for (uint32_t c = 0; c < uv_w; ++c) {
            cb[r * uv_w + c] = uv_row[c * 2];
            cr[r * uv_w + c] = uv_row[c * 2 + 1];
        }
    }
    // NV12 data fully copied — FramePtr can drop when caller releases it

    v4l2_plane plane{};
    v4l2_buffer buf{};
    buf.type                  = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.index                 = static_cast<unsigned>(slot);
    buf.memory                = V4L2_MEMORY_MMAP;
    buf.length                = 1;
    buf.m.planes              = &plane;
    buf.m.planes[0].bytesused = width_ * height_ * 3 / 2;
    buf.m.planes[0].length    = static_cast<unsigned>(output_bufs_[slot].size);

    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        std::cerr << "JpegHwEncoder: VIDIOC_QBUF OUTPUT failed: " << strerror(errno) << "\n";
        std::lock_guard<std::mutex> lock(output_mutex_);
        free_output_slots_.push(slot);
        return false;
    }
    return true;
}

// ── poll_thread_fn() ───────────────────────────────────────────────────────
// ── 2h: DQBUF OUTPUT (return slot) + DQBUF CAPTURE (deliver JPEG) ─────────

void JpegHwEncoder::poll_thread_fn()
{
    while (!abort_) {
        pollfd p{fd_, POLLIN, 0};
        int ret = poll(&p, 1, 100);

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "JpegHwEncoder: poll error: " << strerror(errno) << "\n";
            break;
        }
        if (!(p.revents & POLLIN))
            continue;

        // ── Dequeue OUTPUT (input slot returned by encoder) ───────────────
        {
            v4l2_plane  plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
                std::lock_guard<std::mutex> lock(output_mutex_);
                free_output_slots_.push(static_cast<int>(buf.index));
            }
        }

        // ── Dequeue CAPTURE (JPEG output ready) ───────────────────────────
        {
            v4l2_plane  plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
                const size_t jpeg_size = buf.m.planes[0].bytesused;

                // Deliver to MjpegServer (copies into latest_jpeg_)
                if (output_ready_cb_ && jpeg_size > 0)
                    output_ready_cb_(
                        static_cast<const uint8_t*>(capture_bufs_[buf.index].mem),
                        jpeg_size);

                // Re-queue the capture buffer so encoder can use it again
                buf.m.planes[0].bytesused = 0;
                buf.m.planes[0].length    = static_cast<unsigned>(capture_bufs_[buf.index].size);
                xioctl(fd_, VIDIOC_QBUF, &buf);
            }
        }
    }
}
