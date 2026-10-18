#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <SafetyHook.hpp>
#include <utility/FunctionHook.hpp>

// Where our game is in the story (TASKS 2.3, 2.1.3). Logs every change as "[Story] ...":
// - the phase (sdk::get_phase_name), polled on the game thread;
// - scene state flags the story scripts set and reset (SceneStateSystem.set / .reset script functions).
class StoryWatcher {
public:
    struct Point {
        std::string phase{};
        int32_t phase_id{-1};
        uint32_t scene_state_changes{0}; // how many flags were set/reset since the start, a cheap "something happened"
        uint32_t event{0}; // story event (cutscene) playing now, ev%04x; 0 if none
        uint32_t last_event{0}; // id of the last story event that started
        uint32_t events{0}; // events and movies started since the start

        bool operator==(const Point& other) const = default;
    };

    static StoryWatcher& get();

    // Game thread, every think (also during loading: the phase changes there).
    void update();

    Point current();

private:
    StoryWatcher() = default;

    void hook_scene_states();
    void hook_events();
    enum class EventKind { START, END, MOVIE };
    void on_event(uint32_t id, EventKind kind);
    static bool on_scene_state(bool set, void* system, void* name);
    static bool set_hook(void* system, void* name);
    static bool reset_hook(void* system, void* name);

    std::mutex m_mutex{};
    Point m_point{};
    std::unordered_map<uint32_t, bool> m_scene_states{}; // CRC of the flag name -> set
    bool m_hooks_tried{false};
    std::unique_ptr<FunctionHook> m_set_hook{};
    std::unique_ptr<FunctionHook> m_reset_hook{};
    std::unique_ptr<safetyhook::MidHook> m_event_hook{};
    std::unique_ptr<safetyhook::MidHook> m_event_end_hook{};
    std::unique_ptr<safetyhook::MidHook> m_movie_hook{};
};
