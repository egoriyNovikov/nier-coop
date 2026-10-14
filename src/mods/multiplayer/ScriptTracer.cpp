#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <Windows.h>
#include <hde64.h>

#include <spdlog/spdlog.h>

#include <sdk/ScriptFunctions.hpp>
#include <utility/Crc32.hpp>
#include <utility/Module.hpp>

#include "ScriptTracer.hpp"

namespace {
// One hook callback per traced function: the mid hook context does not say which function was hit.
constexpr size_t MAX_TRACED = 4096;

using Thunks = std::array<safetyhook::MidHookFn, MAX_TRACED>;

constexpr Thunks THUNKS = []<size_t... I>(std::index_sequence<I...>) {
    return Thunks{&ScriptTracer::on_call<I>...};
}(std::make_index_sequence<MAX_TRACED>{});

bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

// The table only stores CRC32 of the names. The names themselves are strings in the executable
// ("EntityLayout.enableCreateEntity", "Pl0000.changePlayer"...): hash every candidate string.
std::unordered_map<uint32_t, std::string> find_script_names(const std::unordered_map<uint32_t, sdk::ScriptFunction*>& by_crc) {
    std::unordered_map<uint32_t, std::string> names{};

    const auto game = utility::get_executable();
    const auto size = utility::get_module_size(game).value_or(0);
    const auto begin = (const char*)game;

    for (size_t i = 1; i + 4 < size; ++i) {
        if (begin[i - 1] != '\0' || !is_name_char(begin[i])) {
            continue;
        }

        size_t len = 0;
        size_t dots = 0;

        while (i + len < size && len < 96 && is_name_char(begin[i + len])) {
            dots += begin[i + len] == '.';
            ++len;
        }

        if (i + len >= size || begin[i + len] != '\0' || dots != 1 || len < 3 || begin[i] == '.' || begin[i + len - 1] == '.') {
            i += len;
            continue;
        }

        const auto crc = crc32((uint8_t*)begin + i, len);

        if (by_crc.contains(crc) && !names.contains(crc)) {
            names[crc] = std::string{begin + i, len};
        }

        i += len;
    }

    return names;
}

// safetyhook moves the first 5 bytes into a trampoline and only fixes 32-bit relative operands.
bool is_hookable(uintptr_t function) {
    size_t length = 0;

    while (length < 5) {
        hde64s hde{};
        const auto insn_length = hde64_disasm((const void*)(function + length), &hde);

        if (insn_length == 0 || (hde.flags & F_ERROR) != 0) {
            return false;
        }

        // Short jumps can't be relocated.
        if (hde.opcode == 0xEB || hde.opcode == 0xE3 || (hde.opcode >= 0x70 && hde.opcode <= 0x7F)) {
            return false;
        }

        length += insn_length;

        // A tiny function: the jump would overwrite whatever follows, only int3 padding is fine.
        if (hde.opcode == 0xC3 || hde.opcode == 0xC2) {
            for (auto i = length; i < 5; ++i) {
                if (*(const uint8_t*)(function + i) != 0xCC) {
                    return false;
                }
            }

            return true;
        }
    }

    return true;
}
}

ScriptTracer& ScriptTracer::get() {
    static ScriptTracer tracer;
    return tracer;
}

void ScriptTracer::exclude(uintptr_t function) {
    std::scoped_lock _{m_mutex};
    m_excluded.insert(function);
}

void ScriptTracer::set_enabled(bool enabled) {
    if (enabled) {
        std::scoped_lock _{m_mutex};

        if (!m_installed) {
            install();
            m_installed = true;
        }
    }

    if (m_enabled.exchange(enabled) != enabled) {
        spdlog::info("[Script] Tracing {}", enabled ? "enabled" : "disabled");
    }
}

void ScriptTracer::set_known_objects(const void* player, uint32_t player_model, const void* buddy, uint32_t buddy_model) {
    m_player = player;
    m_player_model = player_model;
    m_buddy = buddy;
    m_buddy_model = buddy_model;
}

void ScriptTracer::install() {
    const auto start = std::chrono::steady_clock::now();
    const auto scripts = sdk::ScriptFunctions::get().all();

    std::unordered_map<uint32_t, sdk::ScriptFunction*> by_crc{};

    for (auto script : scripts) {
        by_crc[script->crc] = script;
    }

    const auto names = find_script_names(by_crc);

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    size_t unnamed = 0;
    size_t skipped = 0;
    std::unordered_set<uintptr_t> hooked{};

    for (auto script : scripts) {
        const auto function = script->function;

        if (function == 0 || m_excluded.contains(function) || hooked.contains(function)) {
            continue;
        }

        if (m_traced.size() >= MAX_TRACED) {
            ++skipped;
            continue;
        }

        if (!is_hookable(function)) {
            ++skipped;
            continue;
        }

        auto hook = builder.create_mid((void*)function, THUNKS[m_traced.size()]);

        if (hook == nullptr) {
            ++skipped;
            continue;
        }

        const auto name = names.find(script->crc);
        unnamed += name == names.end();

        auto traced = std::make_unique<Traced>();
        traced->name = name != names.end() ? name->second : fmt::format("crc {:08x}", script->crc);
        traced->function = function;

        m_traced.push_back(std::move(traced));
        m_hooks.push_back(std::move(hook));
        hooked.insert(function);
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    spdlog::info("[Script] {} script functions, {} traced ({} without a name), {} skipped, took {} ms",
        scripts.size(), m_traced.size(), unnamed, skipped, ms);
}

void ScriptTracer::log_call(size_t index, uintptr_t this_ptr) {
    if (!m_enabled || index >= m_traced.size()) {
        return;
    }

    auto& traced = *m_traced[index];

    // Only the first call of a burst: functions called every frame show up once, not 60 times a second.
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto last = traced.last_call_ms.exchange(now);

    if (now - last < 2000) {
        return;
    }

    const auto object = (const void*)this_ptr;

    if (object != nullptr && object == m_player.load()) {
        spdlog::info("[Script] {} on player {:x}", traced.name, m_player_model.load());
    } else if (object != nullptr && object == m_buddy.load()) {
        spdlog::info("[Script] {} on buddy {:x}", traced.name, m_buddy_model.load());
    } else {
        spdlog::info("[Script] {} on {:x}", traced.name, this_ptr);
    }
}
