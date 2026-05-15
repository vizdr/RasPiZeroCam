// src/record/recorder.h
// GStreamer-based H.264/MP4 recorder.
//
// Design:
//   - Receives NV12 frames via push_frame() from the camera consumer thread.
//   - Wraps each frame's DMA pointer in a GstBuffer (zero-copy): the DMA
//     buffer is not returned to libcamera until GStreamer releases the buffer.
//   - Encodes with the Pi's V4L2 M2M hardware H.264 encoder (v4l2h264enc).
//   - Muxes into MP4 (moov atom at front → file readable while growing).
//   - State machine: IDLE → RECORDING → STOPPING → IDLE.
//
// Thread model:
//   start() / stop() / restart()  — call from command / main thread only.
//   push_frame()                  — call from camera consumer thread.
//   GStreamer's internal threads handle encoding and writing.
//
// GStreamer pipeline (built at start() time with actual width/height/fps):
//   appsrc name=src format=time is-live=true block=false
//     caps=video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1
//   ! v4l2h264enc extra-controls="controls,video_bitrate=BITRATE"
//   ! h264parse config-interval=-1
//   ! mp4mux faststart=true
//   ! filesink name=sink sync=false location=PATH

#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

// Forward-declare GStreamer types to avoid pulling all of GLib/GStreamer
// into every translation unit that includes this header.
typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;

#include "camera/frame_buffer.h"
#include "camera/resolution.h"

// ── State ──────────────────────────────────────────────────────────────────

enum class RecorderState : uint8_t {
    IDLE,       // no pipeline, no file
    RECORDING,  // pipeline running, push_frame() accepted
    STOPPING,   // EOS sent, draining
};

// ── Recorder ───────────────────────────────────────────────────────────────

class Recorder {
public:
    // recordings_dir: directory where MP4 files are created (auto-created).
    explicit Recorder(std::string recordings_dir = "recordings");
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    // Build and start the GStreamer pipeline.
    // cfg provides width/height/fps/bitrate for caps and encoder settings.
    // filename: use auto-generated name if empty.
    bool start(const ResolutionConfig& cfg,
               const std::string&      filename = "");

    // Send EOS, wait for the pipeline to flush and finalise the MP4, then
    // tear down. Blocks until the file is complete (max 10-second timeout).
    bool stop();

    // stop() followed by start() with the next auto-generated filename.
    bool restart(const ResolutionConfig& cfg);

    // ── Frame input ────────────────────────────────────────────────────────

    // Push one NV12 frame into the pipeline. Called from the consumer thread.
    // Returns false if not currently in RECORDING state (caller should check
    // is_recording() before calling in a tight loop, or tolerate false returns).
    bool push_frame(FramePtr frame);

    // ── Status ─────────────────────────────────────────────────────────────

    RecorderState state()            const noexcept;
    bool          is_recording()     const noexcept;
    std::string   current_filename() const;
    std::string   next_filename()    const;   // preview without starting

private:
    // ── Configuration ──────────────────────────────────────────────────────
    std::string      recordings_dir_;
    std::string      current_file_;
    ResolutionConfig cfg_{};

    // ── GStreamer objects ──────────────────────────────────────────────────
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_   = nullptr;
    GstBus*     bus_      = nullptr;

    // ── State ──────────────────────────────────────────────────────────────
    mutable std::mutex         mtx_;
    std::atomic<RecorderState> state_{RecorderState::IDLE};

    // ── Timestamp tracking ─────────────────────────────────────────────────
    // base_pts_: nanoseconds of the first frame's steady_clock timestamp.
    // GStreamer PTS = frame timestamp − base_pts_, so the first frame has PTS=0.
    uint64_t base_pts_    = 0;
    bool     first_frame_ = true;

    // ── Helpers ────────────────────────────────────────────────────────────
    std::string build_pipeline_str(const ResolutionConfig& cfg,
                                   const std::string&       path) const;
    void        poll_bus();    // check for errors/warnings, no blocking
    void        teardown();    // NULL state, unref all GStreamer objects
};
