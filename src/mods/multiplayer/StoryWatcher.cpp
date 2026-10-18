#include <spdlog/spdlog.h>

#include <sdk/ScriptFunctions.hpp>
#include <sdk/Story.hpp>

#include "ScriptTracer.hpp"
#include "StoryWatcher.hpp"

StoryWatcher& StoryWatcher::get() {
    static StoryWatcher watcher{};
    return watcher;
}

void StoryWatcher::update() {
    if (!m_hooks_tried) {
        m_hooks_tried = true;
        hook_scene_states();
    }

    const auto phase = sdk::get_phase_name();
    const auto phase_id = sdk::get_phase_id();

    std::scoped_lock _{m_mutex};

    if (phase != m_point.phase || phase_id != m_point.phase_id) {
        spdlog::info("[Story] Phase {} ({}) -> {} ({})", m_point.phase.empty() ? "none" : m_point.phase, m_point.phase_id,
            phase.empty() ? "none" : phase, phase_id);
        m_point.phase = phase;
        m_point.phase_id = phase_id;
    }
}

StoryWatcher::Point StoryWatcher::current() {
    std::scoped_lock _{m_mutex};
    return m_point;
}

void StoryWatcher::hook_scene_states() {
    auto& functions = sdk::ScriptFunctions::get();
    const auto set = functions.find("SceneStateSystem.set");
    const auto reset = functions.find("SceneStateSystem.reset");

    if (set == nullptr || reset == nullptr) {
        spdlog::error("[Story] SceneStateSystem.set/reset not found, scene states won't be logged");
        return;
    }

    // The script tracer must not hook them a second time.
    ScriptTracer::get().exclude(set->function);
    ScriptTracer::get().exclude(reset->function);

    m_set_hook = std::make_unique<FunctionHook>(set->function, &StoryWatcher::set_hook);
    m_reset_hook = std::make_unique<FunctionHook>(reset->function, &StoryWatcher::reset_hook);

    if (!m_set_hook->create() || !m_reset_hook->create()) {
        spdlog::error("[Story] Failed to hook SceneStateSystem.set/reset");
    }
}

bool StoryWatcher::set_hook(void* system, void* name) {
    return on_scene_state(true, system, name);
}

bool StoryWatcher::reset_hook(void* system, void* name) {
    return on_scene_state(false, system, name);
}

// name is a hap::SceneStateName const&: a pointer to data with the CRC at +0x20 and, for short names,
// the text at +0x38 (sdk/hap/SceneStateName.hpp; the text part is a guess, logged only if it looks like text).
bool StoryWatcher::on_scene_state(bool set, void* system, void* name) {
    auto& self = get();
    const auto& hook = set ? self.m_set_hook : self.m_reset_hook;
    const auto result = hook->get_original<bool(void*, void*)>()(system, name);

    uint32_t crc{0};
    std::string text{};

    if (name != nullptr) {
        const auto data = *(const uint8_t**)name;

        if (data != nullptr) {
            crc = *(const uint32_t*)(data + 0x20);
            const auto chars = (const char*)(data + 0x38);

            for (size_t i = 0; i < 64 && chars[i] >= 0x20 && chars[i] < 0x7F; ++i) {
                text += chars[i];
            }
        }
    }

    // Scripts set the same flags every frame and reset hundreds of never-set ones on a chapter load:
    // only real changes are logged and counted.
    std::scoped_lock _{self.m_mutex};
    const auto it = self.m_scene_states.find(crc);
    const auto known = it != self.m_scene_states.end();

    if ((known && it->second == set) || (!known && !set)) {
        self.m_scene_states[crc] = set;
        return result;
    }

    self.m_scene_states[crc] = set;
    ++self.m_point.scene_state_changes;
    spdlog::info("[Story] Scene state {} {:08x} \"{}\"", set ? "set" : "reset", crc, text);
    return result;
}
