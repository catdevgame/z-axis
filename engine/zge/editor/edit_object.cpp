// A park editor object (class "EditObject"): a piece of the park with its
// piece definition, where it stands (its own matrix under the anchor it hangs
// from, and in the world), its anchors that other pieces hang from, its display,
// and its kind's methods (a plain piece's or a tower's): moving, saving and
// loading what is its own, its world matrix, its flags and its display.
// The park's cost (what its pieces add up to, against a limit) is kept here too.

#include "zge/editor/edit_object.h"
#include "zge/editor/game.h"
#include "zge/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_display.h"
#include "zge/editor/park_messages.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_ui.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_object.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"
        constexpr uint32_t kEditTowerObjectClass = sym::g_editTowerObjectName; // "EditTowerObject"

        ee::Ptr<EditObjectMethods> methodsOf(ee::Ptr<EditObjectPart> part)
        {
            return ee::Ptr<EditObjectMethods>(static_cast<uint32_t>(part->methods));
        }

        int32_t setAnchorListOwnerTail(uint32_t anchors, ee::Ptr<EditObjectPart> part) { return ee::tailCall<int32_t>(sym::setAnchorListOwner, anchors, part); }
    }

    void addParkCost(ee::Ptr<EditObjectPart> part);
    void removeParkCost(ee::Ptr<EditObjectPart> part);

    uint32_t editObjectParentAnchor(ee::Ptr<EditObjectPart> part)
    {
        return static_cast<uint32_t>(part->parentAnchor);
    }
    READABLE_FUNCTION(editObjectParentAnchor, Verified);

    int32_t setEditObjectAnchors(ee::Ptr<EditObjectPart> part, uint32_t anchors)
    {
        part->anchors = static_cast<int32_t>(anchors);
        return setAnchorListOwnerTail(anchors, part);
    }
    READABLE_FUNCTION_WAITS(setEditObjectAnchors, Verified);

    uint32_t editObjectAnchors(ee::Ptr<EditObjectPart> part)
    {
        return static_cast<uint32_t>(part->anchors);
    }
    READABLE_FUNCTION(editObjectAnchors, Verified);

    uint32_t editObjectDef(ee::Ptr<EditObjectPart> part)
    {
        return static_cast<uint32_t>(part->pieceDef);
    }
    READABLE_FUNCTION(editObjectDef, Verified);

    void setEditObjectDef(ee::Ptr<EditObjectPart> part, uint32_t pieceDef)
    {
        part->pieceDef = static_cast<int32_t>(pieceDef);
    }
    READABLE_FUNCTION(setEditObjectDef, Verified);

    // Its piece's cell offset and footprint, in cells (x, y, z).
    void editObjectCellOffset(ee::Ptr<EditObjectPart> part, ee::Ptr<CellIndex> offset)
    {
        game::tail(pieceDefCellOffset, editObjectDef(part), &offset->x, &offset->y, &offset->z);
    }
    READABLE_FUNCTION(editObjectCellOffset, Verified);

    void editObjectFootprint(ee::Ptr<EditObjectPart> part, ee::Ptr<CellIndex> footprint)
    {
        game::tail(pieceDefFootprint, editObjectDef(part), &footprint->x, &footprint->y, &footprint->z);
    }
    READABLE_FUNCTION(editObjectFootprint, Verified);

    void setEditObjectMethods(ee::Ptr<EditObjectPart> part, uint32_t methods)
    {
        part->methods = static_cast<int32_t>(methods);
    }
    READABLE_FUNCTION(setEditObjectMethods, Verified);

    void setEditObjectDisplay(ee::Ptr<EditObjectPart> part, uint32_t display)
    {
        part->display = static_cast<int32_t>(display);
    }
    READABLE_FUNCTION(setEditObjectDisplay, Verified);

    uint32_t editObjectDisplay(ee::Ptr<EditObjectPart> part)
    {
        return static_cast<uint32_t>(part->display);
    }
    READABLE_FUNCTION(editObjectDisplay, Verified);

    // Its world matrix.
    ee::Ptr<zge::Matrix> editObjectMatrix(ee::Ptr<EditObjectPart> part)
    {
        return &part->matrix;
    }
    READABLE_FUNCTION(editObjectMatrix, Verified);

    // A new object of the same piece, with a display of its own.
    ee::Ptr<EditObjectPart> cloneEditObject(ee::Ptr<EditObjectPart> part)
    {
        const ee::Ptr<EditObjectPart> clone(newEditObject(pieceDefClassName(part->pieceDef), part->pieceDef));
        clone->display = static_cast<int32_t>(newEditDisplay(pieceDefObjectHash(clone->pieceDef), 0));
        return clone;
    }
    READABLE_FUNCTION_WAITS(cloneEditObject, Verified);

    // A display of its piece's object, placed at its world matrix.
    int32_t makeEditObjectDisplay(ee::Address object, uint32_t copyFlags)
    {
        const ee::Ptr<EditObjectPart> part(object);
        const uint32_t display = newEditDisplay(pieceDefObjectHash(part->pieceDef), copyFlags);
        part->display = static_cast<int32_t>(display);
        return game::tail(setDisplayMatrix, display, &part->matrix);
    }
    READABLE_FUNCTION_WAITS(makeEditObjectDisplay, Verified);

    // Flag `index` of the object, as its kind keeps them (kObjectHasHeight ...).
    int32_t editObjectFlag(ee::Ptr<EditObjectPart> part, uint32_t index)
    {
        return methodsOf(part)->flagAt(part, index);
    }
    READABLE_FUNCTION(editObjectFlag, Verified);

    void releaseEditObjectDisplay(ee::Ptr<EditObjectPart> part)
    {
        methodsOf(part)->releaseDisplay(part);
    }
    READABLE_FUNCTION(releaseEditObjectDisplay, Verified);

    // Moved by its kind's translate, and every piece on it hears so.
    void translateEditObject(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> move)
    {
        methodsOf(part)->translate(part, move);
        game::tail(broadcastEditMessage, part, sym::makeMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(translateEditObject, Verified);

    // Saved: its kind's own data, a word with a bit for each anchor that holds
    // a piece, and those anchors. Bug kept: the bits count only the anchors that
    // hold a piece (the first such anchor is bit 0, the next bit 1, ...), while
    // loadEditObject reads bit n as the object's n-th anchor; the two agree only
    // while the anchors that hold pieces come first.
    void saveEditObject(ee::Ptr<EditObjectPart> part, uint32_t stream)
    {
        ee::Local<uint32_t> holding;
        uint32_t bit = 0;
        methodsOf(part)->saveExtra(part, stream);
        *holding = 0;
        for (uint32_t anchor = firstAnchor(part->anchors); anchor != 0; anchor = nextAnchor(anchor))
        {
            if (anchorHoldsPiece(anchor) != 0)
            {
                *holding = *holding | 1u << (bit & 31u);
                ++bit;
            }
        }
        parkStreamWrite(stream, holding, 4);
        for (uint32_t anchor = firstAnchor(part->anchors); anchor != 0; anchor = nextAnchor(anchor))
        {
            if (anchorHoldsPiece(anchor) != 0)
                saveAnchor(anchor, stream);
        }
    }
    READABLE_FUNCTION_WAITS(saveEditObject, Verified);

    // Loaded: its kind's own data, then each anchor whose bit is set (bit n for
    // the n-th anchor: see saveEditObject). Returns the word of bits, or 0 when
    // the object has no anchors.
    uint32_t loadEditObject(ee::Ptr<EditObjectPart> part, uint32_t stream)
    {
        ee::Local<uint32_t> holding;
        methodsOf(part)->loadExtra(part, stream);
        parkStreamRead(stream, holding, 4);
        if (*holding == 0)
            return 0;
        uint32_t anchor = firstAnchor(part->anchors);
        if (anchor == 0)
            return 0;
        for (uint32_t bit = 1; anchor != 0; anchor = nextAnchor(anchor), bit <<= 1)
        {
            if ((*holding & bit) != 0)
                loadAnchor(anchor, stream);
        }
        return *holding;
    }
    READABLE_FUNCTION_WAITS(loadEditObject, Verified);

    // Its piece's box, in the world (both corners through the world matrix).
    void editObjectWorldBox(ee::Ptr<EditObjectPart> part, uint32_t /*park: unread*/, ee::Ptr<zge::Box> box)
    {
        pieceDefBox(part->pieceDef, box);
        box->low = zge::transformed(part->matrix, box->low);
        box->high = zge::transformed(part->matrix, box->high);
    }
    READABLE_FUNCTION(editObjectWorldBox, Verified);

    // A matrix's translation, its position.
    void matrixPosition(ee::Ptr<zge::Matrix> matrix, ee::Ptr<zge::Vec4> position)
    {
        zge::aligned(position) = matrix->translation();
    }
    READABLE_FUNCTION(matrixPosition, Verified);

    // Placed: the position into its own matrix, and its world matrix made again.
    void setEditObjectPosition(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> position)
    {
        part->localMatrix.translation() = zge::quadwordOf(position);
        game::tail(updateEditObjectMatrix, part);
    }
    READABLE_FUNCTION_WAITS(setEditObjectPosition, Verified);

    // Its world matrix made again, by its kind.
    void updateEditObjectMatrix(ee::Ptr<EditObjectPart> part)
    {
        methodsOf(part)->updateMatrix(part);
    }
    READABLE_FUNCTION(updateEditObjectMatrix, Verified);

    // Made: a plain piece's methods, its matrices the identity, no owner, its
    // piece, no display and no parent anchor, the flags of a plain piece, its
    // piece's anchors, its cost (none for a tower) added to the park's.
    void constructEditObject(uint32_t object, uint32_t pieceDef)
    {
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        setEditObjectMethods(part, sym::g_editObjectMethods);
        zge::matrixIdentity(&part->localMatrix);
        zge::matrixIdentity(&part->matrix);
        setEditObjectOwner(part, 0);
        setEditObjectDef(part, pieceDef);
        part->display = 0;
        part->parentAnchor = 0;
        part->flags[kObjectHasHeight] = 0;
        part->flags[kObjectRemovable] = 1;
        part->flags[kObjectShowsBox] = 1;
        part->flags[kObjectSnaps] = 1;
        setEditObjectAnchors(part, newPieceAnchors(pieceDefAnchors(pieceDef)));
        if (zge::stringCompareNoCase(editObjectClassName(part), kEditTowerObjectClass) == 0)
            part->cost = 0;
        else
            part->cost = 1;
        game::tail(addParkCost, part);
    }
    READABLE_FUNCTION_WAITS(constructEditObject, Verified);

    // A message to the object goes on to each of its anchors (queued), then to
    // the object's handler.
    void editObjectMessage(uint32_t object, uint32_t message)
    {
        const uint32_t part = editObjectPart(object, kEditObjectClass);
        for (uint32_t anchor = firstAnchor(ee::Ptr<EditObjectPart>(part)->anchors); anchor != 0; anchor = nextAnchor(anchor))
            queueEditMessageTarget(messageSender(message), anchor);
        game::tail(runMessageObjectHandler, message, part);
    }
    READABLE_FUNCTION_WAITS(editObjectMessage, Verified);

    // A plain piece's methods.

    void editObjectReleaseDisplay(ee::Ptr<EditObjectPart> part)
    {
        if (part->display != 0)
            freeEditDisplay(part->display);
        part->display = 0;
    }
    READABLE_FUNCTION_WAITS(editObjectReleaseDisplay, Verified);

    void editObjectTranslate(ee::Ptr<EditObjectPart> part, ee::Ptr<zge::Vec4> move)
    {
        part->localMatrix.translation() += *move;
    }
    READABLE_FUNCTION(editObjectTranslate, Verified);

    void editObjectSpareMethod()
    {
    }
    READABLE_FUNCTION(editObjectSpareMethod, Verified);

    void editObjectSpareMethod2()
    {
    }
    READABLE_FUNCTION(editObjectSpareMethod2, Verified);

    void editObjectLoadExtra()
    {
    }
    READABLE_FUNCTION(editObjectLoadExtra, Verified);

    // A plain piece saves which piece it is, as the catalog's index (16 bits).
    void editObjectSaveExtra(ee::Ptr<EditObjectPart> part, uint32_t stream)
    {
        ee::Local<uint16_t> pieceIndex;
        *pieceIndex = static_cast<uint16_t>(pieceDefIndex(part->pieceDef));
        parkStreamWrite(stream, pieceIndex, 2);
    }
    READABLE_FUNCTION(editObjectSaveExtra, Verified);

    // Its world matrix: its own under its parent anchor's (or its own, hanging
    // from nothing), and its display placed there.
    void editObjectUpdateMatrix(ee::Ptr<EditObjectPart> object)
    {
        const ee::Ptr<EditObjectPart> part = object;
        const ee::Ptr<zge::Matrix> world = &part->matrix;
        if (part->parentAnchor != 0)
            zge::matrixMultiply(world, ee::Ptr<zge::Matrix>(static_cast<uint32_t>(anchorMatrix(part->parentAnchor))), &part->localMatrix);
        else
            part->matrix = part->localMatrix;
        if (part->display != 0)
            game::tail(setDisplayMatrix, part->display, world);
    }
    READABLE_FUNCTION_WAITS(editObjectUpdateMatrix, Verified);

    int32_t editObjectFlagAt(ee::Ptr<EditObjectPart> part, uint32_t index)
    {
        return part->flags[index];
    }
    READABLE_FUNCTION(editObjectFlagAt, Verified);

    void destroyEditPiece(ee::Address object)
    {
        game::tail(deleteEditObject, object);
    }
    READABLE_FUNCTION_WAITS(destroyEditPiece, Verified);

    namespace
    {
        // The park's cost: what its pieces add up to. Its limit: the meter is
        // full there, and no more pieces go in.
        inline ee::Global<int32_t, sym::g_parkCost> g_parkCost;
        constexpr ee::Float kParkCostLimit = ee::Float(game::readable::perGame(300.0f, 300.0f, 200.0f)); // 200 in BMX XXX

        // The cost as a float, read as unsigned (the compiler's conversion: a
        // cost past 2^31 halves, converts and doubles).
        ee::Float costAsFloat(int32_t cost)
        {
            if (cost >= 0)
                return ee::toFloat(cost);
            const ee::Float half = ee::toFloat(static_cast<int32_t>((static_cast<uint32_t>(cost) & 1u) | (static_cast<uint32_t>(cost) >> 1)));
            return half + half;
        }

        // The park meter shows the cost against the limit, full at the limit.
        void showParkCost(int32_t cost)
        {
            ee::Float level = costAsFloat(cost) * ee::Float(game::readable::perGame(0.0033333334f, 0.0033333334f, 0.005f)); // over kParkCostLimit
            if (ee::Float(1.0f) < level)
                level = ee::Float(1.0f);
            game::tail(setPowerLevel, level);
        }
    }

    void showEditObjectDisplay(ee::Ptr<EditObjectPart> part, uint32_t shown)
    {
        if (part->display != 0)
            game::tail(showEditDisplay, part->display, shown);
    }
    READABLE_FUNCTION_WAITS(showEditObjectDisplay, Verified);

    int32_t editObjectCost(ee::Ptr<EditObjectPart> part)
    {
        return part->cost;
    }
    READABLE_FUNCTION(editObjectCost, Verified);

    void addParkCost(ee::Ptr<EditObjectPart> part)
    {
        const int32_t cost = editObjectCost(part);
        *g_parkCost = *g_parkCost + cost;
        showParkCost(*g_parkCost);
    }
    READABLE_FUNCTION_WAITS(addParkCost, Verified);

    void removeParkCost(ee::Ptr<EditObjectPart> part)
    {
        const int32_t cost = editObjectCost(part);
        *g_parkCost = *g_parkCost - cost;
        showParkCost(*g_parkCost);
    }
    READABLE_FUNCTION_WAITS(removeParkCost, Verified);

    int32_t parkCostAllowsMore()
    {
        return costAsFloat(*g_parkCost) < kParkCostLimit ? 1 : 0;
    }
    READABLE_FUNCTION(parkCostAllowsMore, Verified);
}
