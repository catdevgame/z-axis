#pragma once

class PS2Machine;

namespace dave_mirra
{
    // ZX_DEV_START_LEVEL (developers, the collection's Custom Freeplay):
    // from boot straight into a FREERIDE level, through the game's own menus
    // with presses of its own, waiting on the game's state. Nothing is
    // installed without it (dave_mirra_devstart.cpp).
    void installDevStart(PS2Machine &runtime);
}
