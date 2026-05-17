// src/power/battery_monitor.cpp

#include "battery_monitor.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace std::chrono_literals;

// ── Constructor / Destructor ───────────────────────────────────────────────

BatteryMonitor::BatteryMonitor(INA219& sensor)
    : sensor_(sensor)
{}

BatteryMonitor::~BatteryMonitor()
{
    if (!abort_) stop();
}

// ── start / stop ───────────────────────────────────────────────────────────

void BatteryMonitor::start(PushCallback on_update)
{
    push_cb_ = std::move(on_update);
    abort_   = false;
    // Take one immediate reading so get_json() returns valid data straight away
    read_and_store();
    thread_ = std::thread(&BatteryMonitor::thread_fn, this);
    std::cout << "BatteryMonitor: started (every " << POLL_INTERVAL_S << " s)\n";
}

void BatteryMonitor::stop()
{
    abort_ = true;
    if (thread_.joinable()) thread_.join();
}

// ── Polling thread ─────────────────────────────────────────────────────────

void BatteryMonitor::thread_fn()
{
    while (!abort_) {
        // Sleep in 100ms increments so stop() responds quickly
        for (int i = 0; i < POLL_INTERVAL_S * 10 && !abort_; ++i)
            std::this_thread::sleep_for(100ms);

        if (abort_) break;

        read_and_store();

        if (push_cb_) {
            std::string json;
            {
                std::lock_guard<std::mutex> lk(data_mutex_);
                json = build_json();
            }
            push_cb_(json);
        }

        if (low_battery_)
            std::cerr << "BatteryMonitor: LOW BATTERY (" << percentage_ << "%)\n";
    }
}

// ── INA219 read ────────────────────────────────────────────────────────────

void BatteryMonitor::read_and_store()
{
    const float v   = sensor_.read_bus_voltage_v();
    const float i   = sensor_.read_current_ma();
    const float p   = sensor_.read_power_mw();
    const int   pct = sensor_.read_percentage();
    const bool  chg = sensor_.is_charging();

    std::lock_guard<std::mutex> lk(data_mutex_);
    voltage_    = v;
    current_ma_ = i;
    power_mw_   = p;
    percentage_ = pct;
    charging_   = chg;
    low_battery_ = (pct < LOW_BATTERY_PCT);
}

// ── JSON builders ──────────────────────────────────────────────────────────

std::string BatteryMonitor::build_json() const
{
    // Called with data_mutex_ already held by the caller.
    std::ostringstream j;
    j << std::fixed << std::setprecision(3);
    j << "{"
      << "\"type\":\"battery_update\","
      << "\"voltage_v\":"   << voltage_    << ","
      << "\"current_ma\":"  << current_ma_ << ","
      << "\"power_mw\":"    << power_mw_   << ","
      << "\"percentage\":"  << percentage_ << ","
      << "\"charging\":"    << (charging_ ? "true" : "false") << ","
      << "\"low_battery_warning\":" << (low_battery_ ? "true" : "false")
      << "}";
    return j.str();
}

std::string BatteryMonitor::get_json() const
{
    std::lock_guard<std::mutex> lk(data_mutex_);
    return build_json();
}
