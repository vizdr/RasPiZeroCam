// src/stream/gst_camera_source.cpp

#include "gst_camera_source.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "camera/frame_buffer.h"

using namespace std::chrono_literals;

// Forward declaration — trampoline defined after on_jpeg_sample
static GstFlowReturn on_new_sample_cb(GstAppSink* sink, void* userdata);

// ── Constructor / Destructor ───────────────────────────────────────────────

GstCameraSource::GstCameraSource(const ResolutionConfig& cfg, int jpeg_quality)
    : cfg_(cfg), jpeg_quality_(jpeg_quality)
{
    const char* home = std::getenv("HOME");
    if (home) {
        const std::string plugin_dir = std::string(home) + "/gst-plugins";
        std::string gst_path = plugin_dir;
        const char* existing = std::getenv("GST_PLUGIN_PATH");
        if (existing && *existing)
            gst_path += std::string(":") + existing;
        setenv("GST_PLUGIN_PATH", gst_path.c_str(), 1);
    }

    gst_init(nullptr, nullptr);

    if (home) {
        gst_registry_scan_path(gst_registry_get(),
            (std::string(home) + "/gst-plugins").c_str());
    }
}

GstCameraSource::~GstCameraSource()
{
    if (running_) stop();
}

// ── build_pipeline() ───────────────────────────────────────────────────────
//
// tee with two branches:
//   Branch 1 (MJPEG): v4l2jpegenc → appsink  — hardware JPEG, 30fps, 0 CPU
//   Branch 2 (Record): valve → videoconvert → openh264enc → mp4mux → filesink
//
// The valve starts closed (drop=true). start_recording() opens it.
// openh264enc is used instead of v4l2h264enc because the bcm2835-codec
// cannot serve two simultaneous V4L2 M2M consumers from the same tee.

std::string GstCameraSource::build_pipeline() const
{
    // Two-branch tee:
    //   Branch 1 (MJPEG):   v4l2jpegenc → appsink  — HW JPEG, 30fps, 0 CPU
    //   Branch 2 (NV12 raw): appsink                — raw NV12 for Recorder
    //
    // Note: v4l2h264enc fails alongside v4l2jpegenc (both use bcm2835-codec
    // and cannot share the same ISP tee DMA-BUF stream). Recording uses the
    // existing Recorder class (openh264enc SW encoder in a separate pipeline).
    // GStreamer filesink cannot change location while PLAYING, so we avoid
    // embedding the recorder into this pipeline altogether.
    std::ostringstream ss;
    ss << "libcamerasrc"
       << " ! video/x-raw,format=NV12"
       << ",width="     << cfg_.width
       << ",height="    << cfg_.height
       << ",framerate=" << cfg_.fps << "/1"
       << " ! tee name=t"

       // Branch 1: MJPEG (HW JPEG encoder)
       << " t. ! queue max-size-buffers=2 leaky=downstream"
       << " ! v4l2jpegenc"
       << " extra-controls=\"controls,compression_quality=" << jpeg_quality_ << "\""
       << " ! appsink name=mjpeg_sink emit-signals=false sync=false"
       << " max-buffers=2 drop=true"

       // Branch 2: Raw NV12 for software recording
       // leaky=downstream: drop old frames if recorder is slow — never stall MJPEG
       << " t. ! queue max-size-buffers=4 leaky=downstream"
       << " ! appsink name=rec_sink emit-signals=false sync=false"
       << " max-buffers=2 drop=true";

    return ss.str();
}

// ── start() ────────────────────────────────────────────────────────────────

bool GstCameraSource::start()
{
    if (running_) return false;

    const std::string pipe_str = build_pipeline();
    std::cout << "GstCameraSource pipeline:\n  " << pipe_str << "\n";

    GError* err = nullptr;
    pipeline_   = gst_parse_launch(pipe_str.c_str(), &err);
    if (!pipeline_ || err) {
        std::cerr << "GstCameraSource: gst_parse_launch failed: "
                  << (err ? err->message : "unknown") << "\n";
        if (err) g_error_free(err);
        return false;
    }

    // Retrieve named elements
    appsink_  = gst_bin_get_by_name(GST_BIN(pipeline_), "mjpeg_sink");
    rec_sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "rec_sink");
    bus_      = gst_element_get_bus(pipeline_);

    if (!appsink_ || !rec_sink_ || !bus_) {
        std::cerr << "GstCameraSource: failed to retrieve pipeline elements\n";
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        return false;
    }

    // Wire MJPEG delivery callback
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = on_new_sample_cb;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &cbs, this, nullptr);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "GstCameraSource: pipeline failed to reach PLAYING\n";
        if (appsink_)  gst_object_unref(appsink_);
        if (rec_sink_) gst_object_unref(rec_sink_);
        if (bus_)      gst_object_unref(bus_);
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = rec_sink_ = nullptr;
        bus_ = nullptr;
        return false;
    }

    // Start the NV12 drain thread (feeds Recorder when recording is active)
    abort_rec_  = false;
    rec_thread_ = std::thread(&GstCameraSource::rec_drain_thread_fn, this);

    running_   = true;
    abort_bus_ = false;
    bus_thread_ = std::thread(&GstCameraSource::bus_thread_fn, this);

    std::cout << "GstCameraSource: streaming " << cfg_.label
              << " q=" << jpeg_quality_ << "\n";
    return true;
}

// ── stop() ─────────────────────────────────────────────────────────────────

void GstCameraSource::stop()
{
    if (!running_) return;

    // Finalize any active recording first
    if (recording_) stop_recording();

    running_   = false;
    abort_bus_ = true;
    abort_rec_ = true;

    if (rec_thread_.joinable()) rec_thread_.join();

    if (pipeline_)
        gst_element_set_state(pipeline_, GST_STATE_NULL);

    if (bus_thread_.joinable()) bus_thread_.join();

    if (appsink_)  { gst_object_unref(appsink_);  appsink_  = nullptr; }
    if (rec_sink_) { gst_object_unref(rec_sink_);  rec_sink_ = nullptr; }
    if (bus_)      { gst_object_unref(bus_);        bus_      = nullptr; }
    if (pipeline_) { gst_object_unref(pipeline_);  pipeline_ = nullptr; }

    std::cout << "GstCameraSource: stopped\n";
}

// ── NV12 drain thread ──────────────────────────────────────────────────────
//
// Continuously pulls NV12 GstSamples from the tee's rec_sink appsink.
// When recording is active, wraps each sample in a FramePtr and pushes it
// to the Recorder (which handles H264 encoding in its own GStreamer pipeline).

void GstCameraSource::rec_drain_thread_fn()
{
    while (!abort_rec_) {
        // Non-blocking pull — returns nullptr immediately if no sample ready
        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(rec_sink_), 20 * GST_MSECOND);

        if (!sample) continue;

        if (recording_ && recorder_) {
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                // Wrap NV12 data in a FramePtr the Recorder can accept.
                // We keep ONE extra ref in the lambda for cleanup.
                // The original ref from try_pull_sample is released below.
                GstSample* held = gst_sample_ref(sample);   // +1 for lambda
                GstBuffer* hbuf = buf;
                GstMapInfo hmap = map;

                auto frame        = std::make_shared<Frame>();
                frame->data       = map.data;
                frame->data_size  = map.size;
                frame->width      = cfg_.width;
                frame->height     = cfg_.height;
                frame->stride     = cfg_.width;   // NV12 packed stride = width
                frame->format     = PixelFormat::NV12;
                frame->dma_fd     = -1;           // system memory (not DMA-BUF)
                // Use buffer PTS for timestamp (libcamerasrc sets this from CLOCK_MONOTONIC)
                const GstClockTime pts = GST_BUFFER_PTS(buf);
                frame->timestamp  = std::chrono::steady_clock::time_point(
                    std::chrono::nanoseconds(GST_CLOCK_TIME_IS_VALID(pts) ? pts :
                        static_cast<GstClockTime>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count())));
                frame->release    = [held, hbuf, hmap]() mutable {
                    gst_buffer_unmap(hbuf, &hmap);
                    gst_sample_unref(held);   // releases the lambda's ref
                };

                recorder_->push_frame(frame);
                // Release the original ref — lambda holds the cleanup ref
                gst_sample_unref(sample);
            } else {
                gst_sample_unref(sample);
            }
        } else {
            gst_sample_unref(sample);
        }
    }
}

// ── set_resolution() ───────────────────────────────────────────────────────

bool GstCameraSource::set_resolution(const ResolutionConfig& cfg)
{
    if (recording_) stop_recording();
    stop();
    cfg_ = cfg;
    std::cout << "GstCameraSource: switching to " << cfg_.label << "\n";
    return start();
}

// ── start_recording() ──────────────────────────────────────────────────────
//
// Creates a fresh Recorder (which starts its own GStreamer recording pipeline)
// and enables the NV12 drain thread to feed it frames.

bool GstCameraSource::start_recording(const std::string& filepath)
{
    if (!running_ || recording_) return false;

    recorder_ = std::make_unique<Recorder>("recordings");
    // sw_encoder=true: NV12 system-memory frames from the tee appsink
    if (!recorder_->start(cfg_, filepath, /*sw_encoder=*/true)) {
        std::cerr << "GstCameraSource: Recorder failed to start\n";
        recorder_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(rec_file_mutex_);
        current_file_ = filepath;
        rec_start_    = std::chrono::steady_clock::now();
    }
    recording_ = true;

    std::cout << "GstCameraSource: recording → " << filepath << "\n";
    return true;
}

// ── stop_recording() ───────────────────────────────────────────────────────

bool GstCameraSource::stop_recording()
{
    if (!recording_) return false;
    recording_ = false;

    // Stop the Recorder — sends EOS to its pipeline, waits for moov atom
    if (recorder_) {
        recorder_->stop();
        recorder_.reset();
    }

    const std::string file = current_recording();
    std::cout << "GstCameraSource: recording stopped → " << file << "\n";
    return true;
}

// ── Recording helpers ──────────────────────────────────────────────────────

std::string GstCameraSource::current_recording() const
{
    std::lock_guard<std::mutex> lk(rec_file_mutex_);
    return current_file_;
}

int GstCameraSource::recording_seconds() const
{
    if (!recording_) return 0;
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - rec_start_).count());
}

// ── take_snapshot() ────────────────────────────────────────────────────────
//
// Arms snapshot_pending_ and waits for the next MJPEG frame to arrive in
// on_jpeg_sample(). The frame is written to filepath. Blocks up to 200 ms.

bool GstCameraSource::take_snapshot(const std::string& filepath)
{
    if (!running_) return false;

    {
        std::lock_guard<std::mutex> lk(snapshot_mutex_);
        snapshot_path_ = filepath;
    }
    snapshot_pending_ = true;

    // Wait up to 500 ms — longer when recording is active (CPU load)
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (snapshot_pending_.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    if (snapshot_pending_.load()) {
        snapshot_pending_ = false;   // cancel if timed out
        return false;
    }
    return true;
}

// ── MJPEG appsink delivery ─────────────────────────────────────────────────

void GstCameraSource::on_jpeg_sample(GstAppSink* sink)
{
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo  map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {

        // Deliver to MjpegServer
        if (jpeg_cb_)
            jpeg_cb_(map.data, map.size);

        // Opportunistic snapshot: save this frame if one is requested
        if (snapshot_pending_.load()) {
            std::lock_guard<std::mutex> lk(snapshot_mutex_);
            if (!snapshot_path_.empty()) {
                std::ofstream f(snapshot_path_, std::ios::binary);
                if (f) f.write(reinterpret_cast<const char*>(map.data), map.size);
                snapshot_path_.clear();
            }
            snapshot_pending_ = false;
        }

        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
}

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
            bus_, 200 * GST_MSECOND,
            static_cast<GstMessageType>(
                GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
        if (!msg) continue;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* gerr = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            std::cerr << "GstCameraSource error: " << gerr->message << "\n";
            if (dbg) std::cerr << "  debug: " << dbg << "\n";
            g_error_free(gerr); g_free(dbg);
            running_ = false;
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
            std::cout << "GstCameraSource: pipeline EOS — stopping\n";
            running_ = false;
            break;
        default: break;
        }
        gst_message_unref(msg);
    }
}
