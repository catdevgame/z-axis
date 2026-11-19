#pragma once

// A game function outside the rider's module that the rider calls and that
// has no readable body on this line yet (action.c's rumble): called where it
// is, by its address's symbol, and a plain call of its owner's declaration
// when that lands.
// Scaffolding (address-call): docs/SCAFFOLDING.md

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/message.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/rider/game_calls.h.h"

namespace game::bmx::rider::calls
{
    // A rumble asked of a player's pad: which motor (0 the small one,
    // on or off; another the large one, by strength), which of the
    // game's timings, and the strength.
    struct Rumble
    {
        uint32_t motor;
        uint32_t timing;
        uint8_t strength;
    };
    static_assert(offsetof(Rumble, strength) == 8);
    inline void rumble(uint32_t player, Rumble *request) { ee::game(sym::rumble, player, request); }
}
