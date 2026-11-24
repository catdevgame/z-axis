#pragma once

#include <cstdint>

class PS2Machine;
struct R5900Context;

// Dave Mirra Freestyle BMX 2's restart point (Practice: engine/overrides/
// zge_practice.h, the rider's point zge_practice_rider.h).
namespace dave_mirra::practice
{
    // VBlank: the practice_point and Restart keys (each only when pressed in
    // a level). True when the Restart key was taken by Practice. Out of the
    // level, the point is dropped.
    bool onVsync(uint8_t *rdram, bool inLevel, bool pointPressed, bool restartPressed);

    // The play mode's frame (actionFrame) while a turn runs and play is live:
    // a point made or gone back to; true when the return ran in place of it.
    bool playFrame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
