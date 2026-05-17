// src/stream/gst_camera_source.h
// GStreamer-based camera source with MJPEG streaming, H264 recording and snapshot.
//
// Pipeline (Phase 4 — tee with two branches):
//   libcamerasrc
//   ! video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1
//   ! tee name=t
//   t. ! queue leaky=downstream
//      ! v4l2jpegenc compression_quality=Q     (HW JPEG, DMA-BUF, 30fps)
//      ! appsink name=mjpeg_sink               → MjpegServer + snapshot
//   t. ! queue leaky=downstream max-size-buffers=4
//      ! valve name=rec_valve drop=true        (gate — open on record_start)
//      ! videoconvert
//      ! openh264enc                           (SW H264 — HW h264enc fails alongside HW JPEG)
//      ! h264parse ! mp4mux name=muxer
//      ! filesink name=rec_sink sync=false
//
// Why openh264enc instead of v4l2h264enc for recording:
//   The bcm2835-codec cannot serve two simultaneous V4L2 M2M DMA-BUF consumers
//   from the same ISP tee (validated: v4l2h264enc fails alongside v4l2jpegenc).
//   openh264enc uses ~25-35% of one Cortex-A53 core at 640×480 — manageable.
//   See BottlenecksJPEG.md for full analysis.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "camera/resolution.h"
#include "record/recorder.h"

typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;
typedef struct _GstAppSink GstAppSink;

class GstCameraSource {
public:
    using JpegCallback = std::function<void(const uint8_t* data, size_t size)>;

    GstCameraSource(const ResolutionConfig& cfg, int jpeg_quality = 85);
    ~GstCameraSource();

    GstCameraSource(const GstCameraSource&)            = delete;
    GstCameraSource& operator=(const GstCameraSource&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────
    bool start();
    void stop();
    bool is_running() const noexcept { return running_.load(); }

    // Change resolution at runtime — stops recording, rebuilds pipeline (~2s gap).
    bool set_resolution(const ResolutionConfig& cfg);
    const ResolutionConfig& current_config() const noexcept { return cfg_; }

    void set_jpeg_callback(JpegCallback cb) { jpeg_cb_ = std::move(cb); }

    // ── Recording ──────────────────────────────────────────────────────────
    // Open the recording valve and start writing H264 MP4 to filepath.
    bool start_recording(const std::string& filepath);

    // Close the valve, flush mp4mux (EOS injection), finalize the file.
    // Blocks up to 5 s waiting for the muxer to write the moov atom.
    bool stop_recording();

    bool        is_recording()      const noexcept { return recording_.load(); }
    std::string current_recording() const;
    int         recording_seconds() const;   // elapsed seconds since start

    // ── Snapshot ───────────────────────────────────────────────────────────
    // Capture the next JPEG frame from the MJPEG appsink and write to filepath.
    // Blocks up to 200 ms (one frame at 30fps ≈ 33 ms).
    bool take_snapshot(const std::string& filepath);

    // Called by the file-scope appsink trampoline — must be public.
    void on_jpeg_sample(GstAppSink* sink);

private:
    std::string build_pipeline() const;
    void        bus_thread_fn();
    void        rec_drain_thread_fn();   // pulls NV12 from rec_sink, feeds Recorder

    ResolutionConfig cfg_;
    int              jpeg_quality_;
    JpegCallback     jpeg_cb_;

    // ── GStreamer elements ─────────────────────────────────────────────────
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_  = nullptr;   // mjpeg_sink  — JPEG frames
    GstElement* rec_sink_ = nullptr;   // rec_sink    — NV12 frames for Recorder
    GstBus*     bus_      = nullptr;

    std::thread       bus_thread_;
    std::atomic<bool> abort_bus_{false};
    std::atomic<bool> running_  {false};

    // ── Recording state ────────────────────────────────────────────────────
    // Recording is delegated to the existing Recorder class (separate GStreamer
    // pipeline: appsrc → openh264enc → mp4mux → filesink). This avoids the
    // GStreamer filesink location-change-while-PLAYING limitation.
    std::unique_ptr<Recorder>              recorder_;
    std::atomic<bool>                      recording_{false};
    std::string                            current_file_;
    std::chrono::steady_clock::time_point  rec_start_;
    mutable std::mutex                     rec_file_mutex_;

    // Background thread that drains the NV12 appsink and feeds Recorder
    std::thread       rec_thread_;
    std::atomic<bool> abort_rec_{false};

    // ── Snapshot state ─────────────────────────────────────────────────────
    std::atomic<bool> snapshot_pending_{false};
    std::string       snapshot_path_;
    std::mutex        snapshot_mutex_;
};
