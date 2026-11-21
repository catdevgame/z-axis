#pragma once
// BMX XXX's game set up as a level starts and ended as it is left (game.c,
// game_setup.cpp). DM2's line (games/dm2/game/round.cpp: gameSetup,
// gameCleanup).

#include <cstdint>

namespace game::bmx
{
    // The empty hook gameSetup starts with: always 1.
    int32_t unusedGameSetupHook();
    // The game's set-up as a level starts: the round from no phase (a
    // session with one rider runs its turn from the start), each player's
    // replay and a copy of the rider's model, the round's replay, the HORSE
    // word, the game's records from the saved ones, the level's quarters,
    // the objectives, the texture effects, the riders put away, and the game
    // task.
    void gameSetup();
    // The game's end as a level is left: the game task, the texture effects
    // and the objectives ended; each player's rider gets its model back and
    // the replays are freed (the round's own only with two players or more:
    // one player's doubles as it).
    void gameCleanup();
}
