#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive
{
    // (replay.cpp)
    void initReplay(uint32_t bytes);
    void replayAndPads(ee::Ptr<ee::Float> frameTime);
}
