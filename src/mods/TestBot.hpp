#pragma once

#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

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

    void read_commands();
    void start(const Command& command);
    void finish(const std::string& line);
    void take_screenshot(const std::string& name);
    void scan_strings(const std::string& pattern);
    void find_xrefs(uintptr_t target, uintptr_t window);
    std::string describe_state();

    std::deque<Command> m_queue{};
    bool m_busy{false};
    std::string m_current{};
    std::chrono::steady_clock::time_point m_until{};
    std::ofstream m_out{};
    uint32_t m_frame{0};

    std::unique_ptr<FunctionHook> m_xinput_hook{};
};
