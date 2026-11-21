#pragma once

// The game's save and load (game_save.cpp).

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace game::bmx
{
    // The game save's contents: the saved game and the saved records written into a save's
    // buffer (with each rider's model as it is now), and read back out of one into the game
    // (the riders' models and the volumes set from it; 0 for a buffer of another version).
    uint32_t writeGameSave(ee::Address buffer);
    uint32_t readGameSave(ee::Address buffer);
    // (game_save.cpp)
    int32_t freeParkSaveIcon();
    int32_t openSaveUi(uint32_t displayMode);
    int32_t playSaveShutdown();
    // The game's save client registered with the save system and its icon read (the game's start).
    void registerGameSaveClient();
    void shutdownGameSave();
    int32_t startGameAutoLoad();
    int32_t startGameLoad();
    int32_t startGameSave();
    int32_t startGameSilentLoad();
    int32_t stopParkSaveSystem();
}
