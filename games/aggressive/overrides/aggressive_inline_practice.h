#pragma once

#include <cstdint>

class PS2Machine;
struct R5900Context;

// Aggressive Inline's restart point (Practice: engine/overrides/zge_practice.h).
namespace aggressive_inline::practice
{
    // VBlank in a level being played: the practice_point and Restart keys
    // (`restartPressed` only when a restart would be taken). True when the
    // Restart key was taken by Practice. Out of the level, or in a level
    // loaded again, the point is dropped.
    bool onVsync(uint8_t *rdram, bool pointPressed, bool restartPressed);

    // The level's update (game mode 2, `frameSeconds` its time): a pending
    // point or return runs here; true if the return ran in place of the
    // update (the caller returns without updating).
    bool runPending(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, float frameSeconds);
}
