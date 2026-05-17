// src/stream/jpeg_hw_encoder.h
// Hardware JPEG encoder using Pi's V4L2 M2M codec at /dev/video31.
//
// Design:
//   INPUT  (OUTPUT queue): YU12 / I420 planar — V4L2_MEMORY_MMAP
//   OUTPUT (CAPTURE queue): JPEG compressed   — V4L2_MEMORY_MMAP
//
// Unlike H264HardwareEncoder (which uses V4L2_MEMORY_DMABUF to avoid
// copying), JPEG encoder does NOT accept NV12 — only planar YU12/I420.
// Therefore each encode() call de-interleaves NV12→YU12 (one memcpy per
// plane, ~4 ms at 640×480) into a kernel-allocated MMAP input buffer.
// The FramePtr can be released as soon as encode() returns — no keepalive.
//
// Pre-encoding model (key difference from TurboJPEG software path):
//   Hardware encodes ONE frame; all HTTP clients share the resulting bytes.
//   Encoding cost is O(1) regardless of number of connected browsers.
//
// Thread model:
//   encode()         — camera consumer thread, non-blocking
//   poll_thread_fn() — dedicated thread: V4L2 DQBUF + callback delivery

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "camera/frame_buffer.h"

class JpegHwEncoder {
public:
    // Fires from pollThread when an encoded JPEG frame is ready.
    // data points into the encoder's MMAP capture buffer — valid only
    // for the duration of the callback. Copy if you need to keep it.
    using OutputReadyCallback =
        std::function<void(const uint8_t* data, size_t size)>;

    // Opens /dev/video31, configures YU12→JPEG, allocates MMAP buffers.
    JpegHwEncoder(uint32_t width, uint32_t height, uint32_t stride,
                  int quality   = 85,
                  const char*   device = "/dev/video31");
    ~JpegHwEncoder();

    JpegHwEncoder(const JpegHwEncoder&)            = delete;
    JpegHwEncoder& operator=(const JpegHwEncoder&) = delete;

    // De-interleave NV12→YU12 into a free MMAP slot, then VIDIOC_QBUF.
    // Returns false if no slot is available (caller drops the frame).
    // The FramePtr is not held after this call returns.
    bool encode(const FramePtr& frame);

    void set_output_ready_callback(OutputReadyCallback cb)
    {
        output_ready_cb_ = std::move(cb);
    }

    bool is_open() const noexcept { return fd_ >= 0; }

private:
    // 3 input slots allow one in the encoder, one being filled,
    // one ready — enough for continuous 30fps pipelining.
    static constexpr int NUM_OUTPUT_BUFFERS  = 3;
    // 3 JPEG output buffers so the encoder can always write into one
    // while we are delivering the previous one to HTTP clients.
    static constexpr int NUM_CAPTURE_BUFFERS = 3;

    void poll_thread_fn();

    int      fd_      = -1;
    uint32_t width_   = 0;
    uint32_t height_  = 0;
    uint32_t stride_  = 0;

    // MMAP buffers for INPUT (OUTPUT queue) — encoder reads from here
    struct MmapBuf { void* mem = nullptr; size_t size = 0; };
    std::array<MmapBuf, NUM_OUTPUT_BUFFERS>  output_bufs_{};

    // MMAP buffers for OUTPUT (CAPTURE queue) — encoder writes JPEG here
    std::array<MmapBuf, NUM_CAPTURE_BUFFERS> capture_bufs_{};
    int num_capture_bufs_ = 0;

    // Free input slot pool — poll_thread returns slots after DQBUF OUTPUT
    std::mutex      output_mutex_;
    std::queue<int> free_output_slots_;

    std::thread       poll_thread_;
    std::atomic<bool> abort_{false};

    OutputReadyCallback output_ready_cb_;
};
