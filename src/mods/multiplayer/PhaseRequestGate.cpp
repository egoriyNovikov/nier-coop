#include <intrin.h>
#include <Windows.h>

#include <spdlog/spdlog.h>

#include <sdk/Game.hpp>
#include <sdk/Story.hpp>

#include "PhaseRequestGate.hpp"

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
}

void PhaseRequestGate::set_leader(bool hold, const std::string& leader_phase) {
    std::scoped_lock _{m_mutex};
    m_hold = hold;
    m_leader_phase = leader_phase;
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
        lock.unlock();
        self.m_hook->get_original<void()>()();
        return;
    }

    if (!story) {
        spdlog::info("[Phase] Client tried to change phase in {} (caller +0x{:x}, not the story's call): let through", phase, rva);
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
    }
}

void PhaseRequestGate::release(const char* why) {
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_held_at).count();
    spdlog::info("[Phase] Letting the held phase request from {} through after {} ms: {}", m_held_phase, waited, why);
    m_held = false;
    m_hook->get_original<void()>()();
}
