// src/camera/camera_manager.h
// libcamera wrapper — captures frames and pushes them zero-copy into a FrameQueue.
//
// Responsibilities:
//   - Owns the libcamera CameraManager and Camera lifecycle
//   - Allocates and mmaps DMA buffers once at open()
//   - Converts each completed libcamera Request into a shared_ptr<Frame>
//     whose release() callback re-queues the DMA buffer to the camera
//   - Pushes frames into the FrameQueue for downstream consumers
//     (MJPEG server, recorder, snapshot) — no pixel data is copied
//
// Thread model:
//   - open() / close() / set_resolution() — call from main thread only
//   - requestCompleted callback — fired by libcamera's internal thread;
//     push() into FrameQueue is lock-free SPSC-safe

#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <string>

#include <libcamera/libcamera.h>

#include "frame_buffer.h"
#include "resolution.h"

// ── MappedBuffer ───────────────────────────────────────────────────────────
// Holds the mmap'd address and length of one DMA plane.
// Lifetime is tied to the FrameBufferAllocator — valid between open() and close().

struct MappedBuffer {
    uint8_t* ptr    = nullptr;
    size_t   length = 0;
};

// ── CameraManager ──────────────────────────────────────────────────────────

class CameraManager {
public:
    // queue — the SPSC ring buffer shared with downstream consumers.
    // Frames dropped by push() (queue full) are silently re-queued to libcamera.
    explicit CameraManager(CameraFrameQueue& queue);
    ~CameraManager();

    // Non-copyable, non-movable — owns OS resources (fds, mmap regions)
    CameraManager(const CameraManager&)            = delete;
    CameraManager& operator=(const CameraManager&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    // Discover the camera, configure the requested resolution, allocate
    // DMA buffers, mmap them, create requests, and start streaming.
    // Returns false if no camera is found or configuration fails.
    bool open(Resolution res = Resolution::MEDIUM);

    // Stop streaming, unmap buffers, release the camera.
    void close();

    // Tear down and restart with a new resolution.
    // Blocks until all in-flight requests are drained before reconfiguring.
    bool set_resolution(Resolution res);

    // ── Status ─────────────────────────────────────────────────────────────

    bool       is_running()         const noexcept { return running_; }
    Resolution current_resolution() const noexcept { return resolution_; }

    const ResolutionConfig& current_config() const noexcept
    {
        return config_of(resolution_);
    }

private:
    // ── libcamera objects ──────────────────────────────────────────────────
    std::unique_ptr<libcamera::CameraManager>      cam_manager_;
    std::shared_ptr<libcamera::Camera>             camera_;
    std::unique_ptr<libcamera::CameraConfiguration> cam_config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream*                             stream_  = nullptr;

    // ── Per-buffer mmap regions ────────────────────────────────────────────
    // Key: raw FrameBuffer pointer (stable for the lifetime of the allocator)
    std::unordered_map<libcamera::FrameBuffer*, MappedBuffer> mapped_;

    // ── Pending requests ───────────────────────────────────────────────────
    std::vector<std::unique_ptr<libcamera::Request>> requests_;

    // ── Downstream frame queue (not owned) ────────────────────────────────
    CameraFrameQueue& queue_;

    // ── State ──────────────────────────────────────────────────────────────
    std::atomic<bool> running_    {false};
    Resolution        resolution_ {Resolution::MEDIUM};
    uint32_t          stride_     {0};   // bytes per row — set by configure_stream()

    // ── Internal helpers ───────────────────────────────────────────────────

    // Steps 3a–3e — called in order by open()
    bool init_camera_manager();
    bool acquire_camera();
    bool configure_stream(const ResolutionConfig& cfg);
    bool allocate_and_map_buffers();
    bool create_requests();
    void start_streaming();

    // Step 3f — close helpers
    void stop_streaming();
    void unmap_buffers();
    void release_camera();

    // libcamera signal handler — called on libcamera's internal thread
    void on_request_completed(libcamera::Request* request);

    // Re-queue a completed request so the camera can reuse the buffer
    void requeue_request(libcamera::Request* request);
};
