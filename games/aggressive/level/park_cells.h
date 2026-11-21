#pragma once

// The park editor's park (park_cells.cpp): its grid of cells and their anchors,
// its stream, the breakables' pool and the tool set's buttons.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_cells.h.

#include "aggressive/level/breakables.h"
#include "zge/editor/park_cells.h"
#include "aggressive/level/edit_records.h"
#include "zge/memory/pool.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    // (park_cells.cpp)
    ee::Ptr<zge::Pool> breakablePool();
    uint32_t breakableRecordObject(ee::Ptr<Breakable> breakable);
    uint32_t breakableRecordState(ee::Ptr<Breakable> breakable);
    void clearParkPieces(uint32_t park);
    uint32_t parkDepth(uint32_t park);
    uint32_t parkWidth(uint32_t park);
    int32_t restoreBreakables(uint32_t park, uint32_t catalog);
    void setBreakableRecordState(ee::Ptr<Breakable> breakable, uint32_t state);
}

