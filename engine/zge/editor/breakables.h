#pragma once

// The park's breakables: the pieces a test run can knock down, one record
// each in a pool of their own (park_cells.cpp makes and empties it; AI's
// breakables.cpp walks it for the skater's effects).

#include "zge/editor/edit_records.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge::editor
{
    struct Breakable
    {
        uint32_t state;          // +0x00 non-zero while it stands; 0 once knocked down (it is not put back)
        uint32_t piece;          // +0x04 its index in the catalog
        CellIndex cell;          // +0x08 the cell it sat on
        zge::EulerAngles angles; // +0x14 the anchor's
        uint32_t node;           // +0x1C what it showed
    };
    static_assert(sizeof(Breakable) == 0x20 && offsetof(Breakable, angles) == 0x14 && offsetof(Breakable, node) == 0x1C);
}
