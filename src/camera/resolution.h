// src/camera/resolution.h
// Resolution presets for the Pi Camera v1 (OV5647) pipeline.
// Matches the three operating modes defined in VideoStreamRasPiZero2.md §10.

#pragma once

#include <cstdint>
#include <string_view>

// ── Preset identifiers ─────────────────────────────────────────────────────

enum class Resolution : uint8_t {
    LOW    = 0,   //  640×480  @ 30 fps — weak WiFi, max range
    MEDIUM = 1,   // 1280×720  @ 30 fps — default
    HIGH   = 2,   // 1920×1080 @ 25 fps — strong signal only
};

// ── Per-preset parameters ──────────────────────────────────────────────────

struct ResolutionConfig {
    uint32_t    width;
    uint32_t    height;
    uint32_t    fps;
    uint32_t    bitrate_kbps;   // target H.264 bitrate for the encoder
    std::string_view label;     // human-readable name for logging / UI
};

// ── Lookup table (indexed by Resolution enum value) ────────────────────────

inline constexpr ResolutionConfig RESOLUTION_TABLE[] = {
    {  640,  480, 30,   500, "640x480@30"   },  // LOW
    { 1280,  720, 30,  1500, "1280x720@30"  },  // MEDIUM
    { 1920, 1080, 25,  3000, "1920x1080@25" },  // HIGH
};

// ── Accessor helpers ───────────────────────────────────────────────────────

inline constexpr const ResolutionConfig& config_of(Resolution r) noexcept
{
    return RESOLUTION_TABLE[static_cast<uint8_t>(r)];
}

inline constexpr Resolution resolution_from_string(std::string_view s) noexcept
{
    if (s == "640x480"   || s == "low")    return Resolution::LOW;
    if (s == "1920x1080" || s == "high")   return Resolution::HIGH;
    return Resolution::MEDIUM;                   // default for "1280x720" or anything else
}

// ── Still-capture resolutions (Phase D — dual-stream) ──────────────────────
//
// These presets describe the StillCapture stream added in Phase D.
// In Phase C, request_snapshot() delivers a frame at the current video
// resolution instead; the interface is identical so no call-site changes.
//
// OV5647 (Pi Camera v1) sensor modes:
//   FULL       — full 5 MP sensor read-out, maximum image quality
//   MEDIUM_4K3 — 2× binned 1.3 MP, faster ISP, lower power
//   WIDESCREEN — 16:9 crop of the full sensor (4 MP)

enum class StillResolution : uint8_t {
    FULL       = 0,   // 2592 × 1944 — 5 MP (full sensor)
    MEDIUM_4K3 = 1,   // 1296 ×  972 — 1.3 MP (2× binned, faster)
    WIDESCREEN = 2,   // 2592 × 1458 — 4 MP 16:9 crop
};

struct StillResolutionConfig {
    uint32_t         width;
    uint32_t         height;
    std::string_view label;
};

inline constexpr StillResolutionConfig STILL_RESOLUTION_TABLE[] = {
    { 2592, 1944, "2592x1944 (5 MP)"  },   // FULL
    { 1296,  972, "1296x972 (1.3 MP)" },   // MEDIUM_4K3
    { 2592, 1458, "2592x1458 (4 MP)"  },   // WIDESCREEN
};

inline constexpr const StillResolutionConfig& still_config_of(StillResolution r) noexcept
{
    return STILL_RESOLUTION_TABLE[static_cast<uint8_t>(r)];
}
