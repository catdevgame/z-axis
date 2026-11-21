// The particle effects' clock and the affectors' empty first word: DM2's own
// code (its game block), word for word Aggressive Inline's
// (games/aggressive/fx/effects_clock.cpp). DM2 has two affectors where AI has
// five.

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/dm2/fx/effects_clock.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The effects' clock, float milliseconds (the game's effects frame
        // advances it).
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;
        // A frame: a sixtieth of a second.
        constexpr ee::Float kFrameMs = 16.666668f;
    }

    // effectsClock: the effects' clock, whole milliseconds.
    int32_t effectsClock()
    {
        return ee::toInt(*g_clockMs);
    }
    READABLE_FUNCTION(effectsClock, Verified);

    // advanceEffectsClock: the clock moved on by a frame (the play's frames
    // call it).
    uint32_t advanceEffectsClock()
    {
        *g_clockMs = *g_clockMs + kFrameMs;
        return 1;
    }
    READABLE_FUNCTION(advanceEffectsClock, Verified);

    // affectorNothing: every affector object's first word (+0x00 this, +0x04
    // its function; DM2's two affectors alike); empty, called by nothing.
    void affectorNothing()
    {
    }
    READABLE_FUNCTION(affectorNothing, Draft);
}
