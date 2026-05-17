// src/record/recorder.cpp

#include "recorder.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <filesystem>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

// ── Constructor / Destructor ───────────────────────────────────────────────

Recorder::Recorder(std::string recordings_dir)
    : recordings_dir_(std::move(recordings_dir))
{
    gst_init(nullptr, nullptr);
}

Recorder::~Recorder()
{
    if (state_.load(std::memory_order_acquire) != RecorderState::IDLE)
        stop();
}

// ── start() ────────────────────────────────────────────────────────────────

bool Recorder::start(const ResolutionConfig& cfg, const std::string& filename)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (state_.load(std::memory_order_relaxed) != RecorderState::IDLE) {
        std::cerr << "Recorder::start() called in non-IDLE state\n";
        return false;
    }

    std::filesystem::create_directories(recordings_dir_);

    cfg_          = cfg;
    current_file_ = filename.empty() ? next_filename() : filename;
    first_output_ = true;
    base_ts_us_   = 0;

    // ── Start hardware encoder ─────────────────────────────────────────────
    try {
        hw_encoder_ = std::make_unique<H264HardwareEncoder>(
            cfg_, cfg_.bitrate_kbps * 1000);
    } catch (const std::exception& e) {
        std::cerr << "Recorder: hardware encoder failed: " << e.what() << "\n";
        return false;
    }

    // ── Build GStreamer mux pipeline (no encoder element) ──────────────────
    // appsrc receives pre-encoded H264 byte-stream NAL units from the V4L2 encoder.
    const std::string pipe_str = build_pipeline_str(cfg_, current_file_);
    std::cout << "Recorder pipeline: " << pipe_str << "\n";

    GError* err = nullptr;
    pipeline_   = gst_parse_launch(pipe_str.c_str(), &err);

    if (!pipeline_ || err) {
        std::cerr << "Recorder: gst_parse_launch failed: "
                  << (err ? err->message : "unknown") << "\n";
        if (err) g_error_free(err);
        hw_encoder_.reset();
        teardown_gst();
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    bus_    = gst_element_get_bus(pipeline_);

    if (!appsrc_ || !bus_) {
        std::cerr << "Recorder: failed to get appsrc or bus\n";
        hw_encoder_.reset();
        teardown_gst();
        return false;
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Recorder: pipeline failed to reach PLAYING\n";
        hw_encoder_.reset();
        teardown_gst();
        return false;
    }

    // ── Wire encoder output → GStreamer appsrc ────────────────────────────
    hw_encoder_->set_output_ready_callback(
        [this](const uint8_t* data, size_t size, int64_t ts_us, bool /*keyframe*/) {

        poll_bus();  // check for pipeline errors

        // PTS relative to first encoded frame
        if (first_output_) {
            base_ts_us_  = ts_us;
            first_output_ = false;
        }
        const int64_t pts_ns = (ts_us - base_ts_us_) * 1000;

        // Copy H264 NAL data into a new GstBuffer
        // (capture buffer must be re-queued immediately after callback returns)
        GstBuffer* buf = gst_buffer_new_and_alloc(static_cast<gsize>(size));
        gst_buffer_fill(buf, 0, data, size);
        GST_BUFFER_PTS(buf)      = static_cast<GstClockTime>(pts_ns);
        GST_BUFFER_DURATION(buf) = GST_SECOND / cfg_.fps;

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
        if (ret != GST_FLOW_OK)
            std::cerr << "Recorder: appsrc push returned " << ret << "\n";
    });

    // input_done_callback: FramePtr drops here → Frame::release() → libcamera requeue
    hw_encoder_->set_input_done_callback([](FramePtr /*frame*/) {
        // frame destroyed on scope exit
    });

    state_.store(RecorderState::RECORDING, std::memory_order_release);
    std::cout << "Recorder: started → " << current_file_ << "\n";
    return true;
}

// ── stop() ─────────────────────────────────────────────────────────────────

bool Recorder::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (state_.load(std::memory_order_relaxed) != RecorderState::RECORDING) {
        std::cerr << "Recorder::stop() called in non-RECORDING state\n";
        return false;
    }

    state_.store(RecorderState::STOPPING, std::memory_order_release);

    // Destroy hardware encoder: blocks until all in-flight frames are drained
    // (pollThread waits for all NUM_OUTPUT_BUFFERS slots to return before exiting).
    hw_encoder_.reset();

    // Signal end of stream and wait for mp4mux to finalise the moov atom
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));

    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus_, 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));

    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* gerr = nullptr;
            gst_message_parse_error(msg, &gerr, nullptr);
            std::cerr << "Recorder: pipeline error during stop: "
                      << gerr->message << "\n";
            g_error_free(gerr);
        }
        gst_message_unref(msg);
    } else {
        std::cerr << "Recorder: EOS timed out — file may be truncated\n";
    }

    teardown_gst();
    state_.store(RecorderState::IDLE, std::memory_order_release);
    std::cout << "Recorder: stopped → " << current_file_ << "\n";
    return true;
}

// ── restart() ──────────────────────────────────────────────────────────────

bool Recorder::restart(const ResolutionConfig& cfg)
{
    stop();
    return start(cfg);
}

// ── push_frame() ───────────────────────────────────────────────────────────

bool Recorder::push_frame(FramePtr frame)
{
    if (state_.load(std::memory_order_acquire) != RecorderState::RECORDING)
        return false;
    if (!hw_encoder_ || frame->dma_fd < 0)
        return false;

    const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame->timestamp.time_since_epoch()).count();

    return hw_encoder_->encode(frame->dma_fd, frame->data_size, ts_us, frame);
}

// ── Status ─────────────────────────────────────────────────────────────────

RecorderState Recorder::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

bool Recorder::is_recording() const noexcept
{
    return state_.load(std::memory_order_acquire) == RecorderState::RECORDING;
}

std::string Recorder::current_filename() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return current_file_;
}

std::string Recorder::next_filename() const
{
    const auto now  = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "rec_%Y%m%d_%H%M%S.mp4", &tm);
    return recordings_dir_ + "/" + buf;
}

// ── GStreamer mux pipeline (H264 byte-stream → MP4) ────────────────────────

std::string Recorder::build_pipeline_str(const ResolutionConfig& cfg,
                                          const std::string& path) const
{
    // appsrc receives pre-encoded H264 NAL units from H264HardwareEncoder.
    // No encoder element in this pipeline — encoding is done by /dev/video11.
    std::ostringstream ss;
    ss << "appsrc name=src format=time is-live=true block=false"
       << " caps=video/x-h264"
       << ",stream-format=byte-stream"
       << ",alignment=au"
       << ",width="     << cfg.width
       << ",height="    << cfg.height
       << ",framerate=" << cfg.fps << "/1"
       << " ! h264parse"
       << " ! mp4mux"
       << " ! filesink name=sink sync=false location=" << path;
    return ss.str();
}

// ── GStreamer helpers ──────────────────────────────────────────────────────

void Recorder::poll_bus()
{
    if (!bus_) return;
    GstMessage* msg = gst_bus_pop_filtered(
        bus_,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
    if (!msg) return;

    GError* gerr = nullptr;
    gchar*  dbg  = nullptr;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(msg, &gerr, &dbg);
        std::cerr << "Recorder pipeline error: " << gerr->message << "\n";
        if (dbg) std::cerr << "  debug: " << dbg << "\n";
        state_.store(RecorderState::IDLE, std::memory_order_release);
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(msg, &gerr, &dbg);
        std::cerr << "Recorder pipeline warning: " << gerr->message << "\n";
        break;
    default: break;
    }

    if (gerr) g_error_free(gerr);
    if (dbg)  g_free(dbg);
    gst_message_unref(msg);
}

void Recorder::teardown_gst()
{
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsrc_) { gst_object_unref(appsrc_);   appsrc_   = nullptr; }
    if (bus_)    { gst_object_unref(bus_);       bus_      = nullptr; }
    if (pipeline_){ gst_object_unref(pipeline_); pipeline_ = nullptr; }
}
