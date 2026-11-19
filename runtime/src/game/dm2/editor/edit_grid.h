// DM2's park editor: where the grid's cells are in the world, and the
// editor's pulse (the cursor's colours swing by it). DM2's own code, from
// the editor's files after ./src/com/editor/editGui.c.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/math/vec.h"

#include <cstdint>

namespace game::dm2
{
    // A cell of the grid is 5 units square.
    constexpr ee::Float kCellSize = 5.0f;
    constexpr ee::Float kHalfCell = 2.5f;

    // The centre of the cell in the world: 5 units a cell around the grid's
    // middle, y the cell's ground (its level, negated). The column and the
    // row are taken unsigned.
    void cellWorldPosition(ee::Ptr<ParkEditor> editor, uint32_t column, uint32_t row, ee::Ptr<zge::Vec4> position);

    // The pulse's frame counted; the cosine of the count times `rate` (65536
    // to a turn).
    void stepPulse();
    ee::Float pulseCosine(int32_t rate);
}
