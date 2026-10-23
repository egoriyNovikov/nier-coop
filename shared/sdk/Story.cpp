#include <Windows.h>

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

namespace {
uintptr_t find_phase_request_call() {
    static const auto call = []() -> uintptr_t {
        // The Pl0000 state handler that asks for the next phase after the Bunker launch (+0x50d68d on 7020666):
        // mov edx, 6bff8489h; lea rcx, [rsp+60h]; call; mov [rsp+70h], esi; lea rdx, [rsp+60h]; mov rcx, rdi; call;
        // nop; lea rcx, [rsp+68h]; call; nop; call <request>
        const auto code = utility::scan(utility::get_executable(),
            "ba 89 84 ff 6b 48 8d 4c 24 60 e8 ? ? ? ? 89 74 24 70 48 8d 54 24 60 48 8b cf e8 ? ? ? ? 90 48 8d 4c 24 68 e8 ? ? ? ? 90 e8");

        if (!code) {
            spdlog::error("[Story] Failed to find the phase request function");
            return 0;
        }

        return *code + 0x2c;
    }();

    return call;
}
}

uintptr_t get_phase_request_caller() {
    const auto call = find_phase_request_call();
    return call != 0 ? call + 5 : 0;
}

void* get_phase_request_function() {
    static void* function = []() -> void* {
        const auto call = find_phase_request_call();

        if (call == 0) {
            return nullptr;
        }

        const auto request = utility::calculate_absolute(call + 1);

        // mov dword ptr [state], 1 (the body is two stores and a ret)
        if (*(const uint16_t*)request != 0x05c7 || *(const uint32_t*)(request + 6) != 1) {
            spdlog::error("[Story] The phase request function at {:x} doesn't look as expected", request);
            return nullptr;
        }

        spdlog::info("[Story] Phase request function: {:x}", request);
        return (void*)request;
    }();

    return function;
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

std::string get_graph_name(const ActionGraph* graph) {
    const auto readable = [](const void* address, size_t size) {
        MEMORY_BASIC_INFORMATION info{};
        return address != nullptr && VirtualQuery(address, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT
            && (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) != 0 && (info.Protect & PAGE_GUARD) == 0
            && (uintptr_t)address + size <= (uintptr_t)info.BaseAddress + info.RegionSize;
    };

    if (!readable(graph, sizeof(void*))) {
        return {};
    }

    const auto description = *(const uint8_t* const*)graph;

    if (!readable(description, 0x30)) {
        return {};
    }

    const auto name = *(const char* const*)(description + 0x28);

    if (!readable(name, 64)) {
        return {};
    }

    std::string result{};

    // Shift-JIS as the game keeps it (most names are Japanese, e.g. 宝箱 for a chest, 依頼者_q210 for a quest giver).
    for (size_t i = 0; i < 64 && (uint8_t)name[i] >= 0x20; ++i) {
        result += name[i];
    }

    return result;
}

void* get_talk_function() {
    static void* function = []() -> void* {
        // push rbx; sub rsp, 40h; mov [rsp+20h], -2; mov rbx, rcx; mov edx, 28cb0efh (ON_TALK)
        const auto code = utility::scan(utility::get_executable(), "40 53 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 8b d9 ba fb 0e cb 28");

        if (!code) {
            spdlog::error("[Story] Failed to find the talk function");
            return nullptr;
        }

        spdlog::info("[Story] Talk function: {:x}", *code);
        return (void*)*code;
    }();

    return function;
}

void* find_layout_entry(uint32_t layout_id) {
    using Find = void* (*)(uint32_t);
    static Find find = []() -> Find {
        // push rsi; push rdi; push r12; push r14; push r15; sub rsp, 40h; mov [rsp+20h], -2; mov [rsp+70h], rbx;
        // mov [rsp+80h], rbp; mov edi, ecx; mov r10, [table]
        const auto code = utility::scan(utility::get_executable(),
            "40 56 57 41 54 41 56 41 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 70 48 89 ac 24 80 00 00 00 8b f9 4c 8b 15");

        if (!code) {
            spdlog::error("[Story] Failed to find the layout lookup function");
            return nullptr;
        }

        spdlog::info("[Story] Layout lookup function: {:x}", *code);
        return (Find)*code;
    }();

    return find != nullptr && layout_id >= 0x80 ? find(layout_id) : nullptr;
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
