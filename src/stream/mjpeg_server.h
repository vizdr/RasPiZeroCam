// src/stream/mjpeg_server.h
// HTTP server delivering a live MJPEG stream to browser clients.
//
// Encoding model:
//   GstCameraSource encodes JPEG frames in hardware (v4l2jpegenc via DMA-BUF).
//   MjpegServer receives pre-encoded JPEG bytes via on_jpeg_ready() and
//   distributes them to all connected HTTP clients.
//   No per-client encoding: N clients costs the same as 1 client.
//
// Browser usage:  <img src="http://pi:8080/stream.mjpeg">
// Protocol:       HTTP/1.1 multipart/x-mixed-replace

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera/resolution.h"

namespace httplib { class Server; class Request; class Response; }

class MjpegServer {
public:
    explicit MjpegServer(int port = 8080, std::string web_root = "web");
    ~MjpegServer();

    MjpegServer(const MjpegServer&)            = delete;
    MjpegServer& operator=(const MjpegServer&) = delete;

    // Start HTTP server in a background thread.
    bool start(const ResolutionConfig& cfg);

    // Stop server and close all client connections.
    void stop();

    // Called by GstCameraSource when a hardware-encoded JPEG frame arrives.
    // Copies `size` bytes from `data` into latest_jpeg_ and notifies clients.
    // Thread-safe — called from GStreamer's internal appsink thread.
    void on_jpeg_ready(const uint8_t* data, size_t size);

    bool is_running()        const noexcept { return running_.load(); }
    int  connected_clients() const noexcept { return client_count_.load(); }

private:
    // Block until latest_seq_ > current_seq, or timeout.
    // Returns {jpeg_bytes, new_seq} — jpeg_bytes is empty on timeout.
    std::pair<std::vector<uint8_t>, uint64_t>
        wait_for_jpeg(uint64_t current_seq, int timeout_ms) const;

    void handle_stream  (const httplib::Request&, httplib::Response&);
    void handle_snapshot(const httplib::Request&, httplib::Response&);
    void handle_status  (const httplib::Request&, httplib::Response&);

    int              port_;
    std::string      web_root_;
    ResolutionConfig cfg_{};

    // Shared latest JPEG — written by GstCameraSource, read by HTTP threads
    mutable std::mutex              jpeg_mutex_;
    mutable std::condition_variable jpeg_cv_;
    std::vector<uint8_t>            latest_jpeg_;
    uint64_t                        latest_seq_{0};

    std::unique_ptr<httplib::Server> server_;
    std::thread                      server_thread_;
    std::atomic<bool>                running_{false};
    std::atomic<int>                 client_count_{0};
};
