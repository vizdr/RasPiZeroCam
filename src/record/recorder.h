// src/record/recorder.h
// Hardware H.264 recorder using:
//   - H264HardwareEncoder (V4L2 M2M, /dev/video11) for encoding
//   - GStreamer (appsrc → h264parse → mp4mux → filesink) for muxing only
//
// Zero-copy encoding path:
//   libcamera DMA buffer fd → H264HardwareEncoder::encode()
//     → VIDIOC_QBUF(OUTPUT, V4L2_MEMORY_DMABUF, fd)
//     → bcm2835-codec reads from physical RAM
//     → VIDIOC_DQBUF(CAPTURE) → H264 NAL data
//     → output_ready_callback_ → gst_app_src_push_buffer() (copy to GstBuffer)
//     → h264parse → mp4mux → filesink → SD card
//
// The DMA buffer is held by the FramePtr keepalive until VIDIOC_DQBUF(OUTPUT)
// confirms the encoder is done, then returned to libcamera.
//
// State machine: IDLE → RECORDING → STOPPING → IDLE
// Thread model:
//   start() / stop() / restart()  — command / main thread
//   push_frame()                  — camera consumer thread
//   H264HardwareEncoder internals — two private threads (poll + output)
//   GStreamer pipeline            — GStreamer internal threads

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "camera/frame_buffer.h"
#include "camera/resolution.h"
#include "record/h264_hw_encoder.h"

// Forward-declare GStreamer types
typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;

// ── State ──────────────────────────────────────────────────────────────────

enum class RecorderState : uint8_t {
    IDLE,
    RECORDING,
    STOPPING,
};

// ── Recorder ───────────────────────────────────────────────────────────────

class Recorder {
public:
    explicit Recorder(std::string recordings_dir = "recordings");
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    bool start(const ResolutionConfig& cfg, const std::string& filename = "");
    bool stop();
    bool restart(const ResolutionConfig& cfg);

    // ── Frame input ────────────────────────────────────────────────────────

    // Submit one NV12 frame for encoding. Non-blocking.
    // Returns false when not recording or if the encoder has no free slot.
    bool push_frame(FramePtr frame);

    // ── Status ─────────────────────────────────────────────────────────────

    RecorderState state()            const noexcept;
    bool          is_recording()     const noexcept;
    std::string   current_filename() const;
    std::string   next_filename()    const;

private:
    std::string      recordings_dir_;
    std::string      current_file_;
    ResolutionConfig cfg_{};

    // ── Hardware encoder ───────────────────────────────────────────────────
    std::unique_ptr<H264HardwareEncoder> hw_encoder_;

    // ── GStreamer mux pipeline (H264 → MP4) ────────────────────────────────
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_   = nullptr;
    GstBus*     bus_      = nullptr;

    // ── PTS tracking ───────────────────────────────────────────────────────
    // First encoded frame's timestamp (µs) used as the PTS zero point.
    int64_t base_ts_us_   = 0;
    bool    first_output_ = true;

    // ── State ──────────────────────────────────────────────────────────────
    mutable std::mutex         mtx_;
    std::atomic<RecorderState> state_{RecorderState::IDLE};

    // ── Helpers ────────────────────────────────────────────────────────────
    std::string build_pipeline_str(const ResolutionConfig& cfg,
                                   const std::string& path) const;
    void poll_bus();
    void teardown_gst();
};
