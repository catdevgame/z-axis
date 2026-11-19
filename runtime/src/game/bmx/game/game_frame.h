#pragma once
// BMX XXX's game frame (game.c, game_frame.cpp): what the game does each
// frame of play for the riders. DM2's line (game/dm2/game/game_frame.cpp),
// with the running turn's end made a frame of each mode's own.

#include <cstdint>

namespace game::bmx
{
    // The game's frame for the riders, then the round's clock. Returns the
    // round's phase.
    uint8_t gameRiderFrame();
    // A player's frame of the running turn's end, for the modes that share
    // it (all but the last three): the stages of RoundState::turnEndStage.
    void turnEndFrame(uint32_t player);
    // The game task's frame (gameSetup makes the task): the level
    // objectives' highlights drawn. Always 1.
    uint32_t gameTaskFrame();
}
