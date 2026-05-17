// src/control/ws_server.h
// WebSocket server on port 8081 using cpp-httplib's built-in WebSocket support.
//
// Each browser connection runs one handler invocation (blocks in read loop).
// Active connections are tracked in a mutex-protected set so broadcast()
// can push to all clients simultaneously from any thread (e.g. BatteryMonitor).
//
// Thread model:
//   start()      — launches httplib::Server in a background thread
//   handler      — one thread per connected client (httplib's thread pool)
//   broadcast()  — called from BatteryMonitor thread; thread-safe

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace httplib      { class Server; }
namespace httplib::ws  { class WebSocket; }

class WsServer {
public:
    // Called on the client's handler thread when a text message arrives.
    // Return value is sent back to that client (empty = no reply).
    using MessageHandler = std::function<std::string(const std::string& json)>;

    explicit WsServer(int port = 8081);
    ~WsServer();

    WsServer(const WsServer&)            = delete;
    WsServer& operator=(const WsServer&) = delete;

    // Start the WebSocket server in a background thread.
    bool start(MessageHandler on_message);

    // Stop the server and close all client connections.
    void stop();

    // Push JSON to every currently connected client.
    // Thread-safe — may be called from any thread.
    void broadcast(const std::string& json);

    int  connected_clients() const noexcept { return client_count_.load(); }
    bool is_running()        const noexcept { return running_.load(); }

private:
    void add_client   (httplib::ws::WebSocket* ws);
    void remove_client(httplib::ws::WebSocket* ws);

    int                              port_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                      server_thread_;
    std::atomic<bool>                running_{false};
    std::atomic<int>                 client_count_{0};

    mutable std::mutex                             clients_mutex_;
    std::unordered_set<httplib::ws::WebSocket*>    clients_;
};
