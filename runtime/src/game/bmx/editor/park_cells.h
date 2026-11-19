#pragma once

// The park editor's park (park_cells.cpp): its grid of cells and their anchors,
// its stream, the breakables' pool and the tool set's buttons.
// BMX XXX's own part of it; what both games have is zge/editor/park_cells.h.

#include "bmx/editor/breakables.h"
#include "zge/editor/park_cells.h"
#include "bmx/editor/edit_records.h"
#include "zge/memory/pool.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    void clearParkPieces(uint32_t park);
    void remakeParkDynamics(uint32_t park, uint32_t catalog);
}

