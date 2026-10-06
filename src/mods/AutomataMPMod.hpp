#pragma once

#include <chrono>
#include <array>
#include <atomic>
#include <future>
#include <optional>

#include "../Mod.hpp"

#include "multiplayer/MidHooks.hpp"
#include "multiplayer/PlayerHook.hpp"

#include "multiplayer/NierClient.hpp"
#include "multiplayer/Player.hpp"
#include "multiplayer/EntitySync.hpp"
#include "multiplayer/CoopServer.hpp"

class AutomataMPMod : public Mod {
public:
    static std::shared_ptr<AutomataMPMod> get();
public:

    ~AutomataMPMod();

    std::string_view get_name() const override { return "AutomataMPMod"; }
    std::optional<std::string> on_initialize() override;

public:
    bool is_server() {
        return m_client != nullptr && m_client->is_master_client();
    }

    void on_entity_created(sdk::Entity* entity, sdk::EntitySpawnParams* data) {
        if (m_client != nullptr) {
            m_client->on_entity_created(entity, data);
        }
    }
    
    void on_entity_deleted(sdk::Entity* entity) {
        if (m_client != nullptr) {
            m_client->on_entity_deleted(entity);
        }
    }

    void on_draw_ui() override;
    void on_frame() override;
    void on_think() override;
    void shared_think();
    std::tuple<std::string, std::string> validate_connection(std::string ip, std::string port);
    void signal_destroy_client() {
        m_wants_destroy_client = true;
    }

    auto& get_client() const {
        return m_client;
    }

private:
    std::chrono::high_resolution_clock::time_point m_next_think;

    bool m_is_server{ false };
    bool m_wants_destroy_client{false};
    
    std::mutex m_hook_guard;

    MidHooks m_mid_hooks;
    PlayerHook m_player_hook;

    std::unique_ptr<NierClient> m_client;
    // Guards m_client between the game thread (on_think) and the render thread (on_frame, on_draw_ui).
    std::recursive_mutex m_client_mutex;
    // Time of the last on_think, on_think does not run in the main menu and on loading screens.
    std::atomic<std::chrono::steady_clock::time_point> m_last_think{};

private:
    void display_coop();
    void start_connect(const std::string& host, const std::string& port, bool is_host);
    void update_connection();
    void destroy_client();

    struct PendingConnect {
        std::string host;
        std::string port;
        std::chrono::steady_clock::time_point next_attempt;
        int attempts_left;
    };

    CoopServer m_coop_server;
    std::optional<PendingConnect> m_pending_connect{};
    std::string m_coop_status{};
    bool m_is_hosting{false};
    bool m_coop_use_story_buddy{true};
    bool m_coop_auto_swap{true};
    bool m_coop_spawn_partner{false};
    bool m_coop_sync_enemies{false};
    std::array<char, 256> m_join_address_input{};
    std::array<char, 16> m_host_port_input{};

private:
    void display_servers();
    void display_manual_connect();
    struct ServerData {
        std::string ip;
        std::string port;
        std::string name;
        uint32_t num_players;
    };

    std::vector<std::unique_ptr<ServerData>> m_servers;
    std::chrono::steady_clock::time_point m_last_server_update{};
    std::future<std::string> m_server_future;

    // imgui stuff
    std::array<char, 256> m_ip_connect_input{};
    std::array<char, 256> m_port_connect_input{};
    std::array<char, 256> m_password_input{};
    std::array<char, 256> m_name_input{};
    std::array<char, 256> m_master_server_input{};
};
