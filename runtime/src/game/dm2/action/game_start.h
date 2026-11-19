#pragma once

// The game's start-up (game_start.cpp): DM2's own code (action.c).

#include <cstdint>

namespace game::dm2
{
    // The game's start-up, once: the saved game made new (every character's
    // record from the game's tables, the settings' defaults, the players'
    // set-up), then the pads, the sound, the UI and the game's parts set up,
    // and the intro state asked for. Answers that state.
    uint8_t initGame();
}
