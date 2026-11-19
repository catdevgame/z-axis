#pragma once

// A park editor object's own part (class "EditObject", edit_object.cpp): its
// piece, matrices, anchors, display and flags, and the park's cost.

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include <cstddef>
#include "zge/editor/edit_records.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_object.cpp)
    ee::Ptr<EditObjectPart> cloneEditObject(ee::Ptr<EditObjectPart> part);
    void destroyEditPiece(ee::Address object);
    uint32_t editObjectAnchors(ee::Ptr<EditObjectPart> part);
    void editObjectCellOffset(ee::Ptr<EditObjectPart> part, ee::Ptr<CellIndex> offset);
    uint32_t editObjectDef(ee::Ptr<EditObjectPart> part);
    uint32_t editObjectDisplay(ee::Ptr<EditObjectPart> part);
    int32_t editObjectFlag(ee::Ptr<EditObjectPart> part, uint32_t index);
    void editObjectFootprint(ee::Ptr<EditObjectPart> part, ee::Ptr<CellIndex> footprint);
    ee::Ptr<zge::Matrix> editObjectMatrix(ee::Ptr<EditObjectPart> part);
    uint32_t editObjectParentAnchor(ee::Ptr<EditObjectPart> part);
    void editObjectWorldBox(ee::Ptr<EditObjectPart> part, uint32_t /*park: unread*/, ee::Ptr<zge::Box> box);
    uint32_t loadEditObject(ee::Ptr<EditObjectPart> part, uint32_t stream);
    int32_t makeEditObjectDisplay(ee::Address object, uint32_t copyFlags);
    void matrixPosition(ee::Ptr<zge::Matrix> matrix, ee::Ptr<zge::Vec4> position);
    int32_t parkCostAllowsMore();
    void releaseEditObjectDisplay(ee::Ptr<EditObjectPart> part);
    void saveEditObject(ee::Ptr<EditObjectPart> part, uint32_t stream);
    int32_t setEditObjectAnchors(ee::Ptr<EditObjectPart> part, uint32_t anchors);
    void setEditObjectDisplay(ee::Ptr<EditObjectPart> part, uint32_t display);
    void setEditObjectMethods(ee::Ptr<EditObjectPart> part, uint32_t methods);
    void setEditObjectPosition(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> position);
    void showEditObjectDisplay(ee::Ptr<EditObjectPart> part, uint32_t shown);
    void translateEditObject(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> move);
    void updateEditObjectMatrix(ee::Ptr<EditObjectPart> part);

    // A kind's methods (a plain piece's table, or a tower's), each taking
    // the object's EditObject part.
    struct EditObjectMethods
    {
        ee::FunctionPtr<void(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> move)> translate; // +0x00
        ee::FunctionPtr<void()> spare;                                     // +0x04 empty in both kinds
        ee::FunctionPtr<void()> spare2;                                    // +0x08 empty in both kinds
        ee::FunctionPtr<void(ee::Ptr<EditObjectPart> part, uint32_t stream)> loadExtra;      // +0x0C
        ee::FunctionPtr<void(ee::Ptr<EditObjectPart> part, uint32_t stream)> saveExtra;      // +0x10
        ee::FunctionPtr<void(ee::Ptr<EditObjectPart> part)> updateMatrix;                    // +0x14
        ee::FunctionPtr<int32_t(ee::Ptr<EditObjectPart> part, uint32_t index)> flagAt;       // +0x18
        ee::FunctionPtr<void(ee::Ptr<EditObjectPart> part)> releaseDisplay;                  // +0x1C
    };
    static_assert(sizeof(EditObjectMethods) == 0x20);

    // (edit_object.cpp, for the games' own parts)
    void removeParkCost(ee::Ptr<EditObjectPart> part);
}
