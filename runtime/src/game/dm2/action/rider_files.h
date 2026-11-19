#pragma once

// The riders' and the bikes' data files (rider_files.cpp): read once and kept
// in memory for every rider set up after. DM2's own code (action.c).

#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/action/rider_files.h.h"

namespace game::dm2
{
    // "assets\riders\rider.zad" and "assets\riders\bike.zad" in memory (read
    // on the first call; 0 if the file is not there), with the file's bytes
    // to `bytesOut` unless it is 0.
    uint32_t riderDataFile(ee::Ptr<uint32_t> bytesOut);
    uint32_t bikeDataFile(ee::Ptr<uint32_t> bytesOut);
    // Each file's copy on the heap (0 until read; the game's end frees them).
    inline ee::Global<uint32_t, sym::g_riderDataFile> g_riderDataFile;
    inline ee::Global<uint32_t, sym::g_bikeDataFile> g_bikeDataFile;
}
