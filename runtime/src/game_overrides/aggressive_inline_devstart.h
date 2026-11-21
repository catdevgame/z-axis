#pragma once

class PS2Runtime;

namespace aggressive_inline
{
    // ZX_DEV_START_LEVEL (developers, tests): from boot straight into a
    // free skate level, through the game's own menus with presses of its
    // own (no control channel), waiting on the game's state as
    // tools/routes.py does. Nothing is installed without it.
    void installDevStart(PS2Runtime &runtime);
}
