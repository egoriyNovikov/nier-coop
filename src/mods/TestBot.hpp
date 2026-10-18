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
//   snap <name>, filter <name> <=N|!=N|same:<snap>|diff:<snap>|reset>, cands [n]   memory search like Cheat Engine
//   read <hex rva> <hex length>  hex dump of the exe's memory (research)
//   xref <hex rva> [hex window]  code addressing [rva - window, rva] RIP-relatively (research)
//   scan <regex>                 strings in the exe's writable sections matching regex, as "found +0x<offset> <text>"
class TestBot : public Mod {
public:
    std::string_view get_name() const override {
        return "TestBot";
    }

    std::optional<std::string> on_initialize() override;
    void on_present() override;

    static DWORD WINAPI xinput_get_state(DWORD user_index, XINPUT_STATE* state);

private:
    struct Command {
        std::string text{};
        std::string verb{};
        std::string arg1{};
        std::string arg2{};
        std::string arg3{};
    };

    void bot_thread();
    void tick();
    void read_commands();
    void start(const Command& command);
    void finish(const std::string& line);
    void take_screenshot(const std::string& name);
    void scan_strings(const std::string& pattern);
    void find_xrefs(uintptr_t target, uintptr_t window);
    void take_snapshot(const std::string& name);
    void filter_candidates(const std::string& name, const std::string& rule);
    void list_candidates(size_t count);

    struct Snapshot {
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ranges{}; // section rva, bytes
    };

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
};
