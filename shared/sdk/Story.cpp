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

int32_t get_phase_step() {
    static const auto step = []() -> int32_t* {
        // mov [rcx+98h], eax; mov [step], eax; mov [rdi+654h], ebp (the game applies a pending step, +0x9480b0 on 7020666)
        const auto code = utility::scan(utility::get_executable(), "89 81 98 00 00 00 89 05 ? ? ? ? 89 af 54 06 00 00");

        if (!code) {
            spdlog::error("[Story] Failed to find the phase step global");
            return nullptr;
        }

        const auto address = utility::calculate_absolute(*code + 8);
        spdlog::info("[Story] Phase step global: {:x}", address);
        return (int32_t*)address;
    }();

    return step != nullptr ? *step : -2;
}

bool set_phase_step(int32_t step) {
    static const auto set_pending_step = []() -> void (*)(int32_t) {
        // push rbx; sub rsp, 20h; cmp [hud], 0; mov ebx, ecx; jz; mov rax, [lock]; test eax, eax; jz; lea rcx, [lock];
        // call [EnterCriticalSection]; mov rax, [lock]; test ebx, ebx; jz; cmp [pending], ebx (+0x8fae70 on 7020666)
        const auto code = utility::scan(utility::get_executable(),
            "40 53 48 83 ec 20 83 3d ? ? ? ? 00 8b d9 74 ? 48 8b 05 ? ? ? ? 85 c0 74 ? 48 8d 0d ? ? ? ? ff 15 ? ? ? ? 48 8b 05 ? ? ? ? 85 db 74 ? 39 1d");

        if (!code) {
            spdlog::error("[Story] Failed to find the function that sets the phase step");
            return nullptr;
        }

        spdlog::info("[Story] Set phase step function: {:x}", *code);
        return (void (*)(int32_t))*code;
    }();

    if (set_pending_step == nullptr) {
        return false;
    }

    set_pending_step(step);
    return true;
}

namespace {
// bool (hap::Action*, uint32_t state hash, int32_t param), +0x356080 on 7020666.
uintptr_t find_set_action_state() {
    static const auto function = []() -> uintptr_t {
        // push rdi; sub rsp, 40h; mov [rsp+20h], -2; mov [rsp+50h], rbx; mov ebx, r8d; mov rdi, rcx;
        // lea rcx, [rsp+28h]; call <make description>; mov [rsp+38h], ebx; lea rdx, [rsp+28h]; mov rcx, rdi; call <set>
        const auto code = utility::scan(utility::get_executable(),
            "40 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 50 41 8b d8 48 8b f9 48 8d 4c 24 28 e8 ? ? ? ? 89 5c 24 38 48 8d 54 24 28 48 8b cf e8");

        if (!code) {
            spdlog::error("[Story] Failed to find the function that sets an action's state");
            return 0;
        }

        spdlog::info("[Story] Set action state function: {:x}", *code);
        return *code;
    }();

    return function;
}
}

bool set_action_state(void* action, uint32_t state_hash, int32_t param) {
    const auto set_state = (bool (*)(void*, uint32_t, int32_t))find_set_action_state();
    return set_state != nullptr && action != nullptr && set_state(action, state_hash, param);
}

void* get_action_state_function() {
    const auto wrapper = find_set_action_state();
    // The second call of the wrapper (offset 48).
    return wrapper != 0 ? (void*)utility::calculate_absolute(wrapper + 49) : nullptr;
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
