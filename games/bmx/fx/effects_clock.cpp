// The particle effects' clock and the affectors' empty first word: BMX XXX's own
// code (its game block), word for word Aggressive Inline's
// (games/aggressive/fx/effects_clock.cpp).

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/bmx/fx/effects_clock.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The effects' clock, float milliseconds (the game's effects frame
        // advances it).
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;
        // A frame: a sixtieth of a second.
        constexpr ee::Float kFrameMs = 16.666668f;
        // Frames the effects' second task has run.
        inline ee::Global<uint32_t, sym::g_effectsFrames> g_effectsFrames;
    }

    // effectsClock: the effects' clock, whole milliseconds.
    int32_t effectsClock()
    {
        return ee::toInt(*g_clockMs);
    }
    READABLE_FUNCTION(effectsClock, Verified);

    // advanceEffectsClock: the clock moved on by a frame (the effects' first
    // task). Returns 1.
    uint32_t advanceEffectsClock()
    {
        *g_clockMs = *g_clockMs + kFrameMs;
        return 1;
    }
    READABLE_FUNCTION(advanceEffectsClock, Verified);

    // countEffectsFrame: the effects' second task: a frame counted. Returns 1.
    uint32_t countEffectsFrame()
    {
        *g_effectsFrames = *g_effectsFrames + 1;
        return 1;
    }
    READABLE_FUNCTION(countEffectsFrame, Verified);

    // affectorNothing: every affector object's first word (+0x00 this, +0x04
    // its function); empty, called by nothing.
    void affectorNothing()
    {
    }
    READABLE_FUNCTION(affectorNothing, Draft);
}
