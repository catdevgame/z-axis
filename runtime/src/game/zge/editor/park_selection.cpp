// The park editor's selection (class EditSelection): the cursor that eases to its
// cell, the rectangle dragged out from it, and the pieces it holds, each on an
// anchor of its own while it moves with the cursor.

#include "zge/editor/game.h"
#include "zge/memory/regions.h"
#include "zge/text/token_reader.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "zge/editor/edit_records.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/editor/park_selection.cpp.h"

namespace zge::editor
{
    uint32_t toolSetPark(ee::Ptr<EditToolSet> toolSet)
    {
        return toolSet->park;
    }
    READABLE_FUNCTION(toolSetPark, Verified);

    uint32_t toolSetSelection(ee::Ptr<EditToolSet> toolSet)
    {
        return toolSet->selection;
    }
    READABLE_FUNCTION(toolSetSelection, Verified);

    namespace
    {
        constexpr uint32_t kPoolFree = sym::poolFree;
        constexpr uint32_t kPoolReset = sym::poolReset;
        constexpr uint32_t kTranslateEditObject = sym::translateEditObject;
        constexpr uint32_t kSetEditObjectPosition = sym::setEditObjectPosition;
        constexpr uint32_t kMakeDeleteMessage = sym::makeDeleteMessage;
        constexpr uint32_t kConstructEditSelection = sym::constructEditSelection;
        constexpr uint32_t kDestructEditSelection = sym::destructEditSelection;
        constexpr uint32_t kEditSelectionMessage = sym::editSelectionMessage;
        constexpr uint32_t kPositionHeldPiece = sym::positionHeldPiece;
        constexpr uint32_t kCheckHeldPieceFits = sym::checkHeldPieceFits;

        // Handed to the forEach walks: (selection, record, user data).
        constexpr uint32_t kCellPieceTopHeight = sym::cellPieceTopHeight;
        constexpr uint32_t kReleaseHeldPieceIf = sym::releaseHeldPieceIf;
        constexpr uint32_t kAlignPieceToSelection = sym::alignPieceToSelection;
        constexpr uint32_t kCollectSelectedPiece = sym::collectSelectedPiece;
        constexpr uint32_t kSetRecordAnchorFlag = sym::setRecordAnchorFlag;
        constexpr uint32_t kDeleteRecordPiece = sym::deleteRecordPiece;
        constexpr uint32_t kFreeHeldPieceCallback = sym::freeHeldPieceCallback;
        constexpr uint32_t kOffsetRecordCell = sym::offsetRecordCell;
        constexpr uint32_t kPutHeldPiece = sym::putHeldPiece;
        constexpr uint32_t kPlaceHeldPieceChecked = sym::placeHeldPieceChecked;
        constexpr uint32_t kDrawSelectionCell = sym::drawSelectionCell;
        constexpr uint32_t kDrawHeldPiece = sym::drawHeldPiece;
        constexpr uint32_t kRepositionHeldPiece = sym::repositionHeldPiece;

        constexpr uint32_t kNameEditSelection = sym::g_editSelectionName;  // "EditSelection"
        constexpr uint32_t kNameEditBaseClass = sym::g_editSelectionEditBaseClassName;  // "EditBaseClass"
        constexpr uint32_t kNameEditTowerObject = sym::g_selectionEditTowerObjectName; // "EditTowerObject"
        constexpr uint32_t kNameEditAnchor = sym::g_heldPieceEditAnchorName;     // "EditAnchor"

        // editObjectFlag's questions, by what this file does with the answer.
        constexpr uint32_t kFlagHasHeight = 0, kFlagRemovable = 1, kFlagShowsBox = 2, kFlagSnaps = 3;

        constexpr uint32_t kRectangleLimit = 7;               // cells a side, exclusive
        constexpr ee::Float kCursorDeadband = 0.4572f;        // metres: 1.5 feet
        constexpr ee::Float kBoxBottom = 0.3f, kBoxTop = 0.1f;

        uint32_t colourWord(const Colour &colour)
        {
            uint32_t word;
            std::memcpy(&word, &colour, sizeof(word));
            return word;
        }

        void addCell(CellIndex &cell, const CellIndex &offset)
        {
            cell.x += offset.x;
            cell.y += offset.y;
            cell.z += offset.z;
        }

        void negateCell(CellIndex &cell)
        {
            cell.x = -cell.x;
            cell.y = -cell.y;
            cell.z = -cell.z;
        }

    }

    namespace
    {
        // Registered at the addresses of other functions of identical code.
        ee::Ptr<EditObjectPart> attachedObject(uint32_t anchor) { return anchorAttachedObject(anchor); }
        uint32_t anglesOfAnchor(uint32_t anchor) { return static_cast<uint32_t>(anchorAngles(anchor)); }
        uint32_t firstAnchorIn(uint32_t anchors) { return firstAnchor(anchors); }
        uint32_t nextAnchorAfter(uint32_t anchor) { return nextAnchor(anchor); }
        uint32_t cellSizeOf(uint32_t park) { return parkCellSize(park); }
        void refuseEdit() { editRefused(); }
        int32_t poolCountOf(uint32_t pool) { return zge::poolCount(ee::Ptr<zge::Pool>(pool)); }
    }

    // registerEditSelectionClass: the class's size; messages reach only the base class's handler.
    void registerEditSelectionClass()
    {
        game::tail(registerEditClass, kNameEditSelection, static_cast<uint32_t>(sizeof(EditSelection)), kNameEditBaseClass, kConstructEditSelection, kDestructEditSelection, kEditSelectionMessage);
    }
    READABLE_FUNCTION(registerEditSelectionClass, Verified);

    uint32_t constructEditSelection(uint32_t data)
    {
        const uint32_t address = editObjectPart(data, kNameEditSelection);
        const ee::Ptr<EditSelection> selection(address);
        selection->position.x = selection->position.y = selection->position.z = ee::Float(0.0f);
        selection->offset.x = selection->offset.y = selection->offset.z = ee::Float(0.0f);
        selection->angles = AngleWords{};
        selection->park = 0;
        selection->dragging = 0;
        selection->heldPool = zge::createPool(static_cast<uint32_t>(sizeof(SelectionRecord)), 0x20, 0);
        const uint32_t cellPool = zge::createPool(static_cast<uint32_t>(sizeof(SelectionRecord)), 0x100, 0);
        selection->cellPool = cellPool;
        selection->throughTowers = 1;
        selection->moving = 0;
        selection->colours = SelectionColours{{154, 28, 33, 32}, {176, 142, 33, 32}, {154, 28, 33, 32}, {176, 142, 21, 32}};
        return cellPool;
    }
    READABLE_FUNCTION_WAITS(constructEditSelection, Verified);

    uint32_t destructEditSelection(uint32_t data)
    {
        const uint32_t address = editObjectPart(data, kNameEditSelection);
        const ee::Ptr<EditSelection> selection(address);
        clearSelection(address);
        zge::destroyPool(ee::Ptr<zge::Pool>(selection->heldPool));
        return game::tail(zge::destroyPool, ee::Ptr<zge::Pool>(selection->cellPool));
    }
    READABLE_FUNCTION_WAITS(destructEditSelection, Verified);

    void editSelectionMessage()
    {
    }
    READABLE_FUNCTION(editSelectionMessage, Verified);

    void selectionColour(uint32_t selection, ee::Ptr<SelectionColours> coloursOut)
    {
        *coloursOut = ee::Ptr<EditSelection>(selection)->colours;
    }
    READABLE_FUNCTION(selectionColour, Verified);

    void attachSelectionToPark(uint32_t address, uint32_t park)
    {
        ee::Ptr<EditSelection>(address)->park = park;
        parkStartCell(park, &ee::Ptr<EditSelection>(address)->cursorCell);
        copyParkOrigin(park, &ee::Ptr<EditSelection>(address)->position);
        resetSelection(address);
        game::tail(clearSelectionRotation, address);
    }
    READABLE_FUNCTION_WAITS(attachSelectionToPark, Verified);

    int32_t selectionEmpty(uint32_t selection)
    {
        const int32_t held = poolCountOf(ee::Ptr<EditSelection>(selection)->heldPool);
        return static_cast<uint32_t>(held) < 1u;
    }
    READABLE_FUNCTION(selectionEmpty, Verified);

    void orderedCellBounds(uint32_t /*selection*/, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, ee::Ptr<CellIndex> minimum, ee::Ptr<CellIndex> maximum);

    int32_t updateSelectionSize(uint32_t address);

    int32_t forEachHeldPiece(uint32_t selection, uint32_t callback, ee::Address userData);

    int32_t updateSelectionBounds(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        orderedCellBounds(address, &selection->rectangleStart, &selection->rectangleEnd, &selection->boundsMin, &selection->boundsMax);
        updateSelectionSize(address);
        return game::tail(forEachHeldPiece, address, kRepositionHeldPiece, 0);
    }
    READABLE_FUNCTION_WAITS(updateSelectionBounds, Verified);

    void selectionNoop()
    {
    }
    READABLE_FUNCTION(selectionNoop, Verified);

    // updateSelectionCursor: the cursor's position eases to its cell, each axis by the same
    // step (a fifth of the root of the distance left) and landing inside the dead
    // band. Dragging with pieces held or over one cell, the facing is only the
    // offset; otherwise it is the way the eased position lags, at the offset's height.
    // Size.y is tested where the size across the park is z: y is always 1 here, so
    // a rectangle one cell wide counts as one cell.
    void updateSelectionCursor(uint32_t address)
    {
        ee::Local<zge::Vec4> wanted;
        const ee::Ptr<EditSelection> selection(address);
        selection->moving = 0;
        const ee::Float deadband = kCursorDeadband;
        parkCellPosition(selection->park, &selection->cursorCell, wanted);
        *wanted = *wanted + selection->offset;
        zge::Vec4 &position = selection->position;
        const ee::Float dx = position.x - wanted->x, dy = position.y - wanted->y, dz = position.z - wanted->z;
        const ee::Float step = ee::sqrt(ee::sqrt(dx * dx + dy * dy + dz * dz)) * ee::Float(0.2f);
        const auto ease = [&](ee::Float &current, ee::Float target)
        {
            if (current < target - deadband)
            {
                current = current + step;
                selection->moving = 1;
            }
            else if (target + deadband < current)
            {
                current = current - step;
                selection->moving = 1;
            }
            else
                current = target;
        };
        ease(position.x, wanted->x);
        ease(position.y, wanted->y);
        ease(position.z, wanted->z);
        bool offsetOnly = false;
        if (selection->dragging == 1)
            offsetOnly = selectionEmpty(address) == 0 ||
                         (static_cast<uint32_t>(selection->size.x) < 2u && static_cast<uint32_t>(selection->size.y) < 2u);
        if (offsetOnly)
        {
            selection->facing.x = -selection->offset.x;
            selection->facing.y = -selection->offset.y;
            selection->facing.z = -selection->offset.z;
        }
        else
        {
            selection->facing = *wanted - position;
            selection->facing.y = -selection->offset.y;
        }
        forEachHeldPiece(address, kAlignPieceToSelection, 0);
    }
    READABLE_FUNCTION_WAITS(updateSelectionCursor, Verified);

    uint32_t selectionCellAt(uint32_t address, ee::Ptr<const CellIndex> cell)
    {
        const ee::Ptr<EditSelection> selection(address);
        const uint32_t anchor = parkCellAnchor(selection->park, cell);
        if (selection->throughTowers == 1)
        {
            const ee::Ptr<EditObjectPart> object = attachedObject(anchor);
            if (object != 0 && zge::compareStrings(editObjectClassName(object), kNameEditTowerObject) == 0)
                return firstAnchorIn(editObjectAnchors(object));
        }
        return anchor;
    }
    READABLE_FUNCTION(selectionCellAt, Verified);

    void clearSelectionRotation(uint32_t selection)
    {
        selectionNoop();
        ee::Ptr<EditSelection>(selection)->angles = AngleWords{};
    }
    READABLE_FUNCTION(clearSelectionRotation, Verified);

    // rotateCellOffset: the cell turned about the pivot by the angle's y, rounded away
    // from zero to whole cells.
    int32_t rotateCellOffset(uint32_t cellAddress, ee::Address pivotAddress, uint32_t angles)
    {
        const ee::Ptr<CellIndex> cell(cellAddress), pivot(pivotAddress);
        const int32_t acrossX = cell->x - pivot->x, acrossZ = cell->z - pivot->z;
        const zge::Angle turn = ee::Ptr<zge::EulerAngles>(angles)->y;
        const ee::Float sine = zge::angleSin(turn);
        const ee::Float cosine = zge::angleCos(turn);
        const ee::Float x = ee::toFloat(acrossX), z = ee::toFloat(acrossZ);
        const ee::Float turnedX = x * cosine - z * sine;
        ee::Float roundedX;
        if (turnedX < ee::Float(0.0f))
            roundedX = turnedX - ee::Float(0.5f);
        else
            roundedX = turnedX + ee::Float(0.5f);
        const ee::Float turnedZ = x * sine + z * cosine;
        ee::Float roundedZ;
        if (turnedZ < ee::Float(0.0f))
            roundedZ = turnedZ - ee::Float(0.5f);
        else
            roundedZ = turnedZ + ee::Float(0.5f);
        const int32_t result = ee::toInt(roundedX) + pivot->x;
        cell->x = result;
        cell->z = ee::toInt(roundedZ) + pivot->z;
        return result;
    }
    READABLE_FUNCTION(rotateCellOffset, Verified);

    void heldPieceCells(uint32_t /*selection*/, ee::Address address, ee::Address firstAddress, ee::Address lastAddress, uint32_t angles);

    // snapSelectionToPiece: over a piece that snaps, the rectangle takes that piece's cells;
    // otherwise it shrinks to the cursor.
    void snapSelectionToPiece(uint32_t address)
    {
        ee::Local<SelectionRecord> record;
        const ee::Ptr<EditSelection> selection(address);
        const uint32_t anchor = selectionCellAt(address, &selection->cursorCell);
        const ee::Ptr<EditObjectPart> object = attachedObject(anchor);
        if (object != 0 && editObjectFlag(object, kFlagSnaps) != 0)
        {
            const uint32_t parent = editObjectParentAnchor(object);
            record->object = object;
            record->anchor = parent;
            parkCellAtPosition(selection->park, anchorWorldPosition(parent), &record->cell);
            heldPieceCells(address, record, address + offsetof(EditSelection, rectangleStart), address + offsetof(EditSelection, rectangleEnd),
                           anglesOfAnchor(record->anchor));
            selectionNoop();
        }
        else
        {
            selection->rectangleStart = selection->cursorCell;
            selection->rectangleEnd = selection->cursorCell;
        }
        updateSelectionBounds(address);
    }
    READABLE_FUNCTION_WAITS(snapSelectionToPiece, Verified);

    // orderedCellBounds: the two cells sorted per axis; the outputs may be the inputs.
    // y is 0 on both sides, but its sort writes the maximum's x where first.y is
    // below second.y, as the original does: no cell of the editor has a y.
    void orderedCellBounds(uint32_t /*selection*/, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, ee::Ptr<CellIndex> minimum, ee::Ptr<CellIndex> maximum)
    {
        ee::Local<CellIndex> first;
        ee::Local<CellIndex> second;
        *first = *firstCell;
        *second = *secondCell;
        const int32_t firstX = first->x, secondX = second->x;
        if (firstX < secondX)
        {
            minimum->x = firstX;
            maximum->x = secondX;
        }
        else
        {
            minimum->x = secondX;
            maximum->x = firstX;
        }
        const bool yBelow = first->y < second->y;
        minimum->y = 0;
        if (yBelow)
            maximum->x = 0;
        else
            maximum->y = 0;
        const int32_t firstZ = first->z, secondZ = second->z;
        if (firstZ < secondCell->z)
        {
            minimum->z = firstZ;
            maximum->z = secondZ;
        }
        else
        {
            minimum->z = secondZ;
            maximum->z = firstZ;
        }
    }
    READABLE_FUNCTION(orderedCellBounds, Verified);

    int32_t updateSelectionSize(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        selection->size.x = selection->boundsMax.x - selection->boundsMin.x + 1;
        selection->size.y = selection->boundsMax.y - selection->boundsMin.y + 1;
        selection->size.z = selection->boundsMax.z - selection->boundsMin.z + 1;
        return selection->size.z;
    }
    READABLE_FUNCTION(updateSelectionSize, Verified);

    void offsetSelectionCells(uint32_t address, ee::Ptr<CellIndex> offsetAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        const CellIndex offset = *offsetAddress;
        addCell(selection->cursorCell, offset);
        addCell(selection->rectangleEnd, offset);
        if (selection->dragging == 0)
            addCell(selection->rectangleStart, offset);
    }
    READABLE_FUNCTION(offsetSelectionCells, Verified);

    // selectionWithinLimit: the rectangle stays under seven cells a side.
    int32_t selectionWithinLimit(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        const auto span = [](int32_t start, int32_t end) { return end >= start ? end - start : start - end; };
        const int32_t acrossX = span(selection->rectangleStart.x, selection->rectangleEnd.x);
        const int32_t acrossZ = span(selection->rectangleStart.z, selection->rectangleEnd.z);
        if (static_cast<uint32_t>(acrossX + 1) >= kRectangleLimit)
            return 0;
        const int32_t within = static_cast<uint32_t>(acrossZ + 1) < kRectangleLimit;
        return within;
    }
    READABLE_FUNCTION(selectionWithinLimit, Verified);

    int32_t selectionInPark(uint32_t address);

    uint32_t heldPiecesFit(uint32_t selection);

    // moveSelection: a move in world units as whole cells, undone if it leaves the
    // park or the rectangle's limit, cut short at a blocked cell, undone again if
    // the rectangle would leave the park. The held pieces take what is left.
    int32_t moveSelection(uint32_t address, ee::Address offset)
    {
        ee::Local<CellIndex> step;
        ee::Local<CellIndex> clamp;
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<CellIndex> cursor = &selection->cursorCell;
        parkCellStep(selection->park, offset, step);
        offsetSelectionCells(address, step);
        const uint32_t valid = parkCellValid(selection->park, cursor);
        const int32_t withinLimit = selectionWithinLimit(address);
        if (valid == 0 || withinLimit == 0)
        {
            negateCell(*step);
            offsetSelectionCells(address, step);
            return 0;
        }
        if (parkCellBlocked(selection->park, cursor) != 0)
        {
            parkClampCells(selection->park, cursor, step, clamp);
            offsetSelectionCells(address, clamp);
            addCell(*step, *clamp);
        }
        const int32_t inPark = selectionInPark(address);
        const int32_t cellFree = parkCellFree(selection->park, cursor);
        if (inPark == 0 || cellFree == 0)
        {
            negateCell(*step);
            offsetSelectionCells(address, step);
            return 0;
        }
        forEachHeldPiece(address, kOffsetRecordCell, step);
        updateSelectionBounds(address);
        heldPiecesFit(address);
        if (selectionEmpty(address) != 0 && selection->dragging == 0)
            snapSelectionToPiece(address);
        return 1;
    }
    READABLE_FUNCTION_WAITS(moveSelection, Verified);

    // rotateSelection: the turn is undone if the rectangle would leave the park; the
    // caller's turn is left negated then.
    uint32_t rotateSelection(uint32_t address, uint32_t turnAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<AngleWords> turn(turnAddress);
        const ee::Ptr<CellIndex> cursor = &selection->cursorCell;
        const uint32_t start = address + offsetof(EditSelection, rectangleStart), end = address + offsetof(EditSelection, rectangleEnd);
        const auto addTurn = [&]
        {
            selection->angles.x = static_cast<uint16_t>(selection->angles.x + turn->x);
            selection->angles.z = static_cast<uint16_t>(selection->angles.z + turn->z);
            selection->angles.y = static_cast<uint16_t>(selection->angles.y + turn->y);
        };
        addTurn();
        rotateCellOffset(start, cursor, turnAddress);
        rotateCellOffset(end, cursor, turnAddress);
        const uint32_t inPark = static_cast<uint32_t>(selectionInPark(address));
        const uint32_t turned = inPark & static_cast<uint32_t>(parkCellFree(selection->park, cursor));
        if (turned != 0)
        {
            updateSelectionBounds(address);
            forEachHeldPiece(address, kPositionHeldPiece, turnAddress);
            heldPiecesFit(address);
            return turned;
        }
        turn->x = static_cast<uint16_t>(-turn->x);
        turn->y = static_cast<uint16_t>(-turn->y);
        turn->z = static_cast<uint16_t>(-turn->z);
        addTurn();
        rotateCellOffset(start, cursor, turnAddress);
        rotateCellOffset(end, cursor, turnAddress);
        return turned;
    }
    READABLE_FUNCTION_WAITS(rotateSelection, Verified);

    int32_t beginSelectionRectangle(uint32_t selection)
    {
        ee::Ptr<EditSelection>(selection)->dragging = 1;
        return 1;
    }
    READABLE_FUNCTION(beginSelectionRectangle, Verified);

    void endSelectionRectangle(uint32_t selection)
    {
        ee::Ptr<EditSelection>(selection)->dragging = 0;
    }
    READABLE_FUNCTION(endSelectionRectangle, Verified);

    int32_t forEachSelectedCell(uint32_t selection, uint32_t callback, uint32_t userData);

    // selectionTopHeight: every selected cell's pieces get `height` (cellPieceTopHeight;
    // towers by their own cells, not their first anchor); the result is the height
    // of the cursor cell's piece.
    ee::Float selectionTopHeight(uint32_t address, uint32_t height)
    {
        ee::Local<zge::Vec4> position;
        const ee::Ptr<EditSelection> selection(address);
        const uint32_t throughTowers = selection->throughTowers;
        selection->throughTowers = 0;
        forEachSelectedCell(address, kCellPieceTopHeight, height);
        selection->throughTowers = throughTowers;
        const uint32_t anchor = parkCellAnchor(selection->park, &selection->cursorCell);
        const ee::Ptr<EditObjectPart> object = attachedObject(anchor);
        if (!object)
            return ee::Float(0.0f);
        matrixPosition(&object->localMatrix, position);
        return -position->y;
    }
    READABLE_FUNCTION_WAITS(selectionTopHeight, Verified);

    uint32_t newHeldPiece(uint32_t selection)
    {
        const uint32_t address = zge::poolAlloc(ee::Ptr<zge::Pool>(ee::Ptr<EditSelection>(selection)->heldPool));
        const uint32_t anchor = newEditObject(kNameEditAnchor, 0);
        const ee::Ptr<SelectionRecord> record(address);
        record->anchor = anchor;
        record->object = 0;
        return address;
    }
    READABLE_FUNCTION_WAITS(newHeldPiece, Verified);

    namespace
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, uint32_t record) { return ee::tailCall<int32_t>(kPoolFree, pool, record); }
        }
    }

    // freeHeldPiece: the piece still held goes with its record (delete message).
    int32_t freeHeldPiece(uint32_t selection, ee::Address address)
    {
        const ee::Ptr<SelectionRecord> record(address);
        const ee::Ptr<EditObjectPart> piece = record->object;
        if (piece != 0)
            broadcastEditMessage(piece, kMakeDeleteMessage, 0);
        const uint32_t anchor = record->anchor;
        if (anchor != 0)
            deleteEditObject(anchor);
        return tail::poolFree(ee::Ptr<EditSelection>(selection)->heldPool, address);
    }
    READABLE_FUNCTION_WAITS(freeHeldPiece, Verified);

    // settleSelection: a rectangle that left the park comes back by the steps the park
    // gives for its two corners.
    void settleSelection(uint32_t address)
    {
        ee::Local<zge::Vec4> total;
        ee::Local<zge::Vec4> startStep;
        ee::Local<zge::Vec4> endStep;
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<CellIndex> start = &selection->rectangleStart;
        const ee::Ptr<CellIndex> end = &selection->rectangleEnd;
        bool stepped = false;
        total->x = total->y = total->z = ee::Float(0.0f);
        if (parkCellValid(selection->park, start) == 0)
        {
            parkStepCell(selection->park, start, startStep);
            *total = *total + *startStep;
            stepped = true;
        }
        if (parkCellValid(selection->park, end) == 0)
        {
            parkStepCell(selection->park, end, endStep);
            *total = *total + *endStep;
            stepped = true;
        }
        if (stepped)
            moveSelection(address, total);
    }
    READABLE_FUNCTION_WAITS(settleSelection, Verified);

    int32_t positionHeldPiece(uint32_t selection, uint32_t address, uint32_t angles);

    // holdPiece: the piece on a new anchor at the cursor, the rectangle round it.
    uint32_t holdPiece(uint32_t address, ee::Ptr<EditObjectPart> piece)
    {
        const ee::Ptr<EditSelection> selection(address);
        const uint32_t angles = address + offsetof(EditSelection, angles);
        const uint32_t heldAddress = newHeldPiece(address);
        const ee::Ptr<SelectionRecord> held(heldAddress);
        held->object = piece;
        held->cell = selection->cursorCell;
        attachToAnchor(held->anchor, piece);
        positionHeldPiece(address, heldAddress, angles);
        heldPieceCells(address, heldAddress, address + offsetof(EditSelection, rectangleStart), address + offsetof(EditSelection, rectangleEnd), angles);
        if (selectionInPark(address) == 0)
            settleSelection(address);
        updateSelectionBounds(address);
        return game::tail(heldPiecesFit, address);
    }
    READABLE_FUNCTION_WAITS(holdPiece, Verified);

    ee::Ptr<EditObjectPart> takeHeldPiece(uint32_t selection)
    {
        const ee::Ptr<SelectionRecord> record = poolRecordAs<SelectionRecord>(zge::firstPoolLink(ee::Ptr<zge::Pool>(ee::Ptr<EditSelection>(selection)->heldPool)));
        const ee::Ptr<EditObjectPart> piece = record->object;
        record->object = 0;
        freeHeldPiece(selection, record);
        return piece;
    }
    READABLE_FUNCTION_WAITS(takeHeldPiece, Verified);

    int32_t resetSelection(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        selection->rectangleStart = selection->cursorCell;
        selection->rectangleEnd = selection->cursorCell;
        selection->dragging = 0;
        return game::tail(updateSelectionBounds, address);
    }
    READABLE_FUNCTION_WAITS(resetSelection, Verified);

    int32_t clearSelection(uint32_t selection)
    {
        forEachHeldPiece(selection, kFreeHeldPieceCallback, 0);
        return game::tail(resetSelection, selection);
    }
    READABLE_FUNCTION_WAITS(clearSelection, Verified);

    int32_t forEachPoolRecord(uint32_t selection, uint32_t pool, RecordWalk callback, uint32_t userData);

    uint32_t deleteSelectedPieces(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        forEachSelectedCell(address, kCollectSelectedPiece, 0);
        forEachPoolRecord(address, selection->cellPool, kDeleteRecordPiece, 0);
        zge::poolReset(ee::Ptr<zge::Pool>(selection->cellPool));
        return game::tail(heldPiecesFit, address);
    }
    READABLE_FUNCTION_WAITS(deleteSelectedPieces, Verified);

    // placeHeldPieces: 1 when every held piece went down.
    uint32_t placeHeldPieces(uint32_t selection)
    {
        ee::Local<uint32_t> allPlaced;
        *allPlaced = 1;
        if (parkCostAllowsMore() == 0)
        {
            refuseEdit();
            return 0;
        }
        forEachHeldPiece(selection, kPlaceHeldPieceChecked, allPlaced);
        heldPiecesFit(selection);
        return *allPlaced;
    }
    READABLE_FUNCTION_WAITS(placeHeldPieces, Verified);

    namespace
    {
        namespace tail
        {
        int32_t poolReset(uint32_t pool) { return ee::tailCall<int32_t>(kPoolReset, pool); }
        }
    }

    int32_t drawSelection(uint32_t selection, uint32_t colours)
    {
        if (selectionEmpty(selection) != 0)
            return game::tail(forEachSelectedCell, selection, kDrawSelectionCell, colours);
        return game::tail(forEachHeldPiece, selection, kDrawHeldPiece, colours);
    }
    READABLE_FUNCTION_WAITS(drawSelection, Verified);

    int32_t drawSelectionInColour(uint32_t selection)
    {
        return game::tail(drawSelection, selection, selection + offsetof(EditSelection, colours));
    }
    READABLE_FUNCTION_WAITS(drawSelectionInColour, Verified);

    int32_t pieceTreeSelectable(uint32_t selection, ee::Ptr<EditObjectPart> piece);

    namespace
    {
        // translateEditObject, and what it leaves in the result register (cellPieceTopHeight returns it).
        int32_t translateEditObjectKeepingResult(ee::Ptr<EditObjectPart> part, uint32_t move) { return ee::tailCall<int32_t>(kTranslateEditObject, part, move); }
    }

    int32_t cellPieceTopHeight(uint32_t selection, uint32_t address, uint32_t height)
    {
        const ee::Ptr<SelectionRecord> record(address);
        int32_t result = static_cast<int32_t>(anchorReserved(record->anchor));
        if (result == 0)
        {
            const ee::Ptr<EditObjectPart> piece = record->object;
            if (!piece)
                return result;
            result = pieceTreeSelectable(selection, piece);
            if (result != 0)
            {
                result = editObjectFlag(record->object, kFlagHasHeight);
                if (result != 0)
                    return translateEditObjectKeepingResult(record->object, height);
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(cellPieceTopHeight, Verified);

    int32_t forEachSelectionCellInBox(uint32_t address, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, RecordWalk callback, ee::Address userData);

    // collectCellPiece: the record of a piece going away: every other cell it covers
    // lets go of it (the callback's user data is the piece).
    int32_t collectCellPiece(uint32_t selection, uint32_t address)
    {
        ee::Local<CellIndex> firstCell;
        ee::Local<CellIndex> lastCell;
        ee::Local<SelectionRecord> record;
        selectionNoop();
        const uint32_t parent = editObjectParentAnchor(ee::Ptr<SelectionRecord>(address)->object);
        record->object = ee::Ptr<SelectionRecord>(address)->object;
        record->anchor = parent;
        parkCellAtPosition(ee::Ptr<EditSelection>(selection)->park, anchorWorldPosition(parent), &record->cell);
        record->fits = 1;
        heldPieceCells(selection, record, firstCell, lastCell, anglesOfAnchor(record->anchor));
        return forEachSelectionCellInBox(selection, firstCell, lastCell, kReleaseHeldPieceIf, record->object);
    }
    READABLE_FUNCTION_WAITS(collectCellPiece, Verified);

    ee::Ptr<EditObjectPart> releaseHeldPieceIf(uint32_t /*selection*/, uint32_t address, ee::Ptr<EditObjectPart> object)
    {
        const ee::Ptr<SelectionRecord> record(address);
        const ee::Ptr<EditObjectPart> attached = attachedObject(record->anchor);
        if (attached == object)
            return game::tail(detachFromAnchor, record->anchor);
        return attached;
    }
    READABLE_FUNCTION(releaseHeldPieceIf, Verified);

    namespace
    {
        int32_t setEditObjectPosition(uint32_t piece, ee::Ptr<zge::Vec4> position) { return ee::game<int32_t>(kSetEditObjectPosition, piece, position); }
    }

    // alignPieceToSelection: the held piece at minus the facing, in its anchor's frame.
    void alignPieceToSelection(uint32_t selection, uint32_t address)
    {
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::EulerAngles> angles;
        ee::Local<zge::Vec4> turned;
        const ee::Ptr<SelectionRecord> record(address);
        const ee::Ptr<EditObjectPart> piece = record->object;
        if (piece == 0)
            return;
        const uint32_t parent = editObjectParentAnchor(piece);
        const ee::Ptr<zge::Matrix> anchorToWorld(static_cast<uint32_t>(anchorMatrix(parent)));
        zge::matrixToEuler(angles, anchorToWorld);
        angles->x = static_cast<zge::Angle>(-angles->x);
        angles->y = static_cast<zge::Angle>(-angles->y);
        angles->z = static_cast<zge::Angle>(-angles->z);
        zge::matrixIdentity(matrix);
        zge::matrixRotateXYZ(matrix, matrix, angles);
        *turned = zge::rotated(*matrix, ee::Ptr<EditSelection>(selection)->facing);
        turned->x = -turned->x;
        turned->y = -turned->y;
        turned->z = -turned->z;
        setEditObjectPosition(record->object, turned);
    }
    READABLE_FUNCTION_WAITS(alignPieceToSelection, Verified);

    int32_t setRecordAnchorFlag(uint32_t /*selection*/, uint32_t record, ee::Ptr<EditObjectPart> piece)
    {
        return game::tail(reserveAnchor, ee::Ptr<SelectionRecord>(record)->anchor, piece);
    }
    READABLE_FUNCTION(setRecordAnchorFlag, Verified);

    int32_t deleteRecordPiece(uint32_t /*selection*/, uint32_t record)
    {
        return game::tail(broadcastEditMessage, ee::Ptr<SelectionRecord>(record)->object, kMakeDeleteMessage, 0);
    }
    READABLE_FUNCTION_WAITS(deleteRecordPiece, Verified);

    int32_t freeHeldPieceCallback(uint32_t selection, uint32_t record)
    {
        return game::tail(freeHeldPiece, selection, record);
    }
    READABLE_FUNCTION_WAITS(freeHeldPieceCallback, Verified);

    int32_t offsetRecordCell(uint32_t /*selection*/, uint32_t record, uint32_t offset)
    {
        addCell(ee::Ptr<SelectionRecord>(record)->cell, *ee::Ptr<CellIndex>(offset));
        return ee::Ptr<CellIndex>(offset)->z;
    }
    READABLE_FUNCTION(offsetRecordCell, Verified);

    // heldPieceCells: the rectangle a piece covers at its record's cell, turned.
    void heldPieceCells(uint32_t /*selection*/, ee::Address address, ee::Address firstAddress, ee::Address lastAddress, uint32_t angles)
    {
        ee::Local<CellIndex> cellOffset;
        ee::Local<CellIndex> footprint;
        const ee::Ptr<SelectionRecord> record(address);
        const ee::Ptr<CellIndex> first(firstAddress), last(lastAddress);
        const uint32_t cell = address + offsetof(SelectionRecord, cell);
        editObjectCellOffset(record->object, cellOffset);
        editObjectFootprint(record->object, footprint);
        first->y = 0;
        first->x = record->cell.x - cellOffset->x;
        first->z = record->cell.z - cellOffset->z;
        last->y = 0;
        last->x = first->x + footprint->x - 1;
        last->z = first->z + footprint->z - 1;
        rotateCellOffset(firstAddress, cell, angles);
        rotateCellOffset(lastAddress, cell, angles);
    }
    READABLE_FUNCTION(heldPieceCells, Verified);

    // putHeldPiece: the held piece put down on its own cell; the cells of the
    // rectangle give their anchors to it.
    int32_t putHeldPiece(uint32_t selection, uint32_t address)
    {
        ee::Local<CellIndex> first;
        ee::Local<CellIndex> last;
        const ee::Ptr<SelectionRecord> held(address);
        heldPieceCells(selection, address, first, last, selection + offsetof(EditSelection, angles));
        orderedCellBounds(selection, first, last, first, last);
        forEachSelectionCellInBox(selection, first, last, kSetRecordAnchorFlag, held->object);
        const uint32_t anchor = selectionCellAt(selection, &held->cell);
        setAnchorAngles(anchor, anglesOfAnchor(held->anchor));
        detachFromAnchor(anchor);
        attachToAnchor(anchor, held->object);
        held->object = 0;
        return freeHeldPiece(selection, address);
    }
    READABLE_FUNCTION_WAITS(putHeldPiece, Verified);

    // placeHeldPieceChecked: as putHeldPiece for a copy of the piece, which stays held; one
    // that does not fit clears the caller's all-placed.
    int32_t placeHeldPieceChecked(uint32_t selection, uint32_t address, ee::Ptr<uint32_t> allPlaced)
    {
        ee::Local<CellIndex> first;
        ee::Local<CellIndex> last;
        const ee::Ptr<SelectionRecord> held(address);
        const int32_t fits = static_cast<int32_t>(held->fits);
        if (fits == 0)
        {
            *allPlaced = 0;
            return fits;
        }
        heldPieceCells(selection, address, first, last, selection + offsetof(EditSelection, angles));
        orderedCellBounds(selection, first, last, first, last);
        const ee::Ptr<EditObjectPart> copy = cloneEditObject(held->object);
        forEachSelectionCellInBox(selection, first, last, kSetRecordAnchorFlag, copy);
        const uint32_t anchor = selectionCellAt(selection, &held->cell);
        setAnchorAngles(anchor, anglesOfAnchor(held->anchor));
        detachFromAnchor(anchor);
        return attachToAnchor(anchor, copy);
    }
    READABLE_FUNCTION_WAITS(placeHeldPieceChecked, Verified);

    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress);

    // checkHeldPieceFits: every cell under the held piece has to take it; a refusal does
    // not stop the walk over the other rows.
    int32_t checkHeldPieceFits(uint32_t selection, uint32_t address, ee::Ptr<uint32_t> allFit)
    {
        ee::Local<CellIndex> first;
        ee::Local<CellIndex> last;
        ee::Local<CellIndex> cell;
        const ee::Ptr<SelectionRecord> held(address);
        held->fits = 1;
        heldPieceCells(selection, address, first, last, selection + offsetof(EditSelection, angles));
        orderedCellBounds(selection, first, last, first, last);
        for (cell->y = first->y; cell->y <= last->y; ++cell->y)
        {
            for (cell->z = first->z; cell->z <= last->z; ++cell->z)
            {
                for (cell->x = first->x; cell->x <= last->x; ++cell->x)
                {
                    if (cellAcceptsPiece(selection, cell, address) == 0)
                    {
                        held->fits = 0;
                        break;
                    }
                }
            }
        }
        const int32_t fits = static_cast<int32_t>(held->fits);
        if (fits == 0)
            *allFit = 0;
        return fits;
    }
    READABLE_FUNCTION(checkHeldPieceFits, Verified);

    // drawSelectionCell: a removable piece's box, and the cell as a flat box under the
    // facing's offset.
    int32_t drawSelectionCell(uint32_t address, uint32_t recordAddress, ee::Ptr<SelectionColours> colours)
    {
        ee::Local<zge::Box> box;
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<SelectionRecord> record(recordAddress);
        const ee::Ptr<EditObjectPart> piece = record->object;
        if (piece != 0 && editObjectFlag(piece, kFlagShowsBox) != 0)
        {
            editObjectWorldBox(record->object, selection->park, box);
            drawEditBoxLines(box, colourWord(colours->box));
        }
        uint32_t anchor = parkCellAnchor(selection->park, &record->cell);
        const ee::Ptr<EditObjectPart> attached = attachedObject(anchor);
        if (attached != 0 && zge::compareStrings(editObjectClassName(attached), kNameEditTowerObject) == 0)
            anchor = firstAnchorIn(editObjectAnchors(attached));
        const ee::Ptr<zge::Vec4> position(static_cast<uint32_t>(anchorWorldPosition(anchor)));
        const ee::Ptr<zge::Vec4> size(cellSizeOf(selection->park));
        box->high.x = position->x + size->x * ee::Float(0.5f);
        box->high.y = position->y - kBoxTop;
        box->high.z = position->z + size->z * ee::Float(0.5f);
        box->low.x = position->x - size->x * ee::Float(0.5f);
        box->low.y = position->y - kBoxBottom;
        box->low.z = position->z - size->z * ee::Float(0.5f);
        box->low = box->low - selection->facing;
        box->high = box->high - selection->facing;
        return drawEditBox(box, colourWord(colours->box));
    }
    READABLE_FUNCTION_WAITS(drawSelectionCell, Verified);

    void drawHeldPiece(uint32_t address, uint32_t recordAddress, ee::Ptr<SelectionColours> colours)
    {
        ee::Local<zge::Box> box;
        const ee::Ptr<SelectionRecord> held(recordAddress);
        const ee::Ptr<EditObjectPart> piece = held->object;
        if (piece != 0 && editObjectFlag(piece, kFlagShowsBox) != 0)
        {
            editObjectWorldBox(held->object, ee::Ptr<EditSelection>(address)->park, box);
            drawEditBoxLines(box, colourWord(held->fits != 0 ? colours->heldFit : colours->heldMisfit));
        }
    }
    READABLE_FUNCTION_WAITS(drawHeldPiece, Verified);

    int32_t positionHeldPiece(uint32_t selection, uint32_t address, uint32_t angles)
    {
        rotateAnchor(ee::Ptr<SelectionRecord>(address)->anchor, angles);
        return game::tail(rotateCellOffset, address + offsetof(SelectionRecord, cell), selection + offsetof(EditSelection, cursorCell), angles);
    }
    READABLE_FUNCTION_WAITS(positionHeldPiece, Verified);

    int32_t cellInSelectionBox(uint32_t address, ee::Address cellAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<CellIndex> cell(cellAddress);
        if (selection->boundsMax.x >= cell->x && cell->x >= selection->boundsMin.x && selection->boundsMax.z >= cell->z)
        {
            const int32_t within = !(cell->z < selection->boundsMin.z);
            return within;
        }
        return 0;
    }
    READABLE_FUNCTION(cellInSelectionBox, Verified);

    // pieceSelectable: a tower always; another piece if both its corners are in the box.
    uint32_t pieceSelectable(uint32_t address, ee::Ptr<EditObjectPart> piece)
    {
        ee::Local<CellIndex> firstCell;
        ee::Local<CellIndex> lastCell;
        ee::Local<SelectionRecord> record;
        if (zge::compareStrings(kNameEditTowerObject, editObjectClassName(piece)) == 0)
            return 1;
        const uint32_t parent = editObjectParentAnchor(piece);
        if (parent == 0)
            return 1;
        record->object = piece;
        record->fits = 1;
        record->anchor = parent;
        parkCellAtPosition(ee::Ptr<EditSelection>(address)->park, anchorWorldPosition(parent), &record->cell);
        heldPieceCells(address, record, firstCell, lastCell, anglesOfAnchor(parent));
        if (cellInSelectionBox(address, firstCell) == 0)
            return 0;
        return cellInSelectionBox(address, lastCell) != 0 ? 1u : 0u;
    }
    READABLE_FUNCTION(pieceSelectable, Verified);

    // pieceTreeSelectable: the piece and, depth first, what stands on its anchors.
    int32_t pieceTreeSelectable(uint32_t selection, ee::Ptr<EditObjectPart> piece)
    {
        const uint32_t anchors = editObjectAnchors(piece);
        const int32_t selectable = static_cast<int32_t>(pieceSelectable(selection, piece));
        if (selectable == 0 || anchors == 0)
            return selectable;
        for (uint32_t anchor = firstAnchorIn(anchors); anchor != 0; anchor = nextAnchorAfter(anchor))
        {
            const ee::Ptr<EditObjectPart> onAnchor = attachedObject(anchor);
            if (onAnchor != 0 && pieceTreeSelectable(selection, onAnchor) == 0)
                return 0;
        }
        return selectable;
    }
    READABLE_FUNCTION(pieceTreeSelectable, Verified);

    int32_t repositionHeldPiece(uint32_t selection, uint32_t address)
    {
        const uint32_t anchor = selectionCellAt(selection, &ee::Ptr<SelectionRecord>(address)->cell);
        const int32_t position = anchorWorldPosition(anchor);
        return game::tail(setAnchorPosition, ee::Ptr<SelectionRecord>(address)->anchor, position);
    }
    READABLE_FUNCTION_WAITS(repositionHeldPiece, Verified);

    // placePieceWithSelection: a piece put down at once where its anchor is: the selection
    // goes there, holds the piece and puts it down.
    int32_t placePieceWithSelection(uint32_t address, ee::Ptr<EditObjectPart> piece, uint32_t anchor)
    {
        ee::Local<CellIndex> cell;
        ee::Local<uint32_t> userData;
        const ee::Ptr<EditSelection> selection(address);
        *userData = 1;
        resetSelection(address);
        clearSelectionRotation(address);
        selection->angles = *ee::Ptr<AngleWords>(anglesOfAnchor(anchor));
        parkCellAtPosition(selection->park, anchorWorldPosition(anchor), cell);
        selection->cursorCell = *cell;
        selection->rectangleStart = *cell;
        selection->rectangleEnd = *cell;
        holdPiece(address, piece);
        return forEachHeldPiece(address, kPutHeldPiece, userData);
    }
    READABLE_FUNCTION_WAITS(placePieceWithSelection, Verified);

    int32_t forEachSelectedCell(uint32_t selection, uint32_t callback, uint32_t userData)
    {
        const ee::Ptr<EditSelection> part(selection);
        return game::tail(forEachSelectionCellInBox, selection, &part->boundsMin, &part->boundsMax, callback, userData);
    }
    READABLE_FUNCTION_WAITS(forEachSelectedCell, Verified);

    // forEachSelectionCellInBox: the box's cells in the park, each handed to the callback as a
    // record: y, then z, then x.
    int32_t forEachSelectionCellInBox(uint32_t address, ee::Ptr<CellIndex> firstCell, ee::Ptr<CellIndex> secondCell, RecordWalk callback, ee::Address userData)
    {
        ee::Local<CellIndex> minimum;
        ee::Local<CellIndex> maximum;
        ee::Local<CellIndex> cell;
        ee::Local<SelectionRecord> record;
        const ee::Ptr<EditSelection> selection(address);
        orderedCellBounds(address, firstCell, secondCell, minimum, maximum);
        const bool noRows = maximum->y < minimum->y;
        for (cell->y = minimum->y; cell->y <= maximum->y; ++cell->y)
        {
            for (cell->z = minimum->z; cell->z <= maximum->z; ++cell->z)
            {
                for (cell->x = minimum->x; cell->x <= maximum->x; ++cell->x)
                {
                    if (parkCellFree(selection->park, cell) != 0)
                    {
                        const uint32_t anchor = selectionCellAt(address, cell);
                        record->anchor = anchor;
                        record->object = attachedObject(anchor);
                        record->cell = *cell;
                        record->fits = 1;
                        callback(address, record, userData);
                    }
                }
            }
        }
        // No caller reads this: 1 for an empty box, else the row past the last.
        return noRows ? 1 : cell->y;
    }
    READABLE_FUNCTION(forEachSelectionCellInBox, Verified);

    // forEachPoolRecord: over a pool's records; the callback may free its own.
    int32_t forEachPoolRecord(uint32_t selection, uint32_t pool, RecordWalk callback, uint32_t userData)
    {
        int32_t result = 0; // with no records: what the original returns then, its first link, 0
        ee::Ptr<zge::ListNode> link = zge::firstPoolLink(ee::Ptr<zge::Pool>(pool));
        while (link)
        {
            const ee::Ptr<zge::ListNode> next = zge::nextPoolLink(link);
            result = callback(selection, poolRecordAs<SelectionRecord>(link), userData);
            link = next;
        }
        return result;
    }
    READABLE_FUNCTION(forEachPoolRecord, Verified);

    int32_t forEachHeldPiece(uint32_t selection, uint32_t callback, ee::Address userData)
    {
        return game::tail(forEachPoolRecord, selection, ee::Ptr<EditSelection>(selection)->heldPool, callback, userData);
    }
    READABLE_FUNCTION_WAITS(forEachHeldPiece, Verified);

    uint32_t selectionTarget(uint32_t selection)
    {
        return selection;
    }
    READABLE_FUNCTION(selectionTarget, Verified);

    // selectionInPark: the cursor and the rectangle's two corners are all in the park.
    int32_t selectionInPark(uint32_t address)
    {
        const ee::Ptr<EditSelection> selection(address);
        if (parkCellValid(selection->park, &selection->cursorCell) == 0)
            return 0;
        if (parkCellValid(selection->park, &selection->rectangleStart) == 0)
            return 0;
        const int32_t inPark = parkCellValid(selection->park, &selection->rectangleEnd) != 0;
        return inPark;
    }
    READABLE_FUNCTION(selectionInPark, Verified);

    uint32_t heldPiecesFit(uint32_t selection)
    {
        ee::Local<uint32_t> userData;
        *userData = 1;
        forEachHeldPiece(selection, kCheckHeldPieceFits, userData);
        return *userData;
    }
    READABLE_FUNCTION_WAITS(heldPiecesFit, Verified);

    int32_t canPlaceHeldPieces(uint32_t selection)
    {
        return static_cast<int32_t>(heldPiecesFit(selection));
    }
    READABLE_FUNCTION_WAITS(canPlaceHeldPieces, Verified);

    uint32_t cursorAnchor(uint32_t selection)
    {
        return selectionCellAt(selection, &ee::Ptr<EditSelection>(selection)->cursorCell);
    }
    READABLE_FUNCTION(cursorAnchor, Verified);
}
