// DM2's park editor: the park's grid of cells (park_editor.h), what the
// theme allows on each.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    // The theme's kinds of a cell (themeCellKind's bits): the cell can be
    // built on.
    constexpr uint32_t kThemeCellBuildable = 0x01;
    // Scaffolding (unnamed-field): the theme's second bit: the cell's ground
    // gets a tower fitted when the theme is set up, and the terrain tool's
    // cursor is free there (blocked elsewhere). What the theme's file calls
    // it is not read yet (edit_themes).
    constexpr uint32_t kThemeCellBit1 = 0x02;

    // A cleared cell's level.
    constexpr int8_t kClearedCellLevel = 5;

    // The park's grid set up for a theme: the editor's scene entered with
    // the theme's file (once, until the cells are cleared), the grid sized
    // and every cell bare at the theme's level, the towers the theme asks
    // for fitted, the theme's lights collected and the theme made current.
    void setUpParkTheme(ee::Ptr<ParkEditor> editor, uint8_t theme);

    // Every cell's rails, object and tower removed, the grid emptied (no
    // rows, no columns, no theme) and the editor's scene left.
    void clearParkCells(ee::Ptr<ParkEditor> editor);

    // Every cell's rails and object removed (g_parkClearing set meanwhile:
    // the objects' nodes are left to the scene); the towers and the grid's
    // size stay.
    void clearParkObjects(ee::Ptr<ParkEditor> editor);

    // Whether (column, row) is a cell of the park.
    uint32_t cellInPark(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row);

    // Whether the cell is in the park and its theme lets it be built on.
    // With the terrain tool it also sets the cursor's state for the cell.
    uint32_t cellCanBeBuiltOn(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row);

    // Where a marker stands on a cell: the bare cell's centre on its ground,
    // or, with an object on the cell, half a unit under what a ray down
    // onto the object's own cell hits first (`node`, the marker itself, is
    // not hit).
    void cellMarkerPlace(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row, ee::Ptr<zge::Node> node, ee::Ptr<zge::Vec4> position);
}
