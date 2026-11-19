#pragma once

// Conversions as the games' compiled code makes them (shared by the
// readable files; were copies in AI's fx/billboard.h and
// skater/action_tricks.h; zgui's tape widget uses them too).

#include "ee/abi.h"
#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    // A word as the original turns it to a float, unsigned: GCC's
    // conversion takes one with its top bit set from its halves.
    inline ee::Float unsignedToFloat(uint32_t word)
    {
        ee::Float f;
        if (static_cast<int32_t>(word) >= 0)
            f = ee::toFloat(static_cast<int32_t>(word));
        else
        {
            const ee::Float half = ee::toFloat(static_cast<int32_t>((word & 1u) | (word >> 1)));
            f = half + half;
        }
        return f;
    }
    // (with two more words a caller's copy once passed: nothing reads them)
    inline ee::Float unsignedToFloat(uint32_t word, uint32_t, uint32_t) { return unsignedToFloat(word); }
}
