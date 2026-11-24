#pragma once

// Dave Mirra Freestyle BMX 2 (SLUS-20159): ghost riding's rider (runtime/ghost_rider.h).

class PS2Machine;

namespace dave_mirra
{
    // Ghost riding at the boot's asking (ZX_GHOST, ZX_GHOST_TRACK): player one's rider and bike are the rider.
    void installGhost(PS2Machine &runtime);
}
