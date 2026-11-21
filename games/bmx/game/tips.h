#pragma once
// BMX XXX's tips (game.c, tips.cpp): DM2's, on the overlay's hint display.

#include <cstdint>

namespace game::bmx
{
    // A player's tips for the frame: the lowest of the air's tips still to
    // show, then the others in turn until one is on the screen, while the
    // player has the tips on (PlayerSetup kSetupTips).
    void tipsFrame(uint32_t player);
}
