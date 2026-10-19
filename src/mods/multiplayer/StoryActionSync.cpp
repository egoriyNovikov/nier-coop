#include <Windows.h>

#include <spdlog/spdlog.h>

#include <sdk/Game.hpp>
#include <sdk/Story.hpp>

#include "StoryActionSync.hpp"

namespace {
// How long an action has to be running in our graph before we finish it for the host: a command the graph has
// just started usually finishes by itself in a frame or two, and then does what it does (a line, a marker).
constexpr auto RUNNING_GRACE = std::chrono::milliseconds{500};
// The host is ahead: our graph may reach the action only after the earlier ones are finished.
constexpr auto WAIT_LIMIT = std::chrono::seconds{120};

bool readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};

    if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT
        || (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) == 0 || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    return (uintptr_t)address + size <= (uintptr_t)info.BaseAddress + info.RegionSize;
}
}

StoryActionSync& StoryActionSync::get() {
    static StoryActionSync sync{};
    return sync;
}

void StoryActionSync::install() {
    std::scoped_lock _{m_mutex};

    if (m_hook != nullptr) {
        return;
    }

    const auto function = sdk::get_action_state_function();

    if (function == nullptr) {
        spdlog::error("[StoryAction] The action state function wasn't found, story actions won't be shared");
        return;
    }

    m_hook = std::make_unique<FunctionHook>(function, &StoryActionSync::on_set_state);

    if (!m_hook->create()) {
        spdlog::error("[StoryAction] Failed to hook the action state function");
        m_hook.reset();
        return;
    }

    spdlog::info("[StoryAction] Hooked the action state function {}", function);
}

void StoryActionSync::update() {
    std::scoped_lock _{m_mutex};

    // The graphs go away with the world: forget them, they are seen again as their actions change state.
    const auto phase = sdk::get_phase_name();

    if (sdk::is_loading() || phase != m_phase) {
        m_graphs.clear();
        m_seen_finished.clear();
        m_phase = sdk::is_loading() ? std::string{} : phase;
        return;
    }

    note_finished(phase);

    const auto now = Clock::now();
    std::erase_if(m_pending, [&](Pending& pending) { return !advance(pending, phase, now); });
}

void StoryActionSync::note_finished(const std::string& phase) {
    for (auto it = m_graphs.begin(); it != m_graphs.end();) {
        const auto graph = get_graph(it->first);

        if (graph == nullptr) {
            it = m_graphs.erase(it);
            continue;
        }

        for (uint64_t i = 0; i < graph->count; ++i) {
            const auto action = graph->actions[i];

            if (!readable(action, sizeof(*action)) || action->graph != graph || (action->flags & sdk::Action::FINISHED) == 0) {
                continue;
            }

            if (!m_seen_finished.insert((uint64_t)graph->hash << 32 | action->id).second) {
                continue;
            }

            // Bounded: nobody takes them while we are not in a session.
            if (m_finished.size() >= 256) {
                m_finished.erase(m_finished.begin());
            }

            m_finished.push_back({phase, graph->hash, action->id, action->state});
        }

        ++it;
    }
}

bool StoryActionSync::advance(Pending& pending, const std::string& phase, Clock::time_point now) {
    const auto& f = pending.finished;

    if (f.phase != phase) {
        spdlog::info("[StoryAction] The host finished {:08x} of graph {:08x} in phase {}, we are in {}: skipped", f.action, f.graph, f.phase, phase);
        return false;
    }

    const auto action = find_action(f.graph, f.action);

    if (action == nullptr || (action->flags & sdk::Action::STARTED) == 0) {
        if (now - pending.received > WAIT_LIMIT) {
            spdlog::info("[StoryAction] Our graph never reached {:08x} of graph {:08x} the host finished: skipped", f.action, f.graph);
            return false;
        }

        return true;
    }

    if ((action->flags & sdk::Action::FINISHED) != 0) {
        return false;
    }

    if (pending.running_since == Clock::time_point{}) {
        pending.running_since = now;
    }

    if (now - pending.running_since < RUNNING_GRACE) {
        return true;
    }

    const auto changed = sdk::set_action_state(action, f.state != 0 ? f.state : sdk::ACTION_FINISHED);

    spdlog::info("[StoryAction] Finished {:08x} of graph {:08x} with {:08x} as the host did{}", f.action, f.graph, f.state,
        changed ? "" : " (no change)");
    return false;
}

sdk::ActionGraph* StoryActionSync::get_graph(uint32_t hash) {
    const auto it = m_graphs.find(hash);

    if (it == m_graphs.end()) {
        return nullptr;
    }

    const auto graph = it->second;

    if (!readable(graph, sizeof(*graph)) || graph->hash != hash || graph->count > 4096
        || !readable(graph->actions, graph->count * sizeof(void*))) {
        return nullptr;
    }

    return graph;
}

sdk::Action* StoryActionSync::find_action(uint32_t graph_hash, uint32_t id) {
    const auto graph = get_graph(graph_hash);

    if (graph == nullptr) {
        return nullptr;
    }

    for (uint64_t i = 0; i < graph->count; ++i) {
        const auto action = graph->actions[i];

        if (readable(action, sizeof(*action)) && action->graph == graph && action->id == id) {
            return action;
        }
    }

    return nullptr;
}

// Only learns the graphs: what finished is checked in update.
bool StoryActionSync::on_set_state(sdk::Action* action, void* state) {
    auto& self = get();
    const auto changed = self.m_hook->get_original<bool(sdk::Action*, void*)>()(action, state);

    if (changed && action != nullptr && action->graph != nullptr) {
        std::scoped_lock _{self.m_mutex};
        self.m_graphs[action->graph->hash] = action->graph;
    }

    return changed;
}

std::vector<StoryActionSync::Finished> StoryActionSync::take_finished() {
    std::scoped_lock _{m_mutex};
    std::vector<Finished> finished{};
    finished.swap(m_finished);
    return finished;
}

void StoryActionSync::on_remote_finished(const Finished& finished) {
    std::scoped_lock _{m_mutex};
    m_pending.push_back({finished, Clock::now()});
}
