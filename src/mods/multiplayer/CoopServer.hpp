#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

// Runs the standalone Go server (automatamp_server\server.exe next to the game)
// so the host can start a session from the in-game menu.
class CoopServer {
public:
    struct LocalAddress {
        std::string adapter;
        std::string ip;
    };

    ~CoopServer();

    // Returns an error message on failure.
    std::optional<std::string> start(const std::string& port, const std::string& password);
    void stop();
    bool is_running() const;

    static std::filesystem::path get_directory();
    static std::vector<LocalAddress> get_local_addresses();

private:
    HANDLE m_job{nullptr};
    HANDLE m_process{nullptr};
};
