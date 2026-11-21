// DM2's park editor: the ground's towers (ground_towers.h). DM2's own code
// (its editor block).

#include "ground_towers.h"
#include "edit_themes.h"
#include "edit_grid.h"
#include "edit_display.h"
#include "edit_lights.h"

#include "park_grid.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/render/part.h"
#include "zge/assets/geo.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/dm2/editor/ground_towers.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kFlatTowerName{sym::textEditobjTower};      // "Editobj_tower"
        constexpr ee::Ptr<const char> kSlopedTowerName{sym::textEditobjTowerTag}; // "Editobj_towertag"

        // The flat tower the theme's set-up keeps as the model (park_grid.cpp).
        inline ee::Global<ee::Ptr<zge::Node>, sym::g_defaultTower> g_defaultTower;

        // fitCellTower's frame: the four cells' levels by the number the
        // tower's model gives each of its vertices (1 .. 4), and the
        // vertex being moved.
        struct FitTowerFrame
        {
            int8_t levels[5];   // +0x00 [0] is not used
            uint8_t _05[0x0B];
            zge::Vec4 place;    // +0x10
            zge::Vec4 vertex;   // +0x20
            zge::Vec4 uv;       // +0x30 x: which of the four cells the vertex belongs to
        };
        static_assert(offsetof(FitTowerFrame, place) == 0x10 && offsetof(FitTowerFrame, uv) == 0x30);

        ee::Ptr<ee::Ptr<zge::Node>> towerOf(ee::Ptr<ParkEditor> editor, int32_t column, int32_t row)
        {
            return &parkCell(editor, column, row)->tower;
        }

        // A tower stands on the cells with an odd column and an even row.
        bool isTowerCell(uint32_t column, uint32_t row)
        {
            return (column & 1) != 0 && (row & 1) == 0;
        }
    }

    void shareCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor)
    {
        const ee::Ptr<ee::Ptr<zge::Node>> tower = towerOf(editor, column, row);
        *towerOf(editor, column - 1, row) = *tower;
        *towerOf(editor, column - 1, row + 1) = *tower;
        *towerOf(editor, column, row + 1) = *tower;
    }
    READABLE_FUNCTION(shareCellTower, Verified);

    void newCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor)
    {
        *towerOf(editor, column, row) = newWorldObjectInstance(zge::nameHash(kFlatTowerName));
        game::tail(shareCellTower, column, row, editor);
    }
    READABLE_FUNCTION(newCellTower, Verified);

    void renewCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor)
    {
        releaseEditNode(towerOf(editor, column, row));
        *towerOf(editor, column, row) = newWorldObjectInstance(zge::nameHash(kFlatTowerName));
        game::tail(shareCellTower, column, row, editor);
    }
    READABLE_FUNCTION(renewCellTower, Verified);

    void newSlopedCellTower(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;

        releaseEditNode(towerOf(editor, column, row));
        const ee::Ptr<ee::Ptr<zge::Node>> tower = towerOf(editor, column, row);
        *tower = newWorldObjectInstance(zge::nameHash(kSlopedTowerName));
        cellWorldPosition(editor, column, row, position);
        position->y = 0.0f;
        setNodePosition(*tower, position);
        shareCellTower(column, row, editor);
    }
    READABLE_FUNCTION(newSlopedCellTower, Verified);

    uint32_t cellTowerIsSloped(int32_t column, int32_t row, ee::Ptr<ParkEditor> editor)
    {
        const ee::Ptr<zge::Node> tower = *towerOf(editor, column, row);
        if (!tower)
            return 0;
        const uint32_t towerName = zge::objectClass(tower)->nameHash;
        return towerName == zge::nameHash(kSlopedTowerName);
    }
    READABLE_FUNCTION(cellTowerIsSloped, Verified);

    void fitThemeTowers(ee::Ptr<ParkEditor> editor)
    {
        for (uint32_t row = 0; row < editor->rows; ++row)
        {
            for (uint32_t column = 0; column < editor->columns; ++column)
            {
                if (!isTowerCell(column, row))
                    continue;
                if (themeCellKind(editor->theme, static_cast<uint8_t>(column), static_cast<uint8_t>(row)) & kThemeCellBit1)
                    fitCellTower(column, row, editor);
            }
        }
    }
    READABLE_FUNCTION(fitThemeTowers, Verified);

    void freeSlopedTowerTrees(ee::Ptr<ParkEditor> editor)
    {
        for (uint32_t row = 0; row < editor->rows; ++row)
        {
            for (uint32_t column = 0; column < editor->columns; ++column)
            {
                if (!isTowerCell(column, row))
                    continue;
                if (!cellTowerIsSloped(column, row, editor))
                    continue;
                const ee::Ptr<zge::CollisionFrame> frame = zge::nodeCollisionFrame(*towerOf(editor, column, row));
                // The frame's tree and, in the word after it, the tree's size.
                const ee::Ptr<ee::Ptr<uint8_t>> tree = zge::frameTree(frame);
                if (*tree)
                {
                    zge::heapFree(*tree);
                    tree[1] = ee::Ptr<uint8_t>();
                    tree[0] = ee::Ptr<uint8_t>();
                }
            }
        }
    }
    READABLE_FUNCTION(freeSlopedTowerTrees, Verified);

    // The tower of the four cells the cell is one of (made if there is
    // none): a flat one at the cells' level when the four agree, else a
    // sloped one, each vertex of the flat model lowered to its own cell's
    // level; then lit.
    void fitCellTower(uint32_t column, uint32_t row, ee::Ptr<ParkEditor> editor)
    {
        ee::Local<FitTowerFrame> frame;
        const uint32_t towerColumn = (column & 1) ? column : column + 1;
        const uint32_t towerRow = (row & 1) ? row - 1 : row;
        const ee::Ptr<ee::Ptr<zge::Node>> tower = towerOf(editor, static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow));
        const auto levelAt = [&](uint32_t cellColumn, uint32_t cellRow) {
            return parkCell(editor, static_cast<int32_t>(cellColumn), static_cast<int32_t>(cellRow))->level;
        };
        frame->levels[4] = levelAt(towerColumn, towerRow);
        frame->levels[3] = levelAt(towerColumn, towerRow + 1);
        frame->levels[2] = levelAt(towerColumn - 1, towerRow);
        frame->levels[1] = levelAt(towerColumn - 1, towerRow + 1);
        if (!*tower)
            newCellTower(static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow), editor);
        if (frame->levels[1] == frame->levels[2] && frame->levels[1] == frame->levels[3] && frame->levels[1] == frame->levels[4])
        {
            cellWorldPosition(editor, towerColumn, towerRow, &frame->place);
            if (cellTowerIsSloped(static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow), editor))
                renewCellTower(static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow), editor);
            setNodePosition(*tower, &frame->place);
        }
        else
        {
            if (!cellTowerIsSloped(static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow), editor))
                newSlopedCellTower(static_cast<int32_t>(towerColumn), static_cast<int32_t>(towerRow), editor);
            const ee::Ptr<zge::ZgePart> model = (*g_defaultTower)->lods->part;
            const ee::Ptr<zge::ZgePart> mesh = (*tower)->lods->part;
            for (uint32_t vertex = 0; vertex < model->vertexCount; ++vertex)
            {
                zge::partUv(model, vertex, &frame->uv);
                zge::vertexPosition(model, vertex, &frame->vertex);
                const uint32_t cell = sce::libgcc::fptoui(frame->uv.x);
                frame->vertex.y = frame->vertex.y - ee::toFloat((*frame).levels[cell]);
                zge::setPartPosition(mesh, vertex, &frame->vertex);
            }
            zge::refreshNodeBoxAndSphere(*tower);
        }
        lightNodeVertices(*tower, 0);
    }
    READABLE_FUNCTION(fitCellTower, Verified);
}
