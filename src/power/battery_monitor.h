// src/power/battery_monitor.h
// Polls INA219 every 5 seconds on a background thread and pushes JSON to
// all connected WebSocket clients via a callback.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "power/ina219.h"

class BatteryMonitor {
public:
    // Fires from the monitor thread every POLL_INTERVAL_S seconds.
    // Receiver must be thread-safe (WsServer::broadcast).
    using PushCallback = std::function<void(const std::string& json)>;

    static constexpr int POLL_INTERVAL_S = 5;
    static constexpr int LOW_BATTERY_PCT = 20;
    static constexpr int CRITICAL_PCT    = 5;

    explicit BatteryMonitor(INA219& sensor);
    ~BatteryMonitor();

    BatteryMonitor(const BatteryMonitor&)            = delete;
    BatteryMonitor& operator=(const BatteryMonitor&) = delete;

    void start(PushCallback on_update);
    void stop();

    // Synchronous snapshot for the get_battery command — thread-safe.
    std::string get_json() const;

    bool is_running() const noexcept { return !abort_.load(); }

private:
    void thread_fn();
    void read_and_store();
    std::string build_json() const;   // must hold data_mutex_

    INA219&           sensor_;
    PushCallback      push_cb_;
    std::thread       thread_;
    std::atomic<bool> abort_{false};

    mutable std::mutex data_mutex_;
    float voltage_{0.0f};
    float current_ma_{0.0f};
    float power_mw_{0.0f};
    int   percentage_{0};
    bool  charging_{false};
    bool  low_battery_{false};
};
