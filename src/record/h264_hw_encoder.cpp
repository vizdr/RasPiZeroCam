// src/record/h264_hw_encoder.cpp
// Adapted from rpicam-apps encoder/h264_encoder.cpp (BSD-2-Clause).
// Key differences:
//   - V4L2_PIX_FMT_NV12 input (camera native) instead of YUV420
//   - FramePtr keepalive per slot instead of callback-based ownership
//   - No rpicam-apps base class / logging / VideoOptions dependencies

#include "h264_hw_encoder.h"

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

H264HardwareEncoder::H264HardwareEncoder(const ResolutionConfig& cfg,
                                         uint32_t                bitrate_bps,
                                         const char*             device)
{
    // ── Open device ───────────────────────────────────────────────────────
    fd_ = open(device, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
        throw std::runtime_error(std::string("H264HardwareEncoder: failed to open ")
                                 + device + ": " + strerror(errno));
    std::cout << "H264HardwareEncoder: opened " << device << "\n";

    // ── Controls ──────────────────────────────────────────────────────────
    auto set_ctrl = [&](uint32_t id, int32_t value, const char* name) {
        v4l2_control ctrl{id, value};
        if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0)
            std::cerr << "H264HardwareEncoder: failed to set " << name << "\n";
    };

    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE,          static_cast<int32_t>(bitrate_bps), "bitrate");
    set_ctrl(V4L2_CID_MPEG_VIDEO_H264_PROFILE,     V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H264 profile");
    set_ctrl(V4L2_CID_MPEG_VIDEO_H264_LEVEL,       V4L2_MPEG_VIDEO_H264_LEVEL_4_1,   "H264 level");
    set_ctrl(V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1,                                "inline headers");
    set_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,    static_cast<int32_t>(cfg.fps),    "IDR period");

    // ── OUTPUT format — NV12 input (encoder input) ─────────────────────────
    // rpicam-apps uses YUV420; we use NV12 because that is what libcamera
    // delivers. Both are accepted by bcm2835-codec on /dev/video11.
    {
        v4l2_format fmt{};
        fmt.type                                 = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        fmt.fmt.pix_mp.width                     = cfg.width;
        fmt.fmt.pix_mp.height                    = cfg.height;
        fmt.fmt.pix_mp.pixelformat               = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = cfg.width;  // stride = width for NV12
        fmt.fmt.pix_mp.field                     = V4L2_FIELD_ANY;
        fmt.fmt.pix_mp.colorspace                = V4L2_COLORSPACE_REC709;
        fmt.fmt.pix_mp.num_planes                = 1;
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
            throw std::runtime_error("H264HardwareEncoder: failed to set OUTPUT format");
    }

    // ── CAPTURE format — H264 output (encoded bitstream) ─────────────────
    {
        v4l2_format fmt{};
        fmt.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width                      = cfg.width;
        fmt.fmt.pix_mp.height                     = cfg.height;
        fmt.fmt.pix_mp.pixelformat                = V4L2_PIX_FMT_H264;
        fmt.fmt.pix_mp.field                      = V4L2_FIELD_ANY;
        fmt.fmt.pix_mp.colorspace                 = V4L2_COLORSPACE_DEFAULT;
        fmt.fmt.pix_mp.num_planes                 = 1;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline  = 0;
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage     = 512 << 10;  // 512 KB per buffer
        if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
            throw std::runtime_error("H264HardwareEncoder: failed to set CAPTURE format");
    }

    // ── Framerate ─────────────────────────────────────────────────────────
    {
        v4l2_streamparm parm{};
        parm.type                                      = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        parm.parm.output.timeperframe.numerator        = 1;
        parm.parm.output.timeperframe.denominator      = cfg.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
            std::cerr << "H264HardwareEncoder: failed to set framerate\n";
    }

    // ── REQUEST OUTPUT buffers — DMABUF (no kernel allocation) ───────────
    {
        v4l2_requestbuffers req{};
        req.count  = NUM_OUTPUT_BUFFERS;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
            throw std::runtime_error("H264HardwareEncoder: REQBUFS OUTPUT failed");
        std::cout << "H264HardwareEncoder: " << req.count << " OUTPUT slots\n";
        for (unsigned i = 0; i < req.count; ++i)
            free_input_slots_.push(static_cast<int>(i));
    }

    // ── REQUEST CAPTURE buffers — MMAP, query, mmap, pre-queue all ────────
    {
        v4l2_requestbuffers req{};
        req.count  = NUM_CAPTURE_BUFFERS;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
            throw std::runtime_error("H264HardwareEncoder: REQBUFS CAPTURE failed");
        num_capture_buffers_ = static_cast<int>(req.count);
        std::cout << "H264HardwareEncoder: " << req.count << " CAPTURE buffers\n";

        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            v4l2_buffer buf{};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index  = i;
            buf.length = 1;
            buf.m.planes = planes;

            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
                throw std::runtime_error("H264HardwareEncoder: QUERYBUF failed");

            capture_bufs_[i].size = buf.m.planes[0].length;
            capture_bufs_[i].mem  = mmap(nullptr, buf.m.planes[0].length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          fd_, buf.m.planes[0].m.mem_offset);
            if (capture_bufs_[i].mem == MAP_FAILED)
                throw std::runtime_error("H264HardwareEncoder: mmap CAPTURE failed");

            // Pre-queue all capture buffers so the encoder can write into them
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
                throw std::runtime_error("H264HardwareEncoder: QBUF CAPTURE failed");
        }
    }

    // ── STREAMON ──────────────────────────────────────────────────────────
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throw std::runtime_error("H264HardwareEncoder: STREAMON OUTPUT failed");
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throw std::runtime_error("H264HardwareEncoder: STREAMON CAPTURE failed");
    }

    std::cout << "H264HardwareEncoder: streaming started "
              << cfg.width << "x" << cfg.height
              << " " << bitrate_bps / 1000 << " kbps\n";

    poll_thread_   = std::thread(&H264HardwareEncoder::poll_thread_fn,   this);
    output_thread_ = std::thread(&H264HardwareEncoder::output_thread_fn, this);
}

// ── Destructor ─────────────────────────────────────────────────────────────

H264HardwareEncoder::~H264HardwareEncoder()
{
    // Signal pollThread to drain and exit
    abort_poll_ = true;
    if (poll_thread_.joinable())
        poll_thread_.join();

    // Signal outputThread to flush and exit
    abort_output_ = true;
    output_cv_.notify_all();
    if (output_thread_.joinable())
        output_thread_.join();

    // STREAMOFF
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);

    // Free OUTPUT buffer slots
    {
        v4l2_requestbuffers req{};
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;
        xioctl(fd_, VIDIOC_REQBUFS, &req);
    }

    // Unmap and free CAPTURE buffers
    for (int i = 0; i < num_capture_buffers_; ++i)
        if (capture_bufs_[i].mem && capture_bufs_[i].mem != MAP_FAILED)
            munmap(capture_bufs_[i].mem, capture_bufs_[i].size);
    {
        v4l2_requestbuffers req{};
        req.count  = 0;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &req);
    }

    close(fd_);
    fd_ = -1;
    std::cout << "H264HardwareEncoder: closed\n";
}

// ── encode() ───────────────────────────────────────────────────────────────

bool H264HardwareEncoder::encode(int      dma_fd,
                                  size_t   size,
                                  int64_t  timestamp_us,
                                  FramePtr keepalive)
{
    int slot;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (free_input_slots_.empty())
            return false;   // all slots busy — caller drops the frame
        slot = free_input_slots_.front();
        free_input_slots_.pop();
        pending_frames_[slot] = std::move(keepalive);  // held until DQBUF OUTPUT
    }

    v4l2_plane plane{};
    v4l2_buffer buf{};
    buf.type                = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.index               = static_cast<unsigned>(slot);
    buf.field               = V4L2_FIELD_NONE;
    buf.memory              = V4L2_MEMORY_DMABUF;
    buf.length              = 1;
    buf.timestamp.tv_sec    = timestamp_us / 1'000'000;
    buf.timestamp.tv_usec   = timestamp_us % 1'000'000;
    buf.m.planes            = &plane;
    buf.m.planes[0].m.fd    = dma_fd;
    buf.m.planes[0].bytesused = static_cast<unsigned>(size);
    buf.m.planes[0].length    = static_cast<unsigned>(size);

    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        std::cerr << "H264HardwareEncoder: VIDIOC_QBUF OUTPUT failed: "
                  << strerror(errno) << "\n";
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_frames_[slot].reset();
        free_input_slots_.push(slot);
        return false;
    }
    return true;
}

// ── poll_thread_fn() ───────────────────────────────────────────────────────

void H264HardwareEncoder::poll_thread_fn()
{
    while (true) {
        // Wait for activity on either queue (200 ms timeout)
        pollfd p{fd_, POLLIN, 0};
        int ret = poll(&p, 1, 200);

        // Check drain condition: abort requested AND all input slots returned
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            if (abort_poll_ &&
                static_cast<int>(free_input_slots_.size()) == NUM_OUTPUT_BUFFERS)
                break;
        }

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "H264HardwareEncoder: poll error: " << strerror(errno) << "\n";
            break;
        }
        if (!(p.revents & POLLIN))
            continue;

        // ── Dequeue OUTPUT (input slot released by encoder) ───────────────
        {
            v4l2_plane plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory   = V4L2_MEMORY_DMABUF;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
                FramePtr done_frame;
                {
                    std::lock_guard<std::mutex> lock(input_mutex_);
                    done_frame = std::move(pending_frames_[buf.index]);
                    free_input_slots_.push(static_cast<int>(buf.index));
                }
                // Drop done_frame here → Frame::release() → requeue_request()
                if (input_done_cb_)
                    input_done_cb_(std::move(done_frame));
                else
                    done_frame.reset();  // explicit drop if no callback set
            }
        }

        // ── Dequeue CAPTURE (encoded H264 NAL ready) ──────────────────────
        {
            v4l2_plane plane{};
            v4l2_buffer buf{};
            buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.length   = 1;
            buf.m.planes = &plane;

            if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
                int64_t ts_us = static_cast<int64_t>(buf.timestamp.tv_sec) * 1'000'000
                              + buf.timestamp.tv_usec;
                OutputItem item{
                    capture_bufs_[buf.index].mem,
                    buf.m.planes[0].bytesused,
                    buf.m.planes[0].length,
                    buf.index,
                    !!(buf.flags & V4L2_BUF_FLAG_KEYFRAME),
                    ts_us
                };
                {
                    std::lock_guard<std::mutex> lock(output_mutex_);
                    output_queue_.push(item);
                }
                output_cv_.notify_one();
            }
        }
    }
}

// ── output_thread_fn() ─────────────────────────────────────────────────────

void H264HardwareEncoder::output_thread_fn()
{
    while (true) {
        OutputItem item{};
        {
            std::unique_lock<std::mutex> lock(output_mutex_);
            output_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !output_queue_.empty() || abort_output_.load();
            });

            if (abort_output_ && output_queue_.empty())
                return;
            if (output_queue_.empty())
                continue;

            item = output_queue_.front();
            output_queue_.pop();
        }

        // Deliver encoded data to caller (Recorder pushes it to GStreamer appsrc)
        if (output_ready_cb_)
            output_ready_cb_(static_cast<const uint8_t*>(item.mem),
                             item.bytes_used,
                             item.timestamp_us,
                             item.keyframe);

        // Re-queue the capture buffer so the encoder can reuse it
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type              = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory            = V4L2_MEMORY_MMAP;
        buf.index             = item.index;
        buf.length            = 1;
        buf.m.planes          = &plane;
        buf.m.planes[0].length    = static_cast<unsigned>(item.length);
        buf.m.planes[0].bytesused = 0;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            std::cerr << "H264HardwareEncoder: failed to re-queue capture buffer\n";
    }
}
