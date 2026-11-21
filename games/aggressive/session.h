#pragma once

// A level's session (session.cpp): what the rest of the game calls.

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace aggressive
{
    // (session.cpp)
    void endTimedRun();
    uint32_t loadSkaterDynamics(ee::Ptr<uint32_t> bytesOut);
}
