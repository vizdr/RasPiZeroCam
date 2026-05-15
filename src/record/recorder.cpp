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
    // Safe to call multiple times — GStreamer is a no-op after first init.
    gst_init(nullptr, nullptr);
}

Recorder::~Recorder()
{
    if (state_.load(std::memory_order_acquire) != RecorderState::IDLE)
        stop();
}

// ── Public API ─────────────────────────────────────────────────────────────

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
    first_frame_  = true;
    base_pts_     = 0;

    // ── Build and parse the pipeline ───────────────────────────────────────
    const std::string pipe_str = build_pipeline_str(cfg_, current_file_);

    GError*     err  = nullptr;
    pipeline_        = gst_parse_launch(pipe_str.c_str(), &err);

    if (!pipeline_ || err) {
        std::cerr << "Recorder: gst_parse_launch failed: "
                  << (err ? err->message : "unknown error") << "\n";
        if (err) g_error_free(err);
        teardown();
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    bus_    = gst_element_get_bus(pipeline_);

    if (!appsrc_ || !bus_) {
        std::cerr << "Recorder: could not retrieve appsrc or bus from pipeline\n";
        teardown();
        return false;
    }

    // ── Start pipeline ─────────────────────────────────────────────────────
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Recorder: failed to set pipeline to PLAYING\n";
        teardown();
        return false;
    }

    state_.store(RecorderState::RECORDING, std::memory_order_release);
    std::cout << "Recorder: started → " << current_file_ << "\n";
    return true;
}

bool Recorder::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (state_.load(std::memory_order_relaxed) != RecorderState::RECORDING) {
        std::cerr << "Recorder::stop() called in non-RECORDING state\n";
        return false;
    }

    state_.store(RecorderState::STOPPING, std::memory_order_release);

    // ── Signal end of stream ───────────────────────────────────────────────
    // Tells the pipeline there are no more frames; it will flush, finalize
    // the MP4 moov atom, and emit GST_MESSAGE_EOS on the bus.
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));

    // ── Wait for EOS (10-second timeout) ──────────────────────────────────
    // Skipping this wait corrupts the MP4 — the moov atom would be missing.
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus_,
        10 * GST_SECOND,
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
        std::cerr << "Recorder: EOS timed out — " << current_file_
                  << " may be truncated\n";
    }

    teardown();
    state_.store(RecorderState::IDLE, std::memory_order_release);
    std::cout << "Recorder: stopped → " << current_file_ << "\n";
    return true;
}

bool Recorder::restart(const ResolutionConfig& cfg)
{
    // stop() and start() each acquire mtx_ independently — no nested locking.
    stop();
    return start(cfg);
}

// ── Frame input ────────────────────────────────────────────────────────────

bool Recorder::push_frame(FramePtr frame)
{
    if (state_.load(std::memory_order_acquire) != RecorderState::RECORDING)
        return false;

    poll_bus();  // catch pipeline errors without blocking

    // ── Compute PTS ───────────────────────────────────────────────────────
    // libcamera timestamps are nanoseconds from CLOCK_MONOTONIC (steady_clock).
    // GStreamer PTS must start at 0 for the first frame.
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            frame->timestamp.time_since_epoch()).count());

    if (first_frame_) {
        base_pts_   = ts_ns;
        first_frame_ = false;
    }

    const uint64_t pts      = ts_ns - base_pts_;
    const uint64_t duration = (cfg_.fps > 0)
                                  ? (GST_SECOND / cfg_.fps)
                                  : GST_SECOND / 30;

    // ── Zero-copy GstBuffer ───────────────────────────────────────────────
    // The FramePtr stored as user_data keeps the DMA buffer alive until
    // GStreamer releases the GstBuffer after encoding. At that point the
    // shared_ptr destructs, Frame::release() fires, and the buffer is
    // returned to libcamera for the next capture cycle.
    auto* keepalive = new FramePtr(frame);

    GstBuffer* buf = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        frame->data,
        frame->data_size,
        0,
        frame->data_size,
        keepalive,
        [](void* p) { delete static_cast<FramePtr*>(p); });

    GST_BUFFER_PTS(buf)      = pts;
    GST_BUFFER_DURATION(buf) = duration;

    // gst_app_src_push_buffer() takes ownership of buf on success.
    const GstFlowReturn ret = gst_app_src_push_buffer(
        GST_APP_SRC(appsrc_), buf);

    if (ret != GST_FLOW_OK) {
        std::cerr << "Recorder: appsrc push failed (" << ret << ")\n";
        return false;
    }
    return true;
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
    std::tm    tm{};
    localtime_r(&time, &tm);

    char buf[64];
    std::strftime(buf, sizeof(buf), "rec_%Y%m%d_%H%M%S.mp4", &tm);
    return recordings_dir_ + "/" + buf;
}

// ── Private helpers ────────────────────────────────────────────────────────

std::string Recorder::build_pipeline_str(const ResolutionConfig& cfg,
                                          const std::string&       path) const
{
    // v4l2h264enc uses the Pi's VideoCore V4L2 M2M hardware encoder.
    // extra-controls maps to V4L2 controls:
    //   video_bitrate — target bitrate in bits/s (not kbps)
    //   h264_profile  — 4 = High, 1 = Baseline (more compatible)
    //
    // h264parse config-interval=-1 inserts SPS/PPS before every IDR frame,
    // making the stream seekable and allowing the MP4 to be opened mid-record.
    //
    // mp4mux faststart=true writes the moov atom at the beginning of the
    // file rather than the end, so the file is playable while recording.
    //
    // filesink sync=false lets the file write run at full disk speed without
    // waiting for the pipeline clock.

    std::ostringstream ss;
    ss << "appsrc name=src"
       << " format=time"
       << " is-live=true"
       << " block=false"
       << " caps=video/x-raw"
       << ",format=NV12"
       << ",width="     << cfg.width
       << ",height="    << cfg.height
       << ",framerate=" << cfg.fps << "/1"
       << " ! v4l2h264enc"
       << " extra-controls=\"controls"
       << ",video_bitrate=" << (cfg.bitrate_kbps * 1000)
       << ",h264_profile=4\""
       << " ! h264parse config-interval=-1"
       << " ! mp4mux faststart=true"
       << " ! filesink name=sink sync=false location=" << path;

    return ss.str();
}

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
        // Force back to IDLE so the caller can detect and react
        state_.store(RecorderState::IDLE, std::memory_order_release);
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(msg, &gerr, &dbg);
        std::cerr << "Recorder pipeline warning: " << gerr->message << "\n";
        break;
    default:
        break;
    }

    if (gerr) g_error_free(gerr);
    if (dbg)  g_free(dbg);
    gst_message_unref(msg);
}

void Recorder::teardown()
{
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}
