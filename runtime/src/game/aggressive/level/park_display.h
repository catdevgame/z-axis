#pragma once

// What the park editor shows of a piece, and its saved views (park_display.cpp).
// Aggressive Inline's own part of it; what both games have is zge/editor/park_display.h.

#include "ee/plain.h"
#include "zge/editor/park_display.h"
#include "ee/readable.h"
#include "zge/math/box.h"
#include "zge/math/types.h"
#include "aggressive/level/edit_records.h"

#include <cstdint>

namespace zge::editor
{
    // (park_display.cpp)
    void editDisplayBox(uint32_t display, ee::Ptr<zge::Box> out);
    int32_t setDisplayPosition(uint32_t display, uint32_t position);
    int32_t wrapEditDisplay(uint32_t node);
}

