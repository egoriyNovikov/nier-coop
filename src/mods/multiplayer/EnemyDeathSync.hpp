#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <sdk/Math.hpp>

namespace sdk {
class Behavior;
class EntityList;
}

// Shared enemy deaths (co-op concept, section 8, option (b) of TASKS 1.3.9). Every game keeps the
// enemies of its own story; when an enemy dies in one game, the same enemy dies in the other one.
// There are no common ids for enemies between two games, so an enemy is recognized by its model and
// the place it was first seen (where the story spawned it), which is the same in both games.
class EnemyDeathSync {
public:
    struct Death {
        uint32_t model{};
        Vector3f spawn_position{};
        Vector3f position{};
    };

    struct Stats {
        uint32_t tracked{};
        uint32_t deaths_sent{};
        uint32_t deaths_received{};
        uint32_t killed{};
        uint32_t already_dead{};
        uint32_t unmatched{};
    };

    // Game thread, every frame. Finds new enemies and returns the deaths of our own enemies to send.
    void think(sdk::EntityList* entity_list, std::vector<Death>& deaths);

    // Game thread. An enemy died in the other game: kill ours. Retried for a while if we don't have it
    // yet (the other game is a bit ahead).
    void on_remote_death(sdk::EntityList* entity_list, const Death& death);

    // The game destroys every entity on a loading screen.
    void clear();

    Stats take_stats();

private:
    using Clock = std::chrono::steady_clock;

    struct Enemy {
        uint32_t handle{};
        const sdk::Behavior* behavior{};
        std::string name{};
        uint32_t model{};
        Vector3f spawn_position{};
        Vector3f last_position{};
        bool alive{true};
        bool gone{false};
        // Its death is already accounted for: we reported it, or killed it for a remote report.
        bool paired{false};

        // Kill for a remote report, step by step until one works (see advance_kill).
        int kill_step{0};
        bool forced_health{false};
        Clock::time_point next_kill_step{};
    };

    struct PendingDeath {
        Death death{};
        Clock::time_point expires{};
    };

    void discover(sdk::EntityList* entity_list);
    // Returns true if the death was matched to one of our enemies (alive or not).
    bool match_death(sdk::EntityList* entity_list, const Death& death, bool log_miss);
    void start_kill(sdk::EntityList* entity_list, Enemy& enemy);
    void advance_kill(sdk::EntityList* entity_list, Enemy& enemy);

    std::vector<Enemy> m_enemies{};
    // Entities that are not enemies, so RTTI isn't checked again on every scan.
    std::unordered_map<uint32_t, const sdk::Behavior*> m_not_enemies{};
    std::vector<PendingDeath> m_pending{};
    Clock::time_point m_next_discover{};
    Stats m_stats{};
};
