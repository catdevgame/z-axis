#pragma once

// The level's load (load.cpp): its names, scene, folder, scripts and object
// types, and the career's state of the level saved and put back.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::level
{
    // (load.cpp)
    void hookAnimationEvents();
    uint32_t levelFolderName(uint32_t level);
    int32_t levelFromName(ee::Address name);
    uint32_t levelSceneName(uint32_t level, uint8_t splitScreenScene);
    uint32_t limitSmallModelDistances();
    void loadLevelArchive(uint32_t folder);
    uint32_t loadLevelScene(uint32_t level);
    void loadLevelScripts(uint32_t folder, uint32_t file);
    uint32_t registerLevelObjectTypes();
    void restoreCareerLevel();
    void saveCareerLevel();
    void showLoadingProgress(ee::Float progress);
    void unhookAnimationEvents();
}

