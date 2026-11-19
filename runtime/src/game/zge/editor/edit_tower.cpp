// The park editor's tower pieces (class "EditTowerObject"): the raised ground
// of a park. Four cells make one tower; the first of them (corner 1) holds the
// display they share, and each corner raises its own column of the display's
// vertices to its height. A tower moves only up and down between its limits.

#include "zge/editor/edit_tower.h"
#include "zge/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_messages.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_display.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tower.cpp.h"

namespace zge { ee::Ptr<CollisionFrame> nodeCollisionFrame(ee::Ptr<Node>); }
namespace zge { ee::Ptr<ee::Ptr<uint8_t>> frameTree(ee::Ptr<CollisionFrame>); }

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"
        constexpr uint32_t kEditTowerObjectClass = sym::g_editTowerObjectName; // "EditTowerObject"

        // The corner that holds the shared display, and a tower's corner when made.
        constexpr int32_t kDisplayCorner = 1, kNoCorner = -1;
    }

    void setTowerState(ee::Ptr<EditTowerObjectPart> tower, int32_t corner)
    {
        tower->corner = corner;
    }
    READABLE_FUNCTION(setTowerState, Verified);

    // The display goes with the corner that holds it; the others only let go.
    void destructEditTowerObject(uint32_t object)
    {
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(object, kEditTowerObjectClass));
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        if (tower->corner == kDisplayCorner && part->display != 0)
            freeShownDisplay(part->display);
        part->display = 0;
    }
    READABLE_FUNCTION_WAITS(destructEditTowerObject, Verified);

    // Its kind's update-matrix: the world matrix is its own under its parent
    // anchor's (or its own, hanging from nothing), and with a display its corner's
    // column stands at its height.
    void towerUpdateMatrix(uint32_t object)
    {
        const ee::Ptr<EditObjectPart> part(object);
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(object, kEditTowerObjectClass));
        const ee::Ptr<zge::Matrix> world(object + offsetof(EditObjectPart, matrix));
        if (part->parentAnchor != 0)
            zge::matrixMultiply(world, ee::Ptr<zge::Matrix>(static_cast<uint32_t>(anchorMatrix(part->parentAnchor))), ee::Ptr<zge::Matrix>(object));
        else
            *world = part->localMatrix;
        if (part->display != 0)
            game::tail(stretchTowerDisplay, ee::Ptr<EditDisplay>(static_cast<uint32_t>(part->display)), ee::Ptr<zge::Matrix>(object), tower->corner);
    }
    READABLE_FUNCTION_WAITS(towerUpdateMatrix, Verified);

    // Its kind's translate: the move is undone if it takes the tower out of its
    // limits.
    void towerTranslate(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> move)
    {
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(part, kEditTowerObjectClass));
        zge::Vec4 &position = part->localMatrix.translation();
        position += *move;
        const ee::Float y = position.y;
        if (tower->maxY < y || y < tower->minY)
            position -= *move;
    }
    READABLE_FUNCTION(towerTranslate, Verified);

    int32_t towerFlagAt(ee::Ptr<EditObjectPart> part, uint32_t index)
    {
        return part->flags[index];
    }
    READABLE_FUNCTION(towerFlagAt, Verified);

    void setTowerTop(ee::Ptr<EditTowerObjectPart> tower, ee::Float height)
    {
        tower->maxY = -height;
    }
    READABLE_FUNCTION(setTowerTop, Verified);

    void setTowerBottom(ee::Ptr<EditTowerObjectPart> tower, ee::Float height)
    {
        tower->minY = -height;
    }
    READABLE_FUNCTION(setTowerBottom, Verified);

    // A saved tower keeps only its height (its position's y).
    void towerLoadExtra(ee::Ptr<EditObjectPart> part, uint32_t stream)
    {
        parkStreamRead(stream, &part->localMatrix.translation().y, 4);
        game::tail(broadcastEditMessage, part, sym::makeMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(towerLoadExtra, Verified);

    void towerSaveExtra(ee::Ptr<EditObjectPart> part, uint32_t stream)
    {
        game::tail(parkStreamWrite, stream, &part->localMatrix.translation().y, 4);
    }
    READABLE_FUNCTION(towerSaveExtra, Verified);

    // Its kind's release-display: the corner that holds the display frees it.
    void towerReleaseDisplay(ee::Ptr<EditObjectPart> part)
    {
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(part, kEditTowerObjectClass));
        if (tower->corner == kDisplayCorner && part->display != 0)
            freeEditDisplay(part->display);
        part->display = 0;
    }
    READABLE_FUNCTION_WAITS(towerReleaseDisplay, Verified);

    // The shared display's collision tree goes: a tower collides as cells, not
    // as the display (only the corner that holds it).
    void freeTowerCollision(ee::Ptr<EditObjectPart> part)
    {
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(part, kEditTowerObjectClass));
        if (tower->corner != kDisplayCorner)
            return;
        const ee::Ptr<zge::CollisionFrame> frame = zge::nodeCollisionFrame(ee::Ptr<zge::Node>(editDisplayNode(part->display)));
        const ee::Ptr<ee::Ptr<uint8_t>> tree = zge::frameTree(frame); // the frame's tree field
        if (*tree == 0)
            return;
        zge::heapFree(*tree);
        frame->treeBytes = 0;
        frame->tree = ee::Ptr<uint8_t>(0u);
    }
    READABLE_FUNCTION_WAITS(freeTowerCollision, Verified);
}
