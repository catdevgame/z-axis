// DM2's park editor: the copy tool's cells (copy_tool.h). DM2's own code
// (its editor block).

#include "copy_tool.h"
#include "editor_state.h"
#include "edit_core.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "edit_lights.h"
#include "edit_objects.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/copy_tool.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The copied cells, [row * kCopySpan + column], and the scratch of
        // the same size; how many rows and columns are copied, and the
        // quarter turns given to them since.
        inline ee::Global<ee::Ptr<ParkCell>, sym::g_copiedCells> g_copiedCells;
        inline ee::Global<ee::Ptr<ParkCell>, sym::g_copyScratch> g_copyScratch;
        inline ee::Global<uint32_t, sym::g_copiedRows> g_copiedRows;
        inline ee::Global<uint32_t, sym::g_copiedColumns> g_copiedColumns;
        inline ee::Global<uint32_t, sym::g_copiedTurns> g_copiedTurns;

        constexpr uint32_t kCopyBufferBytes = kCopySpan * kCopySpan * sizeof(ParkCell);

    }

    void clearCopiedCells(ee::Ptr<ParkEditor>)
    {
        for (uint32_t row = 0; row < kCopySpan; ++row)
        {
            for (uint32_t column = 0; column < kCopySpan; ++column)
            {
                const uint32_t cell = row * kCopySpan + column;
                if ((*g_copiedCells)[cell].object)
                {
                    releaseEditNode(&(*g_copiedCells)[cell].object->node);
                    zge::heapFree((*g_copiedCells)[cell].object);
                }
                (*g_copiedCells)[cell].object = ee::Ptr<PlacedObject>();
            }
        }
        *g_copiedRows = 0;
        *g_copiedColumns = 0;
    }
    READABLE_FUNCTION(clearCopiedCells, Verified);

    void allocateCopyBuffers()
    {
        *g_copiedCells = ee::Ptr<ParkCell>(zge::heapAllocate(kCopyBufferBytes));
        *g_copyScratch = ee::Ptr<ParkCell>(zge::heapAllocate(kCopyBufferBytes));
        for (uint32_t row = 0; row < kCopySpan; ++row)
            for (uint32_t column = 0; column < kCopySpan; ++column)
                (*g_copiedCells)[row * kCopySpan + column].object = ee::Ptr<PlacedObject>();
        *g_copiedRows = 0;
        *g_copiedColumns = 0;
        *g_copiedTurns = 0;
    }
    READABLE_FUNCTION(allocateCopyBuffers, Verified);

    void freeCopyBuffers()
    {
        clearCopiedCells(parkEditor());
        zge::heapFree(*g_copiedCells);
        game::tail(zge::heapFree, *g_copyScratch);
    }
    READABLE_FUNCTION(freeCopyBuffers, Verified);

    namespace
    {
        // The catalog (edit_objects.cpp): an object's world model by its
        // index.
        inline ee::Global<ee::Ptr<ee::Ptr<EditObjectDef>>, sym::g_editObjects> g_editObjects;
        inline ee::Global<ee::Float, sym::g_parkCapacityUsed> g_parkCapacityUsed;
        // The copied square's size and turns as they were before a turn.
        inline ee::Global<uint32_t, sym::g_copyRowsBeforeTurn> g_rowsBeforeTurn;
        inline ee::Global<uint32_t, sym::g_copyColumnsBeforeTurn> g_columnsBeforeTurn;
        inline ee::Global<uint32_t, sym::g_copyTurnsBeforeTurn> g_turnsBeforeTurn;

        constexpr ee::Float kQuarterTurnDegrees = 90.0f;
        constexpr uint32_t kTurnsTried = 4;

        // Scaffolding (frame-exact): what copySelectedCells' compiler kept
        // in its frame beside the two locals.
        struct CopyFrame
        {
            CellBox selection;                    // +0x00
            uint8_t _18[8];
            zge::Vec4 position;                   // +0x20
            ee::Ptr<zge::Vec4> keptPosition;      // +0x30
            uint32_t keptNextCopyRow;             // +0x34
            ee::Ptr<ee::Ptr<PlacedObject>> keptCellObjects; // +0x38 the first cell's object word
        };
        static_assert(offsetof(CopyFrame, position) == 0x20 && offsetof(CopyFrame, keptPosition) == 0x30);

        struct PasteFrame
        {
            CellBox cursor;     // +0x00
            uint8_t _18[8];
            CellBox objectBox;  // +0x20
            uint8_t _38[8];
            zge::Vec4 position; // +0x40
        };
        static_assert(offsetof(PasteFrame, objectBox) == 0x20 && offsetof(PasteFrame, position) == 0x40);

        ee::Ptr<ParkCell> copiedCell(uint32_t row, uint32_t column)
        {
            return *g_copiedCells + static_cast<ptrdiff_t>(row * kCopySpan + column);
        }

        // A copy of a placed object's record (the engine's heap) with a
        // node of its own, shown on the cell and turned as the object is.
        ee::Ptr<PlacedObject> shownCopy(ee::Ptr<ParkEditor> editor, ee::Ptr<PlacedObject> source, int32_t column, int32_t row,
                                        ee::Ptr<zge::Vec4> position, bool onCell)
        {
            const ee::Ptr<PlacedObject> copy(zge::heapAllocate(sizeof(PlacedObject)));
            *copy = *source;
            const ee::Ptr<zge::Node> node = newWorldObjectInstance((*g_editObjects)[copy->object]->model);
            if (onCell)
            {
                copy->column = static_cast<uint8_t>(column);
                copy->row = static_cast<uint8_t>(row);
            }
            copy->rails = ee::Ptr<CellRails>();
            copy->flags = 0;
            copy->node = node;
            cellWorldPosition(editor, static_cast<uint32_t>(column), static_cast<uint32_t>(row), position);
            setNodePosition(copy->node, position);
            setNodeHeading(copy->node, ee::toFloat(copy->rotation) * kQuarterTurnDegrees);
            return copy;
        }

        // An unsigned number as a float, the way the compiler wrote it.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1) | (value >> 1)));
            return half + half;
        }
    }

    void copySelectedCells(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CopyFrame> frame;
        cursorCellBox(editor, &frame->selection);
        *g_copiedRows = static_cast<uint32_t>(frame->selection.lastRow - frame->selection.firstRow + 1);
        *g_copiedTurns = 0;
        *g_copiedColumns = static_cast<uint32_t>(frame->selection.lastColumn - frame->selection.firstColumn + 1);
        if (frame->selection.lastRow < frame->selection.firstRow)
            return;
        frame->keptPosition = &frame->position;
        uint32_t copyRow = 0;
        for (int32_t row = frame->selection.firstRow; row <= frame->selection.lastRow; ++row, ++copyRow)
        {
            int32_t column = frame->selection.firstColumn;
            frame->keptNextCopyRow = copyRow + 1;
            if (frame->selection.lastColumn < column)
                continue;
            frame->keptCellObjects = &editor->cells[0][0].object;
            for (uint32_t copyColumn = 0; column <= frame->selection.lastColumn; ++column, ++copyColumn)
            {
                const ee::Ptr<PlacedObject> object = parkCell(editor, column, row)->object;
                if (object && column == object->column && row == object->row)
                    copiedCell(copyRow, copyColumn)->object = shownCopy(editor, object, column, row, frame->keptPosition, false);
                else
                    copiedCell(copyRow, copyColumn)->object = ee::Ptr<PlacedObject>();
            }
        }
    }
    READABLE_FUNCTION(copySelectedCells, Verified);

    void pasteCopiedCells(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<PasteFrame> frame;
        cursorCellBox(editor, &frame->cursor);
        uint32_t copyRow = 0;
        for (int32_t row = frame->cursor.firstRow; row <= frame->cursor.lastRow; ++row, ++copyRow)
        {
            uint32_t copyColumn = 0;
            for (int32_t column = frame->cursor.firstColumn; column <= frame->cursor.lastColumn; ++column, ++copyColumn)
            {
                const ee::Ptr<PlacedObject> copied = copiedCell(copyRow, copyColumn)->object;
                if (copied && refuseWhenParkFull(copied->object))
                    return;
            }
        }
        copyRow = 0;
        for (int32_t row = frame->cursor.firstRow; row <= frame->cursor.lastRow; ++row, ++copyRow)
        {
            uint32_t copyColumn = 0;
            for (int32_t column = frame->cursor.firstColumn; column <= frame->cursor.lastColumn; ++column, ++copyColumn)
            {
                if (!copiedCell(copyRow, copyColumn)->object)
                    continue;
                const ee::Ptr<PlacedObject> placed = shownCopy(editor, copiedCell(copyRow, copyColumn)->object, column, row, &frame->position, true);
                *g_parkCapacityUsed = *g_parkCapacityUsed + unsignedToFloat(placedObjectCost(placed));
                placedObjectCellBox(placed, &frame->objectBox);
                for (int32_t coveredRow = frame->objectBox.firstRow; coveredRow <= frame->objectBox.lastRow; ++coveredRow)
                    for (int32_t coveredColumn = frame->objectBox.firstColumn; coveredColumn <= frame->objectBox.lastColumn; ++coveredColumn)
                        parkCell(editor, coveredColumn, coveredRow)->object = placed;
                addNodeLights(placed->node);
                lightNodeVertices(placed->node, 1);
            }
        }
    }
    READABLE_FUNCTION(pasteCopiedCells, Verified);

    void turnCopiedCells(ee::Ptr<ParkEditor> editor)
    {
        // The cursor's rectangle a quarter turn about the selection's
        // corner, until it fits.
        uint32_t turns = 0;
        uint32_t fits;
        do
        {
            ++turns;
            const int8_t columns = editor->selectionColumns;
            const int8_t cursorColumn = editor->cursorColumn;
            const int8_t rows = editor->selectionRows;
            editor->selectionRows = static_cast<int8_t>(-columns);
            editor->selectionColumns = rows;
            editor->cursorColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionColumn) + (editor->cursorRow - editor->selectionRow));
            editor->cursorRow = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionRow) + (editor->selectionColumn - cursorColumn));
            fits = heldPiecesFit(editor);
        } while (!fits && turns < kTurnsTried);
        if (!(turns < kTurnsTried) || !fits)
            return;

        // The copied square the same number of quarter turns: through the
        // scratch, a cell at (row, column) to (columns - 1 - column, row).
        for (; turns != 0; --turns)
        {
            const uint32_t turnsBefore = *g_copiedTurns;
            *g_columnsBeforeTurn = *g_copiedColumns;
            *g_rowsBeforeTurn = *g_copiedRows;
            *g_copiedTurns = (turnsBefore - 1) & 3;
            *g_turnsBeforeTurn = turnsBefore;
            for (uint32_t row = 0; row < kCopySpan; ++row)
            {
                for (uint32_t column = 0; column < kCopySpan; ++column)
                {
                    (*g_copyScratch)[row * kCopySpan + column] = *copiedCell(row, column);
                    copiedCell(row, column)->object = ee::Ptr<PlacedObject>();
                }
            }
            for (uint32_t row = 0; row < *g_rowsBeforeTurn; ++row)
            {
                for (uint32_t column = 0; column < *g_columnsBeforeTurn; ++column)
                {
                    const ee::Ptr<ParkCell> turned = copiedCell(*g_columnsBeforeTurn - (column + 1), row);
                    turned->object = (*g_copyScratch)[row * kCopySpan + column].object;
                    if (turned->object)
                    {
                        turned->object->rotation = static_cast<uint8_t>(turned->object->rotation + 1);
                        turned->object->rotation = turned->object->rotation & 3;
                    }
                }
            }
            *g_copiedColumns = *g_rowsBeforeTurn;
            *g_copiedRows = *g_columnsBeforeTurn;
        }
    }
    READABLE_FUNCTION(turnCopiedCells, Verified);

    namespace
    {
        constexpr uint8_t kCopyFollowsCursor = 1; // the copy tool's second step (ParkEditor::toolStep)
        constexpr int32_t kTintPulseRate = 0x400;
        constexpr ee::Float kTintPulseSwing = 112.0f;
        constexpr ee::Float kTintPulseMiddle = 128.0f;
        constexpr uint8_t kTintAlpha = 0xE0;

        // Scaffolding (frame-exact): showCopiedCellsAtCursor's frame as its
        // compiler laid it out: the loops' counters and steps are kept in
        // it beside the locals.
        struct ShowCopyFrame
        {
            CellBox cursor;                    // +0x00
            uint8_t _18[8];
            zge::VertexColour tint;            // +0x20
            uint8_t _24[0x0C];
            zge::Vec4 position;                // +0x30
            int32_t row;                       // +0x40
            int32_t columnStep;                // +0x44 1, or -1 for an object that spans backwards
            int32_t rowStep;                   // +0x48
            int32_t level;                     // +0x4C the ground's level at the object's cell
            uint32_t blocked;                  // +0x50
            ee::Ptr<zge::Vec4> keptPosition;   // +0x54
            ee::Ptr<zge::VertexColour> keptTint; // +0x58
            int32_t nextRow;                   // +0x5C
            int32_t nextColumn;                // +0x60
            uint32_t nextCopyRow;              // +0x64
            uint32_t nextCopyColumn;           // +0x68
            uint32_t copyRowStart;             // +0x6C the copied row's first cell's index
        };
        static_assert(offsetof(ShowCopyFrame, tint) == 0x20 && offsetof(ShowCopyFrame, position) == 0x30);
        static_assert(offsetof(ShowCopyFrame, row) == 0x40 && offsetof(ShowCopyFrame, copyRowStart) == 0x6C);

        uint8_t tintPulse()
        {
            return static_cast<uint8_t>(ee::toInt(kTintPulseMiddle - pulseCosine(kTintPulseRate) * kTintPulseSwing));
        }
    }

    void showCopiedCellsAtCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<ShowCopyFrame> frame;
        cursorCellBox(editor, &frame->cursor);
        if (editor->toolStep != kCopyFollowsCursor)
            return;
        frame->row = frame->cursor.firstRow;
        if (frame->cursor.lastRow < frame->row)
            return;
        uint32_t copyRow = 0;
        do
        {
            int32_t column = frame->cursor.firstColumn;
            uint32_t copyColumn = 0;
            if (frame->cursor.lastColumn < column)
            {
                frame->nextCopyRow = copyRow + 1;
                frame->nextRow = frame->row + 1;
            }
            else
            {
                frame->nextCopyRow = copyRow + 1;
                frame->copyRowStart = copyRow * kCopySpan;
                frame->nextRow = frame->row + 1;
                do
                {
                    const ee::Ptr<PlacedObject> object = (*g_copiedCells)[frame->copyRowStart + copyColumn].object;
                    if (!object)
                    {
                        frame->nextCopyColumn = copyColumn + 1;
                        frame->nextColumn = column + 1;
                    }
                    else
                    {
                        frame->keptPosition = &frame->position;
                        frame->rowStep = 1;
                        cellWorldPosition(editor, static_cast<uint32_t>(column), static_cast<uint32_t>(frame->row), frame->keptPosition);
                        frame->columnStep = 1;
                        frame->blocked = 0;
                        const int32_t rows = editObjectSpanY(object->object, object->rotation);
                        const int32_t columns = editObjectSpanX(object->object, object->rotation);
                        if (columns < 0)
                            frame->columnStep = -1;
                        if (rows < 0)
                            frame->rowStep = -1;
                        frame->level = parkCell(editor, column, frame->row)->level;
                        setCursorState(editor, kCursorFree);
                        frame->nextCopyColumn = copyColumn + 1;
                        frame->keptTint = &frame->tint;
                        frame->nextColumn = column + 1;
                        // Every cell the object would stand on must be bare
                        // ground at its own cell's level, without rails.
                        if (rows != 0)
                        {
                            int32_t rowOffset = 0;
                            do
                            {
                                if (columns != 0)
                                {
                                    int32_t columnOffset = 0;
                                    do
                                    {
                                        const ee::Ptr<ParkCell> cell = parkCell(editor, column + columnOffset, frame->row + rowOffset);
                                        if (cell->object || frame->level != cell->level || cell->rails)
                                        {
                                            frame->blocked = 1;
                                            setCursorState(editor, kCursorBlocked);
                                        }
                                        columnOffset += frame->columnStep;
                                    } while (columnOffset != columns);
                                }
                                rowOffset += frame->rowStep;
                            } while (rowOffset != rows);
                        }
                        setNodePosition(object->node, frame->keptPosition);
                        setNodeHeading(object->node, ee::toFloat(object->rotation) * kQuarterTurnDegrees);
                        if (frame->blocked)
                        {
                            frame->tint.red = 0xFF;
                            frame->tint.green = tintPulse();
                            const uint8_t pulse = tintPulse();
                            frame->tint.alpha = kTintAlpha;
                            frame->tint.blue = pulse;
                        }
                        else
                        {
                            frame->tint.red = tintPulse();
                            const uint8_t pulse = tintPulse();
                            frame->tint.alpha = kTintAlpha;
                            frame->tint.blue = 0xFF;
                            frame->tint.green = pulse;
                        }
                        setNodeColour(object->node, frame->keptTint);
                    }
                    column = frame->nextColumn;
                    copyColumn = frame->nextCopyColumn;
                } while (!(frame->cursor.lastColumn < column));
            }
            frame->row = frame->nextRow;
            copyRow = frame->nextCopyRow;
        } while (!(frame->cursor.lastRow < frame->row));
    }
    READABLE_FUNCTION(showCopiedCellsAtCursor, Verified);
}
