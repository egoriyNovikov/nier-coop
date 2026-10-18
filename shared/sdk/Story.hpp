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
}
