#include <intrin.h>
#include <Windows.h>

#include <spdlog/spdlog.h>

#include <sdk/Game.hpp>
#include <sdk/ScriptFunctions.hpp>
#include <sdk/Story.hpp>

#include "PhaseRequestGate.hpp"
#include "ScriptTracer.hpp"

PhaseRequestGate& PhaseRequestGate::get() {
    static PhaseRequestGate gate{};
    return gate;
}

void PhaseRequestGate::install() {
    std::scoped_lock _{m_mutex};

    if (m_hook != nullptr) {
        return;
    }

    const auto function = sdk::get_phase_request_function();

    if (function == nullptr) {
        spdlog::error("[Phase] The phase request function wasn't found, the client's phase changes won't wait for the host");
        return;
    }

    // The story's call is the last instruction of the signature the function was found by (see sdk::Story).
    m_story_caller = sdk::get_phase_request_caller();
    m_hook = std::make_unique<FunctionHook>(function, &PhaseRequestGate::on_request);

    if (!m_hook->create()) {
        spdlog::error("[Phase] Failed to hook the phase request function");
        m_hook.reset();
        return;
    }

    spdlog::info("[Phase] Hooked the phase request function {}", function);

    // TASKS 3.2.2: phase changes that don't go through the request. The script function SaveState.loadFromMemory is a
    // jump to the function the main loop applies the next story block with (+0x38e9a0 -> +0x9c5d20 on 7020666): a
    // story script can change the phase directly. Swapped in the script function table, the game's code stays as is.
    if (const auto script = sdk::ScriptFunctions::get().find("SaveState.loadFromMemory"); script != nullptr && script->function != 0) {
        m_load_from_memory = (bool (*)())script->function;
        script->function = (uintptr_t)&PhaseRequestGate::on_load_from_memory;
        ScriptTracer::get().exclude(script->function);
        spdlog::info("[Phase] Watching SaveState.loadFromMemory ({:x})", (uintptr_t)m_load_from_memory);
    } else {
        spdlog::error("[Phase] SaveState.loadFromMemory wasn't found, direct phase changes of the story script are not seen");
    }
}

bool PhaseRequestGate::on_load_from_memory() {
    auto& self = get();
    const auto rva = (uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr);
    const auto phase = sdk::get_phase_name();
    bool hold{};

    {
        std::scoped_lock _{self.m_mutex};
        hold = self.m_hold;
    }

    const auto loaded = self.m_load_from_memory();
    spdlog::info("[Phase] The story script applied the next story block itself (SaveState.loadFromMemory) in {} (caller +0x{:x}{}): {}", phase, rva,
        hold ? ", client, not held" : "", loaded ? "loaded" : "nothing to load");
    return loaded;
}

void PhaseRequestGate::set_leader(bool hold, const std::string& leader_phase, uint32_t leader_requests) {
    std::scoped_lock _{m_mutex};
    m_hold = hold;
    m_leader_phase = leader_phase;

    if (leader_requests != m_leader_requests) {
        m_leader_requests = leader_requests;
        m_leader_requests_changed_at = Clock::now();
    }
}

void PhaseRequestGate::on_request() {
    auto& self = get();
    const auto caller = (uintptr_t)_ReturnAddress();
    const auto rva = caller - (uintptr_t)GetModuleHandleW(nullptr);
    const auto phase = sdk::get_phase_name();
    std::unique_lock lock{self.m_mutex};

    const auto story = caller == self.m_story_caller;

    if (!self.m_hold) {
        spdlog::info("[Phase] The game asks for the next phase in {} (caller +0x{:x}{})", phase, rva, story ? ", story" : "");
        ++self.m_passed;
        lock.unlock();
        self.m_hook->get_original<void()>()();
        return;
    }

    if (!story) {
        spdlog::info("[Phase] Client tried to change phase in {} (caller +0x{:x}, not the story's call): let through", phase, rva);
        ++self.m_passed;
        lock.unlock();
        self.m_hook->get_original<void()>()();
        return;
    }

    if (self.m_held) {
        return;
    }

    self.m_held = true;
    self.m_held_phase = phase;
    self.m_held_leader_phase = self.m_leader_phase;
    self.m_held_leader_requests = self.m_leader_requests;
    self.m_held_at = Clock::now();
    spdlog::info("[Phase] Client tried to change phase in {} (caller +0x{:x}): held until the host's phase changes (host in {})", phase, rva,
        self.m_leader_phase.empty() ? "?" : self.m_leader_phase);
}

void PhaseRequestGate::update() {
    std::scoped_lock _{m_mutex};

    if (!m_held) {
        return;
    }

    if (!m_hold) {
        release("the session is over");
        return;
    }

    const auto phase = sdk::get_phase_name();

    if (!sdk::is_loading() && phase != m_held_phase) {
        spdlog::info("[Phase] Our phase changed to {} by itself, the held request from {} is dropped", phase, m_held_phase);
        m_held = false;
        return;
    }

    // The host's phase changed since: it went on, so do we. Whether it went where our request leads is seen after
    // the loading (the phase comparison of the co-op menu).
    if (!m_leader_phase.empty() && m_leader_phase != m_held_phase && m_leader_phase != m_held_leader_phase) {
        release(("the host's phase changed to " + m_leader_phase).c_str());
        return;
    }

    // The next block can have the same phase name: the flight from the Bunker loads 20_AB_buker_Aerial_battle again and
    // again (smoke run 2026-10-22, the client waited in the dark for good). So the host loading a block lets ours
    // through too, also when the host's came a moment before ours (both games reach the end of a flight together).
    if (m_leader_requests > m_held_leader_requests) {
        release("the host loaded the next story block");
    } else if (m_leader_requests > 0 && Clock::now() - m_leader_requests_changed_at < std::chrono::seconds{10} &&
               m_held_at - m_leader_requests_changed_at < std::chrono::seconds{10}) {
        release("the host loaded the next story block just before");
    }
}

void PhaseRequestGate::release(const char* why) {
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_held_at).count();
    spdlog::info("[Phase] Letting the held phase request from {} through after {} ms: {}", m_held_phase, waited, why);
    m_held = false;
    ++m_passed;
    m_hook->get_original<void()>()();
}
