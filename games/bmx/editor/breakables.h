#pragma once

// The park's breakables: the pieces a test run can knock down, one record
// each in a pool of their own (park_cells.cpp makes and empties it,
// breakables.cpp walks it for the skater's effects).
// BMX XXX's own part of it; what both games have is zge/editor/breakables.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/breakables.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge::editor
{
    // (breakables.cpp)
    int32_t breakableObject(ee::Ptr<Breakable> breakable);
    int32_t breakableWhole(ee::Ptr<Breakable> breakable);
    ee::Ptr<Breakable> firstBreakable();
    ee::Ptr<Breakable> nextBreakable(ee::Ptr<Breakable> breakable);
    int32_t setBreakable(ee::Ptr<Breakable> breakable, uint32_t state);
}
