// src/stream/jpeg_mt_encoder.h
// Multi-threaded JPEG encoder — adapted from rpicam-apps MjpegEncoder.
//
// Design (mirrors rpicam-apps encoder/mjpeg_encoder.cpp):
//   NUM_THREADS encode threads, each with a persistent jpeg_compress_struct.
//   Frames are queued with a monotonic index, encoded in parallel, then
//   reordered by an output thread before delivery.
//
// Encoding path (fast jpeglib route):
//   cinfo.raw_data_in = TRUE + jpeg_write_raw_data()
//   — libjpeg skips its own colour conversion and subsampling stages,
//     going straight to DCT on pre-subsampled YCbCr MCU rows.
//   — Y plane: pointer directly into the DMA buffer (no copy)
//   — UV plane: one de-interleave pass (NV12→separate Cb/Cr, ~4 ms)
//
// Throughput model (Pi Zero 2W, 640×480, quality=85):
//   Single-thread encode: ~57 ms/frame
//   Camera delivers frames every: 33 ms (30 fps)
//   With 4 threads: each thread encodes every 4th frame = every 132 ms
//   132 ms > 57 ms  →  camera is the bottleneck  →  30 fps achieved

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "camera/frame_buffer.h"

// Forward-declare libjpeg struct to avoid including jpeglib.h in the header
struct jpeg_compress_struct;

class JpegMtEncoder {
public:
    using OutputReadyCallback = std::function<void(const uint8_t* data, size_t size)>;

    // One thread per Cortex-A53 core — matches rpicam-apps' NUM_ENC_THREADS
    static constexpr int NUM_THREADS = 4;
    // Drop frames rather than stall the camera consumer
    static constexpr int MAX_QUEUE   = 8;

    JpegMtEncoder(uint32_t width, uint32_t height, uint32_t stride,
                  int quality = 85);
    ~JpegMtEncoder();

    JpegMtEncoder(const JpegMtEncoder&)            = delete;
    JpegMtEncoder& operator=(const JpegMtEncoder&) = delete;

    // Queue one NV12 frame for encoding. Non-blocking.
    // Returns false if the queue is full (frame silently dropped).
    bool encode(FramePtr frame);

    void set_output_ready_callback(OutputReadyCallback cb)
    {
        output_cb_ = std::move(cb);
    }

private:
    // ── Internal queue items ───────────────────────────────────────────────

    struct EncodeItem {
        FramePtr frame;
        uint64_t index;   // monotonically increasing frame number
    };

    struct OutputItem {
        std::vector<uint8_t> jpeg;
        uint64_t             index;
        int                  thread_id; // which thread produced this
    };

    // ── Worker functions ───────────────────────────────────────────────────

    void encode_thread_fn(int id);   // pops from enc_queue_, encodes, pushes to out_queues_[id]
    void output_thread_fn();         // reorders across per-thread queues, fires output_cb_

    // libjpeg raw_data_in encoding on pre-copied heap buffers.
    // Called after the FramePtr is released so the camera DMA buffer is free.
    void do_encode(jpeg_compress_struct& cinfo,
                   uint32_t W, uint32_t H,
                   const std::vector<uint8_t>& y_buf,
                   const std::vector<uint8_t>& cb_buf,
                   const std::vector<uint8_t>& cr_buf,
                   uint8_t*& out_buf, size_t& out_size);

    // ── Config ─────────────────────────────────────────────────────────────
    uint32_t width_, height_, stride_;
    int      quality_;

    // ── Frame index ────────────────────────────────────────────────────────
    std::atomic<uint64_t> next_index_{0};

    // ── Encode queue (shared by all encode threads) ────────────────────────
    std::mutex              enc_mutex_;
    std::condition_variable enc_cv_;
    std::queue<EncodeItem>  enc_queue_;
    std::atomic<bool>       abort_enc_{false};

    // ── Per-thread output queues (for in-order delivery) ──────────────────
    std::array<std::queue<OutputItem>, NUM_THREADS> out_queues_;
    std::mutex              out_mutex_;
    std::condition_variable out_cv_;
    std::atomic<bool>       abort_out_{false};

    // ── Threads ────────────────────────────────────────────────────────────
    std::array<std::thread, NUM_THREADS> enc_threads_;
    std::thread out_thread_;

    // ── Callback ───────────────────────────────────────────────────────────
    OutputReadyCallback output_cb_;
};
