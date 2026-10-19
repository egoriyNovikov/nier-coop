#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <Xinput.h>

#include <utility/FunctionHook.hpp>

#include "../Mod.hpp"

// Test bot of the automated QA (coop-anything playbook/automated-qa.md). Works only in a test copy of the game
// (testmode::enabled()). A test script drives the game through files in the test copy:
// - coop_test_cmd.txt: commands, one per line, read and deleted by the bot (write it to .tmp and rename);
// - coop_test_out.txt: one line per finished command ("ok <command>", "state ...", "shot <path>", "error ...").
// Commands:
//   pad <BUTTON[+BUTTON]> <ms>   hold buttons of a virtual XInput pad 0 (A B X Y START BACK UP DOWN LEFT RIGHT LB RB LT RT)
//   stick <x> <y> <ms>           hold the left stick (-1..1)
//   wait <ms>
//   shot <name>                  screenshot of the game frame to shots\<name>.bmp (rendered by the game, not the desktop)
//   state                        loading flag, player position
//   host [port]                  "Host Game": starts automatamp_server\server.exe and connects (port 6969 by default)
//   join <ip:port> [host]        "Join Game"; "host" = join as the player leading the story
//   coop                         connection, role and the other player as the mod sees them
//   snap <name>, filter <name> <=N|!=N|same:<snap>|diff:<snap>|reset>, cands [n] [snap ...]   memory search like Cheat Engine
//   read <hex rva> <hex length>  hex dump of the exe's memory (research)
//   readh <hex address> <hex length>  hex dump at an absolute address (heap objects)
//   find <hex rva>               pointers to exe base + rva in read-write memory (exe and heap): objects by vtable
//   xref <hex rva> [hex window]  code addressing [rva - window, rva] RIP-relatively (research)
//   scan <regex>                 strings in the exe's writable sections matching regex, as "found +0x<offset> <text>"
//   watch <hex rva> [1|2|4|8|x] hardware breakpoint (up to 4) on writes to exe memory, or x = on executing code; "watch off" clears them and the hits
//   watchh <hex address> [1|2|4|8|x]  the same at an absolute address (fields of heap objects)
//   entities [radius]            entities near the player: name, model, handle, position, distance (nearest first)
//   goto <x> <y> <z> [yaw]       teleport the character we control (Pl0000.setPosRotResetHap), yaw in degrees (0 = +z)
//   near <x> <y> <z> <r> [heap]  float triplets within r of a point in the exe's writable sections (+ heap): where a position is kept
//   rtti <hex address>           class name of the object at an absolute address (MSVC RTTI through its vtable)
//   setstep <n>                  sdk::set_phase_step(n): move the story step as a story conversation does (research, 3.6.1)
//   hits                         what watch caught: instruction, call stack (exe rvas), value after the write / rcx rdx r8 r9 rbx rsi rdi rbp
class TestBot : public Mod {
public:
    std::string_view get_name() const override {
        return "TestBot";
    }

    std::optional<std::string> on_initialize() override;
    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    void on_present() override;

    static DWORD WINAPI xinput_get_state(DWORD user_index, XINPUT_STATE* state);
    static BOOL WINAPI get_cursor_pos(POINT* point);

private:
    struct Command {
        std::string text{};
        std::string verb{};
        std::string arg1{};
        std::string arg2{};
        std::string arg3{};
        std::string arg4{};
    };

    void bot_thread();
    void tick();
    void read_commands();
    void start(const Command& command);
    void finish(const std::string& line);
    void take_screenshot(const std::string& name);
    void scan_strings(const std::string& pattern);
    void find_xrefs(uintptr_t target, uintptr_t window);
    void read_absolute(uintptr_t address, uint64_t length);
    void find_pointers(uintptr_t rva);
    void take_snapshot(const std::string& name);
    void filter_candidates(const std::string& name, const std::string& rule);
    void list_candidates(size_t count, const std::vector<std::string>& snapshots);
    void add_watch(uintptr_t address, uint32_t length);
    void find_near(const glm::vec3& point, float radius, bool heap);
    void describe_rtti(uintptr_t object);
    void clear_watches();
    void arm_threads();
    void list_hits();
    void list_entities(float radius);
    static LONG WINAPI on_exception(EXCEPTION_POINTERS* info);

    struct Watch {
        uintptr_t address{};
        uint32_t length{};
    };

    std::vector<Watch> m_watches{};
    std::unordered_map<DWORD, size_t> m_armed_threads{}; // thread id -> number of watches set in its debug registers
    void* m_exception_handler{};

    struct Snapshot {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ranges{}; // section rva, bytes
    };

    static int value_of(const Snapshot& snapshot, uint32_t rva); // -1 outside the snapshot

    std::unordered_map<std::string, Snapshot> m_snapshots{};
    std::vector<uint32_t> m_candidates{};
    bool m_candidates_started{false};
    std::string describe_state();

    std::deque<Command> m_queue{};
    bool m_busy{false};
    std::string m_current{};
    std::chrono::steady_clock::time_point m_until{};
    std::ofstream m_out{};
    uint32_t m_frame{0}; // bot thread ticks

    std::mutex m_shot_mutex{};
    std::condition_variable m_shot_done{};
    std::string m_shot_request{}; // name of the screenshot the bot thread waits for

    std::unique_ptr<FunctionHook> m_xinput_hook{};
    std::unique_ptr<FunctionHook> m_cursor_hook{}; // GetCursorPos: the game gets no mouse in a test copy
};
