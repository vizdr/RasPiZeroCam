// src/control/ws_server.cpp

#undef CPPHTTPLIB_OPENSSL_SUPPORT   // plain HTTP/WS only

#include "ws_server.h"

#include <iostream>
#include "httplib.h"

// ── Constructor / Destructor ───────────────────────────────────────────────

WsServer::WsServer(int port) : port_(port) {}

WsServer::~WsServer()
{
    if (running_) stop();
}

// ── start / stop ───────────────────────────────────────────────────────────

bool WsServer::start(MessageHandler on_message)
{
    if (running_) return false;
    server_ = std::make_unique<httplib::Server>();

    // Register WebSocket handler at /ws
    server_->WebSocket("/ws",
        [this, on_message](const httplib::Request& /*req*/,
                            httplib::ws::WebSocket& ws) {
            ++client_count_;
            add_client(&ws);
            std::cout << "WsServer: client connected ("
                      << client_count_.load() << " total)\n";

            std::string msg;
            while (true) {
                auto result = ws.read(msg);
                if (result == httplib::ws::ReadResult::Fail) break;
                if (result == httplib::ws::ReadResult::Text && on_message) {
                    const std::string reply = on_message(msg);
                    if (!reply.empty()) ws.send(reply);
                }
            }

            remove_client(&ws);
            --client_count_;
            std::cout << "WsServer: client disconnected ("
                      << client_count_.load() << " remaining)\n";
        });

    running_ = true;
    server_thread_ = std::thread([this] {
        std::cout << "WsServer: listening on port " << port_ << "\n";
        server_->listen("0.0.0.0", port_);
        running_ = false;
    });

    return true;
}

void WsServer::stop()
{
    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
    running_ = false;
    std::cout << "WsServer: stopped\n";
}

// ── broadcast ──────────────────────────────────────────────────────────────

void WsServer::broadcast(const std::string& json)
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto* ws : clients_) {
        if (ws && ws->is_open())
            ws->send(json);
    }
}

// ── Client tracking ────────────────────────────────────────────────────────

void WsServer::add_client(httplib::ws::WebSocket* ws)
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    clients_.insert(ws);
}

void WsServer::remove_client(httplib::ws::WebSocket* ws)
{
    std::lock_guard<std::mutex> lk(clients_mutex_);
    clients_.erase(ws);
}
