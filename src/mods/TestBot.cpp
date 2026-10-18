#include <atomic>
#include <thread>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_map>

#include <d3d11.h>
#include <TlHelp32.h>
#include <wrl.h>
#include <spdlog/spdlog.h>

#include <sdk/Entity.hpp>
#include <sdk/EntityList.hpp>
#include <sdk/Game.hpp>
#include <sdk/Pl0000.hpp>
#include <sdk/Story.hpp>

#include "../AutomataMP.hpp"
#include "../TestMode.hpp"
#include "AutomataMPMod.hpp"
#include "multiplayer/StoryWatcher.hpp"
#include "TestBot.hpp"

using Microsoft::WRL::ComPtr;

namespace {
TestBot* g_bot{};

// Virtual pad 0 as the bot holds it. Read by the XInput hook on the game thread.
std::atomic<WORD> g_buttons{0};
std::atomic<SHORT> g_stick_x{0};
std::atomic<SHORT> g_stick_y{0};
std::atomic<DWORD> g_packet{0};

constexpr auto RELEASE_GAP = std::chrono::milliseconds{150}; // between two commands, so repeated presses register

const std::unordered_map<std::string, WORD> BUTTONS{
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
};
// Triggers are axes in XInput; kept in the high bits of g_buttons, which XInput doesn't use for buttons.
constexpr WORD LT = 0x0400;
constexpr WORD RT = 0x0800;

// Writes caught by the watch command, filled by the exception handler on the writing thread.
struct Hit {
    uint32_t slot;
    uintptr_t rip;
    uintptr_t stack[12];
    uintptr_t args[4]; // rcx, rdx, r8, r9 of an execute breakpoint (the function's first arguments)
    char value[48];    // write: the watched bytes after the write; execute: the string rdx points to
};

constexpr size_t MAX_HITS = 2048;
Hit g_hits[MAX_HITS]{};
std::atomic<size_t> g_hit_count{0};
std::atomic<size_t> g_hits_dropped{0};
uintptr_t g_watch_address[4]{};
bool g_watch_execute[4]{};
}

std::optional<std::string> TestBot::on_initialize() {
    if (!testmode::enabled()) {
        return std::nullopt;
    }

    g_bot = this;
    m_out.open(testmode::root() / "coop_test_out.txt", std::ios::out | std::ios::trunc);
    std::filesystem::create_directories(testmode::root() / "shots");

    const auto xinput = GetModuleHandleW(L"xinput1_4.dll");
    const auto get_state = xinput != nullptr ? GetProcAddress(xinput, "XInputGetState") : nullptr;

    if (get_state == nullptr) {
        spdlog::error("[Test] XInputGetState not found, the bot can't press buttons");
    } else {
        m_xinput_hook = std::make_unique<FunctionHook>(get_state, &TestBot::xinput_get_state);

        if (!m_xinput_hook->create()) {
            spdlog::error("[Test] Failed to hook XInputGetState");
        }
    }

    std::thread{[this] { bot_thread(); }}.detach();
    spdlog::info("[Test] Test bot ready, commands from {}", (testmode::root() / "coop_test_cmd.txt").string());
    finish("ready");
    return std::nullopt;
}

// Pad 0 is the bot's: always connected, so the game sees a controller even on a PC without one.
DWORD WINAPI TestBot::xinput_get_state(DWORD user_index, XINPUT_STATE* state) {
    const auto original = g_bot->m_xinput_hook->get_original<decltype(xinput_get_state)>();

    if (user_index != 0 || state == nullptr) {
        return original(user_index, state);
    }

    const auto buttons = g_buttons.load();
    *state = {};
    state->dwPacketNumber = g_packet.load();
    state->Gamepad.wButtons = buttons & ~(LT | RT);
    state->Gamepad.bLeftTrigger = (buttons & LT) ? 255 : 0;
    state->Gamepad.bRightTrigger = (buttons & RT) ? 255 : 0;
    state->Gamepad.sThumbLX = g_stick_x.load();
    state->Gamepad.sThumbLY = g_stick_y.load();
    return ERROR_SUCCESS;
}

// Screenshots need the render thread: the bot thread asks, the next present takes the shot.
void TestBot::on_present() {
    if (g_bot != this) {
        return;
    }

    std::unique_lock lock{m_shot_mutex};

    if (!m_shot_request.empty()) {
        take_screenshot(m_shot_request);
        m_shot_request.clear();
        m_shot_done.notify_all();
    }
}

// Commands run on their own thread, not in present: the game stops presenting at times (a hung window,
// Steam's text input), and the bot must still answer.
void TestBot::bot_thread() {
    while (true) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

void TestBot::tick() {
    const auto now = std::chrono::steady_clock::now();

    if (m_busy && now >= m_until) {
        if (g_buttons.load() != 0 || g_stick_x.load() != 0 || g_stick_y.load() != 0) {
            // Release first, then give the game a moment before the next command.
            g_buttons = 0;
            g_stick_x = 0;
            g_stick_y = 0;
            ++g_packet;
            m_until = now + RELEASE_GAP;
            return;
        }

        m_busy = false;
        finish("ok " + m_current);
    }

    if (++m_frame % 25 == 0) {
        read_commands();
    }

    // Threads started after the watch command (loading workers) get the breakpoints too.
    if (m_frame % 100 == 0 && !m_watches.empty()) {
        arm_threads();
    }

    if (!m_busy && !m_queue.empty()) {
        const auto command = m_queue.front();
        m_queue.pop_front();
        start(command);
    }
}

void TestBot::read_commands() {
    const auto path = testmode::root() / "coop_test_cmd.txt";
    std::error_code ec{};

    if (!std::filesystem::exists(path, ec)) {
        return;
    }

    std::ifstream file{path};
    std::string line{};

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty() || line[0] == '#') {
            continue;
        }

        Command command{};
        command.text = line;
        std::istringstream words{line};
        words >> command.verb >> command.arg1 >> command.arg2 >> command.arg3 >> command.arg4;
        m_queue.push_back(command);
    }

    file.close();
    std::filesystem::remove(path, ec);
}

void TestBot::start(const Command& command) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = [](const std::string& s) { return std::chrono::milliseconds{s.empty() ? 0 : std::stoi(s)}; };

    m_current = command.text;

    try {
        if (command.verb == "pad") {
            WORD buttons{0};
            std::istringstream names{command.arg1};
            std::string name{};

            while (std::getline(names, name, '+')) {
                if (name == "LT") {
                    buttons |= LT;
                } else if (name == "RT") {
                    buttons |= RT;
                } else if (const auto it = BUTTONS.find(name); it != BUTTONS.end()) {
                    buttons |= it->second;
                } else {
                    finish("error unknown button " + name + ": " + command.text);
                    return;
                }
            }

            g_buttons = buttons;
            ++g_packet;
            m_busy = true;
            m_until = now + ms(command.arg2);
        } else if (command.verb == "stick") {
            g_stick_x = (SHORT)std::clamp(std::stof(command.arg1) * 32767.0f, -32767.0f, 32767.0f);
            g_stick_y = (SHORT)std::clamp(std::stof(command.arg2) * 32767.0f, -32767.0f, 32767.0f);
            ++g_packet;
            m_busy = true;
            m_until = now + ms(command.arg3);
        } else if (command.verb == "wait") {
            m_busy = true;
            m_until = now + ms(command.arg1);
        } else if (command.verb == "shot") {
            std::unique_lock lock{m_shot_mutex};
            m_shot_request = command.arg1.empty() ? "shot" : command.arg1;

            if (!m_shot_done.wait_for(lock, std::chrono::seconds{3}, [this] { return m_shot_request.empty(); })) {
                m_shot_request.clear();
                finish("error shot: the game rendered no frame for 3 s");
            }
        } else if (command.verb == "state") {
            finish(describe_state());
        } else if (command.verb == "host") {
            const auto error = AutomataMPMod::get()->host_game(command.arg1.empty() ? "6969" : command.arg1);
            finish(error ? "error host: " + *error : "ok " + command.text);
        } else if (command.verb == "join") {
            const auto error = AutomataMPMod::get()->join_game(command.arg1, command.arg2 == "host");
            finish(error ? "error join: " + *error : "ok " + command.text);
        } else if (command.verb == "coop") {
            finish(AutomataMPMod::get()->describe_coop());
        } else if (command.verb == "read") {
            // read <hex rva> <hex length>: hex dump of the exe's memory, 16 bytes per line.
            const auto base = (uintptr_t)GetModuleHandleW(nullptr);
            const auto rva = std::stoull(command.arg1, nullptr, 16);
            const auto length = std::min<uint64_t>(std::stoull(command.arg2, nullptr, 16), 0x1000);

            for (uint64_t offset = 0; offset < length; offset += 16) {
                std::ostringstream line{};
                line << "mem +0x" << std::hex << rva + offset << " ";
                std::string text{};

                for (auto k = 0; k < 16; ++k) {
                    const auto b = *(const uint8_t*)(base + rva + offset + k);
                    line << std::setw(2) << std::setfill('0') << (int)b << " ";
                    text += (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }

                finish(line.str() + text);
            }

            finish("ok " + command.text);
        } else if (command.verb == "snap") {
            take_snapshot(command.arg1);
        } else if (command.verb == "filter") {
            filter_candidates(command.arg1, command.arg2);
        } else if (command.verb == "cands") {
            list_candidates(command.arg1.empty() ? 30 : std::stoul(command.arg1));
        } else if (command.verb == "xref") {
            find_xrefs(std::stoull(command.arg1, nullptr, 16), command.arg2.empty() ? 0 : std::stoull(command.arg2, nullptr, 16));
        } else if (command.verb == "watch") {
            if (command.arg1 == "off") {
                clear_watches();
            } else {
                add_watch(std::stoull(command.arg1, nullptr, 16), command.arg2.empty() ? 8 : command.arg2 == "x" ? 0 : std::stoul(command.arg2));
            }
        } else if (command.verb == "entities") {
            list_entities(command.arg1.empty() ? 50.0f : std::stof(command.arg1));
        } else if (command.verb == "goto") {
            const auto entity_list = sdk::EntityList::get();
            const auto player = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

            if (player == nullptr || player->behavior == nullptr) {
                finish("error goto: no character");
            } else {
                const Vector4f target{std::stof(command.arg1), std::stof(command.arg2), std::stof(command.arg3), 1.0f};
                // Optional yaw in degrees around the up axis (0 = +z).
                const auto yaw = command.arg4.empty() ? 0.0f : glm::radians(std::stof(command.arg4));
                player->behavior->as<sdk::Pl0000>()->setPosRotResetHap(target, glm::angleAxis(yaw, glm::vec3{0.0f, 1.0f, 0.0f}));
                finish("ok " + command.text);
            }
        } else if (command.verb == "hits") {
            list_hits();
        } else if (command.verb == "scan") {
            scan_strings(command.text.substr(command.text.find(' ') + 1));
        } else {
            finish("error unknown command: " + command.text);
        }
    } catch (const std::exception& e) {
        finish(std::string{"error "} + e.what() + ": " + command.text);
    }
}

void TestBot::finish(const std::string& line) {
    spdlog::info("[Test] {}", line);
    m_out << line << std::endl;
}

std::string TestBot::describe_state() {
    std::ostringstream s{};
    s << "state loading=" << sdk::is_loading() << " phase=" << sdk::get_phase_name() << " phase_id=" << sdk::get_phase_id();
    const auto story = StoryWatcher::get().current();
    s << " event=ev" << std::hex << std::setw(4) << std::setfill('0') << story.event << std::dec;
    s << " route=" << (story.route != 0 ? std::string{story.route} : "none") << " hero=" << (story.character.empty() ? "none" : story.character);
    s << " events=" << story.events << " last_event=ev" << std::hex << std::setw(4) << std::setfill('0') << story.last_event << std::dec;

    if (!sdk::is_loading()) {
        const auto entity_list = sdk::EntityList::get();
        // The character we control: after a co-op swap that's the story buddy (9S), not the entity named "Player".
        const auto player = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

        if (player != nullptr && player->behavior != nullptr) {
            const auto& p = player->behavior->position();
            s << " player=" << p.x << "," << p.y << "," << p.z << " model=" << std::hex << (uint32_t)player->behavior->model_index() << std::dec;
        } else {
            s << " player=none";
        }
    }

    return s.str();
}

// Research helper: ASCII strings (8+ chars) in the writable sections of the exe that match a regex,
// with their offset from the exe base ("scan ^\d+_\d*_?[ABC]+_" finds phase names like 00_85_A_RobotM_Pro_2ndArea).
void TestBot::scan_strings(const std::string& pattern) {
    const std::regex re{pattern};
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    size_t hits{0};

    for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) {
            continue;
        }

        const auto start = (const char*)(base + section->VirtualAddress);
        const auto size = (size_t)section->Misc.VirtualSize;

        for (size_t j = 0; j < size;) {
            size_t length{0};

            while (j + length < size && start[j + length] >= 0x20 && start[j + length] < 0x7F) {
                ++length;
            }

            if (length >= 8 && start[j + length] == '\0') {
                const std::string s{start + j, length};

                if (std::regex_search(s, re) && hits++ < 50) {
                    std::ostringstream line{};
                    line << "found +0x" << std::hex << (section->VirtualAddress + j) << " " << s;
                    finish(line.str());
                }
            }

            j += length + 1;
        }
    }

    finish("ok scan " + pattern + " (" + std::to_string(hits) + " hits)");
}

// Research helper: code in the exe that addresses [target - window, target] RIP-relatively (the field may sit
// inside a global struct). Works on the decrypted code in memory (on disk .text is encrypted by SteamStub).
// Prints "xref +0x<instruction> -> +0x<address> (<first bytes>)", nearest addresses first, at most 40.
void TestBot::find_xrefs(uintptr_t target, uintptr_t window) {
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    std::vector<std::pair<uintptr_t, uintptr_t>> hits{}; // instruction rva, referenced rva

    for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const auto start = base + section->VirtualAddress;

        for (size_t j = 0; j + 4 <= section->Misc.VirtualSize; ++j) {
            const auto disp = *(const int32_t*)(start + j);

            // The displacement is followed by nothing, an imm8 or an imm32.
            for (const auto extra : {0, 1, 4}) {
                const auto referenced = section->VirtualAddress + j + 4 + extra + (intptr_t)disp;

                if (referenced <= target && referenced + window >= target) {
                    hits.emplace_back(section->VirtualAddress + j, referenced);
                }
            }
        }
    }

    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    for (size_t i = 0; i < hits.size() && i < 40; ++i) {
        std::ostringstream line{};
        line << "xref +0x" << std::hex << hits[i].first << " -> +0x" << hits[i].second << " (";

        for (auto k = 0; k < 12; ++k) {
            line << std::setw(2) << std::setfill('0') << (int)*(const uint8_t*)(base + hits[i].first - 4 + k) << (k == 3 ? "|" : " ");
        }

        line << ")";
        finish(line.str());
    }

    finish("ok xref (" + std::to_string(hits.size()) + " hits)");
}

// Research helpers in the style of Cheat Engine, over the writable sections of the exe (globals):
// snap <name> keeps a copy; filter <name> <rule> narrows the candidate bytes ("=N" equal to N, "!=N",
// "same:<other snapshot>", "diff:<other snapshot>"); the first filter starts from every byte; filter reset clears.
void TestBot::take_snapshot(const std::string& name) {
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    Snapshot snapshot{};

    for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) {
            continue;
        }

        const auto start = (const uint8_t*)(base + section->VirtualAddress);
        snapshot.ranges.push_back({section->VirtualAddress, std::vector<uint8_t>(start, start + section->Misc.VirtualSize)});
    }

    size_t bytes{0};

    for (const auto& range : snapshot.ranges) {
        bytes += range.second.size();
    }

    m_snapshots[name] = std::move(snapshot);
    finish("ok snap " + name + " (" + std::to_string(bytes) + " bytes)");
}

void TestBot::filter_candidates(const std::string& name, const std::string& rule) {
    if (name == "reset") {
        m_candidates.clear();
        m_candidates_started = false;
        finish("ok filter reset");
        return;
    }

    const auto it = m_snapshots.find(name);

    if (it == m_snapshots.end()) {
        finish("error filter: no snapshot " + name);
        return;
    }

    const Snapshot* other{};

    if (rule.starts_with("same:") || rule.starts_with("diff:")) {
        const auto other_it = m_snapshots.find(rule.substr(5));

        if (other_it == m_snapshots.end()) {
            finish("error filter: no snapshot " + rule.substr(5));
            return;
        }

        other = &other_it->second;
    }

    const auto value_of = [](const Snapshot& snapshot, uint32_t rva) -> int {
        for (const auto& [start, bytes] : snapshot.ranges) {
            if (rva >= start && rva < start + bytes.size()) {
                return bytes[rva - start];
            }
        }

        return -1;
    };

    const auto keep = [&](uint32_t rva, uint8_t value) {
        if (rule.starts_with("!=")) {
            return value != (uint8_t)std::stoi(rule.substr(2));
        }

        if (rule.starts_with("=")) {
            return value == (uint8_t)std::stoi(rule.substr(1));
        }

        const auto other_value = value_of(*other, rva);
        return rule.starts_with("same:") ? other_value == value : other_value != value;
    };

    std::vector<uint32_t> result{};

    if (!m_candidates_started) {
        for (const auto& [start, bytes] : it->second.ranges) {
            for (uint32_t i = 0; i < bytes.size(); ++i) {
                if (keep(start + i, bytes[i])) {
                    result.push_back(start + i);
                }
            }
        }

        m_candidates_started = true;
    } else {
        for (const auto rva : m_candidates) {
            const auto value = value_of(it->second, rva);

            if (value >= 0 && keep(rva, (uint8_t)value)) {
                result.push_back(rva);
            }
        }
    }

    m_candidates = std::move(result);
    finish("ok filter " + name + " " + rule + ": " + std::to_string(m_candidates.size()) + " candidates");
}

void TestBot::list_candidates(size_t count) {
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);

    for (size_t i = 0; i < m_candidates.size() && i < count; ++i) {
        std::ostringstream line{};
        line << "cand +0x" << std::hex << m_candidates[i] << " now=" << std::dec << (int)*(const uint8_t*)(base + m_candidates[i]);
        finish(line.str());
    }

    finish("ok cands (" + std::to_string(m_candidates.size()) + ")");
}

// Research helper: hardware write breakpoints (debug registers DR0-DR3) on exe memory in every thread of the
// game. The exception handler records the writing instruction, the call stack unwound through the exe's
// .pdata and the bytes after the write; `hits` prints them. Finds who writes a global (e.g. the story phase).
void TestBot::add_watch(uintptr_t rva, uint32_t length) {
    if (m_watches.size() >= 4) {
        finish("error watch: 4 watches already (watch off clears them)");
        return;
    }

    if (length != 0 && length != 1 && length != 2 && length != 4 && length != 8) {
        finish("error watch: length must be 1, 2, 4, 8 or x");
        return;
    }

    const auto address = (uintptr_t)GetModuleHandleW(nullptr) + rva;

    if (length != 0 && address % length != 0) {
        finish("error watch: the address must be aligned to the length");
        return;
    }

    if (m_exception_handler == nullptr) {
        m_exception_handler = AddVectoredExceptionHandler(1, &TestBot::on_exception);
    }

    g_watch_address[m_watches.size()] = address;
    g_watch_execute[m_watches.size()] = length == 0;
    m_watches.push_back({address, length});
    arm_threads();
    finish("ok " + m_current + " (" + std::to_string(m_armed_threads.size()) + " threads)");
}

void TestBot::clear_watches() {
    m_watches.clear();
    m_armed_threads.clear();
    arm_threads(); // writes empty debug registers
    m_armed_threads.clear();
    finish("ok watch off");
}

void TestBot::arm_threads() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 entry{sizeof(entry)};
    const auto pid = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();

    for (auto ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }

        const auto armed = m_armed_threads.find(entry.th32ThreadID);

        if (armed != m_armed_threads.end() && armed->second == m_watches.size()) {
            continue;
        }

        const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);

        if (thread == nullptr) {
            continue;
        }

        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

            if (GetThreadContext(thread, &context)) {
                DWORD64* registers[4]{&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
                context.Dr7 = 0;

                for (size_t i = 0; i < 4; ++i) {
                    *registers[i] = i < m_watches.size() ? m_watches[i].address : 0;

                    if (i < m_watches.size() && m_watches[i].length == 0) {
                        context.Dr7 |= 1ull << (i * 2); // RW 00, LEN 00: break on execution
                    } else if (i < m_watches.size()) {
                        // LEN encoding: 1 -> 00, 2 -> 01, 8 -> 10, 4 -> 11; RW 01 = break on data writes.
                        const auto length = m_watches[i].length;
                        const DWORD64 len = length == 1 ? 0 : length == 2 ? 1 : length == 8 ? 2 : 3;
                        context.Dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (len << (18 + i * 4));
                    }
                }

                if (SetThreadContext(thread, &context)) {
                    m_armed_threads[entry.th32ThreadID] = m_watches.size();
                }
            }

            ResumeThread(thread);
        }

        CloseHandle(thread);
    }

    CloseHandle(snapshot);
}

LONG WINAPI TestBot::on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto dr6 = info->ContextRecord->Dr6;

    if ((dr6 & 0xF) == 0) {
        return EXCEPTION_CONTINUE_SEARCH; // a single step of someone else (a debugger)
    }

    const auto slot = (uint32_t)((dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3);
    const auto index = g_hit_count.fetch_add(1);

    if (index < MAX_HITS) {
        auto& hit = g_hits[index];
        hit = {};
        hit.slot = slot;
        hit.rip = info->ContextRecord->Rip;

        // Unwind from the writing instruction (data breakpoints trap after it, so Rip is the next one).
        auto context = *info->ContextRecord;

        for (auto& frame : hit.stack) {
            DWORD64 image_base{};
            const auto function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);

            if (function == nullptr) {
                context.Rip = *(DWORD64*)context.Rsp; // leaf function
                context.Rsp += 8;
            } else {
                void* handler_data{};
                DWORD64 establisher{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
            }

            if (context.Rip == 0) {
                break;
            }

            frame = context.Rip;
        }

        if (g_watch_execute[slot]) {
            const auto& c = *info->ContextRecord;
            hit.args[0] = c.Rcx;
            hit.args[1] = c.Rdx;
            hit.args[2] = c.R8;
            hit.args[3] = c.R9;
            MEMORY_BASIC_INFORMATION mbi{};

            if (VirtualQuery((const void*)c.Rdx, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) != 0 &&
                c.Rdx + sizeof(hit.value) <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
                memcpy(hit.value, (const void*)c.Rdx, sizeof(hit.value) - 1);
            }
        } else {
            memcpy(hit.value, (const void*)g_watch_address[slot], sizeof(hit.value) - 1);
        }
    } else {
        ++g_hits_dropped;
    }

    info->ContextRecord->Dr6 = 0;

    if (g_watch_execute[slot]) {
        info->ContextRecord->EFlags |= 0x10000; // resume flag: run the instruction instead of breaking again
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// Research helper: what is around the player (NPCs, triggers with a behavior, enemies), nearest first.
void TestBot::list_entities(float radius) {
    const auto entity_list = sdk::EntityList::get();
    const auto player = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (player == nullptr || player->behavior == nullptr) {
        finish("error entities: no character");
        return;
    }

    const auto origin = player->behavior->position();
    std::vector<std::pair<float, std::string>> found{};

    for (size_t i = 0; i < entity_list->size(); ++i) {
        const auto entity = entity_list->get((uint32_t)i);

        if (entity == nullptr || entity->behavior == nullptr) {
            continue;
        }

        const auto p = entity->behavior->position();
        const auto distance = glm::length(p - origin);

        if (distance > radius) {
            continue;
        }

        std::ostringstream line{};
        line << "entity " << std::string{entity->name, strnlen(entity->name, sizeof(entity->name))} << " model=" << std::hex
             << (uint32_t)entity->behavior->model_index() << " handle=" << entity->handle << std::dec << " pos=" << p.x << ","
             << p.y << "," << p.z << " distance=" << distance;
        found.emplace_back(distance, line.str());
    }

    std::sort(found.begin(), found.end());

    for (size_t i = 0; i < found.size() && i < 500; ++i) {
        finish(found[i].second);
    }

    finish("ok entities (" + std::to_string(found.size()) + ")");
}

// Consecutive hits with the same stack are printed once with a count and the value after the last of them.
void TestBot::list_hits() {
    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    const auto end = base + nt->OptionalHeader.SizeOfImage;
    const auto count = std::min(g_hit_count.load(), MAX_HITS);
    const auto address = [&](uintptr_t a) {
        std::ostringstream s{};
        s << (a >= base && a < end ? "+0x" : "0x") << std::hex << (a >= base && a < end ? a - base : a);
        return s.str();
    };

    for (size_t i = 0; i < count;) {
        const auto& hit = g_hits[i];
        size_t same{1};

        while (i + same < count && g_hits[i + same].rip == hit.rip && !memcmp(g_hits[i + same].stack, hit.stack, sizeof(hit.stack))) {
            ++same;
        }

        const auto& last = g_hits[i + same - 1];
        std::ostringstream line{};
        line << "hit #" << i << " x" << same << " dr" << hit.slot << " at " << address(hit.rip) << " stack";

        for (const auto frame : hit.stack) {
            if (frame != 0) {
                line << " " << address(frame);
            }
        }

        std::string text{};

        for (const auto c : last.value) {
            text += (c >= 0x20 && c < 0x7F) ? c : '.';
        }

        if (g_watch_execute[hit.slot]) {
            line << " args " << std::hex << hit.args[0] << " " << hit.args[1] << " " << hit.args[2] << " " << hit.args[3] << std::dec;
        }

        line << " value " << text;
        finish(line.str());
        i += same;
    }

    finish("ok hits (" + std::to_string(g_hit_count.load()) + " writes, " + std::to_string(g_hits_dropped.load()) + " not kept)");
}

// Copies the back buffer before the mod's own UI is drawn on it and writes a 32-bit BMP.
void TestBot::take_screenshot(const std::string& name) {
    const auto& hook = g_framework->get_d3d11_hook();
    const auto device = hook != nullptr ? hook->get_device() : nullptr;
    const auto swap_chain = hook != nullptr ? hook->get_swap_chain() : nullptr;

    if (device == nullptr || swap_chain == nullptr) {
        finish("error shot: no D3D11 device");
        return;
    }

    ComPtr<ID3D11Texture2D> back_buffer{};
    ComPtr<ID3D11DeviceContext> context{};

    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        finish("error shot: no back buffer");
        return;
    }

    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);

    ComPtr<ID3D11Texture2D> source = back_buffer;

    if (desc.SampleDesc.Count > 1) {
        auto resolved_desc = desc;
        resolved_desc.SampleDesc = {1, 0};
        resolved_desc.BindFlags = 0;
        ComPtr<ID3D11Texture2D> resolved{};
        device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
        context->ResolveSubresource(resolved.Get(), 0, back_buffer.Get(), 0, desc.Format);
        source = resolved;
    }

    auto staging_desc = desc;
    staging_desc.SampleDesc = {1, 0};
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging{};

    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        finish("error shot: can't create a staging texture");
        return;
    }

    context->CopyResource(staging.Get(), source.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};

    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        finish("error shot: can't map the staging texture");
        return;
    }

    const auto rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const auto bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    if (!rgba && !bgra) {
        context->Unmap(staging.Get(), 0);
        finish("error shot: unsupported back buffer format " + std::to_string(desc.Format));
        return;
    }

    const auto path = testmode::root() / "shots" / (name + ".bmp");
    std::ofstream file{path, std::ios::binary};

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = (LONG)desc.Width;
    info.biHeight = -(LONG)desc.Height; // top-down
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file_header.bfType = 'MB';
    file_header.bfOffBits = sizeof(file_header) + sizeof(info);
    file_header.bfSize = file_header.bfOffBits + desc.Width * desc.Height * 4;

    file.write((const char*)&file_header, sizeof(file_header));
    file.write((const char*)&info, sizeof(info));

    std::vector<uint8_t> row(desc.Width * 4);

    for (UINT y = 0; y < desc.Height; ++y) {
        const auto src = (const uint8_t*)mapped.pData + y * mapped.RowPitch;

        for (UINT x = 0; x < desc.Width; ++x) {
            row[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
            row[x * 4 + 3] = 255;
        }

        file.write((const char*)row.data(), row.size());
    }

    context->Unmap(staging.Get(), 0);
    finish("shot " + path.string());
}
