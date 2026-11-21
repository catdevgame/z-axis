// DM2's park editor: the editor core. It owns the editor's record
// (park_editor.h): the cursor and the rectangle of cells it selects, the
// tool entries the top header rolls through, and the park's save stream.
// DM2's own code (zgeif/zoverlay.c in its build).
#pragma once

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "park_editor.h"
#include "zge/container/list.h"
#include "zge/input/pads.h"
#include "zge/math/vec.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    struct EditObjectDef;

    // One entry of the tool roll: a tool, and for the piece tool one
    // category of the catalog. The catalog adds an entry for each category,
    // the themes and the lights add theirs.
    constexpr uint8_t kToolEntryNoCategory = 0xFE;
    struct EditToolEntry
    {
        zge::ListNode link;                // +0x00
        uint8_t tool;                      // +0x08 kEditTool...
        uint8_t category;                  // +0x09 the piece category, kToolEntryNoCategory for the other tools
        char name[0x1A];                   // +0x0A
        uint32_t enabled;                  // +0x24
        ee::Ptr<EditObjectDef> heldObject; // +0x28 the object to hold when the entry is taken, 0: the category's first
    };
    static_assert(offsetof(EditToolEntry, tool) == 0x08 && offsetof(EditToolEntry, name) == 0x0A);
    static_assert(offsetof(EditToolEntry, enabled) == 0x24 && sizeof(EditToolEntry) == 0x2C);

    // The park's save stream: a block of bytes written or read in order.
    struct ParkStream
    {
        ee::Ptr<uint8_t> bytes; // +0x00
        uint32_t position;      // +0x04
        uint32_t size;          // +0x08
    };
    static_assert(sizeof(ParkStream) == 0x0C);

    // What forEachSelectedCell calls for every cell of the selected
    // rectangle: the cell, or 0 for a place that cannot be built on.
    using SelectedCellVisit = ee::FunctionPtr<void(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Address data)>;

    // The core set up for a park and its tool entries freed.
    void initEditorCore(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::Pad> pad);
    void freeEditToolEntries(ee::Ptr<ParkEditor> editor);
    void updateEditorCore(ee::Ptr<ParkEditor> editor);
    void moveEditViewByPad(ee::Ptr<ParkEditor> editor);
    void toolButtonsFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void terrainToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void pieceToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void railToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void startToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void gapToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);
    void copyToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);

    // The cursor.
    void centreCursor(ee::Ptr<ParkEditor> editor);
    void setCursorState(ee::Ptr<ParkEditor> editor, uint8_t state);
    uint32_t cursorFree(ee::Ptr<ParkEditor> editor);
    void cursorWorldPosition(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::Vec4> position);
    void cursorCell(ee::Ptr<ParkEditor> editor, ee::Ptr<uint8_t> column, ee::Ptr<uint8_t> row);
    // The cursor put on a cell, while it selects no more than its own cell.
    void moveCursorToCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row);
    // The cursor put on the own cell of the object under it.
    void moveCursorToObjectCell(ee::Ptr<ParkEditor> editor);

    uint32_t stepCursor(ee::Ptr<ParkEditor> editor, uint8_t direction, uint8_t stretch, uint8_t follow);
    void moveCursor(ee::Ptr<ParkEditor> editor, uint8_t direction, uint8_t stretch, uint8_t follow);
    void moveCursorByPad(ee::Ptr<ParkEditor> editor, uint32_t changed, uint32_t held, uint8_t stretch, uint8_t follow);
    void moveToolCursor(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed);

    // The selected rectangle.
    void resetSelection(ee::Ptr<ParkEditor> editor);
    void resetCursorRules(ee::Ptr<ParkEditor> editor);
    // The selection made the cells the held object would stand on.
    void selectHeldObjectCells(ee::Ptr<ParkEditor> editor);
    uint32_t cellInSelection(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row);
    // The selection grown to hold the cell: 1 if it grew.
    uint32_t stretchCursorToCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row);
    void forEachSelectedCell(ee::Ptr<ParkEditor> editor, SelectedCellVisit visit, ee::Address data);
    // 1 if every selected cell can be built on.
    uint32_t heldPiecesFit(ee::Ptr<ParkEditor> editor);

    // The visits the core hands to forEachSelectedCell.
    void refitCellTower(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell);
    void growSelectionOverCellObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> grew);
    void requireBuildableCell(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> allBuildable);
    void noteCellAtGroundLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found);
    void noteCellAtTopLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found);
    void changeCellLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, uint32_t by);
    void noteLevelNeighbourOutside(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found);
    void eraseCellContents(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell);

    // The tool entries.
    void addEditToolEntry(uint8_t tool, uint8_t category, ee::Ptr<const char> name, uint32_t enabled);
    void setEditToolEntryEnabled(ee::Ptr<const char> name, uint32_t enabled);
    ee::Ptr<char> currentToolEntryName();
    // The entry's tool taken up, and its category's object held.
    void useEditToolEntry(ee::Ptr<EditToolEntry> entry);
    void selectEditToolEntry();
    void selectFirstEditToolEntry();
    uint8_t currentToolCategory();
    void heldObjectChanged(uint8_t object, uint8_t rotation);
    void rewindToolRoll(ee::Ptr<ParkEditor> editor);
    uint32_t refuseWhenParkFull(uint32_t object);

    // The park's stream.
    ee::Ptr<ParkStream> newParkStream(uint32_t bytes);
    void freeParkStream(ee::Ptr<ParkStream> stream);
    void parkStreamWrite(ee::Address stream, ee::Address from, uint32_t count);
    void parkStreamRead(ee::Address stream, ee::Address to, uint32_t count);
    // The whole park through its stream.
    void loadPark(ee::Ptr<ParkEditor> editor, ee::Address stream);
    void savePark(ee::Ptr<ParkEditor> editor, ee::Address stream);
}
