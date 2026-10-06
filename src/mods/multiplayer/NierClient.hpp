#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>

#include <enetpp/client.h>

#include "Player.hpp"
#include "EntitySync.hpp"
#include "schema/Packets_generated.h"

struct Packet;

class NierClient : public enetpp::client {
public:
    NierClient(
        const std::string& host,
        const std::string& port = "6969",
        const std::string& name = "Client",
        const std::string& password = "");
    virtual ~NierClient();

    // Game thread only. Handles packets and drives the puppets.
    void think();
    // Safe from any thread. Only drains network events, never touches game entities.
    // Used while the world is not available (main menu, loading screens).
    void pump();
    // Forget every entity handle we hold. Called when the game starts loading.
    void on_world_unloaded();
    // Game thread only. Gives story buddies back to the game's AI before disconnecting.
    void release_puppets();

    void on_draw_ui();
    void on_frame();
    bool is_connected() { return get_connection_state() == enetpp::CONNECT_CONNECTED; }
    bool is_connecting() { return get_connection_state() == enetpp::CONNECT_CONNECTING; }
    bool is_in_world() const { return m_in_world; }
    bool is_in_session() const { return m_welcome_received; }
    // False while the player controls something that isn't 2B/9S/A2 (Flight Unit, hacking...).
    bool has_local_character() const { return m_has_local_character; }

    void send_packet(nier::PacketType id, const uint8_t* data = nullptr, size_t size = 0);
    void send_animation_start(uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4);
    void send_buttons(const uint32_t* buttons);

    void send_entity_packet(nier::PacketType id, uint32_t guid, const uint8_t* data = nullptr, size_t size = 0);
    void send_entity_create(uint32_t guid, sdk::EntitySpawnParams* data);
    void send_entity_destroy(uint32_t guid);
    void send_entity_data(uint32_t guid, sdk::BehaviorAppBase* entity);
    void send_entity_animation_start(uint32_t guid, uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4);

    void on_entity_created(sdk::Entity* entity, sdk::EntitySpawnParams* data);
    void on_entity_deleted(sdk::Entity* entity);
    
    const auto get_guid() const {
        return m_guid;
    }

    const auto is_master_client() const {
        return m_is_master_client;
    }

    const auto& get_players() const {
        return m_players;
    }

    // Co-op options.
    bool use_story_buddy{true}; // Puppet the game's own buddy (e.g. 9S) instead of spawning a new partner.
    bool auto_swap_character{true}; // Client takes over the buddy if it plays the same character as the host.
    bool spawn_partner{false}; // Spawn a "partner" for the other player when the game gives us no story buddy.
    bool sync_enemies{false}; // Enemies come from the master client. Otherwise everyone keeps the enemies of their own story.

private:
    void on_connect();
    void on_disconnect();
    void on_data_received(const enet_uint8* data, size_t size);
    void on_packet_received(const nier::Packet* packet);
    void on_player_packet_received(nier::PacketType packet_type, const nier::PlayerPacket* packet);
    void on_entity_packet_received(nier::PacketType packet_type, const nier::EntityPacket* packet);

    void send_hello();

    void update_local_player_data();
    void send_player_data();

    bool handle_welcome(const nier::Packet* packet);
    bool handle_create_player(const nier::Packet* packet);
    bool handle_destroy_player(const nier::Packet* packet);

    bool handle_create_entity(const nier::EntityPacket* packet);
    bool handle_destroy_entity(const nier::EntityPacket* packet);
    bool handle_entity_data(const nier::EntityPacket* packet);
    bool handle_entity_animation_start(const nier::EntityPacket* packet);

    bool handle_player_data(const nier::PlayerPacket* packet);
    bool handle_animation_start(const nier::PlayerPacket* packet);
    bool handle_buttons(const nier::PlayerPacket* packet);

    bool can_touch_entities() const { return m_in_game_thread && m_in_world; }
    Player* get_host_player();
    void try_coop_swap();
    void sync_puppets();
    bool bind_puppet(Player& player);
    void release_puppet(Player& player);

    std::unique_ptr<EntitySync> m_network_entities{};

    std::recursive_mutex m_mtx{};
    std::recursive_mutex m_players_mutex{};
    std::string m_hello_name{};
    std::string m_password{};

    bool m_welcome_received{ false };
    bool m_hello_sent{ false };

    bool m_is_master_client{false};
    uint64_t m_guid{};

    std::unordered_map<uint64_t, std::unique_ptr<Player>> m_players{};
    std::vector<std::unique_ptr<Player>> m_pending_destroy{};
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_next_bind_attempt{};

    std::atomic<bool> m_in_world{false};
    bool m_in_game_thread{false};
    bool m_need_entity_announce{false};
    bool m_has_local_character{false};

    // Client character swap (2B -> 9S) state, reset on every world load.
    bool m_swap_done{false};
    bool m_swap_pending{false};
    uint32_t m_swap_from_handle{0};
    uint32_t m_swap_wait_thinks{0};
};