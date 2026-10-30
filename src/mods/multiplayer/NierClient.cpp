#include <thread>
#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <share.h>

#include <sdk/CameraGame.hpp>
#include <sdk/Enums.hpp>
#include <sdk/Game.hpp>
#include <sdk/Story.hpp>

#include "AutomataMP.hpp"
#include "Version.hpp"

#include "schema/Packets_generated.h"
#include "mods/AutomataMPMod.hpp"
#include "PhaseRequestGate.hpp"
#include "StoryActionSync.hpp"
#include "StoryTalkGate.hpp"
#include "StoryWatcher.hpp"
#include "BunkerRingHold.hpp"
#include "NierClient.hpp"

using namespace std;

namespace {
// Runs a callback when leaving the scope.
template <typename T>
struct ScopeExit {
    T fn;
    ~ScopeExit() { fn(); }
};

template <typename T>
ScopeExit(T) -> ScopeExit<T>;

// The character the client takes when it plays the same one as the host.
uint32_t get_counterpart_model(uint32_t model) {
    switch (model) {
    case sdk::EModel::MODEL_2B:
        return sdk::EModel::MODEL_9S;
    case sdk::EModel::MODEL_9S:
        return sdk::EModel::MODEL_2B;
    default:
        return model;
    }
}

bool is_finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool is_character_model(uint32_t model) {
    return model == sdk::EModel::MODEL_2B || model == sdk::EModel::MODEL_9S || model == sdk::EModel::MODEL_A2;
}

// The android the local player controls right now. Early in the story the player often controls
// something else (Flight Unit, hacking, scripted sequences), those can't be synchronized.
sdk::Entity* get_local_character(sdk::EntityList* entity_list) {
    auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (possessed == nullptr || possessed->behavior == nullptr || !possessed->behavior->is_pl0000()) {
        return nullptr;
    }

    if (!is_character_model(possessed->behavior->model_index())) {
        return nullptr;
    }

    return possessed;
}

const char* get_buddy_routine(uint32_t model) {
    switch (model) {
    case sdk::EModel::MODEL_2B:
        return "buddy_2B";
    case sdk::EModel::MODEL_9S:
        return "buddy_9S";
    case sdk::EModel::MODEL_A2:
        return "buddy_A2";
    default:
        return nullptr;
    }
}
}

NierClient::NierClient(const std::string& host, const std::string& port, const std::string& name, const std::string& password, bool wants_host)
    : m_hello_name{ name },
    m_password{ password },
    m_wants_host{ wants_host }
{
    std::scoped_lock _{m_mtx};

    enetpp::global_state::get().deinitialize();
    enetpp::global_state::get().initialize();

    set_trace_handler([](const std::string& s) { spdlog::debug("{}", s); });
    
    // Connecting is asynchronous: enetpp runs the connection on its own thread,
    // callers poll is_connecting()/is_connected().
    // The timeout is also the disconnect timeout, so keep it long enough for internet play.
    enet_uint16 port_num = static_cast<enet_uint16>(std::stoi(port));
    connect(enetpp::client_connect_params().set_channel_count(1).set_server_host_name_and_port(host.c_str(), port_num).set_timeout(chrono::seconds(10)));
}

NierClient::~NierClient() {
    std::scoped_lock _{m_mtx};
    disconnect();

    if (m_log_copy_file != nullptr) {
        fclose(m_log_copy_file);
    }
}

void NierClient::pump() {
    std::scoped_lock _{m_mtx};

    consume_events(
        [this]() { on_connect(); },
        [this]() { on_disconnect(); },
        [this](const enet_uint8* a, size_t b) {
            on_data_received(a, b);
        }
    );
}

void NierClient::on_world_unloaded() {
    std::scoped_lock _{m_mtx};

    if (!m_in_world.exchange(false)) {
        return;
    }

    spdlog::info("[Coop] World unloaded, dropping entity handles");

    {
        std::scoped_lock __{m_players_mutex};

        for (auto& [guid, player] : m_players) {
            if (player == nullptr || guid == m_guid) {
                continue;
            }

            // The game destroys every entity on load, the puppets get rebound in sync_puppets.
            player->set_handle(0);
            player->set_story_buddy(false);
        }

        // Nothing to give back to the AI anymore, the entities are gone.
        m_pending_destroy.clear();
    }

    if (m_network_entities != nullptr) {
        m_network_entities = std::make_unique<EntitySync>(m_network_entities->get_max_guid());
    }

    m_next_bind_attempt.clear();
    m_need_entity_announce = true;

    m_enemy_deaths.clear();
    {
        std::scoped_lock __{m_remote_deaths_mutex};
        m_remote_deaths.clear();
    }

    {
        // The puppets are gone, a new entity may get the same address.
        std::scoped_lock __{m_puppet_input_mutex};
        m_puppet_inputs.clear();
    }

    m_watched_handle = 0;
    m_swap_done = false;
    m_swap_pending = false;
    m_swap_checked_handle = 0;
    m_swap_from_handle = 0;
    m_handed_back_handle = 0;
    m_logged_player_flight_unit = 0;
    m_logged_buddy_flight_unit = 0;
    m_flight_unit_owner = 0;
}

bool NierClient::recover_lost_character() {
    std::scoped_lock _{m_mtx};

    // Fallback for hand_back_deleted_character: the character we took over is already gone, the game has
    // no player. It never expects that: a little later the Pl0000 update reads the player without a null
    // check and crashes (NieRAutomata.exe + 0x6ac8d1). The game's own switch can't be used here, it
    // crashes without a current player too, so only the player handle is written.
    if (m_swap_from_handle == 0) {
        return false;
    }

    const auto handle = m_swap_from_handle;
    m_swap_from_handle = 0; // One attempt per takeover, never every frame.

    auto entity_list = sdk::EntityList::get();
    auto original = entity_list != nullptr ? entity_list->get_by_handle(handle) : nullptr;

    if (original == nullptr || original->behavior == nullptr || !original->behavior->is_pl0000()) {
        spdlog::warn("[Coop] The character we played is gone and our own character is gone too, nothing to switch to");
        return false;
    }

    spdlog::warn("[Coop] The character we played is gone, forcing the player back to our own character {:x}",
        original->behavior->model_index());

    m_no_swap_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    if (!sdk::EntityList::force_possessed_handle(handle)) {
        spdlog::error("[Coop] Could not switch back to our own character");
        return false;
    }

    // Take over the buddy again once the story brings it back.
    m_swap_done = false;
    m_swap_pending = false;
    m_swap_checked_handle = 0;
    return true;
}

void NierClient::hand_back_deleted_character(sdk::Entity* entity) {
    // The story deletes the buddy we took over while it is the player character (9S in a prologue
    // cutscene). Switch back to our own character while the buddy still exists.
    if (entity == nullptr || entity->behavior == nullptr || !entity->behavior->is_pl0000()) {
        return;
    }

    if (hand_back_character(entity, "the game deletes it")) {
        // Not taken over again while the game finishes deleting it.
        m_handed_back_handle = entity->handle;
    }
}

bool NierClient::hand_back_character(sdk::Entity* played_entity, const char* reason) {
    // Back to our own character with the game's own switch: changePlayer makes the current player's
    // buddy (Pl0000 buddy_handle) the player.
    if (m_swap_from_handle == 0 || played_entity == nullptr || played_entity->behavior == nullptr ||
        !played_entity->behavior->is_pl0000()) {
        return false;
    }

    auto entity_list = sdk::EntityList::get();

    if (entity_list == nullptr || entity_list->get_possessed_entity() != played_entity) {
        return false;
    }

    auto played = played_entity->behavior->as<sdk::Pl0000>();
    auto original = entity_list->get_by_handle(m_swap_from_handle);

    if (original == nullptr || original->behavior == nullptr || !original->behavior->is_pl0000()) {
        spdlog::warn("[Coop] Giving back the character we play ({:x}, {}), but our own character is gone",
            played->model_index(), reason);
        return false;
    }

    // In the prologue 9S's buddy is not our 2B at this point (test 0.2.2), changePlayer would pick
    // the wrong character or nobody. Point it at our own character first.
    const auto old_buddy = played->buddy_handle();

    spdlog::warn("[Coop] Giving back the character we play ({:x}, buddy {:x}, {}), switching to our own character {:x} ({:x})",
        played->model_index(), old_buddy, reason, original->behavior->model_index(), original->handle);

    played->buddy_handle() = original->handle;

    // On the prologue bridge the story gives the player and the buddy a Flight Unit each. While we play
    // 9S, 9S gets the player's one and our 2B the buddy's (AI) one. After the hand back the game mounts
    // 2B on hers and it does not take the player's input (tests 0.2.4-0.2.6). Our own character takes
    // the Flight Unit we were riding.
    auto own = original->behavior->as<sdk::Pl0000>();
    auto played_flight_unit = entity_list->get_by_handle(played->flight_unit_handle());

    if (played_flight_unit != nullptr && played_flight_unit->behavior != nullptr &&
        played_flight_unit->behavior->model_index() == sdk::EModel::MODEL_FLIGHT_UNIT &&
        own->flight_unit_handle() != played->flight_unit_handle()) {
        spdlog::info("[Coop] Swapping Flight Units: our character gets {:x} (was {:x})",
            played->flight_unit_handle(), own->flight_unit_handle());
        m_flight_unit_owner = original->handle;
        m_previous_flight_unit = own->flight_unit_handle();
        std::swap(played->flight_unit_handle(), own->flight_unit_handle());
    }

    // Our own character may be the puppet of the other player right now: give it back first, so the
    // puppet state restored later does not overwrite what the hero switch sets on it.
    {
        std::scoped_lock _{m_players_mutex};

        for (auto& [guid, player] : m_players) {
            if (player != nullptr && guid != m_guid && player->get_handle() == original->handle) {
                release_puppet(*player);
            }
        }
    }

    played->changePlayer();

    m_no_swap_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    const auto possessed = entity_list->get_possessed_entity();
    spdlog::info("[Coop] After the switch we play {:x}", possessed != nullptr && possessed->behavior != nullptr
        ? possessed->behavior->model_index() : 0);

    m_swap_from_handle = 0;
    m_swap_done = false;
    m_swap_pending = false;
    m_swap_checked_handle = 0;
    return true;
}

void NierClient::on_story_script(const char* name, sdk::Behavior* self) {
    std::scoped_lock _{m_mtx};

    auto entity_list = sdk::EntityList::get();
    auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;
    const auto on_player = possessed != nullptr && possessed->behavior == self;

    spdlog::info("[Script] {} on {:x} ({})", name, self != nullptr ? self->model_index() : 0,
        on_player ? "player" : "not player");

    const std::string_view script{name};

    static constexpr std::string_view starts[] = {
        "Pl0000.startPhaseEventFlightUnit", "Pl0000.startPhaseEventFlightUnitNoWep", "Pl0000.setBuddyFlightUnit",
        "Pl0000.initEm1100FlightBattle", "Pl0000.initEm1100FinalFlightBattle", "Pl0000.initEm4000FlightBattle",
    };
    static constexpr std::string_view ends[] = {
        "Pl0000.endPhaseEventFlightUnit", "Pl0000.clearGetOnFlightUnit",
        "Pl0000.endEm1100FlightBattle", "Pl0000.endEm1100FinalFlightBattle", "Pl0000.endEm4000FlightBattle",
    };

    if (std::find(std::begin(ends), std::end(ends), script) != std::end(ends)) {
        spdlog::info("[Coop] Flight Unit phase ended");
        return;
    }

    if (std::find(std::begin(starts), std::end(starts), script) != std::end(starts)) {
        spdlog::info("[Coop] Flight Unit phase started");
    }
}

void NierClient::on_flight_unit_spawned(sdk::Entity* entity) {
    spdlog::info("[Coop] Flight Unit spawned ({:x})", entity->handle);
}

void NierClient::log_flight_units(sdk::EntityList* entity_list) {
    // The Flight Unit our character got on hand back is gone: give it back the one it had.
    if (m_flight_unit_owner != 0) {
        auto owner = entity_list->get_by_handle(m_flight_unit_owner);

        if (owner == nullptr || owner->behavior == nullptr || !owner->behavior->is_pl0000()) {
            m_flight_unit_owner = 0;
        } else if (auto& handle = owner->behavior->as<sdk::Pl0000>()->flight_unit_handle();
                   handle != 0 && entity_list->get_by_handle(handle) == nullptr) {
            const auto previous = entity_list->get_by_handle(m_previous_flight_unit) != nullptr ? m_previous_flight_unit : 0;
            spdlog::warn("[Coop] The Flight Unit we took over ({:x}) is gone, back to {:x}", handle, previous);
            handle = previous;
            m_flight_unit_owner = 0;
        }
    }

    auto possessed = get_local_character(entity_list);

    if (possessed == nullptr) {
        return;
    }

    auto player = possessed->behavior->as<sdk::Pl0000>();
    auto buddy = entity_list->get_by_handle(player->buddy_handle());

    const auto player_flight_unit = player->flight_unit_handle();
    const auto buddy_flight_unit = buddy != nullptr && buddy->behavior != nullptr && buddy->behavior->is_pl0000()
        ? buddy->behavior->as<sdk::Pl0000>()->flight_unit_handle() : 0;

    if (player_flight_unit != m_logged_player_flight_unit || buddy_flight_unit != m_logged_buddy_flight_unit) {
        spdlog::info("[Coop] Flight Units: player {:x} has {:x}, buddy {:x} has {:x}", player->model_index(),
            player_flight_unit, buddy != nullptr && buddy->behavior != nullptr ? buddy->behavior->model_index() : 0,
            buddy_flight_unit);

        m_logged_player_flight_unit = player_flight_unit;
        m_logged_buddy_flight_unit = buddy_flight_unit;
    }
}

void NierClient::think() {
    std::scoped_lock _{m_mtx};

    m_in_game_thread = true;
    ScopeExit __{[this] { m_in_game_thread = false; }};

    m_in_world = true;
    update_bunker_ring();

    pump();

    // Enemies are tracked from the start, to know where each one first appeared.
    sync_enemy_deaths(is_connected() && m_welcome_received);
    // Taken every frame: what finished before the session started is not sent later.
    const auto story_actions = StoryActionSync::get().take_finished();

    {
        // The client's own phase changes wait for the host's (TASKS 3.2.2).
        std::scoped_lock __{m_players_mutex};
        const auto host = get_host_player();
        const auto follow = is_connected() && m_welcome_received && !m_is_master_client && share_story_actions && host != nullptr;
        PhaseRequestGate::get().set_leader(follow, follow ? host->get_story().phase : std::string{}, follow ? host->get_story().phase_requests : 0);
        // The client's Game Over waits for the host's while the death is shared (TASKS 1.4.4).
        PhaseRequestGate::get().set_shared_death(follow && is_death_shared(*host));
        // Story talks are the host's too (TASKS 3.2.3).
        StoryTalkGate::get().set_follow(follow);
    }

    // Also when the session is gone: then the ring turns on its own again.
    follow_host_bunker_ring();

    if (!is_connected()) {
        return;
    }

    // Hello needs the model of our character, so it waits until we are in the world.
    if (!m_hello_sent) {
        send_hello();
        return;
    }

    if (!m_welcome_received) {
        return;
    }

    send_log_copy();
    send_story_state();
    send_story_actions(story_actions);
    send_bunker_ring();

    if (!m_story_catch_up.empty()) {
        const auto done = StoryActionSync::get().snapshot();
        spdlog::info("[StoryAction] {} came into our phase: sending the {} actions finished in it so far", m_story_catch_up, done.size());
        send_story_actions(done);
        m_story_catch_up.clear();
    }

    // Joined while not in the world, or came back from a loading screen:
    // let the others know about the enemies around us.
    if (m_need_entity_announce && m_network_entities != nullptr && sync_enemies) {
        m_network_entities->on_enter_server(m_is_master_client);
        m_need_entity_announce = false;
    }

    {
        std::scoped_lock ___{m_players_mutex};

        for (auto& player : m_pending_destroy) {
            release_puppet(*player);
        }

        m_pending_destroy.clear();
    }

    // Story switches (Flight Unit, hacking, cutscenes) show up here, it helps to read the logs.
    if (auto entity_list = sdk::EntityList::get(); entity_list != nullptr) {
        const auto possessed = entity_list->get_possessed_entity();
        const auto model = possessed != nullptr && possessed->behavior != nullptr ? possessed->behavior->model_index() : 0;

        if (model != m_last_possessed_model) {
            spdlog::info("[Coop] Now controlling {:x}", model);
            m_last_possessed_model = model;
        }
    }

    if (auto entity_list = sdk::EntityList::get(); entity_list != nullptr) {
        log_flight_units(entity_list);
    }

    try_coop_swap();
    sync_puppets();

    if (m_players.contains(m_guid)) {
        update_local_player_data();
        watch_local_character();
        watch_own_death();
        apply_partner_death();
        send_player_data();
        stop_stale_puppets();

        // Synchronize the players.
        for (auto& it : m_players) {
            const auto& networked_player = it.second;

            // Do not update the local player here.
            if (networked_player == nullptr || networked_player->get_guid() == m_guid) {
                continue;
            }

            auto npc = networked_player->get_entity();

            // Not bound yet, sync_puppets will retry. Never drive our own character with the other player's data.
            if (npc == nullptr || is_local_character(npc)) {
                continue;
            }

            //spdlog::info("Synchronizing player {}", networkedPlayer->get_guid());

            auto& data = networked_player->get_player_data();
            npc->run_speed_type() = regenny::ERunSpeedType::SPEED_PLAYER;
            npc->flashlight() = data.flashlight();
            npc->speed() = data.speed();
            npc->facing() = data.facing();
            npc->facing2() = data.facing2();
            npc->weapon_index() = data.weapon_index();
            npc->pod_index() = data.pod_index();
            npc->character_controller().held_flags = data.held_button_flags();
            //*npc->getPosition() = *(Vector3f*)&data.position();
            keep_puppet_alive(npc);
        }

        smooth_puppet_positions();
        update_puppet_inputs();
        log_animation_stats();

        if (sync_enemies) {
            m_network_entities->think();
        }
    }
}

void NierClient::on_draw_ui() {
    std::scoped_lock _{m_players_mutex};

    for (auto& it : m_players) {
        if (it.second->get_guid() == m_guid) {
            continue;
        }

        if (ImGui::TreeNode(it.second->get_name().c_str())) {
            if (ImGui::Button("Teleport To")) {
                auto ents = sdk::EntityList::get();
                auto controlled = ents->get_possessed_entity();

                if (controlled != nullptr && controlled->behavior != nullptr) {
                    if (controlled->behavior->is_pl0000()) {
                        controlled->behavior->as<sdk::Pl0000>()->setPosRotResetHap(Vector4f{remote_position(*it.second), 1.0f}, glm::identity<glm::quat>());
                    } else {
                        controlled->behavior->position() = remote_position(*it.second);
                    }
                }
            }

            ImGui::TreePop();
        }
    }
}

void NierClient::on_frame() {
    std::scoped_lock _{m_players_mutex};

    const auto size = g_framework->get_d3d11_rt_size();
    const auto camera = sdk::CameraGame::get();

    for (auto& it : m_players) {
        if (it.second->get_guid() == m_guid) {
            continue;
        }

        if (it.second->get_entity() == nullptr) {
            continue;
        }

        const auto s = camera->world_to_screen(size, it.second->get_entity()->position());

        if (s) {
            ImGui::GetBackgroundDrawList()->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                ImVec2{s->x, s->y + 1},
                0xFF000000,
                it.second->get_name().c_str());

            ImGui::GetBackgroundDrawList()->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                ImVec2{s->x, s->y -1},
                0xFF000000,
                it.second->get_name().c_str());

            ImGui::GetBackgroundDrawList()->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                ImVec2{s->x - 1, s->y},
                0xFF000000,
                it.second->get_name().c_str());

            ImGui::GetBackgroundDrawList()->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                ImVec2{s->x + 1, s->y},
                0xFF000000,
                it.second->get_name().c_str());

            ImGui::GetBackgroundDrawList()->AddText(
                ImGui::GetFont(),
                ImGui::GetFontSize(),
                *(ImVec2*)&*s,
                ImGui::GetColorU32(ImGuiCol_Text),
                it.second->get_name().c_str());
        }
    }
}

void NierClient::on_connect() {
    // Hello is sent from think() once our character exists, so connecting from the main menu is fine.
    spdlog::info("Connected");
}

void NierClient::on_disconnect() {
    // AutomataMPMod::update_connection notices the failed state and tears the client down.
    spdlog::info("Disconnected");
}

void NierClient::on_data_received(const enet_uint8* data, size_t size) {
    try {
        auto verif = flatbuffers::Verifier(data, size);
        const auto packet = flatbuffers::GetRoot<nier::Packet>(data);

        if (!packet->Verify(verif)) {
            spdlog::error("Invalid packet");
            return;
        }

        on_packet_received(packet);
    } catch(const std::exception& e) {
        spdlog::error("Exception occurred during packet processing: {}", e.what());
    } catch(...) {
        spdlog::error("Unknown exception occurred during packet processing");
    }
}

void NierClient::on_packet_received(const nier::Packet* packet) {
    if (packet->id() == nier::PacketType_ID_REJECT) {
        handle_reject(packet);
        return;
    }

    if (!m_welcome_received && packet->id() != nier::PacketType_ID_WELCOME) {
        spdlog::error("Expected welcome packet, but got {} ({}), ignoring", packet->id(), nier::EnumNamePacketType(packet->id()));
        return;
    }

    const nier::PlayerPacket* player_packet = nullptr;

    // Bounced player packets.
    if (packet->id() > nier::PacketType_ID_CLIENT_START && packet->id() < nier::PacketType_ID_CLIENT_END) {
        player_packet = flatbuffers::GetRoot<nier::PlayerPacket>(packet->data()->data());
        flatbuffers::Verifier player_verif(packet->data()->data(), packet->data()->size());

        if (!player_packet->Verify(player_verif)) {
            spdlog::error("Invalid player packet {} ({})", packet->id(), nier::EnumNamePacketType(packet->id()));
            return;
        }

        on_player_packet_received(packet->id(), player_packet);
        return;
    }

    // Standard packets.
    switch(packet->id()) {
        case nier::PacketType_ID_WELCOME: {
            if (handle_welcome(packet)) {
                m_welcome_received = true;
            }

            break;
        }

        case nier::PacketType_ID_SET_MASTER_CLIENT: {
            if (!handle_set_master_client(packet)) {
                spdlog::error("Invalid set master client packet");
            }

            break;
        }

        // The server measures the round trip time.
        case nier::PacketType_ID_PING: {
            send_packet(nier::PacketType_ID_PONG);
            break;
        }

        case nier::PacketType_ID_CREATE_PLAYER: {
            if (!handle_create_player(packet)) {
                spdlog::error("Failed to create player");
            }

            break;
        }

        case nier::PacketType_ID_DESTROY_PLAYER: {
            if (!handle_destroy_player(packet)) {
                spdlog::error("Failed to destroy player");
            }

            break;
        }
        
        case nier::PacketType_ID_SPAWN_ENTITY: [[fallthrough]];
        case nier::PacketType_ID_DESTROY_ENTITY: [[fallthrough]];
        case nier::PacketType_ID_ENTITY_DATA: [[fallthrough]];
        case nier::PacketType_ID_ENTITY_ANIMATION_START: {
            // Enemies only exist in the loaded world. The host re-announces its enemies after we load.
            if (!can_touch_entities() || m_network_entities == nullptr || !sync_enemies) {
                break;
            }

            const auto entity_packet = flatbuffers::GetRoot<nier::EntityPacket>(packet->data()->data());
            flatbuffers::Verifier entity_verif(packet->data()->data(), packet->data()->size());

            if (!entity_packet->Verify(entity_verif)) {
                spdlog::error("Invalid entity packet {} ({})", packet->id(), nier::EnumNamePacketType(packet->id()));
                return;
            }

            on_entity_packet_received(packet->id(), entity_packet);
            break;
        }

        default:
            spdlog::error("Unknown packet type {} ({})", packet->id(), nier::EnumNamePacketType(packet->id()));
            break;
    }

    /*if (data->id >= ID_SHARED_START && data->id < ID_SHARED_END) {
        AutomataMPMod::get()->sharedPacketProcess(data, size);
    }
    else if (data->id >= ID_SERVER_START && data->id < ID_SERVER_END) {
        AutomataMPMod::get()->serverPacketProcess(data, size);
    }*/
}

void NierClient::on_player_packet_received(nier::PacketType packet_type, const nier::PlayerPacket* packet) {
    spdlog::debug("Player packet {} received from {}", nier::EnumNamePacketType(packet_type), packet->guid());

    switch (packet_type) {
    case nier::PacketType_ID_PLAYER_DATA: {
        if (!handle_player_data(packet)) {
            spdlog::debug("Failed to handle player data");
        }

        break;
    }
    case nier::PacketType_ID_ANIMATION_START: {
        if (!handle_animation_start(packet)) {
            spdlog::debug("Failed to handle animation start");
        }

        break;
    }
    case nier::PacketType_ID_BUTTONS: {
        if (!handle_buttons(packet)) {
            spdlog::debug("Failed to handle buttons");
        }

        break;
    }
    case nier::PacketType_ID_STORY_STATE: {
        if (!handle_story_state(packet)) {
            spdlog::debug("Failed to handle story state");
        }

        break;
    }
    case nier::PacketType_ID_STORY_ACTION: {
        if (!handle_story_action(packet)) {
            spdlog::debug("Failed to handle story action");
        }

        break;
    }
    case nier::PacketType_ID_BUNKER_RING: {
        if (!handle_bunker_ring(packet)) {
            spdlog::debug("Failed to handle the Bunker ring");
        }

        break;
    }
    case nier::PacketType_ID_PLAYER_DIED: {
        if (!handle_player_died(packet)) {
            spdlog::debug("Failed to handle the other player's death");
        }

        break;
    }
    case nier::PacketType_ID_ENEMY_DIED: {
        if (!handle_enemy_died(packet)) {
            spdlog::debug("Failed to handle enemy death");
        }

        break;
    }
    default:
        spdlog::error("Unknown player packet type {} ({})", packet_type, nier::EnumNamePacketType(packet_type));
        break;
    }
}

void NierClient::on_entity_packet_received(nier::PacketType packet_type, const nier::EntityPacket* packet) {
    spdlog::debug("Entity packet {} received from {}", nier::EnumNamePacketType(packet_type), packet->guid());

    switch (packet_type) {
    case nier::PacketType_ID_SPAWN_ENTITY: {
        if (!handle_create_entity(packet)) {
            spdlog::error("Failed to handle spawn entity");
        }

        break;
    }
    case nier::PacketType_ID_DESTROY_ENTITY: {
        if (!handle_destroy_entity(packet)) {
            spdlog::error("Failed to handle destroy entity");
        }

        break;
    }
    case nier::PacketType_ID_ENTITY_DATA: {
        if (!handle_entity_data(packet)) {
            spdlog::debug("Failed to handle entity data");
        }

        break;
    }
    case nier::PacketType_ID_ENTITY_ANIMATION_START: {
        if (!handle_entity_animation_start(packet)) {
            spdlog::debug("Failed to handle entity animation start");
        }

        break;
    }
    default:
        spdlog::error("Unknown entity packet type {} ({})", packet_type, nier::EnumNamePacketType(packet_type));
        break;
    }
}

void NierClient::send_packet(nier::PacketType id, const uint8_t* data, size_t size) {
    auto builder = flatbuffers::FlatBufferBuilder{};

    uint32_t dataoffs = 0;

    if (data != nullptr && size > 0) {
        builder.StartVector(size, 1); // byte vector
        for (int64_t i = (int64_t)size - 1; i >= 0; i--) {
            builder.PushElement(data[i]);
        }
        dataoffs = builder.EndVector(size);
    }

    auto packet_builder = nier::PacketBuilder(builder);

    packet_builder.add_magic(1347240270);
    packet_builder.add_id(id);

    if (data != nullptr && size > 0) {
        packet_builder.add_data(dataoffs);
    }

    builder.Finish(packet_builder.Finish());

    this->enetpp::client::send_packet(0, builder.GetBufferPointer(), builder.GetSize(), ENET_PACKET_FLAG_RELIABLE);
}

void NierClient::send_log_copy() {
    if (!m_server_accepts_logs) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < m_next_log_copy) {
        return;
    }

    m_next_log_copy = now + std::chrono::seconds(1);

    if (m_log_copy_file == nullptr) {
        const auto& sinks = spdlog::default_logger()->sinks();
        const auto file_sink = !sinks.empty() ? std::dynamic_pointer_cast<spdlog::sinks::basic_file_sink_mt>(sinks[0]) : nullptr;

        if (file_sink == nullptr) {
            m_server_accepts_logs = false;
            spdlog::error("[Coop] Can't find our log file ({} sinks), the server won't get a copy of it", sinks.size());
            return;
        }

        // Shared read: spdlog keeps writing the file.
        if ((m_log_copy_file = _fsopen(file_sink->filename().c_str(), "rb", _SH_DENYNO)) == nullptr) {
            m_server_accepts_logs = false;
            spdlog::error("[Coop] Can't read our own log {} (errno {}), the server won't get a copy of it",
                file_sink->filename(), errno);
            return;
        }
    }

    // At most 64 KB a second: the log since the game started is sent over the first seconds.
    std::array<uint8_t, 16 * 1024> buffer{};

    for (auto i = 0; i < 4; ++i) {
        clearerr(m_log_copy_file);
        const auto size = fread(buffer.data(), 1, buffer.size(), m_log_copy_file);

        if (size == 0) {
            break;
        }

        send_packet(nier::PacketType_ID_LOG_CHUNK, buffer.data(), size);
    }
}

void NierClient::send_animation_start(uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4) {
    nier::AnimationStart data{anim, variant, a3, a4};

    flatbuffers::FlatBufferBuilder builder(0);
    auto dataoffs = builder.CreateStruct(data);
    builder.Finish(dataoffs);

    ++m_stat_anims_sent;
    send_packet(nier::PacketType_ID_ANIMATION_START, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_buttons(const uint32_t* buttons) {
    flatbuffers::FlatBufferBuilder builder(0);
    const auto dataoffs = builder.CreateVector(buttons, sdk::Pl0000::EButtonIndex::INDEX_MAX);

    nier::Buttons::Builder data_builder(builder);
    data_builder.add_buttons(dataoffs);
    builder.Finish(data_builder.Finish());

    ++m_stat_buttons_sent;
    send_packet(nier::PacketType_ID_BUTTONS, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_enemy_died(const EnemyDeathSync::Death& death) {
    nier::EnemyDied data{death.model, *(nier::Vector3f*)&death.spawn_position, *(nier::Vector3f*)&death.position};

    flatbuffers::FlatBufferBuilder builder(0);
    auto dataoffs = builder.CreateStruct(data);
    builder.Finish(dataoffs);

    send_packet(nier::PacketType_ID_ENEMY_DIED, builder.GetBufferPointer(), builder.GetSize());
}

// The host leads the story: the actions of its story scripts that finished, the client's game finishes them too.
void NierClient::send_story_actions(const std::vector<StoryActionSync::Finished>& actions) {
    if (!m_is_master_client || !share_story_actions) {
        return;
    }

    for (const auto& action : actions) {
        flatbuffers::FlatBufferBuilder builder(0);
        const auto phase = builder.CreateString(action.phase);
        nier::StoryActionBuilder data_builder(builder);
        data_builder.add_phase(phase);
        data_builder.add_graph(action.graph);
        data_builder.add_action(action.action);
        data_builder.add_state(action.state);
        builder.Finish(data_builder.Finish());

        send_packet(nier::PacketType_ID_STORY_ACTION, builder.GetBufferPointer(), builder.GetSize());
        spdlog::info("[StoryAction] Sent {:08x} of graph {:08x} \"{}\" with {:08x} ({})", action.action, action.graph, action.name, action.state, action.phase);
    }
}

void NierClient::sync_enemy_deaths(bool connected) {
    // The old "enemies from the host" scheme deletes and recreates enemies itself.
    if (!share_enemy_deaths || sync_enemies) {
        return;
    }

    auto entity_list = sdk::EntityList::get();

    if (entity_list == nullptr) {
        return;
    }

    std::vector<EnemyDeathSync::Death> remote_deaths{};
    {
        std::scoped_lock _{m_remote_deaths_mutex};
        remote_deaths.swap(m_remote_deaths);
    }

    for (const auto& death : remote_deaths) {
        m_enemy_deaths.on_remote_death(entity_list, death);
    }

    std::vector<EnemyDeathSync::Death> deaths{};
    m_enemy_deaths.think(entity_list, deaths);

    // Not connected: nobody to tell, the deaths are only remembered as ours.
    if (connected) {
        for (const auto& death : deaths) {
            send_enemy_died(death);
        }
    }

    const auto now = std::chrono::steady_clock::now();

    if (now >= m_next_enemy_stats_log) {
        m_next_enemy_stats_log = now + std::chrono::seconds(30);

        const auto stats = m_enemy_deaths.take_stats();

        if (stats.deaths_sent + stats.deaths_received > 0) {
            spdlog::info("[Enemies] 30 s: new enemies {}, deaths sent {} received {}: killed here {}, already dead {}, "
                         "not found {}",
                stats.tracked, stats.deaths_sent, stats.deaths_received, stats.killed, stats.already_dead, stats.unmatched);
        }
    }
}

void NierClient::send_entity_packet(nier::PacketType id, uint32_t guid, const uint8_t* data, size_t size) {
    flatbuffers::FlatBufferBuilder builder(0);
    const auto dataoffs = builder.CreateVector(data, size);

    nier::EntityPacket::Builder data_builder(builder);
    data_builder.add_guid(guid);
    data_builder.add_data(dataoffs);
    builder.Finish(data_builder.Finish());

    send_packet(id, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_entity_create(uint32_t guid, sdk::EntitySpawnParams* data) {
    if (!m_is_master_client) {
        spdlog::debug("Not master client, not sending entity create");
        return;
    }

    // entity packet.
    flatbuffers::FlatBufferBuilder builder(0);
    const auto name = builder.CreateString(data->name);

    nier::EntitySpawnParams::Builder data_builder(builder);
    data_builder.add_name(name);
    data_builder.add_model(data->model);
    data_builder.add_model2(data->model2);

    if (data->matrix != nullptr) {
        data_builder.add_positional((nier::EntitySpawnPositionalData*)data->matrix);
    }

    builder.Finish(data_builder.Finish());

    send_entity_packet(nier::PacketType_ID_SPAWN_ENTITY, guid, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_entity_destroy(uint32_t guid) {
    if (!m_is_master_client) {
        spdlog::debug("Not master client, not sending entity destroy");
        return;
    }

    send_entity_packet(nier::PacketType_ID_DESTROY_ENTITY, guid);
}

void NierClient::send_entity_data(uint32_t guid, sdk::BehaviorAppBase* entity) {
    if (!m_is_master_client) {
        spdlog::debug("Not master client, not sending entity data");
        return;
    }

    flatbuffers::FlatBufferBuilder builder(0);
    nier::EntityData new_data(entity->facing(),
        0.0f, // entity is not a player.
        entity->health(), *(nier::Vector3f*)&entity->position());

    builder.Finish(builder.CreateStruct(new_data));

    m_network_entities->process_entity_data(guid, &new_data);
    send_entity_packet(nier::PacketType_ID_ENTITY_DATA, guid, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_entity_animation_start(uint32_t guid, uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4) {
    if (!m_is_master_client) {
        spdlog::debug("Not master client, not sending entity animation start");
        return;
    }
    
    flatbuffers::FlatBufferBuilder builder(0);
    nier::AnimationStart data{anim, variant, a3, a4};
    builder.Finish(builder.CreateStruct(data));

    send_entity_packet(nier::PacketType_ID_ENTITY_ANIMATION_START, guid, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::on_entity_created(sdk::Entity* entity, sdk::EntitySpawnParams* data) {
    // Without enemy sync the enemies of our own story stay as they are.
    if (m_network_entities == nullptr || !sync_enemies) {
        return;
    }

    if (!m_is_master_client) {
        entity->behavior->terminate(); // destroy the entity. only the server or the master client should create entities.
        return;
    }

    m_network_entities->on_entity_created(entity, data);
}

void NierClient::on_entity_deleted(sdk::Entity* entity) {
    std::scoped_lock _{m_mtx};

    hand_back_deleted_character(entity);

    if (m_network_entities == nullptr) {
        return;
    }

    m_network_entities->on_entity_deleted(entity);
}

void NierClient::send_hello() {
    auto ents = sdk::EntityList::get();
    auto possessed = ents != nullptr ? ents->get_possessed_entity() : nullptr;

    if (possessed == nullptr || possessed->behavior == nullptr) {
        spdlog::error("No possessed entity");
        return;
    }

    // The server disconnects anyone announcing something other than 2B/9S/A2,
    // so never send the model of a Flight Unit or similar.
    uint32_t model = sdk::EModel::MODEL_2B;

    if (auto character = get_local_character(ents); character != nullptr) {
        model = character->behavior->model_index();
    } else if (auto player = ents->get_by_name("Player");
               player != nullptr && player->behavior != nullptr && player->behavior->is_pl0000() && is_character_model(player->behavior->model_index())) {
        model = player->behavior->model_index();
    }

    spdlog::info("[Coop] Hello: controlling model {:x}, announcing model {:x}, host role {}, mod {}",
        possessed->behavior->model_index(), model, m_wants_host, AUTOMATAMP_FULL_VERSION);

    flatbuffers::FlatBufferBuilder builder{};
    const auto name_pkt = builder.CreateString(m_hello_name);
    const auto pwd_pkt = builder.CreateString(m_password);
    const auto version_pkt = builder.CreateString(AUTOMATAMP_FULL_VERSION);

    nier::HelloBuilder hello_builder(builder);
    hello_builder.add_major(nier::VersionMajor_Value);
    hello_builder.add_minor(nier::VersionMinor_Value);
    hello_builder.add_patch(nier::VersionPatch_Value);
    hello_builder.add_name(name_pkt);
    hello_builder.add_password(pwd_pkt);
    hello_builder.add_model(model);
    hello_builder.add_wants_host(m_wants_host);
    hello_builder.add_mod_version(version_pkt);

    builder.Finish(hello_builder.Finish());

    send_packet(nier::PacketType_ID_HELLO, builder.GetBufferPointer(), builder.GetSize());
    m_hello_sent = true;
    m_hello_sent_at = std::chrono::steady_clock::now();
}

void NierClient::update_local_player_data() {
    if (!m_hello_sent || !m_welcome_received || m_guid == 0) {
        return;
    }

    auto it = m_players.find(m_guid);

    if (it == m_players.end() || it->second == nullptr) {
        spdlog::error("Local player not set up");
        return;
    }

    auto entity_list = sdk::EntityList::get();

    if (entity_list == nullptr) {
        return;
    }

    // No character to synchronize right now (Flight Unit, hacking...): pause sending player data.
    auto player = get_local_character(entity_list);
    m_has_local_character = player != nullptr;

    it->second->set_handle(player != nullptr ? player->handle : 0);
}

void NierClient::send_player_data() {
    if (m_guid == 0) {
        spdlog::error("Cannot send player data without GUID");
        return;
    }

    auto it = m_players.find(m_guid);

    if (it == m_players.end() || it->second == nullptr) {
        spdlog::error("Cannot send player data without player");
        return;
    }

    auto& player = it->second;
    
    auto entity = player->get_entity();

    // Not controlling a character right now, see update_local_player_data.
    // A NaN position would break the puppet in the other game (test 2026-10-14), see watch_local_character.
    if (entity == nullptr || !is_finite(entity->position())) {
        return;
    }

    const auto position = m_bunker_ring ? m_bunker_ring->to_ring(entity->position()) : entity->position();
    nier::PlayerData player_data(entity->flashlight(), entity->speed(), entity->facing(), entity->facing2(), entity->weapon_index(),
        entity->pod_index(), entity->character_controller().held_flags, *(nier::Vector3f*)&position);

    flatbuffers::FlatBufferBuilder builder{};
    const auto offs = builder.CreateStruct(player_data);
    builder.Finish(offs);

    send_packet(nier::PacketType_ID_PLAYER_DATA, builder.GetBufferPointer(), builder.GetSize());
}

bool NierClient::handle_welcome(const nier::Packet* packet) {
    spdlog::info("Welcome packet received");

    const auto welcome = flatbuffers::GetRoot<nier::Welcome>(packet->data()->data());
    auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

    if (!welcome->Verify(verif)) {
        spdlog::error("Invalid welcome packet");
        return false;
    }

    m_is_master_client = welcome->isMasterClient();
    m_guid = welcome->guid();
    m_session_id = welcome->sessionId();
    m_server_accepts_logs = welcome->acceptsLogs();
    const auto highest_guid = welcome->highestEntityGuid();

    spdlog::info("[Coop] Joined session {}: guid {}, {}", m_session_id, m_guid,
        m_is_master_client ? "we lead the story" : "the host leads the story");

    if (m_server_accepts_logs) {
        spdlog::info("[Coop] The server keeps a copy of this log");
    }

    m_network_entities = std::make_unique<EntitySync>(highest_guid);

    // Otherwise think() does it as soon as we are back in the world.
    if (can_touch_entities()) {
        m_network_entities->on_enter_server(m_is_master_client);
    } else {
        m_need_entity_announce = true;
    }

    return true;
}

bool NierClient::handle_create_player(const nier::Packet* packet) {
    spdlog::info("Create player packet received");

    const auto create_player = flatbuffers::GetRoot<nier::CreatePlayer>(packet->data()->data());
    auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

    if (!create_player->Verify(verif)) {
        spdlog::error("Invalid create player packet");
        return false;
    }

    std::scoped_lock _{m_players_mutex};

    auto new_player = std::make_unique<Player>();
    new_player->set_guid(create_player->guid());
    new_player->set_name(create_player->name()->c_str());
    new_player->set_model(create_player->model());
    new_player->set_master_client(create_player->isMasterClient());

    spdlog::info("[Coop] Player {} ({}), model {:x}{}", create_player->guid(), create_player->name()->c_str(), create_player->model(),
        create_player->isMasterClient() ? ", leads the story" : "");

    m_players[create_player->guid()] = std::move(new_player);

    // The entity is bound in sync_puppets, which runs on the game thread once the world is loaded.
    return true;
}

bool NierClient::handle_destroy_player(const nier::Packet* packet) {
    spdlog::info("Destroy player packet received");

    const auto destroy_player = flatbuffers::GetRoot<nier::DestroyPlayer>(packet->data()->data());

    std::scoped_lock _{m_players_mutex};

    auto it = m_players.find(destroy_player->guid());

    if (it == m_players.end()) {
        return true;
    }

    if (it->second != nullptr) {
        spdlog::info("[Coop] {} left the session{}", it->second->get_name(),
            it->second->is_master_client() ? ", nobody leads the story until the host is back" : "");
    }

    // Releasing touches game entities, so it's done in think().
    if (it->second != nullptr && it->second->get_handle() != 0) {
        m_pending_destroy.push_back(std::move(it->second));
    }

    m_players.erase(it);
    m_next_bind_attempt.erase(destroy_player->guid());

    return true;
}

bool NierClient::handle_set_master_client(const nier::Packet* packet) {
    if (packet->data() == nullptr || packet->data()->size() < sizeof(nier::SetMasterClient)) {
        return false;
    }

    const auto master = flatbuffers::GetRoot<nier::SetMasterClient>(packet->data()->data());
    const auto guid = master->guid();

    std::scoped_lock _{m_players_mutex};

    m_is_master_client = guid != 0 && guid == m_guid;

    std::string leader = m_is_master_client ? "we" : "nobody, waiting for the host";

    for (auto& [player_guid, player] : m_players) {
        if (player != nullptr) {
            player->set_master_client(player_guid == guid);

            if (player_guid == guid && player_guid != m_guid) {
                leader = player->get_name();
            }
        }
    }

    spdlog::info("[Coop] Story leader changed: {}", leader);

    // Decide again which character we play, the leader may play a different one.
    m_swap_done = false;

    return true;
}

bool NierClient::handle_reject(const nier::Packet* packet) {
    static const std::unordered_map<nier::RejectReason, const char*> reasons{
        {nier::RejectReason_WrongPassword, "Wrong password"},
        {nier::RejectReason_VersionMismatch, "The server runs a different mod version, update the mod or the server"},
        {nier::RejectReason_InvalidModel, "The server does not accept your character"},
        {nier::RejectReason_ServerFull, "The server is full (2 players)"},
        {nier::RejectReason_HostTaken, "Someone else is already the host, join without the host role"},
    };

    m_reject_reason = "Rejected by the server";

    if (packet->data() != nullptr) {
        const auto reject = flatbuffers::GetRoot<nier::Reject>(packet->data()->data());
        auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

        if (reject->Verify(verif)) {
            m_version_rejected = reject->reason() == nier::RejectReason_VersionMismatch;

            if (auto it = reasons.find(reject->reason()); it != reasons.end()) {
                m_reject_reason = it->second;
            }

            if (reject->detail() != nullptr && reject->detail()->size() > 0) {
                m_reject_reason += std::string{" ("} + reject->detail()->c_str() + ")";
            }
        }
    }

    spdlog::error("[Coop] {}", m_reject_reason);
    return true;
}

Player* NierClient::get_host_player() {
    // The server tells who leads the story, see handle_set_master_client.
    for (auto& [guid, player] : m_players) {
        if (player != nullptr && guid != m_guid && player->is_master_client()) {
            return player.get();
        }
    }

    return nullptr;
}

std::string NierClient::get_leader_name() {
    std::scoped_lock _{m_players_mutex};

    const auto host = get_host_player();
    return host != nullptr ? host->get_name() : std::string{};
}

bool NierClient::is_waiting_for_host() {
    if (m_is_master_client) {
        return false;
    }

    std::scoped_lock _{m_players_mutex};
    return get_host_player() == nullptr;
}

void NierClient::try_coop_swap() {
    if (m_is_master_client || !auto_swap_character || !research_swap || m_swap_pending) {
        return;
    }

    std::scoped_lock _{m_players_mutex};

    const auto host = get_host_player();

    if (host == nullptr) {
        return;
    }

    auto entity_list = sdk::EntityList::get();
    auto possessed = get_local_character(entity_list);

    if (possessed == nullptr) {
        return;
    }

    // Decided again whenever the game hands us another character (story switch, after a cutscene).
    if (possessed->handle != m_swap_checked_handle) {
        m_swap_checked_handle = possessed->handle;
        m_swap_done = false;
        m_swap_wait_logged = false;
    }

    if (m_swap_done) {
        return;
    }

    auto local = possessed->behavior->as<sdk::Pl0000>();

    if (local->model_index() != host->get_model()) {
        spdlog::info("[Coop] Playing {:x}, the host plays {:x}: no swap needed", local->model_index(), host->get_model());
        m_swap_done = true;
        return;
    }

    auto buddy = entity_list->get_by_handle(local->buddy_handle());

    // Right after a hand back the buddy is the character the game is deleting: leave it alone.
    if (std::chrono::steady_clock::now() < m_no_swap_until || (buddy != nullptr && buddy->handle == m_handed_back_handle)) {
        return;
    }

    // Checked every think: the buddy often joins the scene later without a loading screen
    // (9S in the prologue), the swap happens as soon as the game puts it in the scene.
    if (buddy == nullptr || buddy == possessed || buddy->behavior == nullptr || !buddy->behavior->is_pl0000() ||
        !is_character_model(buddy->behavior->model_index()) || buddy->behavior->model_index() == local->model_index() ||
        buddy->behavior->isSuspend() != 0) {
        if (!m_swap_wait_logged) {
            spdlog::info("[Coop] Same character as the host, waiting for the story buddy to take over");
            m_swap_wait_logged = true;
        }

        return;
    }

    // A buddy with its own Flight Unit belongs to the game (concept 5.5). On the prologue bridge 9S comes
    // in on his, and if we play him the story later mounts our 2B on an AI Flight Unit that does not take
    // input (1.7.2). Played as 2B, the scene goes like in single player (test 0.2.3).
    if (const auto flight_unit_handle = buddy->behavior->as<sdk::Pl0000>()->flight_unit_handle(); flight_unit_handle != 0) {
        auto flight_unit = entity_list->get_by_handle(flight_unit_handle);

        if (flight_unit != nullptr && flight_unit->behavior != nullptr &&
            flight_unit->behavior->model_index() == sdk::EModel::MODEL_FLIGHT_UNIT) {
            if (m_flight_unit_skip_logged != flight_unit_handle) {
                spdlog::info("[Coop] Story buddy {:x} has a Flight Unit ({:x}), leaving it to the game",
                    buddy->behavior->model_index(), flight_unit_handle);
                m_flight_unit_skip_logged = flight_unit_handle;
            }

            return;
        }
    }

    // The buddy may already be the host's puppet, it becomes ours instead.
    for (auto& [guid, player] : m_players) {
        if (player != nullptr && guid != m_guid && player->get_handle() == buddy->handle) {
            release_puppet(*player);
            m_next_bind_attempt[guid] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    }

    spdlog::info("[Coop] Taking control of the story buddy {:x}", buddy->behavior->model_index());

    // The game's own player <-> buddy switch.
    m_swap_done = true;
    m_swap_from_handle = possessed->handle;
    m_swap_wait_thinks = 0;
    m_swap_pending = true;
    local->changePlayer();
}

void NierClient::sync_puppets() {
    if (!can_touch_entities()) {
        return;
    }

    // Wait for the character switch to happen, otherwise we would bind the character we are about to control.
    if (m_swap_pending) {
        auto entity_list = sdk::EntityList::get();
        auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

        if (possessed != nullptr && possessed->handle != m_swap_from_handle) {
            m_swap_pending = false;
        } else if (++m_swap_wait_thinks > 120) {
            // The game ignored the switch (cutscene, scripted moment): try again later.
            spdlog::warn("[Coop] The game did not switch us to the story buddy, retrying");
            m_swap_pending = false;
            m_swap_done = false;
        } else {
            return;
        }
    }

    std::scoped_lock _{m_players_mutex};

    const auto now = std::chrono::steady_clock::now();

    auto entity_list = sdk::EntityList::get();
    auto possessed = get_local_character(entity_list);

    for (auto& [guid, player] : m_players) {
        if (player == nullptr || guid == m_guid) {
            continue;
        }

        if (auto npc = player->get_entity(); npc != nullptr) {
            // The game made the puppet our character (story switch, a late changePlayer): it is ours now,
            // the other player's data must not move it. Its flags are the game's, nothing to restore.
            if (possessed != nullptr && possessed->behavior == npc) {
                spdlog::warn("[Coop] The puppet of {} ({:x}) is the character we control now, unbinding it",
                    player->get_name(), npc->model_index());
                player->set_handle(0);
                player->set_story_buddy(false);
                m_next_bind_attempt[guid] = now + std::chrono::seconds(2);
                continue;
            }

            if (!player->is_story_buddy()) {
                continue;
            }

            // The story decides where the buddy is: give it back once it's no longer our buddy
            // (story moved on, Flight Unit, hacking...) or the game suspended it.
            const auto buddy_handle = possessed != nullptr ? possessed->behavior->as<sdk::Pl0000>()->buddy_handle() : 0;
            const auto buddy_is_puppet = std::any_of(m_players.begin(), m_players.end(), [&](auto& it) {
                return it.second != nullptr && it.first != m_guid && it.second->get_handle() == buddy_handle;
            });

            const auto flying = possessed != nullptr && get_ridden_flight_unit(possessed->behavior->as<sdk::Pl0000>()) != nullptr;

            if (possessed != nullptr && buddy_is_puppet && npc->isSuspend() == 0 && !flying) {
                continue;
            }

            if (flying) {
                // The flight's script waits for the buddy's AI: with the puppet in its place the flight never turned
                // into the battle from above (camera stayed close, the unit stayed a mech, smoke runs 2026-10-22).
                spdlog::info("[Coop] We fly: the story buddy of {} goes back to the game, only its Flight Unit follows them", player->get_name());
                release_puppet(*player);
                m_next_bind_attempt[guid] = now + std::chrono::seconds(2);
                continue;
            }

            spdlog::info("[Coop] Story buddy of {} is no longer ours, returning it to the game", player->get_name());

            release_puppet(*player);
            m_next_bind_attempt[guid] = now + std::chrono::seconds(2);
            continue;
        }

        if (auto it = m_next_bind_attempt.find(guid); it != m_next_bind_attempt.end() && now < it->second) {
            continue;
        }

        player->set_handle(0);
        player->set_story_buddy(false);

        if (!bind_puppet(*player)) {
            m_next_bind_attempt[guid] = now + std::chrono::seconds(2);
        }
    }
}

bool NierClient::bind_puppet(Player& player) {
    auto entity_list = sdk::EntityList::get();

    if (entity_list == nullptr) {
        return false;
    }

    // Puppets are only bound while we control a character, it's retried later otherwise.
    auto possessed = get_local_character(entity_list);

    if (possessed == nullptr) {
        return false;
    }

    auto local = possessed->behavior->as<sdk::Pl0000>();

    // In a flight the buddy stays the game's, smooth_puppet_positions moves its Flight Unit (TASKS 1.3.13).
    if (get_ridden_flight_unit(local) != nullptr) {
        return false;
    }

    // TASKS 3.1.1: on different routes the other player plays somebody else's story (route A 2B and 9S, route B the
    // other way round), taking over would pull the wrong character out of our scene. The route comes with the first
    // story state, until then nothing is taken over.
    if (!player.get_story().known) {
        return false;
    }

    if (compare_stories(StoryWatcher::get().current(), player.get_story()) == StoryMismatch::routes) {
        if (m_route_block_logged != player.get_name()) {
            m_route_block_logged = player.get_name();
            spdlog::warn("[Coop] {} is on route {}, we are on route {}: different story lines, the story buddy is not taken over",
                player.get_name(), player.get_story().route, StoryWatcher::get().current().route);
        }

        return false;
    }

    m_route_block_logged.clear();

    // Preferred: the buddy the game already gave us (9S next to 2B and so on).
    if (use_story_buddy && research_takeover) {
        auto buddy = entity_list->get_by_handle(local->buddy_handle());

        const auto taken = buddy != nullptr && std::any_of(m_players.begin(), m_players.end(), [&](auto& it) {
            return it.second != nullptr && it.second.get() != &player && it.second->get_handle() == buddy->handle;
        });

        if (buddy != nullptr && buddy != possessed && !taken && buddy->behavior != nullptr && buddy->behavior->is_pl0000() &&
            is_character_model(buddy->behavior->model_index())) {
            // The game keeps the buddy suspended while it isn't part of the scene. Wait for the story
            // instead of spawning a partner, which would replace the story buddy of the local player.
            if (buddy->behavior->isSuspend() != 0) {
                return false;
            }

            // Taking over teleports the buddy to the other player. If that player is far from where our
            // story placed the buddy, our games are at different points: leave the buddy to the story
            // until the other player gets close.
            if (glm::length(remote_position(player) - buddy->behavior->position()) > 50.0f) {
                return false;
            }

            spdlog::info("[Coop] {} takes over the story buddy (model {:x})", player.get_name(), buddy->behavior->model_index());

            // Same AI routines as the spawned partners use, the network input drives the character from now on.
            buddy->assign_ai_routine("PLAYER");
            buddy->assign_ai_routine("player");

            player.set_handle(buddy->handle);
            player.set_story_buddy(true);
            player.set_start_tick(buddy->behavior->tick_count());

            return true;
        }
    }

    // No story buddy right now (prologue, Flight Unit...): wait for the game to give us one.
    if (!spawn_partner) {
        return false;
    }

    auto localplayer = entity_list->get_by_name("Player");

    if (localplayer == nullptr || localplayer->behavior == nullptr || !localplayer->behavior->is_pl0000()) {
        return false;
    }

    // No buddy available: spawn a partner. Use the counterpart character if both play the same one.
    auto model = player.get_model();

    if (!is_character_model(model) || model == local->model_index()) {
        model = get_counterpart_model(local->model_index());
    }

    spdlog::info("[Coop] Spawning partner for {} (model {:x})", player.get_name(), model);

    MidHooks::s_ignore_spawn = true;
    auto ent = entity_list->spawn_entity("partner", model, possessed->behavior->position());
    MidHooks::s_ignore_spawn = false;

    if (ent == nullptr) {
        spdlog::error("Failed to spawn partner");
        return false;
    }

    ent->behavior->as<sdk::Pl0000>()->buddy_handle() = localplayer->handle;
    localplayer->behavior->as<sdk::Pl0000>()->buddy_handle() = ent->handle;

    ent->behavior->setSuspend(false);

    ent->assign_ai_routine("PLAYER");
    ent->assign_ai_routine("player");

    // alternate way of assigning AI/control to the entity easily.
    localplayer->behavior->as<sdk::Pl0000>()->changePlayer();
    localplayer->behavior->as<sdk::Pl0000>()->changePlayer();

    ent->behavior->obj_flags() = -1;
    ent->behavior->as<sdk::Pl0000>()->setBuddyFromNpc();
    ent->behavior->obj_flags() = 0;

    player.set_start_tick(ent->behavior->tick_count());
    player.set_handle(ent->handle);

    spdlog::info(" player assigned handle {:x}", ent->handle);

    return true;
}

void NierClient::release_puppet(Player& player) {
    auto entity_list = sdk::EntityList::get();
    auto npc = player.get_entity();

    if (entity_list == nullptr || npc == nullptr) {
        return;
    }

    auto ent = npc->get_entity();

    if (!player.is_story_buddy()) {
        if (ent != entity_list->get_by_name("Player")) {
            npc->terminate();
        }

        return;
    }

    {
        std::scoped_lock _{m_puppet_input_mutex};

        if (auto it = m_puppet_inputs.find(npc); it != m_puppet_inputs.end()) {
            restore_puppet(npc, it->second);
            m_puppet_inputs.erase(it);
        }
    }

    // Give the story buddy back to the game.
    spdlog::info("[Coop] Returning the story buddy of {} to the AI", player.get_name());

    std::memset(&npc->character_controller().buttons, 0, sizeof(npc->character_controller().buttons));
    npc->character_controller().held_flags = 0;
    npc->run_speed_type() = regenny::ERunSpeedType::SPEED_BUDDY;

    if (const auto routine = get_buddy_routine(npc->model_index()); routine != nullptr) {
        ent->assign_ai_routine(routine);
    }

    ent->assign_ai_routine("buddy");

    player.set_handle(0);
    player.set_story_buddy(false);
}

void NierClient::release_puppets() {
    std::scoped_lock _{m_mtx};

    if (!m_in_world) {
        return;
    }

    m_in_game_thread = true;
    ScopeExit __{[this] { m_in_game_thread = false; }};

    std::scoped_lock ___{m_players_mutex};

    for (auto& player : m_pending_destroy) {
        release_puppet(*player);
    }

    m_pending_destroy.clear();

    for (auto& [guid, player] : m_players) {
        if (player != nullptr && guid != m_guid) {
            release_puppet(*player);
        }
    }
}

bool NierClient::handle_create_entity(const nier::EntityPacket* packet) {
    spdlog::info("Create entity packet received");

    const auto spawn = flatbuffers::GetRoot<nier::EntitySpawnParams>(packet->data()->data());
    auto verif = flatbuffers::Verifier(packet->data()->data(), packet->data()->size());

    if (!spawn->Verify(verif)) {
        spdlog::error("Invalid create entity packet");
        return false;
    }

    auto entity_list = sdk::EntityList::get();

    if (entity_list != nullptr) {
        sdk::EntitySpawnParams params{};
        auto matrix = spawn->positional() != nullptr ? *(sdk::EntitySpawnParams::PositionalData*)spawn->positional() : sdk::EntitySpawnParams::PositionalData{};
        params.matrix = &matrix;
        params.model = spawn->model();
        params.model2 = spawn->model2();
        params.name = spawn->name()->c_str();

        spdlog::info(" Spawning {}", spawn->name()->c_str());

        //const auto pos = spawn->positional() != nullptr ? *(Vector3f*)&spawn->positional()->position() : Vector3f{};
        //auto ent = entityList->spawnEntity(spawn->name()->c_str(), spawn->model(), pos);

        // Allows the client to spawn an entity.
        MidHooks::s_ignore_spawn = true;
        auto ent = entity_list->spawn_entity(params);
        MidHooks::s_ignore_spawn = false;

        if (ent != nullptr) {
            //ent->entity->setSuspend(false);

            spdlog::info(" Entity spawned @ {:x}", (uintptr_t)ent);
            auto new_network_ent = m_network_entities->add_entity(ent, packet->guid());

            if (new_network_ent != nullptr) {
                spdlog::info(" Network entity created");
            }
        } else {
            spdlog::error(" Failed to spawn entity");
        }
    }

    return true;
}

bool NierClient::handle_destroy_entity(const nier::EntityPacket* packet) {
    spdlog::debug("Destroy entity packet received");

    m_network_entities->remove_entity(packet->guid());

    return true;
}

bool NierClient::handle_entity_data(const nier::EntityPacket* packet) {
    spdlog::debug("Entity data packet received");

    const auto entity_data = flatbuffers::GetRoot<nier::EntityData>(packet->data()->data());
    m_network_entities->process_entity_data(packet->guid(), entity_data);

    return true;
}

bool NierClient::handle_entity_animation_start(const nier::EntityPacket* packet) {
    spdlog::debug("Entity animation start packet received");

    const auto guid = packet->guid();
    auto entity_networked = m_network_entities->get_network_entity_from_guid(guid);

    if (entity_networked == nullptr) {
        spdlog::debug(" (nullptr) Entity data packet received for unknown entity {}", guid);
        return false;
    }

    auto animation_data = flatbuffers::GetRoot<nier::AnimationStart>(packet->data()->data());
    auto npc = entity_networked->get_entity() != nullptr ? entity_networked->get_entity()->behavior : nullptr;

    if (npc != nullptr) {
        switch (animation_data->anim()) {
        case sdk::EAnimation::INVALID_CRASHES_GAME:
        case sdk::EAnimation::INVALID_CRASHES_GAME2:
        case sdk::EAnimation::INVALID_CRASHES_GAME3:
        case sdk::EAnimation::INVALID_CRASHES_GAME4:
            return true;
        default:
            if (npc) {
                npc->start_animation(animation_data->anim(), animation_data->variant(), animation_data->a3(), animation_data->a4());
            } else {
                spdlog::error(" Cannot start animation, npc is null");
            }
        }
    }

    return true;
}

bool NierClient::handle_player_data(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();

    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::debug("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::debug("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto player_data = flatbuffers::GetRoot<nier::PlayerData>(packet->data()->data());

    // Older builds sent NaN when the character's position broke (test 2026-10-14): keep the last good data.
    if (!is_finite(*(Vector3f*)&player_data->position()) || !std::isfinite(player_data->speed()) ||
        !std::isfinite(player_data->facing()) || !std::isfinite(player_data->facing2())) {
        if (m_stat_bad_player_data++ == 0) {
            spdlog::warn("[Coop] {} sent a position that is not a number, ignoring it", player_networked->get_name());
        }

        return false;
    }

    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

    if (npc != nullptr && is_local_character(npc)) {
        npc = nullptr;
    }

    // With smoothing, think() moves the puppet towards this position over the next frames. Snapping
    // here fought the puppet's own run animation: it ran ahead, got pulled back on every packet.
    player_networked->set_player_data(*player_data);

    if (npc != nullptr && !smooth_puppet_position) {
        npc->position() = remote_position(*player_networked);
    }

    return true;
}

bool NierClient::handle_animation_start(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();

    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::debug("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::debug("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto animation_data = flatbuffers::GetRoot<nier::AnimationStart>(packet->data()->data());
    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

    ++m_stat_anims_received;

    // The death states (TASKS 1.4.2): the puppet would die here and, being player controlled, call the Game Over of our
    // game (test 2026-10-23). Its player's death comes as ID_PLAYER_DIED instead.
    if (sdk::Pl0000::is_death_state(animation_data->anim())) {
        spdlog::info("[Death] {}'s character goes into death state 0x{:x} in their game, their puppet here doesn't",
            player_networked->get_name(), animation_data->anim());
        return true;
    }

    if (npc != nullptr) {
        switch (animation_data->anim()) {
        case sdk::EAnimation::INVALID_CRASHES_GAME:
        case sdk::EAnimation::INVALID_CRASHES_GAME2:
        case sdk::EAnimation::INVALID_CRASHES_GAME3:
        case sdk::EAnimation::INVALID_CRASHES_GAME4:
        case sdk::EAnimation::Light_Attack:
            return true;
        default:
            if (npc) {
                ++m_stat_anims_applied;
                npc->start_animation(animation_data->anim(), animation_data->variant(), animation_data->a3(), animation_data->a4());
            } else {
                spdlog::error("Cannot start animation, npc is null");
            }
        }
    }
    
    return true;
}

bool NierClient::handle_buttons(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();
    
    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::debug("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::debug("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto buttons = flatbuffers::GetRoot<nier::Buttons>(packet->data()->data());
    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

    ++m_stat_buttons_received;

    if (npc != nullptr && buttons->buttons() != nullptr && buttons->buttons()->size() >= sdk::Pl0000::EButtonIndex::INDEX_MAX) {
        const auto buttons_data = buttons->buttons()->data();

        // Normal path: MidHooks::on_processed_buttons applies one frame per game update of the puppet.
        {
            std::scoped_lock _{m_puppet_input_mutex};

            if (auto it = m_puppet_inputs.find(npc); it != m_puppet_inputs.end() && it->second.driven_by_hook) {
                auto& queue = it->second.buttons;
                ButtonFrame frame{};
                std::copy_n(buttons_data, frame.size(), frame.begin());
                queue.push_back(frame);

                // Lag spike: don't replay a long backlog, the remote player is already past it.
                while (queue.size() > 12) {
                    queue.pop_front();
                    ++m_stat_button_frames_dropped;
                }

                return true;
            }
        }

        // The hook hasn't reached this puppet yet: write the buttons right away, as before.
        const auto size_buttons = sizeof(regenny::CharacterController::buttons);
        memcpy(&npc->character_controller().buttons, buttons_data, size_buttons);

        for (uint32_t i = 0; i < sdk::Pl0000::EButtonIndex::INDEX_MAX; ++i) {
            auto& controller = npc->character_controller();

            if (buttons_data[i] > 0) {
                controller.held_flags |= (1 << i);
            }
        }
    }

    return true;
}

// Our story point, when it changes and every few seconds (a player who joins later gets it too).
void NierClient::send_story_state() {
    constexpr auto RESEND = std::chrono::seconds{5};
    const auto point = StoryWatcher::get().current();
    const auto now = std::chrono::steady_clock::now();

    const auto phase_requests = PhaseRequestGate::get().passed_requests();

    if (point == m_sent_story && phase_requests == m_sent_phase_requests && now < m_next_story_send) {
        return;
    }

    flatbuffers::FlatBufferBuilder builder(0);
    const auto phase = builder.CreateString(point.phase);
    const auto character = builder.CreateString(point.character);
    nier::StoryStateBuilder data_builder(builder);
    data_builder.add_phase(phase);
    data_builder.add_phase_id(point.phase_id);
    data_builder.add_scene_state_changes(point.scene_state_changes);
    data_builder.add_event(point.event);
    data_builder.add_route((uint8_t)point.route);
    data_builder.add_character(character);
    data_builder.add_phase_requests(phase_requests);
    builder.Finish(data_builder.Finish());

    send_packet(nier::PacketType_ID_STORY_STATE, builder.GetBufferPointer(), builder.GetSize());
    m_sent_story = point;
    m_sent_phase_requests = phase_requests;
    m_next_story_send = now + RESEND;
}

bool NierClient::handle_story_state(const nier::PlayerPacket* packet) {
    if (packet->guid() == m_guid || packet->data() == nullptr) {
        return false;
    }

    flatbuffers::Verifier verifier{packet->data()->data(), packet->data()->size()};

    if (!verifier.VerifyBuffer<nier::StoryState>(nullptr)) {
        spdlog::error("[Story] Invalid story state from {}", packet->guid());
        return false;
    }

    const auto data = flatbuffers::GetRoot<nier::StoryState>(packet->data()->data());

    std::scoped_lock _{m_players_mutex};
    const auto it = m_players.find(packet->guid());

    if (it == m_players.end() || it->second == nullptr) {
        return false;
    }

    auto& player = *it->second;
    Player::Story story{};
    story.phase = data->phase() != nullptr ? data->phase()->str() : "";
    story.phase_id = data->phase_id();
    story.scene_state_changes = data->scene_state_changes();
    story.event = data->event();
    story.route = (char)data->route();
    story.character = data->character() != nullptr ? data->character()->str() : "";
    story.phase_requests = data->phase_requests();
    story.known = true;

    if (story.event != player.get_story().event) {
        if (story.event != 0) {
            spdlog::info("[Story] {}: cutscene ev{:04x} started", player.get_name(), story.event);
        } else {
            spdlog::info("[Story] {}: cutscene ev{:04x} ended", player.get_name(), player.get_story().event);
        }
    }

    if (story.route != player.get_story().route || story.character != player.get_story().character) {
        const auto ours = StoryWatcher::get().current();
        const auto route_name = [](char route) { return route != 0 ? std::string{"route "} + route : std::string{"unknown route"}; };
        spdlog::info("[Story] {} is on {} ({}), we are on {} ({}){}", player.get_name(), route_name(story.route),
            story.character.empty() ? "?" : story.character, route_name(ours.route), ours.character.empty() ? "?" : ours.character,
            story.route == ours.route || story.route == 0 || ours.route == 0 ? "" : ": different routes");
    }

    if (story.phase != player.get_story().phase || story.phase_id != player.get_story().phase_id) {
        const auto ours = StoryWatcher::get().current();
        spdlog::info("[Story] {} is in phase {} ({}), we are in {} ({}){}", player.get_name(), story.phase, story.phase_id,
            ours.phase, ours.phase_id, story.phase == ours.phase ? "" : ": different phases");

        // A client came into our phase (joined in the middle of it or loaded it after us): it gets what we have
        // done in it so far (decision 2026-10-20).
        if (m_is_master_client && !story.phase.empty() && story.phase != player.get_story().phase && story.phase == ours.phase) {
            m_story_catch_up = player.get_name();
        }
    }

    player.set_story(story);
    return true;
}

bool NierClient::handle_story_action(const nier::PlayerPacket* packet) {
    // Only the story of the player who leads it.
    if (packet->guid() == m_guid || m_is_master_client || packet->data() == nullptr) {
        return false;
    }

    flatbuffers::Verifier verifier{packet->data()->data(), packet->data()->size()};

    if (!verifier.VerifyBuffer<nier::StoryAction>(nullptr)) {
        return false;
    }

    const auto data = flatbuffers::GetRoot<nier::StoryAction>(packet->data()->data());
    StoryActionSync::Finished finished{};
    finished.phase = data->phase() != nullptr ? data->phase()->str() : std::string{};
    finished.graph = data->graph();
    finished.action = data->action();
    finished.state = data->state();

    spdlog::info("[StoryAction] The host finished {:08x} of graph {:08x} with {:08x} ({}){}", finished.action, finished.graph, finished.state,
        finished.phase, share_story_actions ? "" : ", not following: shared story progress is off");

    if (share_story_actions) {
        StoryActionSync::get().on_remote_finished(finished);
    }

    return true;
}

bool NierClient::handle_enemy_died(const nier::PlayerPacket* packet) {
    if (packet->guid() == m_guid || packet->data() == nullptr || packet->data()->size() < sizeof(nier::EnemyDied)) {
        return false;
    }

    auto data = flatbuffers::GetRoot<nier::EnemyDied>(packet->data()->data());

    EnemyDeathSync::Death death{};
    death.model = data->model();
    death.spawn_position = *(Vector3f*)&data->spawn_position();
    death.position = *(Vector3f*)&data->position();

    std::scoped_lock _{m_remote_deaths_mutex};
    m_remote_deaths.push_back(death);

    return true;
}

void NierClient::update_puppet_inputs() {
    std::scoped_lock _{m_puppet_input_mutex};

    auto entity_list = sdk::EntityList::get();

    // Drop the puppets that are no longer bound, keep the queued buttons of the others.
    for (auto it = m_puppet_inputs.begin(); it != m_puppet_inputs.end();) {
        auto player = m_players.find(it->second.guid);

        if (player == m_players.end() || player->second == nullptr || player->second->get_entity() != it->first) {
            // Still alive (the player left, or the puppet changed): hand it back as it was.
            auto entity = entity_list != nullptr ? entity_list->get_by_handle(it->second.handle) : nullptr;

            if (entity != nullptr && entity->behavior == it->first) {
                restore_puppet(it->first, it->second);
            }

            it = m_puppet_inputs.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& [guid, player] : m_players) {
        if (player == nullptr || guid == m_guid) {
            continue;
        }

        auto npc = player->get_entity();

        if (npc == nullptr) {
            continue;
        }

        auto& input = m_puppet_inputs[npc];
        input.guid = guid;
        input.handle = player->get_handle();
        input.data = player->get_player_data();

        if (puppet_as_player) {
            make_puppet_player_controlled(npc, input);
        } else if (input.made_player) {
            restore_puppet(npc, input);
        }
    }
}

void NierClient::smooth_puppet_positions() {
    const auto now = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration<float>(now - m_last_smooth_time).count();
    m_last_smooth_time = now;

    // First frame, or back from a pause/loading screen.
    if (dt <= 0.0f || dt > 0.25f) {
        dt = 1.0f / 60.0f;
    }

    if (!smooth_puppet_position) {
        return;
    }

    // Farther than this the puppet is somewhere else entirely (teleport, cutscene, respawn): snap.
    constexpr float snap_distance = 5.0f;
    // Below this the puppet's own movement is close enough, leave it alone.
    constexpr float dead_zone = 0.02f;
    // How fast the remaining error goes away: about 15% of it per frame at 60 fps, gone in ~0.25 s.
    constexpr float correction_rate = 10.0f;

    const auto blend = 1.0f - std::exp(-correction_rate * dt);

    for (auto& [guid, player] : m_players) {
        if (player == nullptr || guid == m_guid || !player->has_player_data()) {
            continue;
        }

        // TASKS 1.3.13, decision 2026-10-21: in a flight the buddy is the game's and sits on its Flight Unit, which the
        // game's AI flies in formation; the unit flies to where the other player is (both games fly in the same frame
        // of the flight, smoke run 2026-10-22). The AI pulls it ~0.3 m a frame (smoothed it stayed 2 m off), so the
        // position is set every frame.
        if (const auto unit = follow_flight_units ? get_buddy_flight_unit() : nullptr; unit != nullptr || m_puppet_flight_unit != 0) {
            const auto handle = unit != nullptr ? unit->handle : 0;

            if (handle != m_puppet_flight_unit) {
                if (handle != 0) {
                    spdlog::info("[Coop] {} flies: the buddy's Flight Unit {:x} follows them", player->get_name(), handle);
                } else {
                    spdlog::info("[Coop] The flight is over, the buddy's Flight Unit {:x} is the game's again", m_puppet_flight_unit);
                }

                m_puppet_flight_unit = handle;
            }

            if (unit != nullptr) {
                if (const auto target = remote_position(*player); is_finite(target)) {
                    unit->behavior->position() = target;
                }

                continue;
            }
        }

        auto npc = player->get_entity();

        if (npc == nullptr || is_local_character(npc)) {
            continue;
        }

        // The puppet runs on its own from the network speed and facing (puppet_as_player), this only
        // removes the drift between that and the remote player. The target lags by the network latency,
        // so the puppet trails the remote player by that much instead of jumping back and forth.
        const auto target = remote_position(*player);

        auto& position = npc->position();
        const auto error = target - position;
        const auto distance = glm::length(error);

        m_stat_position_error_sum += distance;
        m_stat_position_error_max = std::max(m_stat_position_error_max, distance);
        ++m_stat_position_samples;

        // A NaN position never comes back by itself, the distance is NaN too.
        if (distance > snap_distance || !std::isfinite(distance)) {
            // Writing the position alone does not move the character's physics: the puppet stayed where it was
            // and ran around there for seconds, then ended up below the floor (smoke run 2026-10-21, the host
            // teleported to the Commander). The game's own teleport resets both, as the bot's goto does.
            npc->setPosRotResetHap(Vector4f{target, 1.0f}, glm::angleAxis(npc->facing(), glm::vec3{0.0f, 1.0f, 0.0f}));
            position = target;
            ++m_stat_position_snaps;
        } else if (distance > dead_zone) {
            position += error * blend;
        }
    }
}

sdk::Behavior* NierClient::get_ridden_flight_unit(sdk::Pl0000* npc) const {
    // Outside the Bunker only: in the hangar the units stand next to their pilots before the launch.
    if (m_bunker_ring || npc == nullptr) {
        return nullptr;
    }

    const auto entity_list = sdk::EntityList::get();
    const auto unit = entity_list != nullptr && npc->flight_unit_handle() != 0 ? entity_list->get_by_handle(npc->flight_unit_handle()) : nullptr;

    if (unit == nullptr || unit->behavior == nullptr || unit->behavior->model_index() != sdk::EModel::MODEL_FLIGHT_UNIT) {
        return nullptr;
    }

    // On it: in flight the puppet hangs about 1.1 m above its unit (smoke run 2026-10-22).
    return glm::length(unit->behavior->position() - npc->position()) < 3.0f ? unit->behavior : nullptr;
}

sdk::Entity* NierClient::get_buddy_flight_unit() const {
    const auto entity_list = sdk::EntityList::get();
    const auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (possessed == nullptr || possessed->behavior == nullptr || !possessed->behavior->is_pl0000() ||
        get_ridden_flight_unit(possessed->behavior->as<sdk::Pl0000>()) == nullptr) {
        return nullptr;
    }

    const auto buddy = entity_list->get_by_handle(possessed->behavior->as<sdk::Pl0000>()->buddy_handle());

    if (buddy == nullptr || buddy->behavior == nullptr || !buddy->behavior->is_pl0000() ||
        get_ridden_flight_unit(buddy->behavior->as<sdk::Pl0000>()) == nullptr) {
        return nullptr;
    }

    return entity_list->get_by_handle(buddy->behavior->as<sdk::Pl0000>()->flight_unit_handle());
}

Vector3f NierClient::remote_position(Player& player) const {
    const auto position = *(Vector3f*)&player.get_player_data().position();
    return m_bunker_ring ? m_bunker_ring->to_world(position) : position;
}

void NierClient::update_bunker_ring() {
    // Outside the Bunker the entity list is walked for the ring twice a second, not every frame.
    if (m_bunker_handle == 0 && m_bunker_search_frames++ % 30 != 0) {
        return;
    }

    const auto had_ring = m_bunker_ring.has_value();
    m_bunker_ring = sdk::BunkerRing::find(m_bunker_handle);

    if (m_bunker_ring.has_value() != had_ring) {
        if (m_bunker_ring) {
            spdlog::info("[Coop] In the Bunker: positions go over the network in the ring's frame (ring at {:.1f} deg)",
                glm::degrees(m_bunker_ring->angle()));
        } else {
            spdlog::info("[Coop] Out of the Bunker: positions go over the network as they are");
        }
    }
}

void NierClient::send_bunker_ring() {
    if (!m_is_master_client || !m_bunker_ring) {
        return;
    }

    // While the ring turns every frame, and once a second while it stands, so a client that comes in later gets it.
    const auto now = std::chrono::steady_clock::now();
    const auto angle = m_bunker_ring->angle();

    if (std::abs(angle - m_sent_ring_angle) < 0.0005f && now - m_sent_ring_time < std::chrono::seconds{1}) {
        return;
    }

    m_sent_ring_angle = angle;
    m_sent_ring_time = now;

    nier::BunkerRing data{angle};
    flatbuffers::FlatBufferBuilder builder{};
    builder.Finish(builder.CreateStruct(data));
    send_packet(nier::PacketType_ID_BUNKER_RING, builder.GetBufferPointer(), builder.GetSize());
}

bool NierClient::handle_bunker_ring(const nier::PlayerPacket* packet) {
    if (packet->guid() == m_guid || m_is_master_client || packet->data() == nullptr || packet->data()->size() < sizeof(uint32_t) + sizeof(nier::BunkerRing)) {
        return false;
    }

    const auto angle = flatbuffers::GetRoot<nier::BunkerRing>(packet->data()->data())->angle();

    if (!std::isfinite(angle)) {
        return false;
    }

    m_host_ring_angle = angle;
    m_host_ring_time = std::chrono::steady_clock::now();
    return true;
}

void NierClient::follow_host_bunker_ring() {
    // In our game the host's puppet is the hero and turns our ring its own way: it ran around the Bunker and ended
    // up below the floor (smoke runs 2026-10-21). With the host's angle both games are the same Bunker.
    std::scoped_lock _{m_players_mutex};
    const auto host = get_host_player();
    const auto follow = is_connected() && m_welcome_received && !m_is_master_client && m_bunker_ring && m_host_ring_angle &&
        std::chrono::steady_clock::now() - m_host_ring_time < std::chrono::seconds{3} &&
        host != nullptr && host->get_story().phase == StoryWatcher::get().current().phase;

    if (follow != m_following_host_ring) {
        m_following_host_ring = follow;

        if (follow) {
            spdlog::info("[Coop] Turning our Bunker ring with the host's (ours {:.1f} deg, the host's {:.1f} deg)",
                glm::degrees(m_bunker_ring->angle()), glm::degrees(*m_host_ring_angle));
        } else {
            spdlog::info("[Coop] Our Bunker ring turns on its own again");
        }
    }

    BunkerRingHold::get().set_hold(follow);

    if (follow && sdk::BunkerRing::set_angle(m_bunker_handle, *m_host_ring_angle)) {
        m_bunker_ring = sdk::BunkerRing::find(m_bunker_handle);
    }
}

bool NierClient::is_death_shared(const Player& other) const {
    // Co-op concept 6 point 5 and 5.3: shared in routes A and B and at the start of route C, while 2B and 9S are
    // together; once one of us plays A2 (route C after the heroes part) each dies alone. Unknown story: shared.
    return StoryWatcher::get().current().character != "A2" && other.get_story().character != "A2";
}

void NierClient::watch_own_death() {
    const auto entity_list = sdk::EntityList::get();
    const auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (possessed == nullptr || possessed->behavior == nullptr || !possessed->behavior->is_pl0000()) {
        return;
    }

    const auto hero = possessed->behavior->as<sdk::Pl0000>();

    if (!hero->is_dying()) {
        m_own_death_seen = false;
        m_dying_for_partner = false;
        return;
    }

    if (m_own_death_seen) {
        return;
    }

    m_own_death_seen = true;

    if (m_dying_for_partner) {
        spdlog::info("[Death] Our character dies (state 0x{:x}) because the other player died", hero->state());
        return;
    }

    std::scoped_lock _{m_players_mutex};
    const Player* other{nullptr};

    for (const auto& [guid, player] : m_players) {
        if (guid != m_guid && player != nullptr) {
            other = player.get();
        }
    }

    if (other == nullptr) {
        spdlog::info("[Death] Our character died in {} (state 0x{:x}), nobody else is in the session", sdk::get_phase_name(), hero->state());
        return;
    }

    if (is_death_shared(*other)) {
        spdlog::info("[Death] Our character died in {} (state 0x{:x}): {} dies too (shared death)", sdk::get_phase_name(), hero->state(),
            other->get_name());
    } else {
        spdlog::info("[Death] Our character died in {} (state 0x{:x}): {} plays on (each dies alone, one of us plays A2)",
            sdk::get_phase_name(), hero->state(), other->get_name());
    }

    // Sent either way: the other game decides by the same rule, and the server log shows the death.
    nier::PlayerDied data{hero->state()};
    flatbuffers::FlatBufferBuilder builder{};
    builder.Finish(builder.CreateStruct(data));
    send_packet(nier::PacketType_ID_PLAYER_DIED, builder.GetBufferPointer(), builder.GetSize());
}

bool NierClient::handle_player_died(const nier::PlayerPacket* packet) {
    if (packet->guid() == m_guid || packet->data() == nullptr || packet->data()->size() < sizeof(uint32_t) + sizeof(nier::PlayerDied)) {
        return false;
    }

    const auto state = flatbuffers::GetRoot<nier::PlayerDied>(packet->data()->data())->state();

    std::scoped_lock _{m_players_mutex};
    const auto it = m_players.find(packet->guid());

    if (it == m_players.end() || it->second == nullptr) {
        return false;
    }

    spdlog::info("[Death] {} died in their game (state 0x{:x})", it->second->get_name(), state);
    m_partner_death = PartnerDeath{packet->guid(), state, std::chrono::steady_clock::now()};
    return true;
}

void NierClient::apply_partner_death() {
    std::scoped_lock _{m_players_mutex};

    if (!m_partner_death) {
        return;
    }

    const auto death = *m_partner_death;
    const auto it = m_players.find(death.guid);

    if (it == m_players.end() || it->second == nullptr) {
        spdlog::info("[Death] The player who died left the session, our character lives on");
        m_partner_death.reset();
        return;
    }

    const auto& other = *it->second;

    if (!is_death_shared(other)) {
        spdlog::info("[Death] {} died, we play on: each dies alone while one of us plays A2", other.get_name());
        m_partner_death.reset();
        return;
    }

    // Not in a loading or a cutscene: wait for the game to be playable, but not for long.
    if (std::chrono::steady_clock::now() - death.at > std::chrono::seconds{30}) {
        spdlog::warn("[Death] {} died 30 s ago and our character couldn't die since (loading or a cutscene), we play on", other.get_name());
        m_partner_death.reset();
        return;
    }

    if (sdk::is_loading() || StoryWatcher::get().current().event != 0) {
        return;
    }

    const auto entity_list = sdk::EntityList::get();
    const auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (possessed == nullptr || possessed->behavior == nullptr || !possessed->behavior->is_pl0000()) {
        return;
    }

    const auto hero = possessed->behavior->as<sdk::Pl0000>();
    m_partner_death.reset();

    if (hero->is_dying()) {
        spdlog::info("[Death] {} died, our character is dying already (state 0x{:x})", other.get_name(), hero->state());
        return;
    }

    spdlog::info("[Death] {} died: our character dies too (shared death)", other.get_name());
    m_dying_for_partner = true;
    hero->kill();
}

void NierClient::keep_puppet_alive(sdk::Pl0000* npc) {
    // Enemies of our game hit the puppet, but its player never got these hits. The game's own invulnerability timer is
    // kept up (setEventMuteki gives 300 frames), and the health stays full in case a hit gets through anyway.
    npc->muteki_timer() = std::max(npc->muteki_timer(), 300.0f);

    if (const auto max = npc->getHpMax(); max > 0 && npc->real_health() < max) {
        npc->real_health() = max;
        npc->health() = (uint32_t)max;
    }
}

bool NierClient::is_local_character(const sdk::Pl0000* npc) const {
    auto entity_list = sdk::EntityList::get();
    auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    return npc != nullptr && possessed != nullptr && possessed->behavior == npc;
}

void NierClient::stop_stale_puppets() {
    // Data comes 60 times a second. Without it the puppet would keep the last speed and buttons and run
    // on (test 2026-10-14: the other game froze, 9S ran into a wall for a minute).
    constexpr auto stale_after = std::chrono::milliseconds(1000);

    const auto now = std::chrono::steady_clock::now();

    for (auto& [guid, player] : m_players) {
        if (player == nullptr || guid == m_guid || !player->has_player_data()) {
            continue;
        }

        const auto stale = now - player->get_last_data_time() > stale_after;

        if (stale && !player->is_data_stale()) {
            spdlog::warn("[Coop] No data from {} for {} ms, stopping their character", player->get_name(),
                std::chrono::duration_cast<std::chrono::milliseconds>(now - player->get_last_data_time()).count());
            player->set_data_stale(true);
            player->stop_movement();
        } else if (!stale && player->is_data_stale()) {
            spdlog::info("[Coop] Data from {} again", player->get_name());
            player->set_data_stale(false);
        }
    }
}

void NierClient::watch_local_character() {
    auto it = m_players.find(m_guid);
    auto entity = it != m_players.end() && it->second != nullptr ? it->second->get_entity() : nullptr;

    if (entity == nullptr) {
        m_watched_handle = 0;
        return;
    }

    const auto handle = it->second->get_handle();
    auto& position = entity->position();
    const auto now = std::chrono::steady_clock::now();

    // Test 2026-10-14: the client's 9S got a NaN position while 9S was talking, the game looked frozen.
    // Put the character back where it last was.
    if (!is_finite(position)) {
        const auto can_restore = handle == m_watched_handle;

        if (now >= m_next_nan_log) {
            m_next_nan_log = now + std::chrono::seconds(1);
            spdlog::error("[Coop] Our character {:x} has a position that is not a number, {}", entity->model_index(),
                can_restore ? fmt::format("putting it back to ({:.1f} {:.1f} {:.1f})", m_watched_position.x,
                    m_watched_position.y, m_watched_position.z) : std::string{"no earlier position to put it back to"});
        }

        if (can_restore) {
            position = m_watched_position;
        }

        return;
    }

    // Test 2026-10-14: the client's character was pulled towards the host while standing still. Logs
    // every jump of our character, and where the other player's puppet is at that moment.
    constexpr float jump_distance = 3.0f;

    if (handle == m_watched_handle) {
        const auto jump = glm::length(position - m_watched_position);

        if (jump > jump_distance) {
            if (now >= m_next_jump_log) {
                m_next_jump_log = now + std::chrono::seconds(1);

                std::string puppets{};

                for (auto& [guid, player] : m_players) {
                    auto npc = player != nullptr && guid != m_guid ? player->get_entity() : nullptr;

                    if (npc != nullptr) {
                        puppets += fmt::format("; puppet of {} at ({:.1f} {:.1f} {:.1f}), {:.1f} m away", player->get_name(),
                            npc->position().x, npc->position().y, npc->position().z, glm::length(npc->position() - position));
                    }
                }

                spdlog::info("[Coop] Our character {:x} jumped {:.1f} m in one frame: ({:.1f} {:.1f} {:.1f}) -> ({:.1f} {:.1f} {:.1f}){}{}",
                    entity->model_index(), jump, m_watched_position.x, m_watched_position.y, m_watched_position.z,
                    position.x, position.y, position.z, puppets,
                    m_jumps_not_logged > 0 ? fmt::format(" ({} more jumps since the last line)", m_jumps_not_logged) : std::string{});
                m_jumps_not_logged = 0;
            } else {
                ++m_jumps_not_logged;
            }
        }
    }

    m_watched_handle = handle;
    m_watched_position = position;
}

void NierClient::make_puppet_player_controlled(sdk::Pl0000* npc, PuppetInput& input) {
    auto sources = npc->controller_input_sources();

    if (!input.made_player) {
        input.saved_player_controlled = npc->is_player_controlled();
        input.saved_control_flags = npc->control_flags();
        std::copy_n(sources, input.saved_input_sources.size(), input.saved_input_sources.begin());
        input.made_player = true;

        spdlog::info("[Anim] Puppet {:x}: player controlled {} -> 1, control flags {:x} -> {:x}, input sources {} {} {} -> off",
            (uintptr_t)npc, input.saved_player_controlled, input.saved_control_flags, input.saved_control_flags & ~0xdu,
            sources[0], sources[1], sources[2]);
    }

    // Same as the game's hero switch does for the new hero, so the controller update runs for the puppet.
    // Every input source stays off: the puppet must not read our gamepad, the network input is written
    // over the result in apply_puppet_input/apply_puppet_movement. Enforced every frame, the game or the
    // story may set them back.
    npc->is_player_controlled() = 1;
    npc->control_flags() &= ~0xdu;
    sources[0] = 0;
    sources[1] = 0;
    sources[2] = 0;
}

void NierClient::restore_puppet(sdk::Pl0000* npc, PuppetInput& input) {
    if (!input.made_player) {
        return;
    }

    input.made_player = false;

    // The game made it the player's character in the meantime: its flags are the game's now.
    auto entity_list = sdk::EntityList::get();
    auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

    if (possessed != nullptr && possessed->behavior == npc) {
        spdlog::info("[Anim] Puppet {:x} is our character now, not restoring its flags", (uintptr_t)npc);
        return;
    }

    spdlog::info("[Anim] Puppet {:x}: restoring player controlled {}, control flags {:x}", (uintptr_t)npc,
        input.saved_player_controlled, input.saved_control_flags);

    npc->is_player_controlled() = input.saved_player_controlled;
    npc->control_flags() = input.saved_control_flags;
    std::copy(input.saved_input_sources.begin(), input.saved_input_sources.end(), npc->controller_input_sources());
}

bool NierClient::apply_puppet_movement(sdk::Pl0000* entity) {
    std::scoped_lock _{m_puppet_input_mutex};

    auto it = m_puppet_inputs.find(entity);

    if (it == m_puppet_inputs.end()) {
        return false;
    }

    auto& input = it->second;

    if (!input.movement_logged) {
        input.movement_logged = true;
        spdlog::info("[Anim] Puppet {:x} now takes its movement from the game's stick update", (uintptr_t)entity);
    }

    // The game turns speed (stick magnitude) and facing into walk/run animations, as for the local player.
    const auto speed = input.data.speed();
    entity->speed() = speed;
    entity->speed_squared() = speed * speed;
    entity->facing2() = input.data.facing2();

    return true;
}

bool NierClient::apply_puppet_input(sdk::Pl0000* entity) {
    std::scoped_lock _{m_puppet_input_mutex};

    auto it = m_puppet_inputs.find(entity);

    if (it == m_puppet_inputs.end()) {
        return false;
    }

    auto& input = it->second;
    auto& controller = entity->character_controller();

    if (!input.driven_by_hook) {
        input.driven_by_hook = true;
        spdlog::info("[Anim] Puppet {:x} now takes its input from the game's button update", (uintptr_t)entity);
    }

    ++m_stat_hook_frames;

    // Movement: the game turns speed and facing into walk/run animations, as for the local player.
    controller.speed = input.data.speed();
    controller.facing = input.data.facing2();
    controller.held_flags = input.data.held_button_flags();

    // One remote frame of buttons per update, nothing pressed when the queue is empty: a press is
    // applied exactly once (attack, jump, dodge), a held button stays in held_flags.
    ButtonFrame frame{};

    if (!input.buttons.empty()) {
        frame = input.buttons.front();
        input.buttons.pop_front();
        ++m_stat_button_frames_applied;
    }

    for (size_t i = 0; i < frame.size(); ++i) {
        controller.buttons[i] = frame[i];

        if (frame[i] != 0) {
            controller.held_flags |= 1u << i;
        }
    }

    return true;
}

void NierClient::log_animation_stats() {
    const auto now = std::chrono::steady_clock::now();

    if (now < m_next_stats_log) {
        return;
    }

    m_next_stats_log = now + std::chrono::seconds(10);

    const auto anims_sent = m_stat_anims_sent.exchange(0);
    const auto anims_received = m_stat_anims_received.exchange(0);
    const auto anims_applied = m_stat_anims_applied.exchange(0);
    const auto buttons_sent = m_stat_buttons_sent.exchange(0);
    const auto buttons_received = m_stat_buttons_received.exchange(0);
    const auto frames_applied = m_stat_button_frames_applied.exchange(0);
    const auto frames_dropped = m_stat_button_frames_dropped.exchange(0);
    const auto hook_frames = m_stat_hook_frames.exchange(0);

    const auto position_samples = std::exchange(m_stat_position_samples, 0);
    const auto position_error_sum = std::exchange(m_stat_position_error_sum, 0.0f);
    const auto position_error_max = std::exchange(m_stat_position_error_max, 0.0f);
    const auto position_snaps = std::exchange(m_stat_position_snaps, 0);
    const auto bad_player_data = std::exchange(m_stat_bad_player_data, 0);

    if (anims_sent + anims_received + buttons_sent + buttons_received + hook_frames + position_samples == 0) {
        return;
    }

    size_t puppets = 0;
    {
        std::scoped_lock _{m_puppet_input_mutex};
        puppets = m_puppet_inputs.size();
    }

    spdlog::info("[Anim] 10 s: sent anims {} buttons {}; received anims {} (applied {}) buttons {}; "
                 "puppets {}, puppet updates {}, button frames applied {} dropped {}; "
                 "position error avg {:.2f} m max {:.2f} m, snaps {}, NaN packets {}",
        anims_sent, buttons_sent, anims_received, anims_applied, buttons_received, puppets, hook_frames, frames_applied,
        frames_dropped, position_samples > 0 ? position_error_sum / position_samples : 0.0f, position_error_max,
        position_snaps, bad_player_data);
}
