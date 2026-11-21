// The particle effects' clock and the affectors' empty first word: AI's own
// code (its game block; DM2 (PS2) has the same words in its own, with two
// affectors where AI has five). AI's classes, emitters and affectors are
// particles.cpp.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/fx/effects_clock.h"

#include <cstdint>
#include "sym/aggressive/fx/effects_clock.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        // The effects' clock, float milliseconds (the game's effects frame
        // advances it).
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;
    }

    // effectsClock: the effects' clock, whole milliseconds.
    int32_t effectsClock()
    {
        return ee::toInt(*g_clockMs);
    }
    READABLE_FUNCTION(effectsClock, Verified);

    // affectorNothing: every affector object's first word (+0x00 this, +0x04
    // its function; AI's five affectors and DM2's two alike); empty, called
    // by nothing.
    void affectorNothing()
    {
    }
    READABLE_FUNCTION(affectorNothing, Verified);
}
