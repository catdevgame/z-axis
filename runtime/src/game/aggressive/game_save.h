#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::skater { struct Player; }

namespace aggressive
{
    // (game_save.cpp)
    int32_t freeParkSaveIcon();
    int32_t openSaveUi(uint32_t displayMode);
    int32_t playSaveShutdown();
    void registerGameSaveClient();
    void saveCareerProgress(ee::Ptr<skater::Player> player);
    uint32_t saveRegionAllocate(uint32_t bytes);
    uint32_t saveRegionFree(ee::Address memory);
    void shutdownGameSave();
    int32_t startGameAutoLoad();
    int32_t startGameLoad();
    int32_t startGameSave();
    int32_t stopParkSaveSystem();
}
