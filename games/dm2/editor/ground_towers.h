// DM2's park editor: the ground's towers. The park's ground is built of
// towers, one node for every four cells; a tower is made on the cell with an
// odd column and an even row and shared with the cells at the column before
// and the row after. A tower whose four cells are level is the world object
// "Editobj_tower"; one whose cells differ is an "Editobj_towertag", its
// vertices moved to each cell's level.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    // The cell's tower set on the other three cells it covers.
    void shareCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor);

    // A flat tower made for the cell and shared; the same with the cell's
    // tower released first; and a sloped one in its place, put at the cell
    // (height 0).
    void newCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor);
    void renewCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor);
    void newSlopedCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor);

    // Whether the cell has a tower and it is a sloped one.
    uint32_t cellTowerIsSloped(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor);

    // The tower of the cell's four fitted to their levels (ground_towers.cpp).
    void fitCellTower(uint32_t column, uint32_t row, ee::Ptr<ParkEditor> editor);
    // A tower fitted on every tower cell the theme marks for one.
    void fitThemeTowers(ee::Ptr<ParkEditor> editor);

    // The collision face tree of every sloped tower freed (the tree's
    // address and size cleared in its collision frame).
    void freeSlopedTowerTrees(ee::Ptr<ParkEditor> editor);
}
