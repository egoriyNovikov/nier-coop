#include <spdlog/spdlog.h>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>

#include "hap/scene_state/SceneStateSystem.hpp"
#include "ScriptFunctions.hpp"
#include "Story.hpp"

namespace sdk {
namespace {
struct PhaseGlobal {
    int32_t id;
    char name[60];
};

PhaseGlobal* get_phase_global() {
    static PhaseGlobal* phase = []() -> PhaseGlobal* {
        // mov edi, [phase.id]; cmp edi, 100h; jnz; lea rdx, <string>; lea rcx, [phase.name]
        const auto code = utility::scan(utility::get_executable(), "8b 3d ? ? ? ? 81 ff 00 01 00 00 75 ? 48 8d 15 ? ? ? ? 48 8d 0d");

        if (!code) {
            spdlog::error("[Story] Failed to find the phase global");
            return nullptr;
        }

        const auto id = utility::calculate_absolute(*code + 2);
        const auto name = utility::calculate_absolute(*code + 24);

        if (name != id + 4) {
            spdlog::error("[Story] Phase global layout changed: id {:x}, name {:x}", id, name);
            return nullptr;
        }

        spdlog::info("[Story] Phase global: {:x}", id);
        return (PhaseGlobal*)id;
    }();

    return phase;
}
}

std::string get_phase_name() {
    const auto phase = get_phase_global();
    return phase != nullptr ? std::string{phase->name, strnlen(phase->name, sizeof(phase->name))} : std::string{};
}

int32_t get_phase_id() {
    const auto phase = get_phase_global();
    return phase != nullptr ? phase->id : -1;
}
}

namespace sdk {
namespace {
bool has_scene_state(void* system, std::string_view name) {
    // find() logs every lookup: look once.
    static const auto available = ScriptFunctions::get().find("SceneStateSystem.has") != nullptr;

    if (system == nullptr || !available) {
        return false;
    }

    return ((hap::scene_state::SceneStateSystem*)system)->has(hap::SceneStateName{name});
}
}

char get_route(void* system) {
    for (const auto route : {'A', 'B', 'C'}) {
        if (has_scene_state(system, std::string{"Route/"} + route)) {
            return route;
        }
    }

    return 0;
}

std::string get_story_character(void* system) {
    for (const auto character : {"2B", "9S", "A2"}) {
        if (has_scene_state(system, std::string{"PL/"} + character)) {
            return character;
        }
    }

    return {};
}
}
