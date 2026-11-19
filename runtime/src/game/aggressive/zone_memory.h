#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/game.h"

#include <cstdint>

namespace aggressive
{
    // (zone_memory.cpp)
    int32_t allocateLockedBlock(uint32_t bytes);
    int32_t freeZoneMemory();
    int32_t newZoneMemory();
    int32_t releaseZoneBlock(ee::Address address);
    // The record pools (zone_memory.cpp): the engine's names, as the engine calls them too (zge/game.h).
    using zge::freePool;
    using zge::newPool;
    using zge::poolGive;
    using zge::poolTake;
}
