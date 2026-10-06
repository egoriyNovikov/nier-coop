#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <sdk/CameraGame.hpp>
#include <sdk/Enums.hpp>

#include "AutomataMP.hpp"

#include "schema/Packets_generated.h"
#include "mods/AutomataMPMod.hpp"
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

NierClient::NierClient(const std::string& host, const std::string& port, const std::string& name, const std::string& password)
    : m_hello_name{ name },
    m_password{ password }
{
    std::scoped_lock _{m_mtx};

    enetpp::global_state::get().deinitialize();
    enetpp::global_state::get().initialize();

    set_trace_handler([](const std::string& s) { spdlog::info("{}", s); });
    
    // Connecting is asynchronous: enetpp runs the connection on its own thread,
    // callers poll is_connecting()/is_connected().
    // The timeout is also the disconnect timeout, so keep it long enough for internet play.
    enet_uint16 port_num = static_cast<enet_uint16>(std::stoi(port));
    connect(enetpp::client_connect_params().set_channel_count(1).set_server_host_name_and_port(host.c_str(), port_num).set_timeout(chrono::seconds(10)));
}

NierClient::~NierClient() {
    std::scoped_lock _{m_mtx};
    disconnect();
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
    m_swap_done = false;
    m_swap_pending = false;
}

void NierClient::think() {
    std::scoped_lock _{m_mtx};

    m_in_game_thread = true;
    ScopeExit __{[this] { m_in_game_thread = false; }};

    m_in_world = true;

    pump();

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

    try_coop_swap();
    sync_puppets();

    if (m_players.contains(m_guid)) {
        update_local_player_data();
        send_player_data();

        // Synchronize the players.
        for (auto& it : m_players) {
            const auto& networked_player = it.second;

            // Do not update the local player here.
            if (networked_player == nullptr || networked_player->get_guid() == m_guid) {
                continue;
            }

            auto npc = networked_player->get_entity();

            // Not bound yet, sync_puppets will retry.
            if (npc == nullptr) {
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
        }

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
                        controlled->behavior->as<sdk::Pl0000>()->setPosRotResetHap(Vector4f{*(Vector3f*)&it.second->get_player_data().position(), 1.0f}, glm::identity<glm::quat>());
                    } else {
                        controlled->behavior->position() = *(Vector3f*)&it.second->get_player_data().position();
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
            m_is_master_client = true;
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
    spdlog::info("Player packet {} received from {}", nier::EnumNamePacketType(packet_type), packet->guid());

    switch (packet_type) {
    case nier::PacketType_ID_PLAYER_DATA: {
        if (!handle_player_data(packet)) {
            spdlog::error("Failed to handle player data");
        }

        break;
    }
    case nier::PacketType_ID_ANIMATION_START: {
        if (!handle_animation_start(packet)) {
            spdlog::error("Failed to handle animation start");
        }

        break;
    }
    case nier::PacketType_ID_BUTTONS: {
        if (!handle_buttons(packet)) {
            spdlog::error("Failed to handle buttons");
        }

        break;
    }
    default:
        spdlog::error("Unknown player packet type {} ({})", packet_type, nier::EnumNamePacketType(packet_type));
        break;
    }
}

void NierClient::on_entity_packet_received(nier::PacketType packet_type, const nier::EntityPacket* packet) {
    spdlog::info("Entity packet {} received from {}", nier::EnumNamePacketType(packet_type), packet->guid());

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
            spdlog::error("Failed to handle entity data");
        }

        break;
    }
    case nier::PacketType_ID_ENTITY_ANIMATION_START: {
        if (!handle_entity_animation_start(packet)) {
            spdlog::error("Failed to handle entity animation start");
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

void NierClient::send_animation_start(uint32_t anim, uint32_t variant, uint32_t a3, uint32_t a4) {
    nier::AnimationStart data{anim, variant, a3, a4};

    flatbuffers::FlatBufferBuilder builder(0);
    auto dataoffs = builder.CreateStruct(data);
    builder.Finish(dataoffs);

    send_packet(nier::PacketType_ID_ANIMATION_START, builder.GetBufferPointer(), builder.GetSize());
}

void NierClient::send_buttons(const uint32_t* buttons) {
    flatbuffers::FlatBufferBuilder builder(0);
    const auto dataoffs = builder.CreateVector(buttons, sdk::Pl0000::EButtonIndex::INDEX_MAX);

    nier::Buttons::Builder data_builder(builder);
    data_builder.add_buttons(dataoffs);
    builder.Finish(data_builder.Finish());

    send_packet(nier::PacketType_ID_BUTTONS, builder.GetBufferPointer(), builder.GetSize());
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
        spdlog::info("Not master client, not sending entity create");
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
        spdlog::info("Not master client, not sending entity destroy");
        return;
    }

    send_entity_packet(nier::PacketType_ID_DESTROY_ENTITY, guid);
}

void NierClient::send_entity_data(uint32_t guid, sdk::BehaviorAppBase* entity) {
    if (!m_is_master_client) {
        spdlog::info("Not master client, not sending entity data");
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
        spdlog::info("Not master client, not sending entity animation start");
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

    spdlog::info("[Coop] Hello: controlling model {:x}, announcing model {:x}", possessed->behavior->model_index(), model);

    flatbuffers::FlatBufferBuilder builder{};
    const auto name_pkt = builder.CreateString(m_hello_name);
    const auto pwd_pkt = builder.CreateString(m_password);

    nier::HelloBuilder hello_builder(builder);
    hello_builder.add_major(nier::VersionMajor_Value);
    hello_builder.add_minor(nier::VersionMinor_Value);
    hello_builder.add_patch(nier::VersionPatch_Value);
    hello_builder.add_name(name_pkt);
    hello_builder.add_password(pwd_pkt);
    hello_builder.add_model(model);

    builder.Finish(hello_builder.Finish());

    send_packet(nier::PacketType_ID_HELLO, builder.GetBufferPointer(), builder.GetSize());
    m_hello_sent = true;
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
    if (entity == nullptr) {
        return;
    }

    nier::PlayerData player_data(entity->flashlight(), entity->speed(), entity->facing(), entity->facing2(), entity->weapon_index(),
        entity->pod_index(), entity->character_controller().held_flags, *(nier::Vector3f*)&entity->position());

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
    const auto highest_guid = welcome->highestEntityGuid();

    spdlog::info("Welcome packet received, isMasterClient: {}, guid: {}", m_is_master_client, m_guid);

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

    spdlog::info(" Player {} ({}), model {:x}", create_player->guid(), create_player->name()->c_str(), create_player->model());

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

    // Releasing touches game entities, so it's done in think().
    if (it->second != nullptr && it->second->get_handle() != 0) {
        m_pending_destroy.push_back(std::move(it->second));
    }

    m_players.erase(it);
    m_next_bind_attempt.erase(destroy_player->guid());

    return true;
}

Player* NierClient::get_host_player() {
    // The server makes the first player to join the master client, and guids only grow,
    // so the remote player with the lowest guid is the host.
    Player* host = nullptr;

    for (auto& [guid, player] : m_players) {
        if (player == nullptr || guid == m_guid) {
            continue;
        }

        if (host == nullptr || guid < host->get_guid()) {
            host = player.get();
        }
    }

    return host;
}

void NierClient::try_coop_swap() {
    if (m_is_master_client || m_swap_done || !auto_swap_character) {
        return;
    }

    std::scoped_lock _{m_players_mutex};

    const auto host = get_host_player();

    if (host == nullptr) {
        return;
    }

    auto entity_list = sdk::EntityList::get();
    auto possessed = get_local_character(entity_list);

    // Wait until we control a character, the swap is decided once per world load.
    if (possessed == nullptr) {
        return;
    }

    m_swap_done = true;

    auto local = possessed->behavior->as<sdk::Pl0000>();

    if (local->model_index() != host->get_model()) {
        spdlog::info("[Coop] Already playing a different character than the host, no swap needed");
        return;
    }

    auto buddy = entity_list->get_by_handle(local->buddy_handle());

    if (buddy == nullptr || buddy == possessed || buddy->behavior == nullptr || !buddy->behavior->is_pl0000() ||
        !is_character_model(buddy->behavior->model_index()) || buddy->behavior->model_index() == local->model_index()) {
        spdlog::info("[Coop] Same character as the host but no story buddy to take over, staying on the current character");
        return;
    }

    spdlog::info("[Coop] Taking control of the story buddy {:x}", buddy->behavior->model_index());

    // The game's own player <-> buddy switch.
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

        if ((possessed != nullptr && possessed->handle != m_swap_from_handle) || ++m_swap_wait_thinks > 120) {
            m_swap_pending = false;
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
            if (!player->is_story_buddy()) {
                continue;
            }

            // The story decides where the buddy is: give it back once it's no longer our buddy
            // (story moved on, Flight Unit, hacking...) or the game suspended it.
            const auto buddy_handle = possessed != nullptr ? possessed->behavior->as<sdk::Pl0000>()->buddy_handle() : 0;
            const auto buddy_is_puppet = std::any_of(m_players.begin(), m_players.end(), [&](auto& it) {
                return it.second != nullptr && it.first != m_guid && it.second->get_handle() == buddy_handle;
            });

            if (possessed != nullptr && buddy_is_puppet && npc->isSuspend() == 0) {
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

    // Preferred: the buddy the game already gave us (9S next to 2B and so on).
    if (use_story_buddy) {
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
            const auto remote_position = *(Vector3f*)&player.get_player_data().position();

            if (glm::length(remote_position - buddy->behavior->position()) > 50.0f) {
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
    spdlog::info("Destroy entity packet received");

    m_network_entities->remove_entity(packet->guid());

    return true;
}

bool NierClient::handle_entity_data(const nier::EntityPacket* packet) {
    spdlog::info("Entity data packet received");

    const auto entity_data = flatbuffers::GetRoot<nier::EntityData>(packet->data()->data());
    m_network_entities->process_entity_data(packet->guid(), entity_data);

    return true;
}

bool NierClient::handle_entity_animation_start(const nier::EntityPacket* packet) {
    spdlog::info("Entity animation start packet received");

    const auto guid = packet->guid();
    auto entity_networked = m_network_entities->get_network_entity_from_guid(guid);

    if (entity_networked == nullptr) {
        spdlog::error(" (nullptr) Entity data packet received for unknown entity {}", guid);
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
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto player_data = flatbuffers::GetRoot<nier::PlayerData>(packet->data()->data());
    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

    if (npc != nullptr) {
        npc->position() = *(Vector3f*)&player_data->position();
    }

    player_networked->set_player_data(*player_data);

    return true;
}

bool NierClient::handle_animation_start(const nier::PlayerPacket* packet) {
    const auto guid = packet->guid();

    // do not update the local player. maybe change this later for forced updates/teleportation commands?
    if (guid == m_guid) {
        return true;
    }

    if (!m_players.contains(guid)) {
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto animation_data = flatbuffers::GetRoot<nier::AnimationStart>(packet->data()->data());
    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

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
        spdlog::error("Player data packet received for unknown player {}", guid);
        return false;
    }

    const auto& player_networked = m_players[guid];

    if (player_networked == nullptr) {
        spdlog::error("(nullptr) Player data packet received for unknown player {}", guid);
        return false;
    }

    auto buttons = flatbuffers::GetRoot<nier::Buttons>(packet->data()->data());
    // Outside the game thread we only keep the data, the next think() applies it.
    auto npc = can_touch_entities() ? player_networked->get_entity() : nullptr;

    if (npc != nullptr) {
        const auto buttons_data = buttons->buttons()->data();
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
