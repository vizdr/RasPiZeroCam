// src/record/h264_hw_encoder.h
// Direct V4L2 M2M H.264 encoder for the Pi's bcm2835-codec at /dev/video11.
//
// Design (adapted from rpicam-apps encoder/h264_encoder.cpp):
//   OUTPUT queue (encoder input):  V4L2_MEMORY_DMABUF  — imports camera DMA fds
//   CAPTURE queue (encoder output): V4L2_MEMORY_MMAP   — kernel-allocated H264 buffers
//
// Why not GStreamer v4l2h264enc:
//   GStreamer enables DMABUF import mode only via buffer-pool caps negotiation
//   between two V4L2 pipeline elements (e.g. libcamerasrc → v4l2h264enc).
//   With appsrc as producer that negotiation never fires, so the encoder
//   falls back to MMAP mode and rejects our DMA-BUF fd on the first frame.
//   Calling VIDIOC_QBUF directly (this class) bypasses GStreamer entirely.
//
// Thread model:
//   encode()         — called from camera consumer thread; non-blocking
//   poll_thread_fn() — owns all V4L2 ioctl; fires callbacks
//   output_thread_fn() — delivers encoded H264 to caller; re-queues capture buf

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "camera/frame_buffer.h"
#include "camera/resolution.h"

class H264HardwareEncoder {
public:
    // Fires on poll_thread when encoder releases the DMA-BUF input slot.
    // The FramePtr held here drops → Frame::release() → requeue to libcamera.
    using InputDoneCallback = std::function<void(FramePtr)>;

    // Fires on output_thread with a pointer into the encoder's MMAP buffer.
    // Valid only for the duration of the call — copy data before returning.
    using OutputReadyCallback = std::function<void(const uint8_t* data,
                                                    size_t          size,
                                                    int64_t         timestamp_us,
                                                    bool            keyframe)>;

    // Opens /dev/video11, configures NV12→H264, allocates buffers, starts streaming.
    H264HardwareEncoder(const ResolutionConfig& cfg,
                        uint32_t                bitrate_bps,
                        const char*             device = "/dev/video11");
    ~H264HardwareEncoder();

    H264HardwareEncoder(const H264HardwareEncoder&)            = delete;
    H264HardwareEncoder& operator=(const H264HardwareEncoder&) = delete;

    // Submit one NV12 DMA-BUF frame for hardware encoding.
    // keepalive is stored per-slot and dropped after VIDIOC_DQBUF(OUTPUT).
    // Returns false if no input slot is free (caller should drop the frame).
    bool encode(int dma_fd, size_t size, int64_t timestamp_us, FramePtr keepalive);

    void set_input_done_callback(InputDoneCallback cb)    { input_done_cb_    = cb; }
    void set_output_ready_callback(OutputReadyCallback cb){ output_ready_cb_  = cb; }

    bool is_open() const noexcept { return fd_ >= 0; }

private:
    // ≥ camera DMA buffer count (4). Extra slots absorb jitter.
    static constexpr int NUM_OUTPUT_BUFFERS  = 6;
    // Output buffers: large pool absorbs processing latency in outputThread.
    static constexpr int NUM_CAPTURE_BUFFERS = 12;

    // V4L2 poll + dequeue loop (runs on poll_thread_)
    void poll_thread_fn();
    // Delivers encoded H264 to caller, re-queues capture buffer (output_thread_)
    void output_thread_fn();

    int fd_ = -1;

    // Input (OUTPUT queue) slot management
    std::mutex      input_mutex_;
    std::queue<int> free_input_slots_;                           // available slots
    std::array<FramePtr, NUM_OUTPUT_BUFFERS> pending_frames_{};  // per-slot keepalive

    // Capture (CAPTURE queue) MMAP'd buffers
    struct CaptureBuffer { void* mem = nullptr; size_t size = 0; };
    std::array<CaptureBuffer, NUM_CAPTURE_BUFFERS> capture_bufs_{};
    int num_capture_buffers_ = 0;

    // Output delivery queue (poll_thread → output_thread)
    struct OutputItem {
        void*        mem;
        size_t       bytes_used;
        size_t       length;
        unsigned int index;
        bool         keyframe;
        int64_t      timestamp_us;
    };
    std::queue<OutputItem>  output_queue_;
    std::mutex              output_mutex_;
    std::condition_variable output_cv_;

    std::thread       poll_thread_;
    std::thread       output_thread_;
    std::atomic<bool> abort_poll_   {false};
    std::atomic<bool> abort_output_ {false};

    InputDoneCallback    input_done_cb_;
    OutputReadyCallback  output_ready_cb_;
};
