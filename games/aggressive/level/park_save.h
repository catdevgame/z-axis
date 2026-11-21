#pragma once

// Saving a park to the memory card and loading one from it (park_save.cpp).

#include "aggressive/level/park_buffer.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::level
{
    // (park_save.cpp)
    void registerParkSaveClient();
    int32_t startParkLoad(ee::Ptr<ParkBuffer> buffer, uint32_t done);
    int32_t startParkSave(ee::Ptr<ParkBuffer> buffer, uint32_t done);
}
