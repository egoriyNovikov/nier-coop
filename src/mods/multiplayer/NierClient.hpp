#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <enetpp/client.h>

#include "Player.hpp"
#include "StoryWatcher.hpp"
#include "EnemyDeathSync.hpp"
#include "EntitySync.hpp"
#include "schema/Packets_generated.h"

struct Packet;

class NierClient : public enetpp::client {
public:
    NierClient(
        const std::string& host,
        const std::string& port = "6969",
        const std::string& name = "Client",
        const std::string& password = "",
        bool wants_host = false);
    virtual ~NierClient();

    // Game thread only. Handles packets and drives the puppets.
    void think();
    // Safe from any thread. Only drains network events, never touches game entities.
    // Used while the world is not available (main menu, loading screens).
    void pump();
    // Forget every entity handle we hold. Called when the game starts loading.
    void on_world_unloaded();
    // Game thread only. Called when the game has no valid player character while in the world.
    // Returns true if we switched back to the character we had before taking over the story buddy.
    bool recover_lost_character();
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
    // Why the server refused us, empty if it didn't.
    const std::string& get_reject_reason() const { return m_reject_reason; }
    bool is_version_rejected() const { return m_version_rejected; }
    // Hello went out long ago and the server neither welcomed nor rejected us.
    bool is_hello_unanswered(std::chrono::steady_clock::duration timeout) const {
        return m_hello_sent && !m_welcome_received && m_reject_reason.empty() && std::chrono::steady_clock::now() - m_hello_sent_at > timeout;
    }
    uint32_t get_session_id() const { return m_session_id; }
    // Name of the remote player leading the story, empty if we lead or nobody does.
    std::string get_leader_name();
    // Nobody leads the story: the host left and we wait for it to come back.
    bool is_waiting_for_host();

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
    // Game thread only. A traced story script function is about to run on this Pl0000 (see MidHooks).
    void on_story_script(const char* name, sdk::Behavior* self);
    // Any thread. The game spawned a Flight Unit (see MidHooks::on_entity_spawn). Only logged.
    void on_flight_unit_spawned(sdk::Entity* entity);
    // Any thread (the game may update characters on worker threads). Called at the end of the game's
    // button processing for a character (MidHooks::on_processed_buttons). If the character is the puppet
    // of a remote player, writes that player's input over what the game computed and returns true.
    bool apply_puppet_input(sdk::Pl0000* entity);
    // Any thread. Called after the game's stick update (MidHooks::on_processed_stick), which recomputes
    // speed and facing after the buttons: writes the remote player's movement over them.
    bool apply_puppet_movement(sdk::Pl0000* entity);
    
    const auto get_guid() const {
        return m_guid;
    }

    const auto is_master_client() const {
        return m_is_master_client;
    }

    // Name and story point of every other player, safe from any thread.
    std::vector<std::pair<std::string, Player::Story>> get_other_stories() {
        std::scoped_lock _{m_players_mutex};
        std::vector<std::pair<std::string, Player::Story>> result{};

        for (const auto& [guid, player] : m_players) {
            if (player != nullptr && guid != m_guid) {
                result.emplace_back(player->get_name(), player->get_story());
            }
        }

        return result;
    }

    const auto& get_players() const {
        return m_players;
    }

    // Co-op options.
    bool use_story_buddy{true}; // Puppet the game's own buddy (e.g. 9S) instead of spawning a new partner.
    bool auto_swap_character{true}; // Client takes over the buddy if it plays the same character as the host.
    bool spawn_partner{false}; // Spawn a "partner" for the other player when the game gives us no story buddy.
    bool sync_enemies{false}; // Enemies come from the master client. Otherwise everyone keeps the enemies of their own story.
    bool puppet_as_player{true}; // Run the game's player input update on puppets, needed for run/walk animations.
    bool share_enemy_deaths{true}; // An enemy that dies in one game dies in the other (each game keeps its own enemies).
    bool smooth_puppet_position{true}; // Pull the puppet towards the network position every frame instead of snapping on each packet.

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
    bool handle_set_master_client(const nier::Packet* packet);
    bool handle_reject(const nier::Packet* packet);

    bool handle_create_entity(const nier::EntityPacket* packet);
    bool handle_destroy_entity(const nier::EntityPacket* packet);
    bool handle_entity_data(const nier::EntityPacket* packet);
    bool handle_entity_animation_start(const nier::EntityPacket* packet);

    bool handle_player_data(const nier::PlayerPacket* packet);
    bool handle_animation_start(const nier::PlayerPacket* packet);
    bool handle_buttons(const nier::PlayerPacket* packet);
    bool handle_enemy_died(const nier::PlayerPacket* packet);
    bool handle_story_state(const nier::PlayerPacket* packet);

    void send_enemy_died(const EnemyDeathSync::Death& death);
    void send_story_state();
    // Sends what was added to our log file since the last call, when the server keeps a copy of it.
    void send_log_copy();
    // Game thread. Tracks our enemies, sends their deaths, applies the deaths from the other game.
    void sync_enemy_deaths(bool connected);

    bool can_touch_entities() const { return m_in_game_thread && m_in_world; }
    Player* get_host_player();
    void try_coop_swap();
    void hand_back_deleted_character(sdk::Entity* entity);
    bool hand_back_character(sdk::Entity* played, const char* reason);
    void log_flight_units(sdk::EntityList* entity_list);
    void sync_puppets();
    bool bind_puppet(Player& player);
    void release_puppet(Player& player);
    // Game thread only. Rebuilds m_puppet_inputs from the bound puppets and their latest player data.
    void update_puppet_inputs();
    // Game thread only. Moves each puppet a bit towards the last network position of its player.
    void smooth_puppet_positions();
    // Game thread only. Stops the puppets whose player sent no data for a while (frozen game, lost connection).
    void stop_stale_puppets();
    // Game thread only. Logs jumps of our own character and puts it back if its position became NaN.
    void watch_local_character();
    // The character we control right now, never driven as a puppet.
    bool is_local_character(const sdk::Pl0000* npc) const;
    void log_animation_stats();

    std::unique_ptr<EntitySync> m_network_entities{};

    EnemyDeathSync m_enemy_deaths{};
    // Deaths from the other game, applied in think() on the game thread.
    StoryWatcher::Point m_sent_story{};
    std::chrono::steady_clock::time_point m_next_story_send{};

    std::mutex m_remote_deaths_mutex{};
    std::vector<EnemyDeathSync::Death> m_remote_deaths{};
    std::chrono::steady_clock::time_point m_next_enemy_stats_log{};

    // Input of the remote players for MidHooks::on_processed_buttons. The game recomputes the buttons,
    // speed and facing of every character in its button processing, so values written when a packet
    // arrives are lost before the character reads them: they are written from that hook instead.
    using ButtonFrame = std::array<uint32_t, 8>;
    struct PuppetInput {
        uint64_t guid{};
        nier::PlayerData data{};
        // Button packets not applied yet, one per frame of the remote player that had a button down.
        std::deque<ButtonFrame> buttons{};
        // The hook reached this puppet at least once: buttons go to the queue, not straight to the controller.
        bool driven_by_hook{false};
        bool movement_logged{false};

        // State of the puppet before it was made player controlled (puppet_as_player), restored on release.
        uint32_t handle{0};
        bool made_player{false};
        uint32_t saved_player_controlled{0};
        uint32_t saved_control_flags{0};
        std::array<uint32_t, 3> saved_input_sources{};
    };
    void make_puppet_player_controlled(sdk::Pl0000* entity, PuppetInput& input);
    void restore_puppet(sdk::Pl0000* entity, PuppetInput& input);
    std::mutex m_puppet_input_mutex{};
    std::unordered_map<sdk::Pl0000*, PuppetInput> m_puppet_inputs{};

    // Animation sync counters, logged every few seconds while something happens ([Anim] lines).
    std::atomic<uint32_t> m_stat_anims_sent{0};
    std::atomic<uint32_t> m_stat_anims_received{0};
    std::atomic<uint32_t> m_stat_anims_applied{0};
    std::atomic<uint32_t> m_stat_buttons_sent{0};
    std::atomic<uint32_t> m_stat_buttons_received{0};
    std::atomic<uint32_t> m_stat_button_frames_applied{0};
    std::atomic<uint32_t> m_stat_button_frames_dropped{0};
    std::atomic<uint32_t> m_stat_hook_frames{0};
    std::chrono::steady_clock::time_point m_next_stats_log{};

    // Position smoothing (smooth_puppet_positions), logged with the [Anim] line.
    std::chrono::steady_clock::time_point m_last_smooth_time{};
    float m_stat_position_error_sum{0.0f};
    float m_stat_position_error_max{0.0f};
    uint32_t m_stat_position_samples{0};
    uint32_t m_stat_position_snaps{0};
    uint32_t m_stat_bad_player_data{0};

    // watch_local_character: our character in the previous think.
    uint32_t m_watched_handle{0};
    Vector3f m_watched_position{};
    std::chrono::steady_clock::time_point m_next_jump_log{};
    uint32_t m_jumps_not_logged{0};
    std::chrono::steady_clock::time_point m_next_nan_log{};

    std::recursive_mutex m_mtx{};
    std::recursive_mutex m_players_mutex{};
    std::string m_hello_name{};
    std::string m_password{};
    bool m_wants_host{false};
    std::string m_reject_reason{};
    bool m_version_rejected{false};
    uint32_t m_session_id{0};

    // The server's copy of our log (Welcome.acceptsLogs): our log file read from the start of the game.
    bool m_server_accepts_logs{false};
    FILE* m_log_copy_file{nullptr};
    std::chrono::steady_clock::time_point m_next_log_copy{};

    bool m_welcome_received{ false };
    bool m_hello_sent{ false };
    std::chrono::steady_clock::time_point m_hello_sent_at{};

    bool m_is_master_client{false};
    uint64_t m_guid{};

    std::unordered_map<uint64_t, std::unique_ptr<Player>> m_players{};
    std::vector<std::unique_ptr<Player>> m_pending_destroy{};
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_next_bind_attempt{};

    std::atomic<bool> m_in_world{false};
    bool m_in_game_thread{false};
    bool m_need_entity_announce{false};
    bool m_has_local_character{false};

    // Client character swap (2B -> 9S) state, decided again for every character the game gives us.
    bool m_swap_done{false};
    bool m_swap_wait_logged{false};
    uint32_t m_flight_unit_skip_logged{0}; // Flight Unit of the buddy we last logged as "not taken"
    uint32_t m_swap_checked_handle{0};
    bool m_swap_pending{false};
    uint32_t m_swap_from_handle{0};
    uint32_t m_swap_wait_thinks{0};
    // The buddy we handed back because the game deleted it: not taken over again right away (test 0.2.3,
    // the takeover ran 4 ms after the hand back, while the game was still deleting it).
    uint32_t m_handed_back_handle{0};
    std::chrono::steady_clock::time_point m_no_swap_until{};
    // Last model we controlled, only to log story switches (Flight Unit, hacking...).
    uint32_t m_last_possessed_model{0};
    // Flight Units (pl0010) owned by the player and the buddy, only to log changes.
    uint32_t m_logged_player_flight_unit{0};
    uint32_t m_logged_buddy_flight_unit{0};
    // After a Flight Unit swap on hand back: our character and the Flight Unit it had before, given back
    // if the one it got disappears (it may be deleted together with 9S).
    uint32_t m_flight_unit_owner{0};
    uint32_t m_previous_flight_unit{0};
};