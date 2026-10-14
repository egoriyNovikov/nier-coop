#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include <sdk/BehaviorAppBase.hpp>
#include <sdk/Entity.hpp>
#include <sdk/EntityList.hpp>
#include <sdk/ScriptFunctions.hpp>

#include "EnemyDeathSync.hpp"

namespace {
// How often the entity list is scanned for new enemies.
constexpr auto discover_interval = std::chrono::milliseconds(100);
// A remote death we can't match yet is retried this long: the enemy may not have spawned here yet.
constexpr auto pending_timeout = std::chrono::seconds(10);
// Same enemy: first seen within this distance in both games...
constexpr float spawn_match_distance = 5.0f;
// ...or standing right where the other one died.
constexpr float position_match_distance = 2.0f;

// Kill steps: the story's own "die" routine first, then health 0, then removing the enemy.
constexpr auto die_routine_wait = std::chrono::milliseconds(1500);
constexpr auto health_zero_wait = std::chrono::seconds(6);

using ScriptMethod = void (*)(sdk::Behavior*);

ScriptMethod get_set_die_routine() {
    static ScriptMethod fn = []() -> ScriptMethod {
        auto function = sdk::ScriptFunctions::get().find("EmBase.setDieRtn");

        if (function == nullptr) {
            spdlog::error("[Enemies] EmBase.setDieRtn not found, enemies die through health 0");
            return nullptr;
        }

        spdlog::info("[Enemies] EmBase.setDieRtn at {:x}", (uintptr_t)function->function);
        return (ScriptMethod)function->function;
    }();

    return fn;
}

bool is_alive_entity(sdk::EntityList* entity_list, uint32_t handle, const sdk::Behavior* behavior) {
    auto entity = entity_list->get_by_handle(handle);
    return entity != nullptr && entity->behavior == behavior;
}
}

void EnemyDeathSync::think(sdk::EntityList* entity_list, std::vector<Death>& deaths) {
    if (entity_list == nullptr) {
        return;
    }

    const auto now = Clock::now();

    if (now >= m_next_discover) {
        m_next_discover = now + discover_interval;
        discover(entity_list);

        // Remote deaths that came before the enemy appeared here.
        if (!m_pending.empty()) {
            std::erase_if(m_pending, [&](const PendingDeath& pending) {
                if (match_death(entity_list, pending.death, false)) {
                    return true;
                }

                if (now >= pending.expires) {
                    ++m_stats.unmatched;
                    spdlog::info("[Enemies] No enemy {:x} near ({:.1f} {:.1f} {:.1f}) here, its death is not applied",
                        pending.death.model, pending.death.spawn_position.x, pending.death.spawn_position.y,
                        pending.death.spawn_position.z);
                    return true;
                }

                return false;
            });
        }
    }

    for (auto& enemy : m_enemies) {
        if (enemy.gone) {
            continue;
        }

        if (!is_alive_entity(entity_list, enemy.handle, enemy.behavior)) {
            enemy.gone = true;
            enemy.alive = false;

            if (enemy.kill_step > 0) {
                spdlog::info("[Enemies] {} ({:x}) is gone after kill step {}", enemy.name, enemy.model, enemy.kill_step);
                enemy.kill_step = 0;
            }

            continue;
        }

        auto behavior = (sdk::BehaviorAppBase*)enemy.behavior;
        enemy.last_position = behavior->position();

        if (enemy.kill_step > 0) {
            advance_kill(entity_list, enemy);
        }

        if (enemy.alive && behavior->health() == 0) {
            enemy.alive = false;

            // Killed for a remote report: the other game already knows.
            if (!enemy.paired) {
                enemy.paired = true;
                ++m_stats.deaths_sent;
                deaths.push_back(Death{enemy.model, enemy.spawn_position, enemy.last_position});

                spdlog::info("[Enemies] Our {} ({:x}) died at ({:.1f} {:.1f} {:.1f}), telling the other game", enemy.name,
                    enemy.model, enemy.last_position.x, enemy.last_position.y, enemy.last_position.z);
            }
        }
    }

    // Gone and accounted for: nothing will match them anymore.
    std::erase_if(m_enemies, [](const Enemy& enemy) { return enemy.gone && enemy.paired; });
}

void EnemyDeathSync::discover(sdk::EntityList* entity_list) {
    const auto size = entity_list->size();

    for (size_t i = 0; i < size; ++i) {
        auto entity = entity_list->get((uint32_t)i);

        if (entity == nullptr || entity->behavior == nullptr) {
            continue;
        }

        const auto handle = entity->handle;
        const auto behavior = entity->behavior;

        if (auto it = m_not_enemies.find(handle); it != m_not_enemies.end() && it->second == behavior) {
            continue;
        }

        const auto known = std::any_of(m_enemies.begin(), m_enemies.end(),
            [&](const Enemy& enemy) { return !enemy.gone && enemy.handle == handle && enemy.behavior == behavior; });

        if (known) {
            continue;
        }

        if (!behavior->is_networkable()) {
            m_not_enemies[handle] = behavior;
            continue;
        }

        auto app = behavior->as<sdk::BehaviorAppBase>();

        Enemy enemy{};
        enemy.handle = handle;
        enemy.behavior = behavior;
        enemy.name = std::string{entity->name, strnlen(entity->name, sizeof(entity->name))};
        enemy.model = behavior->model_index();
        enemy.spawn_position = behavior->position();
        enemy.last_position = enemy.spawn_position;
        enemy.alive = app->health() > 0;
        // Already dead when we first saw it: not our kill to report.
        enemy.paired = !enemy.alive;

        spdlog::debug("[Enemies] Tracking {} ({:x}) at ({:.1f} {:.1f} {:.1f})", enemy.name, enemy.model,
            enemy.spawn_position.x, enemy.spawn_position.y, enemy.spawn_position.z);

        m_enemies.push_back(std::move(enemy));
        ++m_stats.tracked;
    }
}

void EnemyDeathSync::on_remote_death(sdk::EntityList* entity_list, const Death& death) {
    ++m_stats.deaths_received;

    if (entity_list == nullptr || !match_death(entity_list, death, true)) {
        m_pending.push_back(PendingDeath{death, Clock::now() + pending_timeout});
    }
}

bool EnemyDeathSync::match_death(sdk::EntityList* entity_list, const Death& death, bool log_miss) {
    Enemy* best = nullptr;
    float best_score = 0.0f;

    for (auto& enemy : m_enemies) {
        if (enemy.paired || enemy.model != death.model) {
            continue;
        }

        const auto spawn_distance = glm::length(enemy.spawn_position - death.spawn_position);
        const auto position_distance = glm::length(enemy.last_position - death.position);

        if (spawn_distance > spawn_match_distance && position_distance > position_match_distance) {
            continue;
        }

        // Several enemies of one model often spawn at one point (waves): the one closest to where the
        // other died wins.
        const auto score = spawn_distance + 0.25f * position_distance;

        if (best == nullptr || score < best_score) {
            best = &enemy;
            best_score = score;
        }
    }

    if (best == nullptr) {
        if (log_miss) {
            spdlog::info("[Enemies] Enemy {:x} died in the other game, not here yet, waiting for it", death.model);
        }

        return false;
    }

    best->paired = true;

    // We killed it too, about at the same time.
    if (!best->alive || best->gone) {
        ++m_stats.already_dead;
        spdlog::info("[Enemies] {} ({:x}) died in the other game, already dead here", best->name, best->model);
        return true;
    }

    start_kill(entity_list, *best);
    return true;
}

void EnemyDeathSync::start_kill(sdk::EntityList* entity_list, Enemy& enemy) {
    ++m_stats.killed;
    spdlog::info("[Enemies] {} ({:x}) died in the other game, killing ours at ({:.1f} {:.1f} {:.1f})", enemy.name,
        enemy.model, enemy.last_position.x, enemy.last_position.y, enemy.last_position.z);

    auto behavior = (sdk::Behavior*)enemy.behavior;
    const auto set_die_routine = behavior->is("class EmBase") ? get_set_die_routine() : nullptr;

    if (set_die_routine != nullptr) {
        set_die_routine(behavior);
        enemy.kill_step = 1;
        enemy.next_kill_step = Clock::now() + die_routine_wait;
    } else {
        // Animals and the like: straight to health 0.
        enemy.kill_step = 1;
        enemy.next_kill_step = Clock::now();
    }
}

void EnemyDeathSync::advance_kill(sdk::EntityList* entity_list, Enemy& enemy) {
    if (Clock::now() < enemy.next_kill_step) {
        return;
    }

    auto behavior = (sdk::BehaviorAppBase*)enemy.behavior;

    if (enemy.kill_step == 1) {
        if (behavior->health() == 0) {
            spdlog::info("[Enemies] {} ({:x}): the die routine worked (health 0)", enemy.name, enemy.model);
            enemy.kill_step = 0;
            return;
        }

        spdlog::info("[Enemies] {} ({:x}): still has health {}, setting it to 0", enemy.name, enemy.model, behavior->health());
        behavior->health() = 0;
        enemy.forced_health = true;
        enemy.kill_step = 2;
        enemy.next_kill_step = Clock::now() + health_zero_wait;
        return;
    }

    if (enemy.kill_step == 2) {
        // Health 0 alone didn't make the game remove it: remove it ourselves.
        spdlog::info("[Enemies] {} ({:x}): still here {} s after health 0, removing it", enemy.name, enemy.model,
            std::chrono::duration_cast<std::chrono::seconds>(health_zero_wait).count());
        enemy.kill_step = 0;
        ((sdk::Behavior*)behavior)->terminate();
    }
}

void EnemyDeathSync::clear() {
    m_enemies.clear();
    m_not_enemies.clear();
    m_pending.clear();
}

EnemyDeathSync::Stats EnemyDeathSync::take_stats() {
    auto stats = m_stats;
    m_stats = {};
    return stats;
}
