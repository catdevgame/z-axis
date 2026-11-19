// DM2's park editor: the copy tool's cells. The objects of the selected
// cells are copied into a square of 16 by 16 cells of the tool's own (each
// with a copy of the placed object's record and a node of its own), which
// the tool turns and pastes.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kCopySpan = 16; // the copied square's cells each way

    // The copied cells and the scratch they are turned through allocated
    // (the engine's heap), with no cell holding an object; freed again.
    void allocateCopyBuffers();
    void freeCopyBuffers();

    // Every copied object's node released and its record freed; nothing is
    // copied afterwards.
    void clearCopiedCells(ee::Ptr<ParkEditor> editor);

    // The objects whose own cell is in the selection copied into the
    // tool's square, each with a node of its own shown where it stood.
    void copySelectedCells(ee::Ptr<ParkEditor> editor);
    // A copy of every copied object placed on the park under the cursor's
    // rectangle; nothing at all when the park is too full for one of them.
    void pasteCopiedCells(ee::Ptr<ParkEditor> editor);
    // The cursor's rectangle and the copied square turned a quarter, and
    // again until the rectangle fits the park; nothing after four turns.
    void turnCopiedCells(ee::Ptr<ParkEditor> editor);

    // The copy tool's frame while the copy follows the cursor: every copied
    // object shown at its place under the cursor's rectangle, pulsing blue,
    // or red where its cells are not bare ground level with its own (the
    // cursor is blocked then).
    void showCopiedCellsAtCursor(ee::Ptr<ParkEditor> editor);
}
