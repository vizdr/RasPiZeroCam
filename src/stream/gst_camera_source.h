// src/stream/gst_camera_source.h
// GStreamer-based camera source that delivers hardware-encoded JPEG frames.
//
// Pipeline:
//   libcamerasrc
//   ! video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1
//   ! queue max-size-buffers=2 leaky=downstream
//   ! v4l2jpegenc extra-controls="controls,compression_quality=Q"
//   ! appsink name=sink sync=false max-buffers=2 drop=true
//
// Why this achieves 30fps where our manual approach hit 16fps:
//   GStreamer negotiates memory:DMABuf caps between libcamerasrc and
//   v4l2jpegenc at pipeline start. This puts v4l2jpegenc into
//   V4L2_MEMORY_DMABUF input mode — the camera's DMA frames go directly
//   into the hardware JPEG encoder without any copy or conversion.
//   libcamerasrc also manages the ISP buffer lifecycle correctly, allowing
//   the ISP to run at its native 30fps.
//
// libcamerasrc is installed without sudo by extracting from the deb package
// into ~/gst-plugins/. The GstCameraSource constructor registers that path
// with the GStreamer registry before starting the pipeline.
//
// Thread model:
//   start() / stop() — call from main thread
//   jpeg_callback    — fired from GStreamer's internal appsink thread

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "camera/resolution.h"

// Forward-declare GStreamer struct types (avoids pulling all of GLib into
// every translation unit that includes this header).
typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;
typedef struct _GstAppSink GstAppSink;
// GstFlowReturn is an enum used only in private static callback — declared
// in .cpp where gst.h is fully included.

class GstCameraSource {
public:
    // Fires from GStreamer's appsink thread when a hardware-encoded JPEG
    // frame is ready. `data` is valid only during the call — copy it.
    using JpegCallback = std::function<void(const uint8_t* data, size_t size)>;

    GstCameraSource(const ResolutionConfig& cfg, int jpeg_quality = 85);
    ~GstCameraSource();

    GstCameraSource(const GstCameraSource&)            = delete;
    GstCameraSource& operator=(const GstCameraSource&) = delete;

    // Build the GStreamer pipeline and start streaming.
    bool start();

    // Stop streaming and tear down the pipeline.
    void stop();

    bool is_running() const noexcept { return running_.load(); }

    void set_jpeg_callback(JpegCallback cb) { jpeg_cb_ = std::move(cb); }

    // Called by the file-scope GstAppSinkCallbacks trampoline in .cpp.
    // Must be public so the trampoline (a non-member function) can reach it.
    void on_jpeg_sample(GstAppSink* sink);

private:
    std::string build_pipeline() const;


    // Bus monitor thread — logs errors/warnings, triggers stop on EOS or error
    void bus_thread_fn();

    ResolutionConfig cfg_;
    int              jpeg_quality_;
    JpegCallback     jpeg_cb_;

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_  = nullptr;
    GstBus*     bus_      = nullptr;

    std::thread       bus_thread_;
    std::atomic<bool> abort_bus_{false};
    std::atomic<bool> running_   {false};
};
