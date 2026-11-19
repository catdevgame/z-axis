#pragma once

// The park editor's tool set (edit_tools.cpp): its tools, the one in use and
// those put aside, the editor's update and its end.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tools.h.

#include "aggressive/level/edit_records.h"
#include "zge/editor/edit_tools.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    void playToolSound(ee::Address part, uint32_t sound);
}

