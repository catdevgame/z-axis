#pragma once

// The park editor's start tool (edit_tool_start.cpp): its class.

#include "zge/editor/edit_records.h"
#include "ee/route.h"
#include "zge/math/types.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tool_start.cpp)
    int32_t registerEditStartToolClass();

    // The start tool's own part.
    struct EditStartToolPart
    {
        uint32_t pieceDef; // +0x00 the catalog's start object
    };
    static_assert(sizeof(EditStartToolPart) == 4);

    // (edit_tool_start.cpp, for the games' own parts)
    int32_t startToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading);
}
