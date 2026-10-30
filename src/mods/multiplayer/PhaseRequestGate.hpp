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
// session ends first, it is let through at once: the client plays on alone. While the death is shared (TASKS 1.4.4)
// the client's death is held the same way: its Game Over loads when the host's does. Requests of callers not yet
// known go through as before, they are only logged. The story script can also apply the next
// block itself (SaveState.loadFromMemory, found 2026-10-22), bypassing the request: that is only logged for now.
class PhaseRequestGate {
public:
    static PhaseRequestGate& get();

    void install();

    // Game thread, every think while in a session. hold: we follow the host's story. leader_phase: the host's phase,
    // empty if unknown. leader_requests: how many story blocks the host's game has loaded (its passed_requests()).
    void set_leader(bool hold, const std::string& leader_phase, uint32_t leader_requests);

    // Game thread, every think: whether our Game Over waits for the host's (shared death, NierClient::is_death_shared).
    void set_shared_death(bool shared) {
        std::scoped_lock _{m_mutex};
        m_shared_death = shared;
    }

    // Requests for the next story block this game let through since it started; the host sends it in its story state.
    uint32_t passed_requests() {
        std::scoped_lock _{m_mutex};
        return m_passed;
    }

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
    static bool on_load_from_memory();
    void release(const char* why);

    std::recursive_mutex m_mutex{};
    std::unique_ptr<FunctionHook> m_hook{};
    // SaveState.loadFromMemory as the script function table had it.
    bool (*m_load_from_memory)(){nullptr};
    // Return address of the story's own call (the Pl0000 handler after the Bunker launch flight).
    uintptr_t m_story_caller{};
    bool m_hold{false};
    bool m_shared_death{false};
    std::string m_leader_phase{};
    uint32_t m_leader_requests{0};
    Clock::time_point m_leader_requests_changed_at{};
    uint32_t m_passed{0};

    bool m_held{false};
    bool m_held_death{false};          // the request is our character's death
    std::string m_held_phase{};        // our phase when the request came
    std::string m_held_leader_phase{}; // the host's phase then
    uint32_t m_held_leader_requests{0}; // the host's loaded blocks then
    Clock::time_point m_held_at{};
};
