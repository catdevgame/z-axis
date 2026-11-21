#pragma once

// The park editor's selection (class EditSelection, park_selection.cpp): the
// cursor, the rectangle dragged out from it and the pieces it holds.

#include "zge/memory/regions.h"
#include "zge/text/token_reader.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include <cstddef>
#include <cstring>
#include "zge/editor/edit_records.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    // (park_selection.cpp)
    void attachSelectionToPark(uint32_t address, uint32_t park);
    int32_t beginSelectionRectangle(uint32_t selection);
    int32_t canPlaceHeldPieces(uint32_t selection);
    int32_t clearSelection(uint32_t selection);
    void clearSelectionRotation(uint32_t selection);
    uint32_t cursorAnchor(uint32_t selection);
    uint32_t deleteSelectedPieces(uint32_t address);
    int32_t drawSelection(uint32_t selection, uint32_t colours);
    int32_t drawSelectionInColour(uint32_t selection);
    void endSelectionRectangle(uint32_t selection);
    uint32_t holdPiece(uint32_t address, ee::Ptr<EditObjectPart> piece);
    int32_t moveSelection(uint32_t address, ee::Address offset);
    uint32_t placeHeldPieces(uint32_t selection);
    int32_t placePieceWithSelection(uint32_t address, ee::Ptr<EditObjectPart> piece, uint32_t anchor);
    void registerEditSelectionClass();
    int32_t resetSelection(uint32_t address);
    uint32_t rotateSelection(uint32_t address, uint32_t turnAddress);
    void selectionColour(uint32_t selection, ee::Ptr<SelectionColours> coloursOut);
    int32_t selectionEmpty(uint32_t selection);
    uint32_t selectionTarget(uint32_t selection);
    ee::Float selectionTopHeight(uint32_t address, uint32_t height);
    ee::Ptr<EditObjectPart> takeHeldPiece(uint32_t selection);
    uint32_t toolSetPark(ee::Ptr<EditToolSet> toolSet);
    uint32_t toolSetSelection(ee::Ptr<EditToolSet> toolSet);
    void updateSelectionCursor(uint32_t address);

    // A piece the selection holds or a cell it covers: a record of its two pools.
    struct SelectionRecord
    {
        ee::Ptr<EditObjectPart> object;      // +0x00 the piece
        uint32_t anchor;      // +0x04 the EditAnchor it hangs from
        CellIndex cell;       // +0x08
        uint32_t fits;        // +0x14
    };
    static_assert(sizeof(SelectionRecord) == 0x18);

    // The object's part of class EditSelection.
    // Handed to the forEach walks: (selection, record, user data).
    using RecordWalk = ee::FunctionPtr<int32_t(uint32_t, ee::Ptr<SelectionRecord>, uint32_t)>;

    struct EditSelection
    {
        zge::Vec4 position;   // +0x00 eases towards the cursor cell
        AngleWords angles;    // +0x10 the held pieces' turn
        uint8_t _16[10];
        zge::Vec4 facing;     // +0x20 what the camera's facing subtracts
        zge::Vec4 offset;     // +0x30
        uint32_t moving;      // +0x40
        CellIndex rectangleStart; // +0x44
        CellIndex rectangleEnd; // +0x50 moves with the cursor
        CellIndex cursorCell; // +0x5C
        CellIndex boundsMin;  // +0x68
        CellIndex boundsMax;  // +0x74
        uint32_t park;        // +0x80
        uint32_t dragging;    // +0x84 the rectangle's start stays when the cursor moves
        uint8_t _88[4];
        uint32_t throughTowers; // +0x8C a tower's cell means the tower's first anchor
        CellIndex size;       // +0x90
        uint32_t heldPool;    // +0x9C SelectionRecords
        uint32_t cellPool;    // +0xA0 SelectionRecords
        SelectionColours colours; // +0xA4
        uint8_t _b4[12];
    };
    static_assert(sizeof(EditSelection) == 0xC0 && offsetof(EditSelection, angles) == 0x10 && offsetof(EditSelection, facing) == 0x20 &&
                  offsetof(EditSelection, moving) == 0x40 && offsetof(EditSelection, cursorCell) == 0x5C && offsetof(EditSelection, park) == 0x80 &&
                  offsetof(EditSelection, size) == 0x90 && offsetof(EditSelection, colours) == 0xA4);

    // (park_selection.cpp, for the games' own parts)
    int32_t cellInSelectionBox(uint32_t address, ee::Address cellAddress);
    int32_t collectCellPiece(uint32_t selection, uint32_t address);
    int32_t forEachHeldPiece(uint32_t selection, uint32_t callback, ee::Address userData);
    int32_t forEachPoolRecord(uint32_t selection, uint32_t pool, RecordWalk callback, uint32_t userData);
    int32_t forEachSelectedCell(uint32_t selection, uint32_t callback, uint32_t userData);
    int32_t forEachSelectionCellInBox(uint32_t address, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, RecordWalk callback, ee::Address userData);
    int32_t freeHeldPiece(uint32_t selection, ee::Address address);
    void heldPieceCells(uint32_t , ee::Address address, ee::Address firstAddress, ee::Address lastAddress, uint32_t angles);
    uint32_t heldPiecesFit(uint32_t selection);
    uint32_t newHeldPiece(uint32_t selection);
    void offsetSelectionCells(uint32_t address, ee::Ptr<CellIndex> offsetAddress);
    void orderedCellBounds(uint32_t , ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, ee::Ptr<CellIndex> minimum, ee::Ptr<CellIndex> maximum);
    uint32_t pieceSelectable(uint32_t address, ee::Ptr<EditObjectPart> piece);
    int32_t pieceTreeSelectable(uint32_t selection, ee::Ptr<EditObjectPart> piece);
    int32_t rotateCellOffset(uint32_t cellAddress, ee::Address pivotAddress, uint32_t angles);
    uint32_t selectionCellAt(uint32_t address, ee::Ptr<const CellIndex> cell);
    int32_t selectionInPark(uint32_t address);
    void selectionNoop();
    int32_t selectionWithinLimit(uint32_t address);
    void settleSelection(uint32_t address);
    void snapSelectionToPiece(uint32_t address);
    int32_t updateSelectionBounds(uint32_t address);
    int32_t updateSelectionSize(uint32_t address);
}
