#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <SafetyHook.hpp>

// Logs calls of the game's script functions ("Class.method", the functions the story scripts call),
// to see what the story does in cutscenes, Flight Unit, hacking (TASKS 2.1.1). Debug only: it hooks
// thousands of functions, so it is off until enabled in the menu.
class ScriptTracer {
public:
    static ScriptTracer& get();

    // Hooks every script function the first time it is enabled.
    void set_enabled(bool enabled);
    bool is_enabled() const { return m_enabled; }

    // Functions hooked elsewhere (MidHooks), not hooked twice.
    void exclude(uintptr_t function);

    // Game thread: who the player and the buddy are, to name the object a function runs on.
    void set_known_objects(const void* player, uint32_t player_model, const void* buddy, uint32_t buddy_model);

    template <size_t I>
    static void on_call(safetyhook::Context& context) {
        get().log_call(I, context.rcx);
    }

private:
    struct Traced {
        std::string name;
        uintptr_t function;
        std::atomic<int64_t> last_call_ms{0};
    };

    void install();
    void log_call(size_t index, uintptr_t this_ptr);

    std::mutex m_mutex{};
    std::atomic<bool> m_enabled{false};
    bool m_installed{false};
    std::unordered_set<uintptr_t> m_excluded{};
    std::vector<std::unique_ptr<Traced>> m_traced{};
    std::vector<std::unique_ptr<safetyhook::MidHook>> m_hooks{};

    std::atomic<const void*> m_player{nullptr};
    std::atomic<uint32_t> m_player_model{0};
    std::atomic<const void*> m_buddy{nullptr};
    std::atomic<uint32_t> m_buddy_model{0};
};
