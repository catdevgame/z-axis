#pragma once
// BMX XXX's start and end as a game (bmx.c, game_start.cpp): what the
// engine's main runs before its loop and after it. BMX XXX's own code.

namespace game::bmx
{
    // The game's start: the loading picture shown, a new saved game made
    // (the records' defaults, every rider character, the movies open at the
    // start, the volumes, the language) and the two players' set-ups, the
    // engine's modules the game uses started, the level scripts' commands
    // registered, then the objectives, the pads, the UI and the
    // create-a-model module; the game goes to its intro state.
    void initGame();
    // The game's end: the state the game is in left, the rider characters'
    // models and the riders' animations freed, then every module of the
    // start shut down.
    void shutDownGame();
    // The rider characters' models: the player's model script, the
    // lattices, each character's model found, the parts' lock tables.
    void loadRiderModels();
}
