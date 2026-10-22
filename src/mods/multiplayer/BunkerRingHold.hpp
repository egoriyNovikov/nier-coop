#pragma once

#include <atomic>
#include <memory>

#include <utility/FunctionHook.hpp>

// In the client's game the host's puppet is the hero, and the game turns the Bunker's ring by the hero's steps
// (sdk::BunkerRing). The client keeps its ring at the host's angle instead (NierClient::follow_host_bunker_ring);
// while it does, the game's ring controller only keeps track of the hero, as when the story stops the ring, and does
// not turn the ring nor carry the puppet with it.
class BunkerRingHold {
public:
    static BunkerRingHold& get();

    void install();

    // Game thread. hold: the client follows the host's ring.
    void set_hold(bool hold) { m_hold = hold; }

private:
    BunkerRingHold() = default;

    static void on_update(void* controller);

    std::unique_ptr<FunctionHook> m_hook{};
    std::atomic<bool> m_hold{false};
    std::atomic<bool> m_logged{false};
};
