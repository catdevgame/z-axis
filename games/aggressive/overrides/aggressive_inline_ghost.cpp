// Aggressive Inline (SLUS-20327): ghost riding's rider (aggressive_inline_ghost.h).
//
// The rider is player one's skater: the level's first Player (games/aggressive/skater/skater.h, the level object
// g_topState +0x10 points to holds the players from its +0x10), whose scene object (Player::node, +0x14) is its
// skinned model's. The readable renderer's skinned draws of that model's node tree are the rider's
// (engine/zge/host/rider_ghost.cpp).

#include "aggressive_inline_ghost.h"
#include "sym/game_overrides/aggressive_inline_ghost.cpp.h"

#include "machine.h"
#include "runtime/ghost_rider.h"
#include "runtime/ps2_memory_map.h"

#include <cstdint>
#include <cstring>

namespace aggressive_inline
{
    namespace
    {
        constexpr uint32_t kTopState = sym::g_topState; // the top state (game_state.h TopState)
        constexpr uint32_t kTopStateLevel = 0x10u;      // TopState::level, the Level in a level
        constexpr uint32_t kLevelPlayers = 0x10u;       // Level: its first Player (skater.h)
        constexpr uint32_t kPlayerNode = 0x14u;         // Player::node, the skater's scene object

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            address &= 0x1FFFFFFFu;
            if (address == 0u || address > PS2_RAM_SIZE - 4u)
                return 0u;
            uint32_t value = 0;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        }

        size_t riderObjects(const uint8_t *rdram, uint32_t (&objects)[zx::ghost_rider::kMaxRiderObjects])
        {
            const uint32_t state = word(rdram, kTopState);
            const uint32_t level = state ? word(rdram, state + kTopStateLevel) : 0u;
            objects[0] = level ? word(rdram, level + kLevelPlayers + kPlayerNode) : 0u;
            return objects[0] ? 1u : 0u;
        }
    }

    void installGhost(PS2Machine &runtime) { zx::ghost_rider::install(runtime, &riderObjects); }
}
