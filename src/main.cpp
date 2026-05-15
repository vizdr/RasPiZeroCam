// Camera + Recorder + Power integration smoke test.
//
// Consumer loop:
//   CameraFrameQueue::pop()
//     → Recorder::push_frame()   if recording
//     → (future: MjpegServer, Snapshot)

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <sys/utsname.h>

#include "power/ina219.h"
#include "camera/camera_manager.h"
#include "camera/frame_buffer.h"
#include "camera/resolution.h"
#include "record/recorder.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ── Graceful shutdown on Ctrl-C ────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void on_signal(int) { g_running = false; }

// ── Helpers ────────────────────────────────────────────────────────────────

static void print_system_info()
{
    struct utsname u;
    uname(&u);
    std::cout << "Host:   " << u.nodename << "\n";
    std::cout << "Kernel: " << u.release  << "\n";
    std::cout << "Arch:   " << u.machine  << "\n";
    std::cout << "Built:  " << __DATE__ << " " << __TIME__ << "\n\n";
}

static void print_power(INA219& pwr)
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  voltage  " << pwr.read_bus_voltage_v()        << " V\n";
    std::cout << "  current  " << pwr.read_current_ma() / 1000.0f << " A"
              << (pwr.is_charging() ? "  (charging)" : "  (discharging)") << "\n";
    std::cout << "  power    " << pwr.read_power_mw()  / 1000.0f  << " W\n";
    std::cout << "  battery  " << pwr.read_percentage()           << "%\n";
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int, char**)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "=== Pi Zero 2 W — Camera Recorder smoke test ===\n\n";
    print_system_info();

    // ── INA219 (non-fatal) ─────────────────────────────────────────────────
    INA219 power("/dev/i2c-1", 0x43);
    const bool power_ok = power.initialize();
    if (power_ok) {
        std::cout << "── Power (before) ──\n";
        print_power(power);
        std::cout << "\n";
    } else {
        std::cerr << "INA219 not available — continuing without power data\n\n";
    }

    // ── Camera ────────────────────────────────────────────────────────────
    CameraFrameQueue queue;
    CameraManager    cam(queue);

    const ResolutionConfig& cfg = config_of(Resolution::LOW);  // 640×480

    std::cout << "── Opening camera at " << cfg.label << " ──\n";
    if (!cam.open(Resolution::LOW)) {
        std::cerr << "Camera open failed\n";
        return 1;
    }

    // ── Recorder ──────────────────────────────────────────────────────────
    Recorder recorder("recordings");

    // ── Test plan ─────────────────────────────────────────────────────────
    //  Phase 1: capture 5 s without recording (verify camera works)
    //  Phase 2: record for 10 s                (verify encoder + MP4)
    //  Phase 3: restart recording              (verify second file created)
    //  Phase 4: idle 2 s then stop             (verify clean shutdown)

    struct Phase { const char* label; int duration_s; bool record; bool do_restart; };
    const Phase phases[] = {
        { "idle capture",      5,  false, false },
        { "recording #1",     10,  true,  false },
        { "recording #2",      8,  true,  true  },  // restart mid-phase
        { "idle after stop",   2,  false, false },
    };

    uint64_t total_frames   = 0;
    uint64_t dropped_frames = 0;
    uint64_t prev_seq       = UINT64_MAX;

    for (const auto& phase : phases) {
        if (!g_running) break;

        std::cout << "\n── Phase: " << phase.label << " ("
                  << phase.duration_s << " s) ──\n";

        if (phase.record && !recorder.is_recording()) {
            if (!recorder.start(cfg))
                std::cerr << "  WARNING: recorder failed to start\n";
        }

        const auto phase_end = Clock::now() + std::chrono::seconds(phase.duration_s);
        bool restarted = false;

        while (g_running && Clock::now() < phase_end) {
            // Restart halfway through the phase if requested
            if (phase.do_restart && !restarted) {
                auto half = phase_end - std::chrono::seconds(phase.duration_s / 2);
                if (Clock::now() >= half) {
                    std::cout << "  → restarting recorder\n";
                    recorder.restart(cfg);
                    restarted = true;
                }
            }

            auto frame = queue.pop();
            if (!frame) {
                std::this_thread::sleep_for(1ms);
                continue;
            }

            ++total_frames;

            // Detect dropped frames (sequence gap from libcamera)
            if (prev_seq != UINT64_MAX && frame->sequence > prev_seq + 1)
                dropped_frames += frame->sequence - prev_seq - 1;
            prev_seq = frame->sequence;

            // Push to recorder (no-op if not recording)
            if (recorder.is_recording())
                recorder.push_frame(frame);

            // Print stats once per second
            static auto next_print = Clock::now() + 1s;
            if (Clock::now() >= next_print) {
                std::cout << "  frame #" << std::setw(5) << frame->sequence
                          << "  " << frame->data_size << " B"
                          << "  recording=" << (recorder.is_recording() ? "yes" : "no")
                          << "\n";
                next_print = Clock::now() + 1s;
            }
        }

        if (recorder.is_recording())
            recorder.stop();
    }

    // ── Shutdown ───────────────────────────────────────────────────────────
    cam.close();

    // ── Summary ────────────────────────────────────────────────────────────
    std::cout << "\n── Summary ──\n";
    std::cout << "  total frames:   " << total_frames   << "\n";
    std::cout << "  dropped frames: " << dropped_frames << "\n";

    if (power_ok) {
        std::cout << "\n── Power (after) ──\n";
        print_power(power);
    }

    // Verify at least one recording was created
    bool ok = std::filesystem::exists("recordings") &&
              !std::filesystem::is_empty("recordings");

    std::cout << "\nRecordings directory: "
              << (ok ? "contains files ✓" : "empty or missing ✗") << "\n";

    return ok ? 0 : 1;
}
