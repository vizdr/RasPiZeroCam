// src/control/command_dispatcher.h
// JSON command router. Parses {"cmd":"...",...} messages, dispatches to
// registered handler functions, returns JSON response strings.

#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

class CommandDispatcher {
public:
    // Handler receives the full parsed message (including "cmd" field).
    // Returns a JSON string to send back, or "" to send nothing.
    using Handler = std::function<std::string(const nlohmann::json& msg)>;

    void register_command(const std::string& cmd, Handler handler);

    // Parse raw_json, look up cmd, call handler, return response.
    // Returns an error JSON string on parse failure or unknown command.
    std::string dispatch(const std::string& raw_json) const;

private:
    std::unordered_map<std::string, Handler> handlers_;
};
