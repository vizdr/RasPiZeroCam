// Phase C + Phase 3 + Phase 4: MJPEG streaming + WebSocket commands
// + recording + snapshot + set_resolution + file management.

#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <ctime>

#include "power/ina219.h"
#include "power/battery_monitor.h"
#include "stream/gst_camera_source.h"
#include "stream/mjpeg_server.h"
#include "control/ws_server.h"
#include "control/command_dispatcher.h"
#include "camera/resolution.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

// ── Signal handling ────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

// ── Helpers ────────────────────────────────────────────────────────────────

static long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static std::pair<long, long> storage_mb(const std::string &path = "/")
{
    struct statvfs st{};
    if (statvfs(path.c_str(), &st) != 0)
        return {0, 0};
    const long b = static_cast<long>(st.f_frsize);
    const long free = st.f_bavail * b / (1024 * 1024);
    const long used = (st.f_blocks - st.f_bfree) * b / (1024 * 1024);
    return {free, used};
}

// List recordings sorted newest-first.
static json list_recordings_json(const std::string &dir)
{
    json files = json::array();
    if (!fs::exists(dir))
        return files;
    for (auto &e : fs::directory_iterator(dir))
    {
        if (!e.is_regular_file())
            continue;
        auto ext = e.path().extension().string();
        if (ext != ".mp4" && ext != ".jpg")
            continue;
        const double sz_mb = static_cast<double>(e.file_size()) / (1024.0 * 1024.0);
        const auto ftime = e.last_write_time();
        const auto mtime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const time_t ts = std::chrono::system_clock::to_time_t(mtime);
        files.push_back({{"name", e.path().filename().string()},
                         {"size_mb", sz_mb},
                         {"modified", ts}});
    }
    std::sort(files.begin(), files.end(),
              [](const json &a, const json &b)
              { return a["modified"] > b["modified"]; });
    return files;
}

// Delete a recording — validates filename to prevent path traversal.
static bool delete_recording_file(const std::string &dir, const std::string &name)
{
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find("..") != std::string::npos)
        return false;
    const fs::path p = fs::path(dir) / name;
    if (!fs::exists(p) || !fs::is_regular_file(p))
        return false;
    return fs::remove(p);
}

// Next timestamped filename.
static std::string next_name(const std::string &dir, const char *fmt)
{
    const auto now = std::chrono::system_clock::now();
    const time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return dir + "/" + buf;
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int, char **)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP,  SIG_IGN);   // survive SSH session drop / mode switch

    struct utsname u;
    uname(&u);
    std::cout << "=== Pi Zero 2 W — Phase C+3+4 ===\n"
              << "Host:   " << u.nodename << "\n"
              << "Built:  " << __DATE__ << " " << __TIME__ << "\n\n";

    // ── Current resolution (mutable — changed by set_resolution) ──────────
    // Protected by a mutex because set_resolution is called from the WS thread.
    std::mutex res_mutex;
    const ResolutionConfig *cfg_ptr = &config_of(Resolution::MEDIUM);

    // ── INA219 ────────────────────────────────────────────────────────────
    INA219 power("/dev/i2c-1", 0x43);
    const bool power_ok = power.initialize();
    if (!power_ok)
        std::cerr << "INA219 not available\n";

    // ── MJPEG HTTP server (port 8080) ─────────────────────────────────────
    MjpegServer streamer(8080, "web");
    if (!streamer.start(*cfg_ptr))
    {
        std::cerr << "MjpegServer failed\n";
        return 1;
    }

    // ── GStreamer camera source ────────────────────────────────────────────
    GstCameraSource gst_cam(*cfg_ptr, 85);
    gst_cam.set_jpeg_callback([&streamer](const uint8_t *d, size_t n)
                              { streamer.on_jpeg_ready(d, n); });
    if (!gst_cam.start())
    {
        std::cerr << "GstCameraSource failed\n";
        streamer.stop();
        return 1;
    }
    std::cout << "Stream: http://" << u.nodename << ":8080/\n\n";

    // ── WebSocket server (port 8081) ───────────────────────────────────────
    WsServer ws_server(8081);
    CommandDispatcher dispatcher;

    // ── Battery monitor ────────────────────────────────────────────────────
    BatteryMonitor battery(power);
    if (power_ok)
        battery.start([&ws_server](const std::string &j)
                      { ws_server.broadcast(j); });

    // ── Recordings directory ───────────────────────────────────────────────
    const std::string REC_DIR = "recordings";
    fs::create_directories(REC_DIR);

    // ── Register commands ──────────────────────────────────────────────────

    dispatcher.register_command("ping", [](const json &)
                                { return json{{"status", "ok"}, {"ts", now_ms()}}.dump(); });

    dispatcher.register_command("get_battery", [&battery, power_ok](const json &)
                                {
        if (!power_ok)
            return json{{"status","error"},{"message","INA219 not available"}}.dump();
        auto j = json::parse(battery.get_json());
        j["status"] = "ok";
        return j.dump(); });

    dispatcher.register_command("get_status",
                                [&](const json &)
                                {
                                    std::lock_guard<std::mutex> lk(res_mutex);
                                    auto [free_mb, used_mb] = storage_mb("/");
                                    json s;
                                    s["status"] = "ok";
                                    s["camera"] = {{"resolution",
                                                    std::to_string(cfg_ptr->width) + "x" +
                                                        std::to_string(cfg_ptr->height)},
                                                   {"fps", cfg_ptr->fps}};
                                    s["streaming"] = {{"clients", streamer.connected_clients()}};
                                    s["ws"] = {{"clients", ws_server.connected_clients()}};
                                    s["recording"] = {{"active", gst_cam.is_recording()},
                                                      {"file", gst_cam.current_recording()},
                                                      {"duration_s", gst_cam.recording_seconds()}};
                                    s["storage"] = {{"free_mb", free_mb}, {"used_mb", used_mb}};
                                    if (power_ok)
                                        s["battery"] = json::parse(battery.get_json());
                                    return s.dump();
                                });

    // ── set_resolution ─────────────────────────────────────────────────────
    // Stops recording, rebuilds GStreamer pipeline, restarts. ~2s gap in stream.
    dispatcher.register_command("set_resolution", [&](const json &msg)
                                {
        const uint32_t w   = msg.value("width",  0u);
        const uint32_t h   = msg.value("height", 0u);
        /*const uint32_t fps = msg.value("fps",    30u);*/   // fps encoded in preset

        // Match to a preset
        Resolution res = Resolution::MEDIUM;
        if (w == 640  && h == 480)  res = Resolution::LOW;
        else if (w == 1920 && h == 1080) res = Resolution::HIGH;

        const ResolutionConfig& new_cfg = config_of(res);

        {
            std::lock_guard<std::mutex> lk(res_mutex);
            cfg_ptr = &new_cfg;
        }

        if (!gst_cam.set_resolution(new_cfg))
            return json{{"status","error"},{"message","pipeline restart failed"}}.dump();

        const std::string label = std::to_string(new_cfg.width) + "x" +
                                  std::to_string(new_cfg.height) + "@" +
                                  std::to_string(new_cfg.fps);
        return json{{"status","ok"},{"resolution",label}}.dump(); });

    // ── Recording commands ─────────────────────────────────────────────────
    dispatcher.register_command("record_start", [&](const json &msg)
                                {
        if (gst_cam.is_recording())
            return json{{"status","error"},{"message","already recording"}}.dump();
        std::lock_guard<std::mutex> lk(res_mutex);
        const std::string file = msg.value("filename",
            next_name(REC_DIR, "rec_%Y%m%d_%H%M%S.mp4"));
        if (gst_cam.start_recording(file))
            return json{{"status","ok"},{"file",file}}.dump();
        return json{{"status","error"},{"message","failed to start"}}.dump(); });

    dispatcher.register_command("record_stop", [&](const json &)
                                {
        if (!gst_cam.is_recording())
            return json{{"status","error"},{"message","not recording"}}.dump();
        const std::string file = gst_cam.current_recording();
        const int dur = gst_cam.recording_seconds();
        gst_cam.stop_recording();
        return json{{"status","ok"},{"file",file},{"duration_s",dur}}.dump(); });

    dispatcher.register_command("record_restart", [&](const json &)
                                {
        gst_cam.stop_recording();
        std::lock_guard<std::mutex> lk(res_mutex);
        const std::string file = next_name(REC_DIR, "rec_%Y%m%d_%H%M%S.mp4");
        gst_cam.start_recording(file);
        return json{{"status","ok"},{"file",file}}.dump(); });

    // ── Snapshot ───────────────────────────────────────────────────────────
    dispatcher.register_command("snapshot", [&](const json &msg)
                                {
        const std::string file = msg.value("filename",
            next_name(REC_DIR, "snap_%Y%m%d_%H%M%S.jpg"));
        if (gst_cam.take_snapshot(file))
            return json{{"status","ok"},{"file",file}}.dump();
        return json{{"status","error"},{"message","snapshot failed"}}.dump(); });

    // ── File management ────────────────────────────────────────────────────
    dispatcher.register_command("list_recordings", [&](const json &)
                                { return json{{"status", "ok"},
                                              {"files", list_recordings_json(REC_DIR)}}
                                      .dump(); });

    dispatcher.register_command("delete_recording", [&](const json &msg)
                                {
        const std::string name = msg.value("filename","");
        if (delete_recording_file(REC_DIR, name))
            return json{{"status","ok"}}.dump();
        return json{{"status","error"},{"message","file not found or not allowed"}}.dump(); });

    // ── Start WS server ────────────────────────────────────────────────────
    ws_server.start([&dispatcher](const std::string &msg)
                    { return dispatcher.dispatch(msg); });

    // ── Main loop ──────────────────────────────────────────────────────────
    std::cout << "── Running (Ctrl-C to stop) ──\n";

    auto next_rec_push = Clock::now() + 1s;
    auto next_storage_check = Clock::now() + 10s;
    auto next_stat = Clock::now() + 60s;

    while (g_running && gst_cam.is_running())
    {
        std::this_thread::sleep_for(250ms);

        const auto now = Clock::now();

        // recording_status push every 1 s while recording
        if (gst_cam.is_recording() && now >= next_rec_push)
        {
            const std::string file = gst_cam.current_recording();
            long file_sz = 0;
            try
            {
                file_sz = static_cast<long>(fs::file_size(file));
            }
            catch (...)
            {
            }
            ws_server.broadcast(json{
                {"type", "recording_status"},
                {"active", true},
                {"file", file},
                {"duration_s", gst_cam.recording_seconds()},
                {"size_mb", file_sz / (1024.0 * 1024.0)}}
                                    .dump());
            next_rec_push = now + 1s;
        }
        if (!gst_cam.is_recording())
            next_rec_push = now + 1s; // reset timer

        // storage_warning every 10 s when free < 500 MB
        if (now >= next_storage_check)
        {
            const long free_mb = storage_mb("/").first;
            if (free_mb < 500)
                ws_server.broadcast(json{
                    {"type", "storage_warning"}, {"free_mb", free_mb}}
                                        .dump());
            next_storage_check = now + 10s;
        }

        // console stats every 60 s
        if (now >= next_stat)
        {
            std::lock_guard<std::mutex> lk(res_mutex);
            std::cout << cfg_ptr->label
                      << "  stream=" << streamer.connected_clients()
                      << "  ws=" << ws_server.connected_clients()
                      << "  rec=" << (gst_cam.is_recording() ? "yes" : "no");
            if (power_ok)
            {
                auto j = json::parse(battery.get_json());
                std::cout << "  bat=" << j.value("percentage", 0) << "%";
            }
            std::cout << "\n";
            next_stat = now + 60s;
        }
    }

    // ── Shutdown ───────────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    if (power_ok)
        battery.stop();
    ws_server.stop();
    gst_cam.stop();
    streamer.stop();
    return 0;
}
