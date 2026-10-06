#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <fstream>

#include <spdlog/spdlog.h>
#include <json.hpp>

#include <utility/Module.hpp>
#include <utility/String.hpp>

#include "CoopServer.hpp"

#pragma comment(lib, "iphlpapi.lib")

CoopServer::~CoopServer() {
    stop();
}

std::filesystem::path CoopServer::get_directory() {
    const auto game_dir = utility::get_module_directoryw(utility::get_executable());

    return std::filesystem::path{game_dir.value_or(L".")} / "automatamp_server";
}

std::optional<std::string> CoopServer::start(const std::string& port, const std::string& password) {
    stop();

    const auto dir = get_directory();
    const auto exe = dir / "server.exe";

    if (!std::filesystem::exists(exe)) {
        return "server.exe not found in " + utility::narrow(dir.wstring());
    }

    // The co-op server is private: it never announces itself to the public master server.
    {
        nlohmann::json config{
            {"password", password},
            {"masterServer", "http://localhost"},
            {"masterServerNotify", false},
            {"name", "AutomataMP Co-op"},
            {"port", port},
        };

        std::ofstream ofs{dir / "server.json"};

        if (!ofs) {
            return "Cannot write server.json";
        }

        ofs << config.dump(4);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const auto log_path = dir / "server_log.txt";
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = nullptr;
    }

    // The job kills the server together with the game, even if the game crashes.
    m_job = CreateJobObjectW(nullptr, nullptr);

    if (m_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    PROCESS_INFORMATION pi{};
    auto cmdline = L"\"" + exe.wstring() + L"\"";

    const auto created = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi);

    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }

    if (!created) {
        const auto error = GetLastError();
        stop();
        return "Failed to start server.exe (error " + std::to_string(error) + ")";
    }

    if (m_job != nullptr) {
        AssignProcessToJobObject(m_job, pi.hProcess);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;

    spdlog::info("[CoopServer] Started server.exe on port {}", port);

    return std::nullopt;
}

void CoopServer::stop() {
    if (m_process != nullptr) {
        TerminateProcess(m_process, 0);
        CloseHandle(m_process);
        m_process = nullptr;

        spdlog::info("[CoopServer] Stopped server.exe");
    }

    if (m_job != nullptr) {
        CloseHandle(m_job);
        m_job = nullptr;
    }
}

bool CoopServer::is_running() const {
    return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

std::vector<CoopServer::LocalAddress> CoopServer::get_local_addresses() {
    std::vector<LocalAddress> result{};

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto adapters = (IP_ADAPTER_ADDRESSES*)buffer.data();

    const auto flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = (IP_ADAPTER_ADDRESSES*)buffer.data();
    }

    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR) {
        return result;
    }

    for (auto adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (auto addr = adapter->FirstUnicastAddress; addr != nullptr; addr = addr->Next) {
            char ip[INET_ADDRSTRLEN]{};
            const auto sin = (sockaddr_in*)addr->Address.lpSockaddr;

            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == nullptr) {
                continue;
            }

            result.push_back({utility::narrow(adapter->FriendlyName), ip});
        }
    }

    return result;
}
