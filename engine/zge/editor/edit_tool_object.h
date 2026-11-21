#pragma once

// The park editor's object tools (edit_tool_object.cpp): their class.

#include "zge/editor/edit_records.h"
#include "ee/route.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tool_object.cpp)
    int32_t registerEditObjectToolClass();

    // The object tool's own part.
    struct EditObjectToolPart
    {
        uint32_t category;          // +0x00 of the catalog
        ee::Ptr<PieceDef> pieceDef; // +0x04 the piece the selection holds
    };
    static_assert(sizeof(EditObjectToolPart) == 8);
}
