// src/stream/gst_camera_source.cpp

#include "gst_camera_source.h"

#include <iostream>
#include <sstream>
#include <cstdlib>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

// Forward declaration — definition follows on_jpeg_sample below
static GstFlowReturn on_new_sample_cb(GstAppSink* sink, void* userdata);

// ── Constructor / Destructor ───────────────────────────────────────────────

GstCameraSource::GstCameraSource(const ResolutionConfig& cfg, int jpeg_quality)
    : cfg_(cfg), jpeg_quality_(jpeg_quality)
{
    // Register user-installed libcamerasrc plugin directory so GStreamer
    // finds it even though it wasn't installed system-wide.
    // Path: ~/gst-plugins/libgstlibcamera.so (extracted from deb without sudo)
    const char* home = std::getenv("HOME");
    if (home) {
        const std::string plugin_dir = std::string(home) + "/gst-plugins";
        // Set before gst_init so the env var is honoured during initialisation
        std::string gst_path = plugin_dir;
        const char* existing = std::getenv("GST_PLUGIN_PATH");
        if (existing && *existing)
            gst_path += std::string(":") + existing;
        setenv("GST_PLUGIN_PATH", gst_path.c_str(), 1 /*overwrite*/);
    }

    gst_init(nullptr, nullptr);

    // Force-scan the user plugin directory so it's available even if
    // GStreamer's registry cache pre-dates our plugin installation.
    if (home) {
        const std::string plugin_dir = std::string(home) + "/gst-plugins";
        gst_registry_scan_path(gst_registry_get(), plugin_dir.c_str());
    }
}

GstCameraSource::~GstCameraSource()
{
    if (running_)
        stop();
}

// ── start() ────────────────────────────────────────────────────────────────

bool GstCameraSource::start()
{
    if (running_) return false;

    const std::string pipe_str = build_pipeline();
    std::cout << "GstCameraSource pipeline:\n  " << pipe_str << "\n";

    GError* err   = nullptr;
    pipeline_     = gst_parse_launch(pipe_str.c_str(), &err);
    if (!pipeline_ || err) {
        std::cerr << "GstCameraSource: gst_parse_launch failed: "
                  << (err ? err->message : "unknown") << "\n";
        if (err) g_error_free(err);
        return false;
    }

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    bus_     = gst_element_get_bus(pipeline_);
    if (!appsink_ || !bus_) {
        std::cerr << "GstCameraSource: failed to get appsink or bus\n";
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        return false;
    }

    // Wire the JPEG delivery callback (file-scope C function)
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = on_new_sample_cb;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &cbs, this, nullptr);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "GstCameraSource: pipeline failed to reach PLAYING\n";
        gst_object_unref(appsink_); appsink_ = nullptr;
        gst_object_unref(bus_);     bus_     = nullptr;
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        return false;
    }

    running_   = true;
    abort_bus_ = false;
    bus_thread_ = std::thread(&GstCameraSource::bus_thread_fn, this);

    std::cout << "GstCameraSource: streaming "
              << cfg_.label << " q=" << jpeg_quality_ << "\n";
    return true;
}

// ── stop() ─────────────────────────────────────────────────────────────────

void GstCameraSource::stop()
{
    if (!running_) return;
    running_   = false;
    abort_bus_ = true;

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (bus_thread_.joinable())
        bus_thread_.join();

    if (appsink_) { gst_object_unref(appsink_); appsink_ = nullptr; }
    if (bus_)     { gst_object_unref(bus_);     bus_     = nullptr; }
    if (pipeline_){ gst_object_unref(pipeline_); pipeline_ = nullptr; }

    std::cout << "GstCameraSource: stopped\n";
}

// ── Pipeline string ────────────────────────────────────────────────────────

std::string GstCameraSource::build_pipeline() const
{
    // libcamerasrc provides DMA-BUF NV12 frames from the ISP.
    // GStreamer negotiates memory:DMABuf caps with v4l2jpegenc, enabling
    // V4L2_MEMORY_DMABUF input mode — no copy into the hardware encoder.
    //
    // queue leaky=downstream: drop stale frames when appsink is busy;
    //   never block the ISP pipeline.
    // v4l2jpegenc: hardware JPEG encoder at /dev/video31 (bcm2835-codec).
    // appsink: delivers JPEG bytes to on_new_sample; drop=true keeps the
    //   latest frame instead of queuing old ones when HTTP is slow.
    std::ostringstream ss;
    ss << "libcamerasrc"
       << " ! video/x-raw,format=NV12"
       << ",width="     << cfg_.width
       << ",height="    << cfg_.height
       << ",framerate=" << cfg_.fps << "/1"
       << " ! queue max-size-buffers=2 leaky=downstream"
       << " ! v4l2jpegenc"
       << " extra-controls=\"controls,compression_quality=" << jpeg_quality_ << "\""
       << " ! appsink name=sink emit-signals=false sync=false"
       << " max-buffers=2 drop=true";
    return ss.str();
}

// ── appsink sample delivery ────────────────────────────────────────────────

// Public trampoline target — pulls the JPEG sample and fires the callback.
void GstCameraSource::on_jpeg_sample(GstAppSink* sink)
{
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo  map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        if (jpeg_cb_)
            jpeg_cb_(map.data, map.size);
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
}

// File-scope C trampoline — required because GstAppSinkCallbacks uses plain
// C function pointers (no std::function). Returns GST_FLOW_OK = 0.
static GstFlowReturn on_new_sample_cb(GstAppSink* sink, void* userdata)
{
    static_cast<GstCameraSource*>(userdata)->on_jpeg_sample(sink);
    return GST_FLOW_OK;
}

// ── Bus monitor thread ─────────────────────────────────────────────────────

void GstCameraSource::bus_thread_fn()
{
    while (!abort_bus_) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus_,
            200 * GST_MSECOND,
            static_cast<GstMessageType>(
                GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
        if (!msg) continue;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* gerr = nullptr;
            gchar*  dbg  = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            std::cerr << "GstCameraSource error: " << gerr->message << "\n";
            if (dbg) std::cerr << "  debug: " << dbg << "\n";
            g_error_free(gerr);
            g_free(dbg);
            running_ = false;   // signal main thread
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* gerr = nullptr;
            gst_message_parse_warning(msg, &gerr, nullptr);
            std::cerr << "GstCameraSource warning: " << gerr->message << "\n";
            g_error_free(gerr);
            break;
        }
        case GST_MESSAGE_EOS:
            std::cout << "GstCameraSource: EOS\n";
            running_ = false;
            break;
        default: break;
        }
        gst_message_unref(msg);
    }
}
