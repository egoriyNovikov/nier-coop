#pragma once

#include "schema/Packets_generated.h"

namespace sdk {
class Pl0000;
}

class Player {
public:
    void set_guid(uint64_t guid) { m_guid = guid; }

    uint64_t get_guid() const { return m_guid; }

    void set_name(const std::string& name) { m_name = name; }

    const std::string& get_name() const { return m_name; }

    void set_player_data(const nier::PlayerData& movement) { m_player_data = movement; }

    auto& get_player_data() { return m_player_data; }

    uint32_t get_handle() { return m_entity_handle; }

    void set_handle(uint32_t handle) { m_entity_handle = handle; }

    float get_start_tick() { return m_start_tick; }

    void set_start_tick(float tick) { m_start_tick = tick; }

    uint32_t get_model() const { return m_model; }

    void set_model(uint32_t model) { m_model = model; }

    // True if the puppet is the game's own story buddy rather than a "partner" we spawned.
    bool is_story_buddy() const { return m_is_story_buddy; }

    void set_story_buddy(bool value) { m_is_story_buddy = value; }

    sdk::Pl0000* get_entity();

private:
    std::string m_name{};
    uint64_t m_guid{};
    uint32_t m_entity_handle{0};
    float m_start_tick{0.0f};
    uint32_t m_model{0};
    bool m_is_story_buddy{false};
    nier::PlayerData m_player_data;
};