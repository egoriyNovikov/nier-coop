#include <spdlog/spdlog.h>

#include <sdk/Story.hpp>
#include <utility/RTTI.hpp>

#include "StoryActionSync.hpp"
#include "StoryTalkGate.hpp"

namespace {
// The hint stays this long after a blocked talk.
constexpr auto HINT_TIME = std::chrono::seconds{4};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

StoryTalkGate& StoryTalkGate::get() {
    static StoryTalkGate gate{};
    return gate;
}

void StoryTalkGate::install() {
    if (m_hook != nullptr) {
        return;
    }

    const auto function = sdk::get_talk_function();

    if (function == nullptr) {
        spdlog::error("[Talk] The talk function wasn't found, the client can start story talks");
        return;
    }

    m_hook = std::make_unique<FunctionHook>(function, &StoryTalkGate::on_talk);

    if (!m_hook->create()) {
        spdlog::error("[Talk] Failed to hook the talk function");
        m_hook.reset();
        return;
    }

    spdlog::info("[Talk] Hooked the talk function {}", function);
}

bool StoryTalkGate::show_hint() const {
    return now_ms() < m_hint_until_ms;
}

void StoryTalkGate::on_talk(void* target) {
    auto& self = get();
    const auto original = self.m_hook->get_original<void(void*)>();

    if (target == nullptr) {
        return original(target);
    }

    const auto layout = *(const uint32_t*)((uintptr_t)target + sdk::LAYOUT_ID_OFFSET);
    const auto type = utility::rtti::get_type_info(target);
    const auto name = type != nullptr ? type->name() : "?";

    if (!self.m_follow) {
        spdlog::info("[Talk] Talking to {} (layout {:08x})", name, layout);
        return original(target);
    }

    const auto graph = StoryActionSync::get().find_objective_graph(layout);

    if (graph.empty()) {
        spdlog::info("[Talk] Client talks to {} (layout {:08x}): no objective points at it, a talk of our own", name, layout);
        return original(target);
    }

    ++self.m_blocked;
    self.m_hint_until_ms = now_ms() + std::chrono::duration_cast<std::chrono::milliseconds>(HINT_TIME).count();
    spdlog::info("[Talk] Client tried to talk to {} (layout {:08x}), the objective of graph \"{}\": blocked, story talks are the host's",
        name, layout, graph);
}
