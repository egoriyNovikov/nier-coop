#include <string_view>

#include <spdlog/spdlog.h>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>

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
        hook_events();
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

// Story events (cutscenes): the game builds "ev%04x_Start" (the sound event of an event start) from the event id,
// and "movie/ev%04x.usm" when it plays a pre-rendered movie. Found 2026-10-18 on build 7020666.
void StoryWatcher::hook_events() {
    // mov r8d, [rdi+4] (event id); lea rdx, <format>; lea rcx, [rsp+20h]; call sprintf.
    // The same code builds "ev%04x_Start" and "ev%04x_End" (sound events of an event start and end):
    // tell them apart by the format string.
    const auto module = utility::get_executable();
    const auto base = (uintptr_t)module;
    const auto size = ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    std::optional<uintptr_t> event_start{};
    std::optional<uintptr_t> event_end{};

    for (auto at = utility::scan(base, size, "44 8B 47 04 48 8D 15 ? ? ? ? 48 8D 4C 24 20 E8"); at;
         at = utility::scan(*at + 1, base + size - (*at + 1), "44 8B 47 04 48 8D 15 ? ? ? ? 48 8D 4C 24 20 E8")) {
        const auto format = std::string_view{(const char*)utility::calculate_absolute(*at + 7)};

        if (format == "ev%04x_Start") {
            event_start = at;
        } else if (format == "ev%04x_End") {
            event_end = at;
        }
    }

    // mov r9d, [r15+4] (event id); lea r8, "movie/ev%04x.usm"; lea edx, [rdi+40h]; lea rcx, [rbp]; call snprintf
    const auto movie = utility::scan(module, "45 8B 4F 04 4C 8D 05 ? ? ? ? 8D 57 40 48 8D 4D 00 E8");

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    // The hooks sit right after the mov, the id is in r8 / r9 there.
    if (event_start && event_end) {
        m_event_hook = builder.create_mid((void*)(*event_start + 4), [](safetyhook::Context& ctx) {
            get().on_event((uint32_t)ctx.r8, EventKind::START);
        });
        m_event_end_hook = builder.create_mid((void*)(*event_end + 4), [](safetyhook::Context& ctx) {
            get().on_event((uint32_t)ctx.r8, EventKind::END);
        });
    } else {
        spdlog::error("[Story] Event start/end code not found (start {}, end {}), cutscenes won't be tracked",
            event_start.has_value(), event_end.has_value());
    }

    if (movie) {
        m_movie_hook = builder.create_mid((void*)(*movie + 4), [](safetyhook::Context& ctx) {
            get().on_event((uint32_t)ctx.r9, EventKind::MOVIE);
        });
    } else {
        spdlog::error("[Story] Movie code not found, movies won't be logged");
    }
}

void StoryWatcher::on_event(uint32_t id, EventKind kind) {
    static constexpr const char* names[]{"Event start", "Event end", "Movie"};
    spdlog::info("[Story] {} ev{:04x}", names[(int)kind], id);

    std::scoped_lock _{m_mutex};

    if (kind == EventKind::START) {
        m_point.event = id;
        m_point.last_event = id;
        ++m_point.events;
    } else if (kind == EventKind::END && m_point.event == id) {
        m_point.event = 0;
    }
}
