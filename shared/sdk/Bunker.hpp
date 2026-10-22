#pragma once

#include <cmath>
#include <optional>

#include <sdk/Math.hpp>
#include <spdlog/spdlog.h>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>

#include "EntityList.hpp"
#include "Entity.hpp"

namespace sdk {
// The Bunker's ring (found 2026-10-21 on Steam build 7020666). The corridors and rooms are a cylinder around the
// x axis. While the hero walks in the corridor's areas, the ring's controller (get_bunker_ring_update_function)
// turns the ring by the hero's step (with a speed limit) and the hero stays at the bottom; every room, NPC and the
// hangar's flight units turn with it (for each of them the angle around the axis plus the ring's angle stays the
// same). Teleporting the hero does not turn the ring by itself. The object is the behavior of the entity with
// model 0xf1200 at the ring's center; its cObj matrix rows are (0, cos a, -sin a), (0, sin a, cos a), (1, 0, 0),
// the center (+0x40, also the position +0x50) is (-64.85, 188.55, -0.68), euler angles at +0x90 (+0x94 = a).
//
// Two games turn their rings differently (in the client's game the host's puppet is the hero and drives it), so
// a world position means a different place of the Bunker in each. Positions go over the network in the ring's
// frame: the world position turned by the ring's angle.
class BunkerRing {
public:
    // The ring of the current scene, nullopt outside the Bunker. `handle` keeps the ring's entity between calls
    // (0: walk the entity list for it). Game thread.
    static std::optional<BunkerRing> find(uint32_t& handle) {
        const auto entity_list = EntityList::get();

        if (entity_list == nullptr) {
            return std::nullopt;
        }

        if (handle != 0) {
            if (const auto ring = from_entity(entity_list->get_by_handle(handle))) {
                return ring;
            }

            handle = 0;
        }

        for (size_t i = 0; i < entity_list->size(); ++i) {
            const auto entity = entity_list->get((uint32_t)i);

            if (const auto ring = from_entity(entity)) {
                handle = entity->handle;
                return ring;
            }
        }

        return std::nullopt;
    }

    float angle() const { return m_angle; }

    // Turns the ring of the current scene to `angle` (radians): the matrix rows, the orientation quaternion (+0x60,
    // the quaternion of the transposed matrix) and the euler angle +0x94, as the update of the class Bunker leaves
    // them. False outside the Bunker. Game thread.
    static bool set_angle(uint32_t& handle, float angle) {
        if (!find(handle)) {
            return false;
        }

        const auto object = (float*)EntityList::get()->get_by_handle(handle)->behavior;
        const auto c = std::cos(angle);
        const auto s = std::sin(angle);
        object[0x14 / 4] = c;
        object[0x18 / 4] = -s;
        object[0x24 / 4] = s;
        object[0x28 / 4] = c;

        // The quaternion of the transposed rows (0, c, -s), (0, s, c), (1, 0, 0) is (cos f, sin f, cos f, sin f) / sqrt 2
        // with f = pi/4 + angle/2 (checked against the game's values at four angles).
        const auto f = 0.78539816f + angle * 0.5f;
        object[0x60 / 4] = object[0x68 / 4] = std::cos(f) * 0.70710678f;
        object[0x64 / 4] = object[0x6c / 4] = std::sin(f) * 0.70710678f;
        object[0x94 / 4] = angle;
        return true;
    }

    // World position of this game -> the ring's frame, the same for both games.
    Vector3f to_ring(const Vector3f& world) const { return turn(world, m_angle); }
    // The ring's frame -> world position of this game.
    Vector3f to_world(const Vector3f& ring) const { return turn(ring, -m_angle); }

private:
    static std::optional<BunkerRing> from_entity(const Entity* entity) {
        if (entity == nullptr || entity->behavior == nullptr || !entity->behavior->is("class Bunker")) {
            return std::nullopt;
        }

        const auto object = (const float*)entity->behavior;
        const BunkerRing ring{std::atan2(object[0x24 / 4], object[0x14 / 4]), object[0x44 / 4], object[0x48 / 4]};

        if (!std::isfinite(ring.m_angle) || !std::isfinite(ring.m_center_y) || !std::isfinite(ring.m_center_z)) {
            return std::nullopt;
        }

        return ring;
    }

    BunkerRing(float angle, float center_y, float center_z)
        : m_angle{angle}, m_center_y{center_y}, m_center_z{center_z} {}

    Vector3f turn(const Vector3f& p, float angle) const {
        const auto c = std::cos(angle);
        const auto s = std::sin(angle);
        const auto dy = p.y - m_center_y;
        const auto dz = p.z - m_center_z;

        return {p.x, m_center_y + dy * c - dz * s, m_center_z + dy * s + dz * c};
    }

    float m_angle;
    float m_center_y;
    float m_center_z;
};

// The update of the ring's controller, void(controller) (+0x83bb60 on 7020666): measures the hero's step from the
// position it kept ([controller+0x60]; the hero's handle is [controller+0x10]), and inside the corridor's areas turns
// the ring by it. Flags at the start: [+0xb0] / [+0xe0] skip one update, [+0xe8] != 0 only keeps the hero's position.
// For hooks; nullptr if not found.
inline void* get_bunker_ring_update_function() {
    static void* function = []() -> void* {
        // mov rdi, rcx; add rcx, 70h; call; xor r14d, r14d; mov [rdi+0Ch], r14d; mov [rdi+150h], r14d; cmp [rdi+0B0h], r14d
        const auto code = utility::scan(utility::get_executable(),
            "48 8b f9 48 83 c1 70 e8 ? ? ? ? 45 33 f6 44 89 77 0c 44 89 b7 50 01 00 00 44 39 b7 b0 00 00 00");

        if (!code) {
            spdlog::error("[Bunker] Failed to find the ring controller's update");
            return nullptr;
        }

        const auto start = *code - 0x25;

        // push rbp; push rdi; push r14
        if (*(const uint32_t*)start != 0x41575540) {
            spdlog::error("[Bunker] The ring controller's update at {:x} doesn't start as expected", start);
            return nullptr;
        }

        spdlog::info("[Bunker] Ring controller update: {:x}", start);
        return (void*)start;
    }();

    return function;
}
}
