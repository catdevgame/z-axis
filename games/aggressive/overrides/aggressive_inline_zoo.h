#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The cut Zoo level in free skate (ZX_ZOO=1): the level select entry,
    // the level's files (the Xbox release's, made into the PS2's at build
    // time, when the app has them: aggressive_inline_converted.h),
    // extended memory for the game's heap.
    void installZoo(PS2Machine &runtime);
}
