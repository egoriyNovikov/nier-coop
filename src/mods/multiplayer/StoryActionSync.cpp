#include <cctype>
#include <cstring>

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

// Graphs that are not the main story: personal interactions (co-op concept 5.8: chests, gathering, shops, items -
// each player in their own game) and side quests (decision 2026-10-20: only the main story for now). The game names
// its graphs in Shift-JIS; these are the words seen 2026-10-20 on build 7020666 (16_AB_bunker_Order,
// 60_05_AB_Forest_Castle), as Shift-JIS bytes.
constexpr const char* PERSONAL_WORDS[] = {
    "\x95\xf3\x94\xa0", // 宝箱 chest
    "\x8d\xcc\x8e\xe6", // 採取 gathering point
    "\x83\x56\x83\x87\x83\x62\x83\x76", // ショップ shop
    "\x95\x90\x8a\xed\x89\xae", // 武器屋 weapon shop (not 屋 alone: 屋上 is a rooftop)
    "\x93\xb9\x8b\xef\x89\xae", // 道具屋 item shop
    "\x8e\xa9\x94\xcc\x8b\x40", // 自販機 vending machine
    "\x92\xde\x82\xe8", // 釣り fishing
    "\x8e\xc0\x90\xd1", // 実績 achievement
    "\x83\x41\x83\x43\x83\x65\x83\x80", // アイテム item
};

bool is_lead_byte(uint8_t c) {
    return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

// Why the host keeps the graph's actions to itself, nullptr if they are shared. Compares at character starts.
const char* why_not_shared(const std::string& name) {
    for (size_t i = 0; i < name.size(); i += is_lead_byte((uint8_t)name[i]) ? 2 : 1) {
        // A side quest: its graphs carry the quest id, e.g. 依頼者_q210 (the giver of quest q210).
        if (name[i] == 'q' && i + 3 < name.size() && isdigit((uint8_t)name[i + 1]) && isdigit((uint8_t)name[i + 2])
            && isdigit((uint8_t)name[i + 3])) {
            return "side quest";
        }

        for (const auto word : PERSONAL_WORDS) {
            if (name.compare(i, strlen(word), word) == 0) {
                return "personal";
            }
        }
    }

    return nullptr;
}

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

            const auto name = sdk::get_graph_name(graph);

            if (const auto why = why_not_shared(name); why != nullptr) {
                spdlog::info("[StoryAction] {:08x} of graph {:08x} \"{}\" finished, not shared: {}", action->id, graph->hash, name, why);
                continue;
            }

            // Bounded: nobody takes them while we are not in a session.
            if (m_finished.size() >= 256) {
                m_finished.erase(m_finished.begin());
            }

            m_finished.push_back({phase, graph->hash, action->id, action->state, name});
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

std::vector<StoryActionSync::Finished> StoryActionSync::snapshot() {
    std::scoped_lock _{m_mutex};
    std::vector<Finished> finished{};

    if (sdk::is_loading() || m_phase.empty()) {
        return finished;
    }

    for (const auto& [hash, _graph] : m_graphs) {
        const auto graph = get_graph(hash);
        const auto name = graph != nullptr ? sdk::get_graph_name(graph) : std::string{};

        if (graph == nullptr || why_not_shared(name) != nullptr) {
            continue;
        }

        for (uint64_t i = 0; i < graph->count; ++i) {
            const auto action = graph->actions[i];

            if (readable(action, sizeof(*action)) && action->graph == graph && (action->flags & sdk::Action::FINISHED) != 0) {
                finished.push_back({m_phase, graph->hash, action->id, action->state, name});
            }
        }
    }

    return finished;
}

std::vector<std::string> StoryActionSync::describe_graphs() {
    std::scoped_lock _{m_mutex};
    std::vector<std::string> lines{};

    for (const auto& [hash, _graph] : m_graphs) {
        const auto graph = get_graph(hash);

        if (graph == nullptr) {
            continue;
        }

        size_t started{}, finished{};

        for (uint64_t i = 0; i < graph->count; ++i) {
            const auto action = graph->actions[i];

            if (readable(action, sizeof(*action)) && action->graph == graph) {
                started += (action->flags & sdk::Action::STARTED) != 0;
                finished += (action->flags & sdk::Action::FINISHED) != 0;
            }
        }

        lines.push_back(fmt::format("graph {:08x} name={} started={} finished={} actions={} at={}", hash, sdk::get_graph_name(graph), started,
            finished, graph->count, (void*)graph));
    }

    return lines;
}

void StoryActionSync::on_remote_finished(const Finished& finished) {
    std::scoped_lock _{m_mutex};
    m_pending.push_back({finished, Clock::now()});
}
