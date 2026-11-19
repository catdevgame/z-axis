#pragma once

// The park editor's selection (class EditSelection, park_selection.cpp): the
// cursor, the rectangle dragged out from it and the pieces it holds.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_selection.h.

#include "aggressive/level/edit_records.h"
#include "zge/editor/park_selection.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    void cursorCell(uint32_t selection, ee::Ptr<CellIndex> cellOut);
    void cursorCellAngles(uint32_t selection, ee::Ptr<AngleWords> anglesOut);
    ee::Ptr<EditObjectPart> firstHeldPiece(uint32_t selection);
    int32_t pickUpSelectedPieces(uint32_t address);
    uint32_t selectionMoved(uint32_t selection);
    void selectionPosition(uint32_t selection, ee::Ptr<zge::Vec4> positionOut);
    void setSelectionOffset(uint32_t selection, ee::Ptr<zge::Vec4> offset);
}

