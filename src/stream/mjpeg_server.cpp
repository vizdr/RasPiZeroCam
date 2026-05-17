// src/stream/mjpeg_server.cpp

#undef CPPHTTPLIB_OPENSSL_SUPPORT   // plain HTTP only

#include "mjpeg_server.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "httplib.h"

// ── Constructor / Destructor ───────────────────────────────────────────────

MjpegServer::MjpegServer(int port, std::string web_root)
    : port_(port), web_root_(std::move(web_root))
{}

MjpegServer::~MjpegServer()
{
    if (running_)
        stop();
}

// ── start() ────────────────────────────────────────────────────────────────

bool MjpegServer::start(const ResolutionConfig& cfg)
{
    if (running_) return false;
    cfg_    = cfg;
    server_ = std::make_unique<httplib::Server>();

    server_->Get("/stream.mjpeg",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_stream(req, res);
        });

    server_->Get("/snapshot.jpg",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_snapshot(req, res);
        });

    server_->Get("/api/status",
        [this](const httplib::Request& req, httplib::Response& res) {
            handle_status(req, res);
        });

    server_->Get("/",
        [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream f(web_root_ + "/index.html");
            if (f) {
                std::string body((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
                res.set_content(body, "text/html; charset=utf-8");
            } else {
                res.status = 404;
                res.set_content("index.html not found", "text/plain");
            }
        });

    server_->set_base_dir(web_root_.c_str());

    running_ = true;
    server_thread_ = std::thread([this] {
        std::cout << "MjpegServer: listening on port " << port_ << "\n";
        server_->listen("0.0.0.0", port_);
        running_ = false;
    });

    return true;
}

// ── stop() ─────────────────────────────────────────────────────────────────

void MjpegServer::stop()
{
    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
    running_ = false;
    std::cout << "MjpegServer: stopped\n";
}

// ── on_jpeg_ready() ────────────────────────────────────────────────────────

void MjpegServer::on_jpeg_ready(const uint8_t* data, size_t size)
{
    {
        std::lock_guard<std::mutex> lk(jpeg_mutex_);
        latest_jpeg_.assign(data, data + size);
        ++latest_seq_;
    }
    jpeg_cv_.notify_all();
}

// ── wait_for_jpeg() ────────────────────────────────────────────────────────

std::pair<std::vector<uint8_t>, uint64_t>
MjpegServer::wait_for_jpeg(uint64_t current_seq, int timeout_ms) const
{
    std::unique_lock<std::mutex> lock(jpeg_mutex_);
    jpeg_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [&]{ return latest_seq_ > current_seq; });

    if (latest_seq_ <= current_seq)
        return {{}, current_seq};

    return {latest_jpeg_, latest_seq_};
}

// ── MJPEG stream handler ───────────────────────────────────────────────────

void MjpegServer::handle_stream(const httplib::Request& /*req*/,
                                 httplib::Response& res)
{
    ++client_count_;
    uint64_t prev_seq = 0;

    res.set_chunked_content_provider(
        "multipart/x-mixed-replace; boundary=mjpeg",
        [this, prev_seq](size_t /*offset*/, httplib::DataSink& sink) mutable -> bool {
            auto [jpeg, seq] = wait_for_jpeg(prev_seq, 500);
            if (jpeg.empty()) return true;   // timeout — keep alive

            prev_seq = seq;
            std::string hdr =
                "--mjpeg\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                std::to_string(jpeg.size()) + "\r\n\r\n";

            return sink.write(hdr.c_str(), hdr.size()) &&
                   sink.write(reinterpret_cast<const char*>(jpeg.data()),
                               jpeg.size()) &&
                   sink.write("\r\n", 2);
        },
        [this](bool) { --client_count_; }
    );
}

// ── Snapshot handler ───────────────────────────────────────────────────────

void MjpegServer::handle_snapshot(const httplib::Request& /*req*/,
                                   httplib::Response& res)
{
    std::vector<uint8_t> jpeg;
    {
        std::lock_guard<std::mutex> lk(jpeg_mutex_);
        jpeg = latest_jpeg_;
    }
    if (jpeg.empty()) {
        res.status = 503;
        res.set_content("No frame available yet", "text/plain");
        return;
    }
    res.set_content(reinterpret_cast<const char*>(jpeg.data()),
                    jpeg.size(), "image/jpeg");
}

// ── Status handler ─────────────────────────────────────────────────────────

void MjpegServer::handle_status(const httplib::Request& /*req*/,
                                 httplib::Response& res)
{
    std::ostringstream json;
    json << "{"
         << "\"resolution\":\"" << cfg_.width << "x" << cfg_.height << "\","
         << "\"fps\":"          << cfg_.fps   << ","
         << "\"clients\":"      << client_count_.load() << ","
         << "\"voltage_v\":0.0,"
         << "\"battery_pct\":0"
         << "}";
    res.set_content(json.str(), "application/json");
}
