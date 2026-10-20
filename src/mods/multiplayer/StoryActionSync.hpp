#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <utility/FunctionHook.hpp>

namespace sdk {
struct Action;
struct ActionGraph;
}

// Story progress inside a phase (TASKS 3.6.1, co-op concept 5.8: story interactions are the host's, the
// client sees the result). The story runs as script graphs of actions (hap::Action: conditions, commands,
// objectives, areas); a story conversation or trigger finishes an action and the graph goes on by itself.
// The host's game reports every action that finishes; the client's game finishes the same action (found by
// the graph's hash and the action's id, which are the same in both games) with the same state, once its own
// graph has reached it.
class StoryActionSync {
public:
    struct Finished {
        std::string phase{};
        uint32_t graph{};
        uint32_t action{};
        uint32_t state{};
        std::string name{}; // the graph's name, only for our log (Shift-JIS from the game's data, e.g. "M1010_S0030_GM")
    };

    static StoryActionSync& get();

    // Hooks the game. Before a save is loaded: the graph of a phase is learned from its actions' state changes,
    // and loading the world starts them.
    void install();

    // Game thread, every think. Forgets the graphs on loading screens and phase changes, notes the actions
    // of our graphs that finished, finishes the remote actions our graph has reached.
    void update();

    // Actions of our game that finished since the last call (the host sends them).
    std::vector<Finished> take_finished();

    // Every action of our graphs that is finished now, for a client that joins in the middle of the phase.
    std::vector<Finished> snapshot();

    // One line per known graph: hash, name, actions started / finished / all (for the test bot).
    std::vector<std::string> describe_graphs();

    // An action finished in the host's game.
    void on_remote_finished(const Finished& finished);

private:
    using Clock = std::chrono::steady_clock;

    struct Pending {
        Finished finished{};
        Clock::time_point received{};
        Clock::time_point running_since{}; // when we saw it started in our graph; zero until then
    };

    StoryActionSync() = default;

    static bool on_set_state(sdk::Action* action, void* state);
    // The graph if it is still there (a graph can end within the phase and its memory be reused).
    sdk::ActionGraph* get_graph(uint32_t hash);
    sdk::Action* find_action(uint32_t graph, uint32_t action);
    void note_finished(const std::string& phase);
    // False when the pending action is done with (finished here or dropped).
    bool advance(Pending& pending, const std::string& phase, Clock::time_point now);

    // Recursive: finishing an action ourselves goes through the hook.
    std::recursive_mutex m_mutex{};
    std::unique_ptr<FunctionHook> m_hook{};
    std::string m_phase{};
    std::unordered_map<uint32_t, sdk::ActionGraph*> m_graphs{}; // graph hash -> graph, from the state changes seen
    // Actions seen finished (graph hash << 32 | action id): each is reported once. The flag is checked in update,
    // not in the hook: some actions (AreaAction) get it outside the state change function.
    std::unordered_set<uint64_t> m_seen_finished{};
    std::vector<Finished> m_finished{};
    std::vector<Pending> m_pending{};
};
