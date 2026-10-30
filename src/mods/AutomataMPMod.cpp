#include <sstream>
#include <mutex>
#include <regex>

#include <windows.h>

#include <spdlog/spdlog.h>

#include <utility/Input.hpp>
#include <utility/HttpClient.hpp>
#include <json.hpp>

#include <sdk/Entity.hpp>
#include <sdk/EntityList.hpp>
#include <sdk/Enums.hpp>

#include <sdk/Game.hpp>
#include <sdk/ScriptFunctions.hpp>
#include "AutomataMPMod.hpp"
#include "InputFocus.hpp"
#include "Version.hpp"
#include "multiplayer/BunkerRingHold.hpp"
#include "multiplayer/PhaseRequestGate.hpp"
#include "multiplayer/ScriptTracer.hpp"
#include "multiplayer/StoryActionSync.hpp"
#include "multiplayer/StoryTalkGate.hpp"
#include "multiplayer/StoryWatcher.hpp"

using namespace std;

// define default values
#define DEFAULT_MASTER "https://niermaster.praydog.com"
#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT "6969"
#define DEFAULT_NAME "Client"


std::shared_ptr<AutomataMPMod> AutomataMPMod::get() {
    static std::shared_ptr<AutomataMPMod> instance = std::make_shared<AutomataMPMod>();

    return instance;
}

AutomataMPMod::~AutomataMPMod() {
    if (m_client) {
        m_client->disconnect();
    }
}

std::optional<std::string> AutomataMPMod::on_initialize() try {
    spdlog::info("Entering AutomataMPMod {}", AUTOMATAMP_FULL_VERSION);

    std::strcpy(m_ip_connect_input.data(), DEFAULT_IP);
    std::strcpy(m_port_connect_input.data(), DEFAULT_PORT);
    std::strcpy(m_name_input.data(), DEFAULT_NAME);
    std::strcpy(m_master_server_input.data(), DEFAULT_MASTER);
    std::strcpy(m_join_address_input.data(), DEFAULT_IP ":" DEFAULT_PORT);
    std::strcpy(m_host_port_input.data(), DEFAULT_PORT);

    if (const auto user = std::getenv("USERNAME"); user != nullptr && *user != '\0') {
        strncpy_s(m_name_input.data(), m_name_input.size(), user, _TRUNCATE);
    }

    // Do it later.
    enetpp::global_state::get().initialize();
    StoryActionSync::get().install();
    PhaseRequestGate::get().install();
    StoryTalkGate::get().install();
    BunkerRingHold::get().install();

    spdlog::info("Leaving AutomataMPMod.");

    return Mod::on_initialize();
} catch(std::exception& e) {
    spdlog::error("{}", e.what());
    return e.what();
} catch (...) {
    spdlog::error("Unknown exception");
    return "Unknown exception";
}

void AutomataMPMod::display_servers() {
    const auto now = std::chrono::steady_clock::now();

    // Check if server future is ready and parse it into our internal server list.
    if (m_server_future.valid() && m_server_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_servers.clear();

        const auto response = m_server_future.get();
        spdlog::info("Got response: {}", response);

        if (response.empty()) {
            spdlog::info("Empty master server response.");
            strcpy(m_master_server_input.data(), DEFAULT_MASTER);
            return;
        }

        const auto response_json = nlohmann::json::parse(response);

        try {
            for (const auto& [ip, jsondata] : response_json.items()) {
                auto new_server_data = std::make_unique<AutomataMPMod::ServerData>();
                const auto data = jsondata["Data"];

                new_server_data->ip = ip;
                new_server_data->port = data["Port"];
                new_server_data->name = data["Name"];
                new_server_data->num_players = data["NumPlayers"];

                m_servers.push_back(std::move(new_server_data));
            }
        } catch (const std::exception& e) {
            spdlog::error("Error parsing server response: {}", e.what());
        } catch (...) {
            spdlog::error("Unknown Error parsing server response");
        }
    }

    // Render the actual server list.
    for (auto& server : m_servers) {
        ImGui::PushID(server->ip.c_str());

        if (ImGui::Button("Connect")) {
            // if any fields are empty, don't connect.
            if (server->ip.empty() || server->port.empty()) {
                spdlog::error("Empty IP or port.");
                return;
            }

            spdlog::info("Connecting to {}:{}", server->ip, server->port);

			strcpy(m_ip_connect_input.data(), server->ip.c_str());
            strcpy(m_port_connect_input.data(), server->port.c_str());

            start_connect(server->ip, server->port, false, false);

            ImGui::PopID();
            return;
        }

        ImGui::SameLine();

        const auto made = ImGui::TreeNode((server->name + " (" + std::to_string(server->num_players) + " players)").c_str());

        if (made) {
            ImGui::Text("IP: %s", server->ip.c_str());
            ImGui::Text("Port: %s", server->port.c_str());
            ImGui::Text("Name: %s", server->name.c_str());
            ImGui::Text("Players: %s", std::to_string(server->num_players).c_str());
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    if (now - m_last_server_update < std::chrono::seconds(5)) {
        return;
    }

    if (m_server_future.valid()) {
        if (m_server_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }

        m_server_future.wait();
    }

    m_server_future = std::async(std::launch::async, [this]() -> std::string {
        try {
            HttpClient http{};

            const auto servers_url = std::string{m_master_server_input.data()} + "/servers";
            http.get(servers_url, "", "");

            const auto response = http.response();
            m_last_server_update = std::chrono::steady_clock::now();

            return response;
        } catch (...) {
            m_last_server_update = std::chrono::steady_clock::now();
            return "";
        }
    });
}

void AutomataMPMod::display_manual_connect() {
    if (ImGui::Button("Connect") || ImGui::InputText("Connect IP", m_ip_connect_input.data(), m_ip_connect_input.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        
        // validate against master server
        const auto connection_info = validate_connection(m_ip_connect_input.data(), m_port_connect_input.data());
        const auto val_ip = std::get<0>(connection_info);
        const auto val_port = std::get<1>(connection_info);

        if (val_ip != m_ip_connect_input.data() || val_port != m_port_connect_input.data()) {
            spdlog::error("Invalid IP or port.");
            strcpy(m_ip_connect_input.data(), val_ip.c_str());
            strcpy(m_port_connect_input.data(), val_port.c_str());
            return;
        }

        start_connect(val_ip, val_port, false, false);
    }

    ImGui::InputText("Connect Port", m_port_connect_input.data(), m_port_connect_input.size());
    ImGui::InputText("Name", m_name_input.data(), m_name_input.size());
    ImGui::InputText("Password", m_password_input.data(), m_password_input.size());
}

std::optional<std::string> AutomataMPMod::host_game(const std::string& port) {
    if (const auto error = m_coop_server.start(port, m_password_input.data()); error) {
        m_coop_status = *error;
        return error;
    }

    m_is_hosting = true;
    start_connect(DEFAULT_IP, port, true, true);
    return std::nullopt;
}

std::optional<std::string> AutomataMPMod::join_game(const std::string& address_and_port, bool as_host) {
    std::string address = address_and_port;
    std::string port = DEFAULT_PORT;

    if (const auto colon = address.rfind(':'); colon != std::string::npos) {
        port = address.substr(colon + 1);
        address = address.substr(0, colon);
    }

    if (address.empty() || port.empty() || !std::all_of(port.begin(), port.end(), ::isdigit)) {
        m_coop_status = "Enter the address as IP:port, for example 192.168.1.10:6969";
        return m_coop_status;
    }

    start_connect(address, port, false, as_host);
    return std::nullopt;
}

// One line for the test bot: connection, role and what we know about the other player.
std::string AutomataMPMod::describe_coop() {
    std::scoped_lock _{m_client_mutex};
    std::ostringstream s{};
    s << "coop status=\"" << m_coop_status << "\"";

    if (m_client == nullptr) {
        s << " client=none";
        return s.str();
    }

    const auto ours = StoryWatcher::get().current();
    s << " story=" << ours.phase << " route=" << (ours.route != 0 ? std::string{ours.route} : "none");

    for (const auto& [name, story] : m_client->get_other_stories()) {
        s << " other_story=" << (story.known ? story.phase : "unknown");
        s << " other_route=" << (story.known && story.route != 0 ? std::string{story.route} : "none");
    }

    s << " connected=" << m_client->is_connected() << " session=" << m_client->is_in_session()
      << " in_world=" << m_client->is_in_world() << " master=" << m_client->is_master_client()
      << " guid=" << m_client->get_guid();

    const auto mismatch = m_client->story_mismatch();
    s << " mismatch=" << (mismatch == NierClient::StoryMismatch::routes ? "routes" : mismatch == NierClient::StoryMismatch::phases ? "phases" : "none");

    for (const auto& [guid, player] : m_client->get_players()) {
        if (player == nullptr || guid == m_client->get_guid()) {
            continue;
        }

        s << " other=" << guid << ":" << player->get_name() << " model=" << std::hex << player->get_model() << std::dec
          << " handle=" << std::hex << player->get_handle() << std::dec << " data=" << player->has_player_data();

        if (player->has_player_data()) {
            const auto& p = player->get_player_data().position();
            s << " pos=" << p.x() << "," << p.y() << "," << p.z();
        }
    }

    return s.str();
}

void AutomataMPMod::start_connect(const std::string& host, const std::string& port, bool is_host, bool wants_host) {
    std::scoped_lock _{m_client_mutex};

    if (m_client != nullptr) {
        m_wants_destroy_client = true;
    }

    // The freshly started server needs a moment before it listens.
    m_pending_connect = PendingConnect{
        host,
        port,
        std::chrono::steady_clock::now() + (is_host ? std::chrono::milliseconds(500) : std::chrono::milliseconds(0)),
        is_host ? 5 : 1,
        wants_host,
    };

    m_coop_status = "Connecting to " + host + ":" + port + "...";
}

void AutomataMPMod::destroy_client() {
    std::scoped_lock _{m_client_mutex};

    m_pending_connect.reset();
    m_wants_destroy_client = true;

    if (m_is_hosting) {
        m_coop_server.stop();
        m_is_hosting = false;
    }

    m_coop_status = "Disconnected";
}

// Render thread. Creates the client, watches its state and keeps the network
// going while on_think doesn't run (main menu, loading screens).
void AutomataMPMod::update_connection() {
    std::scoped_lock _{m_client_mutex};

    const auto now = std::chrono::steady_clock::now();
    const auto in_world = now - m_last_think.load() < std::chrono::milliseconds(300);

    // on_think takes care of destroying the client while the world is loaded,
    // so story buddies get their AI back.
    if (m_wants_destroy_client && (!in_world || m_client == nullptr)) {
        m_client.reset();
        m_wants_destroy_client = false;
    }

    if (m_client == nullptr && !m_wants_destroy_client && m_pending_connect && m_pending_connect->attempts_left > 0 &&
        now >= m_pending_connect->next_attempt) {
        spdlog::info("[Coop] Connecting to {}:{}", m_pending_connect->host, m_pending_connect->port);

        try {
            m_client = make_unique<NierClient>(m_pending_connect->host, m_pending_connect->port, m_name_input.data(), m_password_input.data(),
                m_pending_connect->wants_host);
        } catch (const std::exception& e) {
            m_coop_status = std::string{"Invalid address: "} + e.what();
            m_pending_connect.reset();
            return;
        }

        --m_pending_connect->attempts_left;
    }

    if (m_client == nullptr || m_wants_destroy_client) {
        return;
    }

    m_client->use_story_buddy = m_coop_use_story_buddy;
    m_client->auto_swap_character = m_coop_auto_swap;
    m_client->spawn_partner = m_coop_spawn_partner;
    m_client->sync_enemies = m_coop_sync_enemies;
    m_client->puppet_as_player = m_coop_puppet_as_player;
    m_client->smooth_puppet_position = m_coop_smooth_position;
    m_client->share_enemy_deaths = m_coop_share_enemy_deaths;
    m_client->share_story_actions = m_coop_share_story;

    if (!in_world) {
        m_client->pump();
    }

    if (m_client->is_connecting()) {
        return;
    }

    if (m_client->is_connected()) {
        if (m_pending_connect) {
            m_pending_connect.reset();
        }

        if (!m_client->is_in_session()) {
            if (m_client->is_hello_unanswered(std::chrono::seconds(10))) {
                m_coop_status = "Connected, but the server does not answer. It may be an older version of the server";

                if (m_is_hosting) {
                    m_coop_status += ": replace automatamp_server\\server.exe in the game folder with the one from this mod version";
                }
            } else {
                m_coop_status = "Connected. Waiting for your character to be in the world...";
            }
        } else {
            if (m_client->is_master_client()) {
                m_coop_status = "In session. You lead the story";
            } else if (const auto leader = m_client->get_leader_name(); !leader.empty()) {
                m_coop_status = "In session. " + leader + " leads the story";
            } else {
                m_coop_status = "In session. Nobody leads the story: waiting for the host to join";
            }

            m_coop_status += " (session " + std::to_string(m_client->get_session_id()) + ")";

            if (!m_client->has_local_character()) {
                m_coop_status += ". Sync paused: you are not controlling 2B/9S/A2 right now";
            }
        }

        return;
    }

    // Failed to connect or the connection was lost.
    const auto was_in_session = m_client->is_in_session();
    m_wants_destroy_client = true;

    // The server told us why, retrying would not help.
    if (const auto& reason = m_client->get_reject_reason(); !reason.empty()) {
        m_coop_status = "Could not join: " + reason;

        if (m_is_hosting && m_client->is_version_rejected()) {
            m_coop_status += ". Your own server is outdated: replace automatamp_server\\server.exe in the game folder "
                             "with the one from this mod version";
        }
        m_pending_connect.reset();
    } else if (was_in_session) {
        m_coop_status = "Connection lost";
        m_pending_connect.reset();
    } else if (m_pending_connect && m_pending_connect->attempts_left > 0) {
        m_pending_connect->next_attempt = now + std::chrono::milliseconds(700);
    } else {
        m_coop_status = "Could not connect";

        if (m_pending_connect) {
            m_coop_status += " to " + m_pending_connect->host + ":" + m_pending_connect->port;
        }

        if (m_is_hosting && !m_coop_server.is_running()) {
            m_coop_status += ". The server stopped, see automatamp_server\\server_log.txt";
        }

        m_pending_connect.reset();
    }
}

void AutomataMPMod::display_coop() {
    // The render thread never waits for the game thread, see on_frame.
    std::unique_lock lock{m_client_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        ImGui::TextWrapped("Status: %s", m_coop_status.empty() ? "Disconnected" : m_coop_status.c_str());
        return;
    }

    ImGui::TextWrapped("Status: %s", m_coop_status.empty() ? "Disconnected" : m_coop_status.c_str());

    // Story point of both games (TASKS 2.6.3): a different phase means the games went apart.
    const auto ours = StoryWatcher::get().current();
    ImGui::TextWrapped("Story: we are in %s, route %s (%s)", ours.phase.empty() ? "(unknown)" : ours.phase.c_str(),
        ours.route != 0 ? std::string{ours.route}.c_str() : "?", ours.character.empty() ? "?" : ours.character.c_str());

    if (m_client != nullptr) {
        for (const auto& [name, story] : m_client->get_other_stories()) {
            if (!story.known) {
                ImGui::TextWrapped("Story: %s (no data yet)", name.c_str());
            } else if (story.route != 0 && ours.route != 0 && story.route != ours.route) {
                ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.2f, 1.0f}, "Story: %s is on route %c, we are on route %c (different routes)", name.c_str(), story.route, ours.route);
            } else if (story.event != 0) {
                ImGui::TextWrapped("Story: %s is watching cutscene ev%04x", name.c_str(), story.event);
            } else if (story.phase != ours.phase) {
                ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.2f, 1.0f}, "Story: %s is in %s (different phase)", name.c_str(), story.phase.c_str());
            } else {
                ImGui::TextWrapped("Story: %s is in the same phase", name.c_str());
            }
        }

        // TASKS 3.1.2: no pulling together yet (3.3 postponed), only the message.
        if (const auto mismatch = m_client->story_mismatch(); mismatch != NierClient::StoryMismatch::none) {
            ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.2f, 1.0f}, "You are on different story lines.");
            ImGui::TextWrapped(mismatch == NierClient::StoryMismatch::routes
                ? "Your games are on different routes: the partner's character is not taken over. Both load the same save to play together."
                : "Your games are at different points of the story. Both load the same save (the host's) to play together.");
        }
    }

    const auto busy = m_client != nullptr || m_pending_connect.has_value();

    if (busy) {
        if (ImGui::Button(m_is_hosting ? "Stop hosting" : "Leave")) {
            destroy_client();
            return;
        }
    }

    if (m_is_hosting) {
        ImGui::Separator();
        ImGui::TextWrapped("Give one of these addresses to your partner (LAN or VPN like Radmin/ZeroTier/Tailscale; "
                           "over the internet use your public IP with UDP port %s forwarded):", m_host_port_input.data());

        for (const auto& address : CoopServer::get_local_addresses()) {
            const auto text = address.ip + ":" + m_host_port_input.data();

            ImGui::PushID(text.c_str());

            if (ImGui::Button("Copy")) {
                ImGui::SetClipboardText(text.c_str());
            }

            ImGui::SameLine();
            ImGui::Text("%s  (%s)", text.c_str(), address.adapter.c_str());
            ImGui::PopID();
        }
    }

    if (m_client != nullptr && m_client->is_in_session()) {
        ImGui::Separator();
        m_client->on_draw_ui();
    }

    if (busy) {
        return;
    }

    ImGui::InputText("Name", m_name_input.data(), m_name_input.size());
    ImGui::InputText("Password", m_password_input.data(), m_password_input.size(), ImGuiInputTextFlags_Password);

    ImGui::Separator();
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("Port", m_host_port_input.data(), m_host_port_input.size(), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();

    if (ImGui::Button("Host Game")) {
        host_game(m_host_port_input.data());
    }

    ImGui::Separator();
    ImGui::InputText("Host address", m_join_address_input.data(), m_join_address_input.size());

    if (ImGui::Button("Join Game")) {
        join_game(m_join_address_input.data(), m_join_as_host);
    }

    ImGui::SameLine();
    ImGui::Checkbox("I am the host (I lead the story)", &m_join_as_host);

    ImGui::Separator();
    ImGui::Checkbox("Partner controls my story buddy", &m_coop_use_story_buddy);
    ImGui::Checkbox("Take over the buddy if I play the host's character", &m_coop_auto_swap);
    ImGui::Checkbox("Spawn a partner when there is no story buddy", &m_coop_spawn_partner);
    ImGui::Checkbox("Shared enemy deaths: an enemy killed in one game dies in the other", &m_coop_share_enemy_deaths);
    ImGui::Checkbox("Shared story progress: the client's game follows the host's story steps", &m_coop_share_story);
    ImGui::Checkbox("Shared enemies from the host (both players must enable it)", &m_coop_sync_enemies);
    ImGui::Checkbox("Partner animations: drive the partner like a player", &m_coop_puppet_as_player);
    ImGui::Checkbox("Smooth partner movement (off: snap to every network position)", &m_coop_smooth_position);
    ImGui::TextWrapped("You can connect from the main menu, your character joins once you are in the world. "
                       "On a dedicated server both press Join Game, the player leading the story ticks "
                       "\"I am the host\", the order does not matter. Both players should load the same save.");
    ImGui::TextDisabled("Mod version %s", AUTOMATAMP_FULL_VERSION);
}

void AutomataMPMod::on_draw_ui() {
    if (ImGui::CollapsingHeader("Co-op", ImGuiTreeNodeFlags_DefaultOpen)) {
        display_coop();
    }

    if (!ImGui::CollapsingHeader("AutomataMP (advanced)")) {
        return;
    }

    std::unique_lock lock{m_client_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        return;
    }

    ImGui::Checkbox("Debug hotkeys (F3 suspend, F4/F9/F10 partner, F6 swap, F7 buddy)", &m_debug_hotkeys);

    if (ImGui::Checkbox("Trace story script calls to the log (debug, the first enable takes a few seconds)", &m_trace_scripts)) {
        ScriptTracer::get().set_enabled(m_trace_scripts);
    }

    if (m_client) {
        if (ImGui::Button("Disconnect")) {
            destroy_client();
            return;
        }
        ImGui::Text("State: Client");
    } else {
        ImGui::Text("State: Disconnected");
    }

	ImGui::InputText("Master Server", m_master_server_input.data(), m_master_server_input.size());
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Servers")) {
        display_servers();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Manual Connection")) {
        display_manual_connect();
        ImGui::TreePop();
    }
}

void AutomataMPMod::on_frame() {
    // Render thread: never block on the game thread. While on_think holds the lock it may be
    // waiting inside a game function, blocking here as well can freeze the whole game.
    std::unique_lock lock{m_client_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        return;
    }

    update_connection();

    std::string state = "Disconnected";
    auto mismatch = NierClient::StoryMismatch::none;

    if (m_client && m_client->is_in_session()) {
        state = m_client->is_master_client() ? "Co-op: Host" : "Co-op: Client";
        mismatch = m_client->story_mismatch();
    } else if (m_client || m_pending_connect) {
        state = "Co-op: Connecting";
    }

    ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0, 0), ImGui::GetColorU32(ImGuiCol_Text), state.c_str());

    // TASKS 3.1.2: also on screen, the Co-op menu is usually closed.
    if (mismatch != NierClient::StoryMismatch::none) {
        const auto text = mismatch == NierClient::StoryMismatch::routes
            ? "You are on different story lines (different routes): the partner is not taken over"
            : "You are on different story lines: load the same save to play together";
        ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0, ImGui::GetFontSize() + 2.0f), 0xFF3366FF, text);
    }

    // TASKS 3.2.3: a story talk the client tried.
    if (StoryTalkGate::get().show_hint()) {
        ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0, (ImGui::GetFontSize() + 2.0f) * 2.0f), 0xFF33CCFF,
            "The host does this: story talks and quests are the host's in co-op");
    }

    if (m_client && !m_wants_destroy_client) {
        m_client->on_frame();
    }
}

bool AutomataMPMod::set_coop_option(std::string_view name, bool value) {
    if (name == "share_story") {
        m_coop_share_story = value;
    } else if (name == "share_enemy_deaths") {
        m_coop_share_enemy_deaths = value;
    } else if (name == "takeover") {
        NierClient::research_takeover = value;
    } else if (name == "swap") {
        NierClient::research_swap = value;
    } else if (name == "input_focus") {
        InputFocus::enabled = value;
    } else if (name == "flight_follow") {
        NierClient::follow_flight_units = value;
    } else if (name == "trace_scripts") {
        m_trace_scripts = value;
        ScriptTracer::get().set_enabled(value);
    } else {
        return false;
    }

    spdlog::info("[Coop] Option {} = {}", name, value);
    return true;
}

void AutomataMPMod::on_think() {
    std::scoped_lock _{m_client_mutex};

    m_last_think = std::chrono::steady_clock::now();
    StoryWatcher::get().update();
    StoryActionSync::get().update();

    if (m_client == nullptr) {
        PhaseRequestGate::get().set_leader(false, {}, 0);
        StoryTalkGate::get().set_follow(false);
    }

    PhaseRequestGate::get().update();

    if (m_wants_destroy_client) {
        if (m_client != nullptr) {
            m_client->release_puppets();
            m_client.reset();
        }

        m_wants_destroy_client = false;
        return;
    }

    // The connection stays alive through loading screens, only the entity handles are dropped.
    if (sdk::is_loading()) {
        if (m_client != nullptr) {
            m_client->on_world_unloaded();
        }

        return;
    }

    auto entity_list = sdk::EntityList::get();

    if (!entity_list) {
        return;
    }

    auto player = entity_list->get_by_name("Player");

    if (!player) {
        log_world_state("Player not found");
        return;
    }

    if (m_debug_hotkeys) {
        debug_hotkeys(entity_list, player);
    }

    shared_think();
}

// Developer keys, off by default: they suspend the player, swap characters and spawn entities.
void AutomataMPMod::debug_hotkeys(sdk::EntityList* entity_list, sdk::Entity* player) {
    auto partners = entity_list->get_all_by_name("partner");

    if (utility::was_key_down(VK_F4)) {
        for (auto i : partners) {
            if (i->behavior != nullptr) {
                i->behavior->position() = player->behavior->position();
            }
        }
    }

    if (utility::was_key_down(VK_F6)) {
        player->behavior->as<sdk::Pl0000>()->changePlayer();
    }

    if (utility::was_key_down(VK_F7)) {
        for (auto& i : *entity_list) {
            if (!i.ent || !i.handle || !i.ent->behavior) {
                continue;
            }

            if (i.ent->behavior->as<sdk::BehaviorAppBase>()->health() == 0) {
                continue;
            }

            auto pl0000 = i.ent->behavior->as<sdk::Pl0000>();

            pl0000->obj_flags() = -1;
            pl0000->setBuddyFromNpc();
            pl0000->obj_flags() = 8;
            pl0000->setBuddyFromNpc();
            pl0000->obj_flags() = 1;
        }
    }

    if (utility::was_key_down(VK_F9)) {
        auto ent = entity_list->spawn_entity("partner", sdk::EModel::MODEL_2B, player->behavior->position());

        if (ent) {
            ent->assign_ai_routine("buddy_2B");
            ent->assign_ai_routine("buddy");

            ent->behavior->as<sdk::Pl0000>()->buddy_handle() = player->handle;
            player->behavior->as<sdk::Pl0000>()->buddy_handle() = ent->handle;

            ent->behavior->setSuspend(false);

            ent->behavior->obj_flags() = -1;
            ent->behavior->as<sdk::Pl0000>()->setBuddyFromNpc();
            ent->behavior->obj_flags() = 1;
        }

        spdlog::info("[Debug] Spawned partner {:x}", (uintptr_t)ent);
    }

    if (utility::was_key_down(VK_F10)) {
        for (auto p : partners) {
            p->behavior->terminate();
        }
    }

    if (utility::was_key_down(VK_F3)) {
        player->behavior->setSuspend(!player->behavior->isSuspend());
    }
}

// Logs only when the reason changes, the checks run every frame.
void AutomataMPMod::log_world_state(const char* state) {
    if (m_world_state == state) {
        return;
    }

    m_world_state = state;

    if (state != nullptr) {
        spdlog::info("[Coop] {}", state);
    }
}

void AutomataMPMod::shared_think() {
    //spdlog::info("Shared think");

    //static uint32_t(*changePlayer)(Entity* player) = (decltype(changePlayer))0x1401ED500;

    auto entity_list = sdk::EntityList::get();

    if (!entity_list) {
        return;
    }

    // main player entity that game is originally controlling
    auto player = entity_list->get_by_name("Player");

    if (!player) {
        log_world_state("Player not found");
        return;
    }

    auto controlled_entity = entity_list->get_possessed_entity();

    if (!controlled_entity || !controlled_entity->behavior) {
        log_world_state("Controlled entity invalid");

        if (m_client) {
            m_client->recover_lost_character();
        }

        return;
    }

    log_world_state(nullptr);

    /*if (m_client && controlledEntity->name != string("partner")) {
        auto realBuddy = entityList->getByHandle(controlledEntity->entity->getBuddyHandle());

        if (realBuddy && realBuddy->entity) {
            //realBuddy->entity->setBuddyFlags(0);
            realBuddy->entity->setSuspend(false);
            //changePlayer(player->entity);
            player->entity->changePlayer();
        }

        return;
    }*/

    if (controlled_entity->behavior->is_pl0000()) {
        m_mid_hooks.add_overriden_entity(controlled_entity->behavior);
        m_player_hook.re_hook(controlled_entity->behavior->as<sdk::Pl0000>());
        controlled_entity->behavior->obj_flags() = 0;

        auto real_buddy = entity_list->get_by_handle(controlled_entity->behavior->as<sdk::Pl0000>()->buddy_handle());

        if (ScriptTracer::get().is_enabled()) {
            const auto buddy = real_buddy != nullptr ? real_buddy->behavior : nullptr;
            ScriptTracer::get().set_known_objects(controlled_entity->behavior, controlled_entity->behavior->model_index(),
                buddy, buddy != nullptr ? buddy->model_index() : 0);
        }
    }

    if (m_client) {
        m_client->think();
    }
}

std::tuple<std::string, std::string> AutomataMPMod::validate_connection(std::string ip, std::string port) {
    std::string valid_ip = DEFAULT_IP;
    std::string valid_port = DEFAULT_PORT;
    bool is_port_valid = false;

    if (!std::regex_match(ip, std::regex("^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$"))) {
        return { valid_ip, valid_port };
    }

    for (auto& server : m_servers) {
        valid_port = server->port;
        if (server->ip == ip && valid_port == port) {
            is_port_valid = true;
        }
    }

    return { ip, is_port_valid ? port : valid_port };
}
