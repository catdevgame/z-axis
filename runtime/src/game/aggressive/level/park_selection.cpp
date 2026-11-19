// The park editor's selection (class EditSelection): the cursor that eases to its
// cell, the rectangle dragged out from it, and the pieces it holds, each on an
// anchor of its own while it moves with the cursor.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_selection.cpp.

#include "zge/memory/regions.h"
#include "zge/editor/park_selection.h"
#include "zge/text/token_reader.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "aggressive/level/edit_records.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "aggressive/level/matrix_points.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/park_selection.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kAngleSin = sym::angleSin;           // hooked by a game override: by address
        constexpr uint32_t kPoolFree = sym::poolFree;
        constexpr uint32_t kPoolReset = sym::poolReset;
        // Registered at the addresses of other functions of identical code (game::callAt).
        constexpr uint32_t kPoolCount = sym::poolCount;
        constexpr uint32_t kParkCellSize = sym::parkCellSize;
        constexpr uint32_t kFirstAnchor = sym::firstAnchor;
        constexpr uint32_t kNextAnchor = sym::nextAnchor;
        constexpr uint32_t kAnchorAttachedObject = sym::anchorAttachedObject;
        constexpr uint32_t kAnchorAngles = sym::anchorAngles;
        constexpr uint32_t kEditRefused = sym::editRefused;
        constexpr uint32_t kEditObjectParentAnchor = sym::editObjectParentAnchor;
        constexpr uint32_t kEditObjectAnchors = sym::editObjectAnchors;
        constexpr uint32_t kEditObjectCellOffset = sym::editObjectCellOffset;
        constexpr uint32_t kEditObjectFootprint = sym::editObjectFootprint;
        constexpr uint32_t kCloneEditObject = sym::cloneEditObject;
        constexpr uint32_t kEditObjectFlag = sym::editObjectFlag;
        constexpr uint32_t kTranslateEditObject = sym::translateEditObject;
        constexpr uint32_t kEditObjectWorldBox = sym::editObjectWorldBox;
        constexpr uint32_t kMatrixPosition = sym::matrixPosition;
        constexpr uint32_t kSelectionEmpty = sym::selectionEmpty;
        constexpr uint32_t kUpdateSelectionBounds = sym::updateSelectionBounds;
        constexpr uint32_t kSelectionNoop = sym::selectionNoop;
        constexpr uint32_t kSelectionCellAt = sym::selectionCellAt;
        constexpr uint32_t kClearSelectionRotation = sym::clearSelectionRotation;
        constexpr uint32_t kRotateCellOffset = sym::rotateCellOffset;
        constexpr uint32_t kSnapSelectionToPiece = sym::snapSelectionToPiece;
        constexpr uint32_t kOrderedCellBounds = sym::orderedCellBounds;
        constexpr uint32_t kUpdateSelectionSize = sym::updateSelectionSize;
        constexpr uint32_t kOffsetSelectionCells = sym::offsetSelectionCells;
        constexpr uint32_t kSelectionWithinLimit = sym::selectionWithinLimit;
        constexpr uint32_t kMoveSelection = sym::moveSelection;
        constexpr uint32_t kNewHeldPiece = sym::newHeldPiece;
        constexpr uint32_t kFreeHeldPiece = sym::freeHeldPiece;
        constexpr uint32_t kSettleSelection = sym::settleSelection;
        constexpr uint32_t kHoldPiece = sym::holdPiece;
        constexpr uint32_t kResetSelection = sym::resetSelection;
        constexpr uint32_t kClearSelection = sym::clearSelection;
        constexpr uint32_t kDrawSelection = sym::drawSelection;
        constexpr uint32_t kCollectCellPiece = sym::collectCellPiece;
        constexpr uint32_t kHeldPieceCells = sym::heldPieceCells;
        constexpr uint32_t kCellInSelectionBox = sym::cellInSelectionBox;
        constexpr uint32_t kPieceSelectable = sym::pieceSelectable;
        constexpr uint32_t kPieceTreeSelectable = sym::pieceTreeSelectable;
        constexpr uint32_t kForEachSelectedCell = sym::forEachSelectedCell;
        constexpr uint32_t kForEachSelectionCellInBox = sym::forEachSelectionCellInBox;
        constexpr uint32_t kForEachPoolRecord = sym::forEachPoolRecord;
        constexpr uint32_t kForEachHeldPiece = sym::forEachHeldPiece;
        constexpr uint32_t kSelectionInPark = sym::selectionInPark;
        constexpr uint32_t kHeldPiecesFit = sym::heldPiecesFit;
        constexpr uint32_t kCellAcceptsPiece = sym::cellAcceptsPiece;
        constexpr uint32_t kCollectSelectedPiece = sym::collectSelectedPiece;
        constexpr uint32_t kHoldRecordPiece = sym::holdRecordPiece;

        // editObjectFlag's questions, by what this file does with the answer.
        constexpr uint32_t kFlagHasHeight = 0, kFlagRemovable = 1, kFlagShowsBox = 2, kFlagSnaps = 3;
        constexpr ee::Float kLevelTolerance = 0.01f;          // a piece's cells at the height of its anchor

    }

    namespace
    {
        // Registered at the addresses of other functions of identical code.
        ee::Ptr<EditObjectPart> attachedObject(uint32_t anchor) { return anchorAttachedObject(anchor); }
        uint32_t anglesOfAnchor(uint32_t anchor) { return static_cast<uint32_t>(anchorAngles(anchor)); }
    }

    void orderedCellBounds(uint32_t /*selection*/, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, ee::Ptr<CellIndex> minimum, ee::Ptr<CellIndex> maximum);

    int32_t updateSelectionSize(uint32_t address);

    int32_t forEachHeldPiece(uint32_t selection, uint32_t callback, ee::Address userData);

    void heldPieceCells(uint32_t /*selection*/, ee::Address address, ee::Address firstAddress, ee::Address lastAddress, uint32_t angles);

    int32_t selectionInPark(uint32_t address);

    uint32_t heldPiecesFit(uint32_t selection);

    int32_t forEachSelectedCell(uint32_t selection, uint32_t callback, uint32_t userData);

    namespace
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, uint32_t record) { return ee::tailCall<int32_t>(kPoolFree, pool, record); }
        }
    }

    int32_t forEachPoolRecord(uint32_t selection, uint32_t pool, RecordWalk callback, uint32_t userData);

    void emptySelectionHook()
    {
    }
    READABLE_FUNCTION(emptySelectionHook, Draft);

    namespace
    {
        namespace tail
        {
        int32_t poolReset(uint32_t pool) { return ee::tailCall<int32_t>(kPoolReset, pool); }
        }
    }

    int32_t pickUpSelectedPieces(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        forEachSelectedCell(address, kCollectSelectedPiece, 0);
        forEachPoolRecord(address, selection->cellPool, kHoldRecordPiece, 0);
        return tail::poolReset(selection->cellPool);
    }
    READABLE_FUNCTION_WAITS(pickUpSelectedPieces, Verified);

    int32_t pieceTreeSelectable(uint32_t selection, ee::Ptr<EditObjectPart> piece);

    int32_t forEachSelectionCellInBox(uint32_t address, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, RecordWalk callback, ee::Address userData);

    void collectSelectedPiece(uint32_t address, uint32_t recordAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<SelectionRecord> record(recordAddress);
        const ee::Ptr<EditObjectPart> piece = record->object;
        if (piece == 0)
        {
            return;
        }
        const int32_t removable = editObjectFlag(piece, kFlagRemovable);
        if (removable != 0)
        {
            const uint32_t copy = zge::poolAlloc(ee::Ptr<zge::Pool>(selection->cellPool));
            *ee::Ptr<SelectionRecord>(copy) = *record;
            game::tail(collectCellPiece, address, recordAddress); return;
        }
    }
    READABLE_FUNCTION_WAITS(collectSelectedPiece, Verified);

    uint32_t holdRecordPiece(uint32_t selection, uint32_t record)
    {
        return game::tail(holdPiece, selection, ee::Ptr<SelectionRecord>(record)->object);
    }
    READABLE_FUNCTION_WAITS(holdRecordPiece, Verified);

    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress);

    void selectionPosition(uint32_t selection, ee::Ptr<zge::Vec4> positionOut)
    {
        *positionOut = ee::Ptr<EditSelection>(selection)->position;
    }
    READABLE_FUNCTION(selectionPosition, Verified);

    void cursorCellAngles(uint32_t selection, ee::Ptr<AngleWords> anglesOut)
    {
        const uint32_t anchor = selectionCellAt(selection, &ee::Ptr<EditSelection>(selection)->cursorCell);
        *anglesOut = *ee::Ptr<AngleWords>(anglesOfAnchor(anchor));
    }
    READABLE_FUNCTION(cursorCellAngles, Verified);

    ee::Ptr<EditObjectPart> firstHeldPiece(uint32_t selection)
    {
        const ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(ee::Ptr<EditSelection>(selection)->heldPool));
        if (!link)
            return 0;
        return poolRecordAs<SelectionRecord>(link)->object;
    }
    READABLE_FUNCTION(firstHeldPiece, Verified);

    void cursorCell(uint32_t selection, ee::Ptr<CellIndex> cellOut)
    {
        *cellOut = ee::Ptr<EditSelection>(selection)->cursorCell;
    }
    READABLE_FUNCTION(cursorCell, Verified);

    // cellAcceptsPiece: an empty cell in the park at the height of the piece's anchor.
    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        if (parkCellFree(selection->park, cell) == 0)
            return 0;
        const uint32_t anchor = selectionCellAt(address, cell);
        if (attachedObject(anchor) != 0)
            return 0;
        const ee::Ptr<zge::Vec4> position(static_cast<uint32_t>(anchorWorldPosition(anchor)));
        const uint32_t parent = editObjectParentAnchor(ee::Ptr<SelectionRecord>(recordAddress)->object);
        const ee::Ptr<zge::Vec4> parentPosition(static_cast<uint32_t>(anchorWorldPosition(parent)));
        const bool level = !(kLevelTolerance < ee::abs(position->y - parentPosition->y));
        return level ? 1u : 0u;
    }
    READABLE_FUNCTION(cellAcceptsPiece, Verified);

    ee::Ptr<EditObjectPart> cursorPiece(uint32_t selection)
    {
        const uint32_t anchor = selectionCellAt(selection, &ee::Ptr<EditSelection>(selection)->cursorCell);
        return attachedObject(anchor);
    }
    READABLE_FUNCTION(cursorPiece, Draft);

    void setSelectionOffset(uint32_t selection, ee::Ptr<zge::Vec4> offset)
    {
        ee::Ptr<EditSelection>(selection)->offset = *offset;
    }
    READABLE_FUNCTION(setSelectionOffset, Verified);

    uint32_t selectionMoved(uint32_t selection)
    {
        return ee::Ptr<EditSelection>(selection)->moving;
    }
    READABLE_FUNCTION(selectionMoved, Verified);
}
