#pragma once
// DM2's tips (game.c): the hints a player with tips on is shown, each once,
// when the rider is doing what the tip is about.

#include <cstdint>

namespace game::dm2
{
    // The player's tips for the frame: while no tip is on the screen, the
    // first tip still to show whose moment has come is sent to the overlay.
    void tipsFrame(uint32_t player);
}
