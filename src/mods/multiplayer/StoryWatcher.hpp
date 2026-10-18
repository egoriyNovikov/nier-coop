#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

        bool operator==(const Point& other) const = default;
    };

    static StoryWatcher& get();

    // Game thread, every think (also during loading: the phase changes there).
    void update();

    Point current();

private:
    StoryWatcher() = default;

    void hook_scene_states();
    static bool on_scene_state(bool set, void* system, void* name);
    static bool set_hook(void* system, void* name);
    static bool reset_hook(void* system, void* name);

    std::mutex m_mutex{};
    Point m_point{};
    std::unordered_map<uint32_t, bool> m_scene_states{}; // CRC of the flag name -> set
    bool m_hooks_tried{false};
    std::unique_ptr<FunctionHook> m_set_hook{};
    std::unique_ptr<FunctionHook> m_reset_hook{};
};
