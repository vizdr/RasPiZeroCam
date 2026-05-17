// Phase C entry point: MJPEG streaming via GStreamer libcamerasrc + v4l2jpegenc.
//
// Architecture:
//   GstCameraSource owns the camera via libcamerasrc.
//   GStreamer negotiates DMA-BUF caps → v4l2jpegenc runs in DMABUF import mode.
//   Hardware JPEG frames arrive in on_jpeg_ready() at ~30fps.
//   MjpegServer distributes pre-encoded JPEG to N HTTP clients — no per-client encoding.
//
// Note: GstCameraSource and CameraManager cannot coexist (libcamera exclusive
// ownership). Recording via H264HardwareEncoder will be integrated via a
// GStreamer tee branch in Phase D.

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <sys/utsname.h>

#include "power/ina219.h"
#include "stream/gst_camera_source.h"
#include "stream/mjpeg_server.h"
#include "camera/resolution.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ── Signal handling ────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

// ── main ───────────────────────────────────────────────────────────────────

int main(int, char**)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    struct utsname u; uname(&u);
    std::cout << "=== Pi Zero 2 W — Phase C MJPEG Streaming ===\n"
              << "Host:   " << u.nodename << "\n"
              << "Kernel: " << u.release  << "\n"
              << "Built:  " << __DATE__ << " " << __TIME__ << "\n\n";

    // ── INA219 (non-fatal) ─────────────────────────────────────────────────
    INA219 power("/dev/i2c-1", 0x43);
    const bool power_ok = power.initialize();
    if (!power_ok)
        std::cerr << "INA219 not available\n";

    // ── Resolution ────────────────────────────────────────────────────────
    // MEDIUM (1280×720) proven to deliver 30fps with GstCameraSource.
    // Switch to LOW (640×480) for weaker WiFi.
    const ResolutionConfig& cfg = config_of(Resolution::MEDIUM);

    // ── MJPEG HTTP server ─────────────────────────────────────────────────
    MjpegServer streamer(8080, "web");
    if (!streamer.start(cfg)) {
        std::cerr << "MjpegServer failed to start\n";
        return 1;
    }
    std::cout << "Stream: http://" << u.nodename << ":8080/\n\n";

    // ── GStreamer camera source ────────────────────────────────────────────
    // Delivers hardware-encoded JPEG frames directly to MjpegServer.
    // No consumer loop needed — GStreamer drives everything internally.
    GstCameraSource gst_cam(cfg, 85 /*jpeg quality*/);

    gst_cam.set_jpeg_callback([&streamer](const uint8_t* data, size_t size) {
        streamer.on_jpeg_ready(data, size);
    });

    if (!gst_cam.start()) {
        std::cerr << "GstCameraSource failed to start\n";
        streamer.stop();
        return 1;
    }

    // ── Periodic stats ────────────────────────────────────────────────────
    std::cout << "── Running (Ctrl-C to stop) ──\n";
    auto next_stat = Clock::now() + 10s;

    while (g_running && gst_cam.is_running()) {
        std::this_thread::sleep_for(500ms);

        if (Clock::now() >= next_stat) {
            std::cout << "stream_clients=" << streamer.connected_clients();
            if (power_ok) {
                std::cout << "  bat="     << power.read_percentage()          << "%"
                          << "  V="       << std::fixed << std::setprecision(2)
                                          << power.read_bus_voltage_v()
                          << "  I="       << std::setprecision(3)
                                          << power.read_current_ma()/1000.0f  << "A";
            }
            std::cout << "\n";
            next_stat = Clock::now() + 10s;
        }
    }

    // ── Shutdown ───────────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    gst_cam.stop();
    streamer.stop();
    return 0;
}
