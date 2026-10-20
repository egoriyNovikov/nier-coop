#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <utility/FunctionHook.hpp>

// The client's own phase changes wait for the host's (TASKS 3.2.2, co-op concept 6 point 4, decision 2026-10-20).
// The game asks for the next story block through one small function (sdk::get_phase_request_function); in the
// client's game the request of the story is held back until the host's phase changes too, then let through. If the
// session ends first, it is let through at once: the client plays on alone. Requests from the player's death and
// from callers not yet known go through as before, they are only logged.
class PhaseRequestGate {
public:
    static PhaseRequestGate& get();

    void install();

    // Game thread, every think while in a session. hold: we follow the host's story. leader_phase: the host's phase,
    // empty if unknown.
    void set_leader(bool hold, const std::string& leader_phase);

    // Game thread, every think. Lets a held request through when its time has come.
    void update();

    bool is_holding() {
        std::scoped_lock _{m_mutex};
        return m_held;
    }

private:
    using Clock = std::chrono::steady_clock;

    PhaseRequestGate() = default;

    static void on_request();
    void release(const char* why);

    std::recursive_mutex m_mutex{};
    std::unique_ptr<FunctionHook> m_hook{};
    // Return address of the story's own call (the Pl0000 handler after the Bunker launch flight).
    uintptr_t m_story_caller{};
    bool m_hold{false};
    std::string m_leader_phase{};

    bool m_held{false};
    std::string m_held_phase{};        // our phase when the request came
    std::string m_held_leader_phase{}; // the host's phase then
    Clock::time_point m_held_at{};
};
