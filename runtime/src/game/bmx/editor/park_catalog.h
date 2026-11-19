#pragma once

// The park editor's catalog of pieces (park_catalog.cpp): categories and
// piece definitions from the theme's piece file.
// BMX XXX's own part of it; what both games have is zge/editor/park_catalog.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/park_catalog.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"

#include <cstdint>

namespace zge::editor
{
    void measurePieceDefs(uint32_t catalog, uint32_t cellRecord);
    uint32_t pieceDefCategory(uint32_t piece);
    uint32_t pieceDefSecretId(uint32_t piece);
}

