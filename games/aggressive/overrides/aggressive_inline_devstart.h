#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // ZX_DEV_START_LEVEL (developers, tests): from boot straight into a
    // free skate level, through the game's own menus with presses of its
    // own (no control channel), waiting on the game's state as
    // games/aggressive/tools/routes.py does. Nothing is installed without it.
    void installDevStart(PS2Machine &runtime);
}
