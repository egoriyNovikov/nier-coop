#include <cstdint>

#include <spdlog/spdlog.h>

#include <sdk/Bunker.hpp>

#include "BunkerRingHold.hpp"

BunkerRingHold& BunkerRingHold::get() {
    static BunkerRingHold hold{};
    return hold;
}

void BunkerRingHold::install() {
    if (m_hook != nullptr) {
        return;
    }

    const auto function = sdk::get_bunker_ring_update_function();

    if (function == nullptr) {
        spdlog::error("[Bunker] The ring controller wasn't found, the client's Bunker ring will turn on its own");
        return;
    }

    m_hook = std::make_unique<FunctionHook>(function, &BunkerRingHold::on_update);

    if (!m_hook->create()) {
        spdlog::error("[Bunker] Failed to hook the ring controller");
        m_hook.reset();
        return;
    }

    spdlog::info("[Bunker] Hooked the ring controller {}", function);
}

void BunkerRingHold::on_update(void* controller) {
    auto& self = get();
    const auto original = self.m_hook->get_original<void(void*)>();

    if (!self.m_hold) {
        original(controller);
        return;
    }

    // [controller+0xe8] != 0: the update only copies the hero's position to [controller+0x60] (the position the
    // next step is measured from) and ends (+0x83bc60 on 7020666).
    auto& stopped = *(int32_t*)((uintptr_t)controller + 0xe8);
    const auto was = stopped;

    if (!self.m_logged.exchange(true)) {
        spdlog::info("[Bunker] Ring controller {} held for the host's ring (stop flag was {})", controller, was);
    }

    stopped = 1;
    original(controller);
    stopped = was;
}
