#pragma once

// The game's start and its records (game_start.cpp): what the rest of the game calls.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace aggressive
{
    // (game_start.cpp)
    int32_t initGame();
    int32_t recordBestScore(uint32_t row, uint32_t level, uint32_t value, ee::Address name);
    void setPendingStateAction(uint32_t nextState);
    int32_t setPendingStateDelay(uint32_t frames);
    int32_t shutdownGame();
    uint32_t summaryScreenEntry(uint32_t index);
    int32_t switchGameState(uint32_t state);
    int32_t updateLevelRecord(uint32_t row, uint32_t level, ee::Address name, ee::Float value);
}
