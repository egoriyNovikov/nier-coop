#pragma once

#include <cstdint>
#include <string>

namespace sdk {
// Current story phase of the game, e.g. "16_AB_bunker_Order", "60_05_AB_Forest_Castle".
// Found 2026-10-18 on Steam build 7020666 (exe SHA-256 5171bed0...f5726a): a global { int32 id; char name[]; }.
// The same name is kept in a few more globals (+0x13ecc34, +0x13f8548, +0x145b6a8 from the exe base on that build).
// Empty string if the signature wasn't found.
std::string get_phase_name();
// The int32 right before the name. The game compares it with 0x100; meaning unknown.
int32_t get_phase_id();
// Story step inside the phase: an int32 global the game resets to -1 when a phase starts (+0x9c5a00) and
// raises as the story goes on. Found 2026-10-18 on build 7020666 (+0x14952c8): on the "Test" save in
// 16_AB_bunker_Order it is 130 and the conversation with the Commander makes it 140. The game writes it
// from the pending value set by set_phase_step() (+0x9480b6). -2 if not found.
int32_t get_phase_step();
// Moves the story step inside the phase, as the game does after a story conversation: the conversation's
// data holds the step it leads to and the game calls a setter with it (+0x8fae70 on 7020666, called from
// +0x3e37f4; +0x3d59a7 calls it with -1). The setter keeps the value as pending in the HUD manager
// (UIPlHudManager global +0xf68af0: [+0x650] value, [+0x654] flag; 0 and the current value are ignored),
// the HUD manager's update applies it on the next frame. False if the setter wasn't found.
bool set_phase_step(int32_t step);
// Changes the state of an action of the phase's script graph (a hap::Action) the way the game does
// (+0x356080 on 7020666): builds the state description { hash, state object looked up by the hash, param }
// and passes it to +0x3ec550, which stores the hash at [action+0x20], the param at [action+0x30], sets the
// "finished" flag (0x800000 in [action+0x1c]) for ACTION_FINISHED and notifies listeners of the action id.
// False if the function wasn't found or the action already had that state. Game thread only.
constexpr uint32_t ACTION_FINISHED = 0x26b80d1f;
bool set_action_state(void* action, uint32_t state_hash, int32_t param = 0);
// The function every state change of an action goes through (+0x3ec550 on 7020666):
// bool (hap::Action* action, const Description* state), true if the state changed. For hooks; nullptr if not found.
void* get_action_state_function();

// Layout of the phase's script graph, found 2026-10-19 on 7020666. A graph belongs to a script node named after
// the quest step (e.g. "M1010_S0030_GM" in 16_AB_bunker_Order) and holds its actions:
// graph [+0x18] uint32 hash (the same in every run), [+0x30] hap::Action** array, [+0x38] count;
// action [+0x8] graph, [+0x18] uint32 id (the same in every run), [+0x1c] flags, [+0x20] state hash.
struct ActionGraph {
    uint8_t unknown0[0x18];
    uint32_t hash;
    uint8_t unknown1c[0x30 - 0x1c];
    struct Action** actions;
    uint64_t count;
};

struct Action {
    // Flags: started (the graph reached it), finished (set together with the state ACTION_FINISHED).
    static constexpr uint32_t STARTED = 0x200000;
    static constexpr uint32_t FINISHED = 0x800000;

    void* vtable;
    ActionGraph* graph;
    void* unknown10;
    uint32_t id;
    uint32_t flags;
    uint32_t state;
};

// Route of the story: 'A', 'B' or 'C', 0 if unknown. The game keeps it as the scene state flags "Route/A",
// "Route/B", "Route/C" (Chapter Select sets one of them, build 7020666: +0x95cd65). Game thread only.
// system: the SceneStateSystem the game's scripts call (StoryWatcher takes it from SceneStateSystem.set).
char get_route(void* system);
// Hero of the route as the scene state flags "PL/2B", "PL/9S", "PL/A2" say: "2B", "9S", "A2", empty if none.
// Game thread only.
std::string get_story_character(void* system);
}
