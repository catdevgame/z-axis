#pragma once

// The park editor's object tools (edit_tool_object.cpp): their class.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tool_object.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/edit_tool_object.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    uint32_t pieceUnlocked(ee::Ptr<PieceDef> piece);
}
