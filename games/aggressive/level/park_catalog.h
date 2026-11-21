#pragma once

// The park editor's catalog of pieces (park_catalog.cpp): categories and
// piece definitions from the theme's piece file.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_catalog.h.

#include "aggressive/level/edit_records.h"
#include "zge/editor/park_catalog.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"

#include <cstdint>

namespace zge::editor
{
    void measurePieceDefs(uint32_t catalog, uint32_t cellRecord);
    uint32_t pieceUnlocked(ee::Ptr<PieceDef> pieceDef);
}

