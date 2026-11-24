#pragma once

// Aggressive Inline (SLUS-20327): ghost riding's rider (runtime/ghost_rider.h).

class PS2Machine;

namespace aggressive_inline
{
    // Ghost riding at the boot's asking (ZX_GHOST, ZX_GHOST_TRACK): player one's skater is the rider.
    void installGhost(PS2Machine &runtime);
}
