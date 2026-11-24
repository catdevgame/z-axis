#pragma once

// BMX XXX (SLUS-20415): ghost riding's rider (runtime/ghost_rider.h).

class PS2Machine;

namespace bmx_xxx
{
    // Ghost riding at the boot's asking (ZX_GHOST, ZX_GHOST_TRACK): player one's rider and bike are the rider.
    void installGhost(PS2Machine &runtime);
}
