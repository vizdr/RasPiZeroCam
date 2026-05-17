// src/control/command_dispatcher.cpp

#include "command_dispatcher.h"

#include <iostream>

void CommandDispatcher::register_command(const std::string& cmd, Handler handler)
{
    handlers_[cmd] = std::move(handler);
}

std::string CommandDispatcher::dispatch(const std::string& raw_json) const
{
    nlohmann::json msg;
    try {
        msg = nlohmann::json::parse(raw_json);
    } catch (const std::exception& e) {
        std::cerr << "CommandDispatcher: JSON parse error: " << e.what() << "\n";
        return nlohmann::json{{"status","error"},{"message","invalid JSON"}}.dump();
    }

    const std::string cmd = msg.value("cmd", "");
    if (cmd.empty())
        return nlohmann::json{{"status","error"},{"message","missing cmd"}}.dump();

    auto it = handlers_.find(cmd);
    if (it == handlers_.end()) {
        std::cerr << "CommandDispatcher: unknown command: " << cmd << "\n";
        return nlohmann::json{{"status","error"},
                              {"message","unknown command"},
                              {"cmd",cmd}}.dump();
    }

    try {
        return it->second(msg);
    } catch (const std::exception& e) {
        std::cerr << "CommandDispatcher: handler error for " << cmd
                  << ": " << e.what() << "\n";
        return nlohmann::json{{"status","error"},{"message",e.what()}}.dump();
    }
}
