// src/camera/frame_buffer.h
// Zero-copy frame queue for the libcamera pipeline.
//
// Design:
//   - Frame holds a non-owning pointer into DMA-mapped memory.
//     No pixel data is ever copied — only a shared_ptr<Frame> moves through the queue.
//   - When the last shared_ptr<Frame> is destroyed, Frame::release() is called,
//     which unmaps and returns the buffer to libcamera for reuse.
//   - FrameQueue<N> is a lock-free SPSC ring buffer (single producer = capture
//     thread, single consumer = encode / stream / record thread).
//     For fan-out to multiple consumers, hold the shared_ptr in a dispatcher
//     and push it into per-consumer queues before releasing your own reference.
//
// C++ standard: 17

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>

// ── Stream type ────────────────────────────────────────────────────────────
//
// Tags which libcamera stream produced this frame.
// Phase C: all frames are Video (single stream).
// Phase D: CameraManager adds a StillCapture stream; still frames carry
//          StreamType::Still so downstream consumers (Snapshot) can
//          distinguish them from the continuous video feed.

enum class StreamType : uint8_t {
    Video = 0,   // continuous video stream — recorder, MJPEG server
    Still = 1,   // single-shot still capture — snapshot encoder (Phase D)
};

// ── Pixel formats (FourCC values matching V4L2 / libcamera) ────────────────

enum class PixelFormat : uint32_t {
    YUYV   = 0x56595559,  // YUV 4:2:2 packed       — 2 bytes/pixel
    NV12   = 0x3231564E,  // YUV 4:2:0 semi-planar  — 1.5 bytes/pixel
    RGB888 = 0x33424752,  // 24-bit RGB packed       — 3 bytes/pixel
    MJPEG  = 0x47504A4D,  // JPEG compressed         — variable size
};

// ── Frame ──────────────────────────────────────────────────────────────────
//
// Non-copyable RAII wrapper around one camera buffer.
// Pixel data is owned by the DMA allocator; Frame merely views it.

struct Frame {
    // Non-owning pointer to DMA-mapped pixel data.
    // Lifetime is guaranteed for as long as this Frame object is alive.
    uint8_t* data      = nullptr;
    size_t   data_size = 0;

    // Geometry
    uint32_t    width   = 0;
    uint32_t    height  = 0;
    uint32_t    stride  = 0;       // bytes per row — may be > width × bpp
    PixelFormat format  = PixelFormat::NV12;

    // Timing
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;

    // Which libcamera stream this frame came from.
    // Default Video keeps Phase C code unchanged.
    StreamType stream_type = StreamType::Video;

    // Called on destruction — returns the DMA buffer to libcamera.
    // Set by the capture layer when creating the Frame.
    std::function<void()> release;

    ~Frame()
    {
        if (release)
            release();
    }

    // Non-copyable: two Frame objects must not release the same buffer.
    Frame()                        = default;
    Frame(const Frame&)            = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&)                 = default;
    Frame& operator=(Frame&&)      = default;

    // Convenience helpers
    bool   valid()  const noexcept { return data != nullptr; }
    size_t bytes()  const noexcept { return data_size; }
};

// ── FrameQueue<N> ──────────────────────────────────────────────────────────
//
// Lock-free single-producer / single-consumer ring buffer.
// N must be a power of 2. Capacity = N frames.
//
// Only shared_ptr<Frame> moves through the queue — pixel data never moves.
//
// Thread safety:
//   push()  — safe to call from one thread only (capture thread)
//   pop()   — safe to call from one thread only (consumer thread)
//   size() / empty() — approximate; do not use for synchronisation

template<size_t N>
class FrameQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0,
                  "FrameQueue<N>: N must be a power of 2 and >= 2");

public:
    // Producer — returns false (frame dropped) if the queue is full.
    bool push(std::shared_ptr<Frame> frame) noexcept
    {
        const size_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) == N)
            return false;                        // full — caller decides to drop or wait

        slots_[w & (N - 1)] = std::move(frame);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer — returns nullptr if the queue is empty.
    std::shared_ptr<Frame> pop() noexcept
    {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return nullptr;                      // empty

        auto frame = std::move(slots_[r & (N - 1)]);
        read_.store(r + 1, std::memory_order_release);
        return frame;
    }

    bool   empty() const noexcept { return size() == 0; }
    bool   full()  const noexcept { return size() == N; }
    size_t size()  const noexcept
    {
        return write_.load(std::memory_order_acquire) -
               read_ .load(std::memory_order_acquire);
    }

    static constexpr size_t capacity() noexcept { return N; }

private:
    std::array<std::shared_ptr<Frame>, N> slots_;

    // Placed on separate cache lines to prevent false sharing:
    //   capture thread writes write_, reads read_
    //   consumer thread writes read_,  reads write_
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_ {0};
};

// ── Convenience aliases ────────────────────────────────────────────────────

using FramePtr         = std::shared_ptr<Frame>;
using CameraFrameQueue = FrameQueue<8>;   // 8 slots — matches libcamera's typical buffer count

// Phase D: still stream only ever has 1 frame in flight at a time
// (one DMA buffer armed, one slot to hold the result until the consumer reads it).
using StillFrameQueue  = FrameQueue<2>;
