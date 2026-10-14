#include <array>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <SafetyHook.hpp>
#include <asmjit/asmjit.h>

#include <sdk/Entity.hpp>
#include <sdk/EntityList.hpp>
#include <sdk/Enums.hpp>
#include <sdk/ScriptFunctions.hpp>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/Input.hpp>
#include <utility/RTTI.hpp>

#include "Mods.hpp"
#include "mods/AutomataMPMod.hpp"
#include "AutomataMP.hpp"

#include "MidHooks.hpp"
#include "ScriptTracer.hpp"

using namespace std;

MidHooks* g_mid_hooks = nullptr;

namespace {
// Story script functions on Pl0000 that move characters between player, buddy and Flight Unit.
// Logged to see what the story does; the client also gives the story buddy back before a Flight Unit
// phase starts (see NierClient::on_story_script).
constexpr std::array k_traced_scripts{
    "Pl0000.changePlayer",
    "Pl0000.changePlayerFinal",
    "Pl0000.changePlayerFinalA2",
    "Pl0000.setPlayerFromNPC",
    "Pl0000.setBuddyFromNpc",
    "Pl0000.setNpcFromBuddy",
    "Pl0000.destroyBuddy",
    "Pl0000.startPhaseEventFlightUnit",
    "Pl0000.startPhaseEventFlightUnitNoWep",
    "Pl0000.endPhaseEventFlightUnit",
    "Pl0000.setBuddyFlightUnit",
    "Pl0000.clearGetOnFlightUnit",
    "Pl0000.initEm1100FlightBattle",
    "Pl0000.endEm1100FlightBattle",
    "Pl0000.initEm1100FinalFlightBattle",
    "Pl0000.endEm1100FinalFlightBattle",
    "Pl0000.initEm4000FlightBattle",
    "Pl0000.endEm4000FlightBattle",
};

template <size_t I>
void on_traced_script(safetyhook::Context& context) {
    AutomataMPMod::get()->on_story_script(k_traced_scripts[I], (sdk::Behavior*)context.rcx);
}
}

uintptr_t get_on_update_function() {
    static uintptr_t on_update_function = []() -> uintptr_t {
        spdlog::info("[MidHooks] Finding on_update_function...");

        const auto middle = utility::scan(utility::get_executable(), "? 89 ? e8 00 00 00 ? 89 ? f0 00 00 00 ? ? ? e8 ? ? ? ? ? ? ? ? ? 00 00");

        if (!middle) {
            spdlog::error("[MidHooks] Failed to find on_update_function.");
            return 0;
        }

        const auto int3s = utility::scan_reverse(*middle, 0x100, "CC CC CC");

        if (!int3s) {
            spdlog::error("[MidHooks] Failed to find int3s.");
            return 0;
        }

        const auto result = *int3s + 3;

        spdlog::info("[MidHooks] on_update_function: {:x}", result);

        return result;
    }();

    return on_update_function;
}

// gets an instruction near the end of the function that has an easy-ish to scan for pattern
uintptr_t get_on_processed_buttons() {
    static uintptr_t on_processed_buttons = []() -> uintptr_t {
        spdlog::info("[MidHooks] Finding on_processed_buttons...");

        const auto middle = utility::scan(utility::get_executable(), "C7 ? 90 07 00 00 14 00 00 00 48");

        if (!middle) {
            spdlog::error("[MidHooks] Failed to find on_processed_buttons.");
            return 0;
        }

        const auto result = *middle + 10;

        spdlog::info("[MidHooks] on_processed_buttons: {:x}", result);

        return result;
    }();

    return on_processed_buttons;
}

// near the top of ProcessButtons
uintptr_t get_on_set_held_flags() {
    static uintptr_t pre_process_buttons = []() -> uintptr_t {
        spdlog::info("[MidHooks] Finding on_set_held_flags...");

        const auto middle = get_on_processed_buttons();
        const auto ref = utility::scan_reverse(middle, 0x1000, "CC CC CC");

        if (!ref) {
            spdlog::error("[MidHooks] Failed to find pre_process_buttons.");
            return 0;
        }

        const auto func = *ref + 3;

        spdlog::info("[MidHooks] pre_process_buttons: {:x}", func);

        const auto result = utility::scan_disasm(func, 0x100, "48 89 81 40 07 00 00");

        if (!result) {
            spdlog::error("[MidHooks] Failed to find on_set_held_Flags.");
            return 0;
        }

        spdlog::info("[MidHooks] on_set_held_flags: {:x}", *result);

        return *result;
    }();

    return pre_process_buttons;
}

// In the controller update (base + 0x500ee0), right after the stick update (base + 0x52ac90) that
// recomputes speed and facing. RBX is the character controller.
uintptr_t get_on_processed_stick() {
    static uintptr_t on_processed_stick = []() -> uintptr_t {
        spdlog::info("[MidHooks] Finding on_processed_stick...");

        // mov rcx, rbx; call ProcessButtons; mov rdx, rsi; mov rcx, rbx; call stick update; cmp [rip+x], 5
        const auto call_site = utility::scan(utility::get_executable(), "48 8B CB E8 ? ? ? ? 48 8B D6 48 8B CB E8 ? ? ? ? 83 3D ? ? ? ? 05");

        if (!call_site) {
            spdlog::error("[MidHooks] Failed to find on_processed_stick.");
            return 0;
        }

        const auto result = *call_site + 19;

        spdlog::info("[MidHooks] on_processed_stick: {:x}", result);

        return result;
    }();

    return on_processed_stick;
}

uintptr_t get_entity_terminate_fn() {
    static uintptr_t entity_terminate_fn = []() -> uintptr_t {
        spdlog::info("[MidHooks] Finding entity_terminate_fn...");

        const auto terminate = sdk::ScriptFunctions::get().find("Behavior.terminate");

        if (!terminate) {
            spdlog::error("[MidHooks] Failed to find entity_terminate_fn.");
            return 0;
        }

        const auto terminate_script_fn = terminate->function;

        spdlog::info("[MidHooks] entity_terminate_script_fn: {:x}", terminate_script_fn);

        const auto ref = utility::scan_disasm(terminate_script_fn, 0x100, "0F 85");

        if (!ref) {
            spdlog::error("[MidHooks] Failed to find entity_terminate_fn.");
            return 0;
        }

        const auto result = utility::calculate_absolute(*ref + 2);

        spdlog::info("[MidHooks] entity_terminate_fn: {:x}", result);

        return result;
    }();

    return entity_terminate_fn;
}

MidHooks::MidHooks() {
    spdlog::info("[MidHooks] Initializing...");

    g_mid_hooks = this;

    // required to allow button to be held down without resetting
    /*m_hook.hook(0x140262B22, [=](const VehHook::RuntimeInfo& info) {
        auto entity = Address(info.context->Rcx).get(-0xCA0).as<Entity*>();

        auto& player = AutomataMPMod::get()->getPlayers()[1];

        if (entity == player.get_entity()) {
            entity->getCharacterController()->heldFlags = player.get_player_data().heldButtonFlags;
        }
    });*/

    // Early version hooks
    /*add_hook(0x140263006, &MidHooks::on_processed_buttons);
    add_hook(0x1404F9AA0, &MidHooks::onPreEntitySpawn);
    add_hook(0x1404F9BE9, &MidHooks::onPostEntitySpawn);
    add_hook(0x1404F8DE0, &MidHooks::on_entity_terminate);
    add_hook(0x140519460, &MidHooks::on_update);*/

    /*add_hook(0x140263006, &MidHooks::on_processed_buttons);
    add_hook(0x1404F9AA0, &MidHooks::onPreEntitySpawn);
    add_hook(0x1404F9BE9, &MidHooks::onPostEntitySpawn);
    add_hook(0x1404F8DE0, &MidHooks::on_entity_terminate);*/

    const auto [spawn_fn, spawn_this] = sdk::EntityList::get_spawn_entity_fn();
    m_entity_spawn_hook = add_inline_hook((uintptr_t)spawn_fn, &MidHooks::entity_spawn_hook);
    add_hook(get_entity_terminate_fn(), &MidHooks::on_entity_terminate);
    // todo: hook the other version of the terminate function (the script function)
    add_hook(get_on_update_function(), &MidHooks::on_update);
    add_hook(get_on_processed_buttons(), &MidHooks::on_processed_buttons);

    if (const auto stick = get_on_processed_stick(); stick != 0) {
        add_hook(stick, &MidHooks::on_processed_stick);
    }

    add_hook(get_on_set_held_flags(), [](safetyhook::Context& context) {
        auto entity = Address(context.rcx).get(-0xCA0).as<sdk::Pl0000*>();

        const auto& amp = AutomataMPMod::get();
        const auto& client = amp->get_client();

        if (client == nullptr) {
            return;
        }

        const auto& players = client->get_players();

        auto it = std::find_if(players.begin(), players.end(), [&](auto& player) {
            return player.second->get_entity() == entity;
        });

        if (it == players.end() || it->second == nullptr) {
            return;
        }

        if (entity == it->second->get_entity()) {
            entity->character_controller().held_flags = it->second->get_player_data().held_button_flags();
        }
    });

    [this]<size_t... I>(std::index_sequence<I...>) {
        (add_script_hook(k_traced_scripts[I], &on_traced_script<I>), ...);
    }(std::make_index_sequence<k_traced_scripts.size()>{});

    spdlog::info("[MidHooks] Initialized.");
}

void MidHooks::on_processed_buttons(safetyhook::Context& context) {   
    // OLD VERSION USES R8. THE REGISTER IS THE CHARACTER CONTROLLER OF THE ENTITY.
    //auto entity = Address(info.context->R8).get(-0xCA0).as<Entity*>();
    auto entity = Address(context.rbx).get(-0xCA0).as<sdk::Pl0000*>();

    // A remote player's puppet: the game just computed its buttons, speed and facing from the AI,
    // replace them with the network input before the character acts on them.
    if (const auto& client = AutomataMPMod::get()->get_client(); client != nullptr && client->apply_puppet_input(entity)) {
        return;
    }

    if (m_overriden_entities.count(entity) != 0) {
        // Only send the buttons of the character we control right now. After a player <-> buddy
        // switch the old character is still in the set but is driven by the network.
        auto entity_list = sdk::EntityList::get();
        auto possessed = entity_list != nullptr ? entity_list->get_possessed_entity() : nullptr;

        if (possessed == nullptr || possessed->behavior != entity) {
            return;
        }

        const auto amp = AutomataMPMod::get();
        auto& client = amp->get_client();

        if (client == nullptr) {
            return;
        }

        for (auto button : entity->character_controller().buttons) {
            if (button != 0) {
                client->send_buttons(entity->character_controller().buttons);
                break;
            }
        }
    }
}

void MidHooks::on_processed_stick(safetyhook::Context& context) {
    auto entity = Address(context.rbx).get(-0xCA0).as<sdk::Pl0000*>();

    if (const auto& client = AutomataMPMod::get()->get_client(); client != nullptr) {
        client->apply_puppet_movement(entity);
    }
}

sdk::Entity* MidHooks::on_entity_spawn(void* rcx, void* rdx) {
    // No lock may be held while calling into the game: when a cutscene starts the game spawns
    // from several threads and waits on its own locks inside the spawn function, a mutex held
    // across the call deadlocks the game. EntitySync has its own locking for the bookkeeping.
    auto spawnParams = (sdk::EntitySpawnParams*)rdx;
    auto entity = m_entity_spawn_hook->call<sdk::Entity*, void*, void*>(rcx, rdx);

    if (entity) {
        if (s_ignore_spawn) {
            return entity;
        }

        // The only allowed types that should be networked.
        // Other types are just too much for networking to handle
        // or are just completely unnecessary, and should only be
        // spawned client-side.
        if (entity->behavior->is_networkable()) {
            spdlog::info("[MidHooks] Spawned enemy: {}", spawnParams->name);
            AutomataMPMod::get()->on_entity_created(entity, spawnParams);
        }

        if (entity->behavior != nullptr && entity->behavior->model_index() == sdk::EModel::MODEL_FLIGHT_UNIT) {
            AutomataMPMod::get()->on_flight_unit_spawned(entity);
        }
    }

    return entity;
}

void MidHooks::on_entity_terminate(safetyhook::Context& context) {
    auto ent = (sdk::Entity*)context.rcx;

    AutomataMPMod::get()->on_entity_deleted(ent);
}

// Potential on_update candidates
/*
Function calls:
0x0000000140519460: 1
0x000000014051DCF0: 1
0x00000001401A35C0: 5
0x00000001405B6AC0: 5
0x00000001401A34F0: 5
0x00000001401A3440: 5
0x00000001401A3640: 5
0x00000001401A36C0: 5
0x00000001408E6730: 5
0x0000000140519470: 1
0x0000000140001060: 1
0x00000001408E6A10: 5
0x00000001408134B0: 5
0x00000001408082C0: 5
0x0000000140813520: 5
0x00000001408710A0: 5
0x00000001408710F0: 5
0x00000001408711A0: 5
0x00000001408E6900: 5
0x00000001408135F0: 5
0x0000000140938030: 5
*/

void MidHooks::on_update(safetyhook::Context& info) {
    auto entityList = sdk::EntityList::get();
    auto player = entityList->get_by_name("Player");

    if (!player) {
        return;
    }

    auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_think();
    }

    if (utility::was_key_down(VK_F1) && AutomataMPMod::get()->is_server()) {
        /*static std::unordered_map<uint32_t, uint32_t> models{ { EModel::MODEL_2B, EModel::MODEL_9S }, 
                                                              { EModel::MODEL_9S, EModel::MODEL_A2 }, 
                                                              { EModel::MODEL_A2, EModel::MODEL_2B }};

        auto ent = entityList->spawnEntity("Player", models[*player->entity->getModel()], *player->entity->getPosition());

        if (ent) {
            player->entity->setBuddyHandle(ent->handle);
            ent->entity->setSuspend(false);
            player->entity->changePlayer();
        }*/

        sdk::EntitySpawnParams params;
        sdk::EntitySpawnParams::PositionalData matrix;
        matrix.position = Vector4f{player->behavior->position(), 1.0f};
        matrix.unknown = *Address(player->behavior).get(0x90).as<Vector4f*>();
        params.name = "FreeEnemy";
        params.matrix = &matrix;
        params.model = 0x21020;
        params.model2 = 0x21020;

        auto test = entityList->spawn_entity(params);

        if (test) {
            test->behavior->setSuspend(false);
        }

        return;
    }

    if (utility::was_key_down(VK_F2)) {
        auto enemies = entityList->get_all_by_name("FreeEnemy");

        for (auto i : enemies) {
            if (!i->behavior) {
                continue;
            }

            i->behavior->terminate();
        }
    }
}

void MidHooks::add_overriden_entity(sdk::Behavior* ent) {
    m_overriden_entities.insert(ent);
}

void MidHooks::add_hook(uintptr_t address, MemberMidCallbackFn cb) {
    std::scoped_lock _{m_hook_mutex};

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    using namespace asmjit;
    using namespace asmjit::x86;

    //std::scoped_lock _{m_jit_mux};
    CodeHolder code{};
    code.init(m_jit.environment());

    Assembler a{&code};

    const void* realCb = (void*&)cb;

    a.mov(rdx, rcx);
    a.mov(rcx, this);
    a.mov(rax, realCb);
    a.jmp(rax);

    uintptr_t code_addr{};
    m_jit.add(&code_addr, &code);

    m_mid_hooks.emplace_back(builder.create_mid((void*)address, (safetyhook::MidHookFn)code_addr));
}

void MidHooks::add_script_hook(const char* name, safetyhook::MidHookFn cb) {
    const auto script = sdk::ScriptFunctions::get().find(name);

    if (script == nullptr || script->function == 0) {
        spdlog::error("[MidHooks] Script function {} not found, not traced", name);
        return;
    }

    add_hook(script->function, cb);
    ScriptTracer::get().exclude(script->function);
}

void MidHooks::add_hook(uintptr_t address, safetyhook::MidHookFn cb) {
    std::scoped_lock _{m_hook_mutex};

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    m_mid_hooks.emplace_back(builder.create_mid((void*)address, cb));
}

void MidHooks::add_hook(uintptr_t address, MemberInlineCallbackFn cb) {
    std::scoped_lock _{m_hook_mutex};

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    using namespace asmjit;
    using namespace asmjit::x86;

    //std::scoped_lock _{m_jit_mux};
    CodeHolder code{};
    code.init(m_jit.environment());

    Assembler a{&code};

    auto result = std::make_unique<HookAndParams>();

    const void* realCb = (void*&)cb;

    a.mov(rax, result.get());
    a.mov(ptr(rax), rcx);
    a.mov(ptr(rax, 8), rdx);
    a.mov(ptr(rax, 0x10), r8);
    a.mov(ptr(rax, 0x18), r9);
    a.mov(rcx, this);
    a.mov(rdx, result.get());
    a.mov(rax, realCb);
    a.jmp(rax);

    uintptr_t code_addr{};
    m_jit.add(&code_addr, &code);

    result->hook = builder.create_inline((void*)address, (void*)code_addr);
    m_inline_hooks_with_params.emplace_back(std::move(result));
}

safetyhook::InlineHook* MidHooks::add_inline_hook(uintptr_t address, void* cb) {
    std::scoped_lock _{m_hook_mutex};

    auto factory = SafetyHookFactory::init();
    auto builder = factory->acquire();

    m_inline_hooks.emplace_back(builder.create_inline((void*)address, cb));
    return m_inline_hooks.back().get();
}

sdk::Entity* MidHooks::entity_spawn_hook(void* rcx, void* rdx) {
    return g_mid_hooks->on_entity_spawn(rcx, rdx);
}
