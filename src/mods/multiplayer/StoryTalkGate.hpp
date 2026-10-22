#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <utility/FunctionHook.hpp>

// Story talks are the host's (TASKS 3.2.3, co-op concept 5.8: quest NPCs, choices in dialogs, quest starts - only the
// host; owner's choice 2026-10-22: block the quest and story ones, merchants and plain chatter stay with each player).
// In the client's game pressing the talk button next to an NPC that an objective of the story or of a side quest points
// at does nothing but show a hint: the conversation is the host's, its result comes over with the host's story actions
// (StoryActionSync). The game's "on talk" of the target (sdk::get_talk_function) is skipped for it.
class StoryTalkGate {
public:
    static StoryTalkGate& get();

    void install();

    // Game thread, every think: true while we are the client in a session that follows the host's story.
    void set_follow(bool follow) { m_follow = follow; }

    // Talks blocked since the game started (for the test bot).
    uint32_t blocked() const { return m_blocked; }

    // Render thread: whether the hint is to be on screen now.
    bool show_hint() const;

private:
    using Clock = std::chrono::steady_clock;

    StoryTalkGate() = default;

    static void on_talk(void* target);

    std::unique_ptr<FunctionHook> m_hook{};
    std::atomic<bool> m_follow{false};
    std::atomic<uint32_t> m_blocked{0};
    std::atomic<int64_t> m_hint_until_ms{0};
};
