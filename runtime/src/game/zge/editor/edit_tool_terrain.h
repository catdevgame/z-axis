#pragma once

// The park editor's terrain tool (edit_tool_terrain.cpp): its class.

#include "zge/editor/edit_records.h"
#include "ee/route.h"
#include "zge/math/types.h"
#include "zge/zgui/widget.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tool_terrain.cpp)
    int32_t registerEditTerrainToolClass();

    // The terrain tool's own part.
    struct EditTerrainToolPart
    {
        int32_t mode;   // +0x00 kTerrainMoving..kTerrainChosen
        ee::Float step; // +0x04 metres a press raises or lowers
    };
    static_assert(sizeof(EditTerrainToolPart) == 8);
}
