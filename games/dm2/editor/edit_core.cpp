// DM2's park editor: the editor core (edit_core.h): the cursor, the selected
// rectangle, the tool entries and the park's stream. DM2's own code.

#include "edit_core.h"
#include "editor_state.h"
#include "copy_tool.h"
#include "edit_camera.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "edit_gui.h"
#include "edit_lights.h"
#include "edit_objects.h"
#include "edit_pools.h"
#include "edit_themes.h"
#include "gap_tool.h"
#include "ground_towers.h"
#include "park_grid.h"
#include "park_scores.h"
#include "rail_tool.h"
#include "start_tool.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/input/pads.h"
#include "zge/memory/regions.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/editor/edit_core.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The cursor's part of the editor's record, cursorColumn up to the
        // tool's rules for it: a move that fails puts it back whole.
        struct CursorBlock
        {
            uint8_t bytes[offsetof(ParkEditor, levelRepeat) - offsetof(ParkEditor, cursorColumn)];
        };
        static_assert(sizeof(CursorBlock) == 0x24);
        ee::Ptr<CursorBlock> cursorBlock(ee::Ptr<ParkEditor> editor)
        {
            return ee::Ptr<int8_t>(&editor->cursorColumn).cast<CursorBlock>();
        }

        // The tool entries: their pool (an index in the editor's pool
        // table), the list in the order they were added, and the one in use.
        inline ee::Global<int8_t, sym::g_toolEntryPool> g_toolEntryPool;
        inline ee::Global<zge::List, sym::g_toolEntries> g_toolEntries;
        inline ee::Global<ee::Ptr<EditToolEntry>, sym::g_currentToolEntry> g_currentToolEntry;
        // 1 while the piece tool's copy of the cursor waits to be taken back
        // (the core's frame does that).
        inline ee::Global<uint8_t, sym::g_cursorSaved> g_cursorSaved;
        inline ee::Global<CursorBlock, sym::g_savedCursor> g_savedCursor;
        inline ee::Global<CursorBlock, sym::g_cursorScratch> g_cursorScratch;
        // Set by a tool's frame that used the pad's buttons itself: the
        // frame then leaves the tool and the cursor alone.
        inline ee::Global<uint32_t, sym::g_inputTaken> g_inputTaken;
        // The tool the copy tool was taken up from, to go back to.
        inline ee::Global<uint8_t, sym::g_toolBeforeCopy> g_toolBeforeCopy;

        constexpr uint32_t kToolEntryPoolRecords = 0x20;
        // The ground's levels run from 0 to this.
        constexpr int8_t kTopLevel = 30;

        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<ParkLights, sym::g_parkLights> g_parkLights;
        inline ee::Global<ee::Float, sym::g_parkCapacityUsed> g_parkCapacityUsed;
        inline ee::Global<ee::Float, sym::g_parkCapacity> g_parkCapacity;
        // A quarter turn for every 90 degrees of the view's heading.
        inline ee::Global<ee::Float, sym::g_quarterTurnsADegree> g_quarterTurnsADegree;

        constexpr ee::Ptr<const char> kStopFrameTitlePath{sym::textParkFullTitlePath}; // "ParkEditor.StopFrame.Title"
        constexpr ee::Ptr<const char> kStopFramePath{sym::textParkFullFramePath};      // "ParkEditor.StopFrame"
        constexpr ee::Ptr<const char> kIconRollPath{sym::textIconRollPath};            // "ParkEditor.MainUI.TopHeader.IconRoll"
        constexpr ee::Ptr<const char> kTextRollAngle{sym::textRollAngle};              // "RollAngle"
        constexpr ee::Ptr<const char> kPauseMenuPath{sym::textPauseMenuPath};          // "ParkEditor.MainUI.PauseMenu"
        constexpr ee::Ptr<const char> kTopHeaderPath{sym::textTopHeaderPath};          // "ParkEditor.MainUI.TopHeader"
        constexpr ee::Ptr<const char> kTextScrollLeft{sym::textScrollLeft};            // "ScrollLeft"
        constexpr ee::Ptr<const char> kTextScrollRight{sym::textScrollRight};          // "ScrollRight"
        constexpr ee::Ptr<const char> kTapeFramePath{sym::textTapeFramePath};          // "ParkEditor.MainUI.TapeFrame"
        constexpr ee::Ptr<const char> kTapePath{sym::textCoreTapePath};                    // "ParkEditor.MainUI.TapeFrame.Tape"
        constexpr ee::Ptr<const char> kTextDistance{sym::textDistance};                // "Distance"
        constexpr ee::Ptr<const char> kTextDeInit{sym::textCoreDeInit};                // "DeInit"
        constexpr ee::Ptr<const char> kTextText{sym::textCoreText};                    // "Text"
        constexpr ee::Ptr<const char> kTextInit{sym::textCoreInit};                    // "Init"
        constexpr ee::Ptr<const char> kTextTooManyLights{sym::textTooManyLights};      // "Too many Lights!"
        constexpr ee::Ptr<const char> kTextTooManyObjects{sym::textTooManyObjects};    // "Too many Objects!"
        constexpr ee::Ptr<const char> kTextTooManyGaps{sym::textTooManyGaps};          // "Too many Gaps!"
        constexpr ee::Ptr<const char> kTopToolPath{sym::textTopToolPath};              // "ParkEditor.MainUI.TopHeader.TopTool"
        constexpr ee::Ptr<const char> kTextChangePieceRight{sym::textChangePieceRight}; // "ChangePieceRight"
        constexpr ee::Ptr<const char> kTextChangePieceLeft{sym::textChangePieceLeft};  // "ChangePieceLeft"
        constexpr ee::Ptr<const char> kKeyboardPath{sym::textKeyboardPath};            // "Commonui.keyboardFrame.keyboard"
        constexpr ee::Ptr<const char> kTextString{sym::textKeyboardString};            // "String"
        constexpr ee::Ptr<const char> kMainUiPath{sym::textMainUiPath};                // "ParkEditor.MainUI"
        constexpr ee::Ptr<const char> kTextNameGap{sym::textNameGap};                  // "NameGap"
        // The most gaps a park holds, and how many it has (gap_tool.h).
        inline ee::Global<uint32_t, sym::g_gapCount> g_gapCount;
        inline ee::Global<uint32_t, sym::g_maxGaps> g_maxGaps;
        // The theme's place in the theme menu (edit_themes.cpp).
        inline ee::Global<uint32_t, sym::g_parkThemePosition> g_parkThemePosition;
        // The held object's turns tried when Circle turns it until it fits.
        constexpr uint32_t kTurnsTried = 16;
        // The park's stream starts with this.
        constexpr uint32_t kParkStreamVersion = 0x100;
        // What zgui's PLAY sends the root to run an ACTION.
        constexpr uint32_t kPlayMessage = 0x8000;
        constexpr uint32_t kMaxParkLights = 50;
        // The icon roll's angle with the first entry in front.
        constexpr ee::Float kRollStartAngle = 16384.0f;

        // A step's direction on the grid.
        constexpr uint8_t kNextColumn = 0;
        constexpr uint8_t kPreviousRow = 1;
        constexpr uint8_t kPreviousColumn = 2;
        constexpr uint8_t kNextRow = 3;
        // The selection reaches at most this many cells from its corner.
        constexpr ee::Float kMaxSelectionSpan = 8.0f;
        constexpr int8_t kFirstRepeatFrames = 15;
        constexpr int8_t kRepeatFrames = 5;
        constexpr ee::Float kStickDeadZone = 0.25f;
        // Degrees (or distance) a frame with a stick pushed all the way.
        constexpr ee::Float kViewStickRate = 1.5f;
        bool stickMoved(ee::Float level)
        {
            if (level > 0.0f && level < kStickDeadZone)
                return false;
            if (level < 0.0f && level > -kStickDeadZone)
                return false;
            return !(level == 0.0f);
        }

        struct UnusedWords
        {
            uint32_t words[3];
        };

        // An unsigned number as a float, the way the compiler wrote it.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1) | (value >> 1)));
            return half + half;
        }

        // The terrain tool's steps (ParkEditor::toolStep).
        constexpr uint8_t kTerrainIdle = 0;
        constexpr uint8_t kTerrainSelecting = 1; // Cross is held: the cursor stretches the selection
        constexpr uint8_t kTerrainLevelling = 2; // the tape frame is open: Up and Down change the level
        constexpr int8_t kLevelFirstRepeatFrames = 5;
        constexpr int8_t kLevelEdgeRepeatFrames = 10;

        // terrainToolFrame's frame: a visit's answer, the level neighbour
        // answer, and the height the tape shows.
        struct TerrainScratch
        {
            uint8_t found;          // +0x00
            uint8_t levelNeighbour; // +0x01
            uint8_t _02[2];
            ee::Float tapeDistance; // +0x04
        };
        static_assert(offsetof(TerrainScratch, tapeDistance) == 4);

        // A widget's ACTION played: what zgui's PLAY does.
        void playUiAction(ee::Ptr<const char> path, ee::Ptr<const char> action)
        {
            const ee::Ptr<zge::zgui::Node> widget = zge::zgui::findNode(*g_uiRoot, path, 0);
            const ee::Ptr<zge::zgui::ListedAction> found = widgetAction(widget, action);
            zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, found, 1);
        }

        // The selected cells one level up or down, unless one is at the end
        // of the range already (`atLimit` finds that). A press changes them
        // and waits kLevelFirstRepeatFrames; held, they change every frame
        // once the wait is over, and wait kLevelEdgeRepeatFrames when the
        // selection comes level with a cell beside it.
        void changeSelectionLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<TerrainScratch> scratch, uint32_t pressed, uint32_t by, SelectedCellVisit atLimit)
        {
            if (pressed)
            {
                scratch->found = 0;
                forEachSelectedCell(editor, atLimit, &scratch->found);
                if (scratch->found)
                    return;
                forEachSelectedCell(editor, sym::changeCellLevel, by);
                forEachSelectedCell(editor, sym::refitCellTower, 0);
                editor->levelRepeat = kLevelFirstRepeatFrames;
                *g_inputTaken = 1;
                return;
            }
            if (editor->levelRepeat != 0)
            {
                editor->levelRepeat = static_cast<int8_t>(static_cast<uint8_t>(editor->levelRepeat) - 1);
                return;
            }
            scratch->found = 0;
            forEachSelectedCell(editor, atLimit, &scratch->found);
            if (scratch->found)
                return;
            forEachSelectedCell(editor, sym::changeCellLevel, by);
            forEachSelectedCell(editor, sym::refitCellTower, 0);
            scratch->levelNeighbour = 0;
            forEachSelectedCell(editor, sym::noteLevelNeighbourOutside, &scratch->levelNeighbour);
            if (scratch->levelNeighbour)
                editor->levelRepeat = kLevelEdgeRepeatFrames;
        }

        // The tool entries' list walked: its first, the one after or before
        // an entry (0 at either end), its last.
        ee::Ptr<EditToolEntry> toolEntryOf(ee::Ptr<zge::ListNode> node) { return zge::recordOfNode<EditToolEntry>(node); }
        ee::Ptr<EditToolEntry> firstToolEntry() { return toolEntryOf(g_toolEntries->head); }
        ee::Ptr<EditToolEntry> lastToolEntry() { return toolEntryOf(g_toolEntries->tail); }
        ee::Ptr<EditToolEntry> nextToolEntry(ee::Ptr<EditToolEntry> entry) { return toolEntryOf(entry->link.next); }
        ee::Ptr<EditToolEntry> previousToolEntry(ee::Ptr<EditToolEntry> entry) { return toolEntryOf(entry->link.prev); }

        // Whether a cell's column (or row) lies in the selection's span:
        // from the corner back over `extent` cells, or forwards when the
        // extent is negative.
        bool inSpan(int32_t cell, int32_t corner, int32_t extent)
        {
            if (cell >= corner && cell < corner - extent)
                return true;
            return cell <= corner && corner - extent < cell;
        }

        // One span of the selection grown to hold a column (or row): true if
        // it grew. A span reaching back keeps its far end and moves its
        // corner; one reaching forwards the other way round.
        bool stretchSpan(int8_t &corner, int8_t &extent, uint8_t cell)
        {
            const int32_t reach = extent;
            const int32_t at = corner;
            if (reach < 0)
            {
                if (at - reach - 1 < cell)
                {
                    extent = static_cast<int8_t>(static_cast<uint8_t>(corner) - cell - 1);
                    return true;
                }
                if (cell < at)
                {
                    const uint8_t was = static_cast<uint8_t>(corner);
                    corner = static_cast<int8_t>(cell);
                    extent = static_cast<int8_t>(static_cast<uint8_t>(extent) - (cell - was));
                    return true;
                }
                return false;
            }
            if (at < cell)
            {
                const uint8_t was = static_cast<uint8_t>(corner);
                corner = static_cast<int8_t>(cell);
                extent = static_cast<int8_t>(static_cast<uint8_t>(extent) - (was - cell));
                return true;
            }
            if (cell < at - reach + 1)
            {
                extent = static_cast<int8_t>(static_cast<uint8_t>(corner) - cell + 1);
                return true;
            }
            return false;
        }
    }

    void initEditorCore(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::Pad> pad)
    {
        *g_toolEntryPool = newEditPool(sizeof(EditToolEntry), kToolEntryPoolRecords);
        zge::initList(&*g_toolEntries);
        editor->pad = pad;
        editor->levelRepeat = 0;
        editor->padRepeat = 0;
        editor->toolStep = 0;
        editor->userLocked = 0;
        editor->cursorColumn = 0;
        editor->cursorRow = 0;
        resetSelection(editor);
        *g_cursorSaved = 0;
    }
    READABLE_FUNCTION(initEditorCore, Verified);

    void freeEditToolEntries(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<ee::Ptr<EditToolEntry>> entry;
        *entry = firstToolEntry();
        while (*entry)
        {
            const ee::Ptr<EditToolEntry> next = nextToolEntry(*entry);
            zge::listRemove(&*g_toolEntries, *entry);
            editPoolFree(*g_toolEntryPool, entry);
            *entry = next;
        }
        resetSelection(editor);
    }
    READABLE_FUNCTION(freeEditToolEntries, Verified);

    void centreCursor(ee::Ptr<ParkEditor> editor)
    {
        editor->cursorColumn = static_cast<int8_t>(editor->columns >> 1);
        editor->cursorRow = static_cast<int8_t>(editor->rows >> 1);
        resetSelection(editor);
        *g_cursorSaved = 0;
    }
    READABLE_FUNCTION(centreCursor, Verified);

    // The first blocked state of a frame stays: later calls that frame
    // change nothing. With the piece tool the held object takes the state's
    // colour.
    void setCursorState(ee::Ptr<ParkEditor> editor, uint8_t state)
    {
        if (!editor->cursorStateLocked)
        {
            editor->cursorState = state;
            if (state == kCursorBlocked)
                editor->cursorStateLocked = 1;
        }
        if (editTool() == kEditToolPiece)
            game::tail(colourHeldObject, editor, state);
    }
    READABLE_FUNCTION(setCursorState, Verified);

    void cursorWorldPosition(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::Vec4> position)
    {
        game::tail(cellWorldPosition, editor, static_cast<uint32_t>(editor->cursorColumn), static_cast<uint32_t>(editor->cursorRow), position);
    }
    READABLE_FUNCTION(cursorWorldPosition, Verified);

    void cursorCell(ee::Ptr<ParkEditor> editor, ee::Ptr<uint8_t> column, ee::Ptr<uint8_t> row)
    {
        *column = static_cast<uint8_t>(editor->cursorColumn);
        *row = static_cast<uint8_t>(editor->cursorRow);
    }
    READABLE_FUNCTION(cursorCell, Verified);

    void moveCursorToCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row)
    {
        if (editor->cursorColumn != editor->selectionColumn || editor->cursorRow != editor->selectionRow)
            return;
        editor->cursorColumn = static_cast<int8_t>(column);
        editor->cursorRow = static_cast<int8_t>(row);
        game::tail(resetSelection, editor);
    }
    READABLE_FUNCTION(moveCursorToCell, Verified);

    uint32_t cellInSelection(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row)
    {
        if (!inSpan(column, editor->selectionColumn, editor->selectionColumns))
            return 0;
        return inSpan(row, editor->selectionRow, editor->selectionRows) ? 1 : 0;
    }
    READABLE_FUNCTION(cellInSelection, Verified);

    uint32_t stretchCursorToCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row)
    {
        if (editor->selectionFollowing)
            return 0;
        uint32_t grew = 0;
        if (stretchSpan(editor->selectionColumn, editor->selectionColumns, column))
            grew = 1;
        if (stretchSpan(editor->selectionRow, editor->selectionRows, row))
            grew = 1;
        return grew;
    }
    READABLE_FUNCTION(stretchCursorToCell, Verified);

    void addEditToolEntry(uint8_t tool, uint8_t category, ee::Ptr<const char> name, uint32_t enabled)
    {
        const ee::Ptr<EditToolEntry> entry(editPoolAllocate(*g_toolEntryPool));
        entry->tool = tool;
        entry->category = category;
        entry->enabled = enabled;
        entry->heldObject = ee::Ptr<EditObjectDef>();
        zge::stringCopy(entry->name, name);
        game::tail(zge::listPush, &*g_toolEntries, entry);
    }
    READABLE_FUNCTION(addEditToolEntry, Verified);

    void setEditToolEntryEnabled(ee::Ptr<const char> name, uint32_t enabled)
    {
        for (ee::Ptr<EditToolEntry> entry = firstToolEntry(); entry; entry = nextToolEntry(entry))
        {
            if (zge::compareStrings(name, entry->name) == 0)
            {
                entry->enabled = enabled;
                return;
            }
        }
    }
    READABLE_FUNCTION(setEditToolEntryEnabled, Verified);

    ee::Ptr<char> currentToolEntryName()
    {
        if (!*g_currentToolEntry)
            return ee::Ptr<char>();
        return ee::Ptr<char>((*g_currentToolEntry)->name);
    }
    READABLE_FUNCTION(currentToolEntryName, Draft);

    void selectEditToolEntry()
    {
        if (!*g_currentToolEntry)
            game::tail(useEditToolEntry, firstToolEntry());
        else
            game::tail(useEditToolEntry, *g_currentToolEntry);
    }
    READABLE_FUNCTION(selectEditToolEntry, Verified);

    void selectFirstEditToolEntry()
    {
        game::tail(useEditToolEntry, firstToolEntry());
    }
    READABLE_FUNCTION(selectFirstEditToolEntry, Verified);

    void moveCursorToObjectCell(ee::Ptr<ParkEditor> editor)
    {
        const ee::Ptr<ParkCell> cell = parkCell(editor, editor->cursorColumn, editor->cursorRow);
        if (!cell->object)
            return;
        editor->cursorColumn = static_cast<int8_t>(cell->object->column);
        editor->cursorRow = static_cast<int8_t>(cell->object->row);
        game::tail(resetSelection, editor);
    }
    READABLE_FUNCTION(moveCursorToObjectCell, Draft);

    void refitCellTower(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell)
    {
        if (cell)
            game::tail(fitCellTower, cell->column, cell->row, editor);
    }
    READABLE_FUNCTION(refitCellTower, Verified);

    void growSelectionOverCellObject(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> grew)
    {
        if (cell && cell->object && stretchCursorOverObject(editor, cell->object))
            *grew = 1;
    }
    READABLE_FUNCTION(growSelectionOverCellObject, Verified);

    // forEachSelectedCell hands only cells that can be built on, so the
    // second question never fails for a cell it hands.
    void requireBuildableCell(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> allBuildable)
    {
        if (!cell || !cellCanBeBuiltOn(editor, static_cast<int8_t>(cell->column), static_cast<int8_t>(cell->row)))
            *allBuildable = 0;
    }
    READABLE_FUNCTION(requireBuildableCell, Verified);

    // The two below read the cell with no test for a place that cannot be
    // built on (the original's).
    void noteCellAtGroundLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found)
    {
        static_cast<void>(editor);
        if (cell->level == 0)
            *found = 1;
    }
    READABLE_FUNCTION(noteCellAtGroundLevel, Verified);

    void noteCellAtTopLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found)
    {
        static_cast<void>(editor);
        if (cell->level >= kTopLevel)
            *found = 1;
    }
    READABLE_FUNCTION(noteCellAtTopLevel, Verified);

    // The rectangle's ends are read again after every visit, and the
    // counters are signed bytes, as the original's.
    void forEachSelectedCell(ee::Ptr<ParkEditor> editor, SelectedCellVisit visit, ee::Address data)
    {
        ee::Local<CellBox> box;
        cursorCellBox(editor, box);
        for (int8_t row = static_cast<int8_t>(box->firstRow); row <= box->lastRow; row = static_cast<int8_t>(row + 1))
        {
            for (int8_t column = static_cast<int8_t>(box->firstColumn); column <= box->lastColumn; column = static_cast<int8_t>(column + 1))
            {
                if (cellCanBeBuiltOn(editor, column, row))
                    visit(editor, parkCell(editor, column, row), data);
                else
                    visit(editor, ee::Ptr<ParkCell>(), data);
            }
        }
    }
    READABLE_FUNCTION(forEachSelectedCell, Verified);

    void useEditToolEntry(ee::Ptr<EditToolEntry> entry)
    {
        if (editTool() != entry->tool)
            setEditTool(entry->tool);
        if (entry->category != kToolEntryNoCategory)
            holdCategoryObject(entry->category, entry->heldObject);
        *g_currentToolEntry = entry;
    }
    READABLE_FUNCTION(useEditToolEntry, Verified);

    void selectHeldObjectCells(ee::Ptr<ParkEditor> editor)
    {
        editor->stretching = 0;
        editor->selectionIsHeldObject = 1;
        editor->selectionColumns = static_cast<int8_t>(-heldObjectSpanX());
        editor->selectionRows = static_cast<int8_t>(-heldObjectSpanY());
        editor->selectionColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorColumn) - heldObjectCentreX());
        editor->selectionRow = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorRow) - heldObjectCentreY());
    }
    READABLE_FUNCTION(selectHeldObjectCells, Verified);

    void resetSelection(ee::Ptr<ParkEditor> editor)
    {
        editor->selectionColumn = editor->cursorColumn;
        editor->selectionRow = editor->cursorRow;
        editor->selectionRows = 1;
        editor->selectionColumns = 1;
        editor->stretching = 0;
        editor->selectionIsHeldObject = 0;
        editor->cursorState = kCursorFree;
        game::tail(resetCursorRules, editor);
    }
    READABLE_FUNCTION(resetSelection, Verified);

    // What a tool's frame may change for the cursor's move that follows it.
    void resetCursorRules(ee::Ptr<ParkEditor> editor)
    {
        editor->terrainInHand = 0;
        editor->cursorMoves = 1;
        editor->cursorStretches = 0;
        editor->selectionFollows = 0;
        editor->cursorStateLocked = 0;
        editor->cursorSnapsToObjects = 0;
    }
    READABLE_FUNCTION(resetCursorRules, Verified);

    // The cursor moved one cell. `direction` counts quarter turns on the
    // screen and is turned by the view's heading to one on the grid. While
    // `stretch` the selection's corner stays and the rectangle reaches to
    // the cursor; with `follow` the rectangle moves along; with neither it
    // becomes the cursor's cell (or the held object's cells). A move onto
    // cells that cannot be built on is tried one cell further, and a move
    // that fails or makes the selection wider than kMaxSelectionSpan is
    // taken back. 1 if the selection can be built on afterwards.
    uint32_t stepCursor(ee::Ptr<ParkEditor> editor, uint8_t direction, uint8_t stretch, uint8_t follow)
    {
        ee::Local<CursorBlock> before;
        ee::Local<uint32_t> allBuildable;
        bool atEdge = false;
        uint32_t moved = 1;
        *allBuildable = 1;
        const uint32_t turn = ((direction + ee::toInt((editor->viewHeading + 225.0f) * *g_quarterTurnsADegree + 2.0f)) & 0xFF) & 3;
        zge::memoryCopy(before, cursorBlock(editor), sizeof(CursorBlock));
        if (stretch && !editor->stretching)
            editor->stretching = 1;

        switch (turn)
        {
        case kNextColumn:
            editor->cursorColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorColumn) + 1);
            if (editor->cursorColumn == editor->columns)
            {
                atEdge = true;
                editor->cursorColumn = static_cast<int8_t>(editor->cursorColumn - 1);
            }
            break;
        case kPreviousRow:
            editor->cursorRow = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorRow) - 1);
            if (editor->cursorRow < 0)
            {
                editor->cursorRow = 0;
                atEdge = true;
            }
            break;
        case kPreviousColumn:
            editor->cursorColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorColumn) - 1);
            if (editor->cursorColumn < 0)
            {
                editor->cursorColumn = 0;
                atEdge = true;
            }
            break;
        case kNextRow:
            editor->cursorRow = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorRow) + 1);
            if (editor->cursorRow == editor->rows)
            {
                atEdge = true;
                editor->cursorRow = static_cast<int8_t>(editor->cursorRow - 1);
            }
            break;
        }

        if (stretch)
        {
            const int32_t columns = editor->selectionColumn - editor->cursorColumn;
            const int32_t rows = editor->selectionRow - editor->cursorRow;
            editor->selectionColumns = static_cast<int8_t>(columns > 0 ? columns + 1 : columns - 1);
            editor->selectionRows = static_cast<int8_t>(rows > 0 ? rows + 1 : rows - 1);
        }
        else
        {
            editor->stretching = 0;
            if (!follow)
            {
                if (!editor->selectionIsHeldObject)
                {
                    editor->selectionColumn = editor->cursorColumn;
                    editor->selectionRow = editor->cursorRow;
                    editor->selectionRows = 1;
                    editor->selectionColumns = 1;
                }
                else
                {
                    editor->selectionColumns = static_cast<int8_t>(-heldObjectSpanX());
                    editor->selectionRows = static_cast<int8_t>(-heldObjectSpanY());
                    editor->selectionColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorColumn) - heldObjectCentreX());
                    editor->selectionRow = static_cast<int8_t>(static_cast<uint8_t>(editor->cursorRow) - heldObjectCentreY());
                }
            }
            else if (!atEdge)
            {
                switch (turn)
                {
                case kNextColumn:
                    editor->selectionColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionColumn) + 1);
                    break;
                case kPreviousRow:
                    editor->selectionRow = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionRow) - 1);
                    break;
                case kPreviousColumn:
                    editor->selectionColumn = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionColumn) - 1);
                    break;
                case kNextRow:
                    editor->selectionRow = static_cast<int8_t>(static_cast<uint8_t>(editor->selectionRow) + 1);
                    break;
                }
            }
        }

        forEachSelectedCell(editor, sym::requireBuildableCell, allBuildable);
        if (!*allBuildable)
        {
            moved = 0;
            if (!stretch && !atEdge)
                moved = stepCursor(editor, direction, 0, follow);
        }
        if (ee::abs(ee::toFloat(editor->selectionColumns)) > kMaxSelectionSpan)
            moved = 0;
        if (ee::abs(ee::toFloat(editor->selectionRows)) > kMaxSelectionSpan)
            moved = 0;
        if (!moved)
        {
            zge::memoryCopy(cursorBlock(editor), before, sizeof(CursorBlock));
            forEachSelectedCell(editor, sym::requireBuildableCell, allBuildable);
            moved = *allBuildable & 0xFF;
        }
        return moved;
    }
    READABLE_FUNCTION(stepCursor, Verified);

    // With the terrain tool the selection then grows over every object it
    // touches, until it holds them whole.
    void moveCursor(ee::Ptr<ParkEditor> editor, uint8_t direction, uint8_t stretch, uint8_t follow)
    {
        ee::Local<uint8_t> grew;
        *grew = 1;
        stepCursor(editor, direction, stretch, follow);
        if (editTool() != kEditToolTerrain)
            return;
        *grew = 1;
        do
        {
            *grew = 0;
            forEachSelectedCell(editor, sym::growSelectionOverCellObject, grew);
        } while (*grew);
    }
    READABLE_FUNCTION(moveCursor, Verified);

    // The directions held on the pad: one step when a direction is pressed,
    // the next after kFirstRepeatFrames frames of holding it, then one every
    // kRepeatFrames.
    void moveCursorByPad(ee::Ptr<ParkEditor> editor, uint32_t changed, uint32_t held, uint8_t stretch, uint8_t follow)
    {
        struct PadDirection
        {
            uint32_t button;
            uint8_t direction;
        };
        static constexpr PadDirection kPadDirections[] = {
            {zge::kPadUp, kPreviousRow},
            {zge::kPadDown, kNextRow},
            {zge::kPadLeft, kNextColumn},
            {zge::kPadRight, kPreviousColumn},
        };
        editor->selectionFollowing = follow;
        for (const PadDirection &pad : kPadDirections)
        {
            if (!(held & pad.button))
                continue;
            if (changed & pad.button)
            {
                moveCursor(editor, pad.direction, stretch, follow);
                editor->padRepeat = kFirstRepeatFrames;
            }
            else if (editor->padRepeat <= 0)
            {
                moveCursor(editor, pad.direction, stretch, follow);
                editor->padRepeat = kRepeatFrames;
            }
            else
            {
                editor->padRepeat = static_cast<int8_t>(static_cast<uint8_t>(editor->padRepeat) - 1);
            }
        }
    }
    READABLE_FUNCTION(moveCursorByPad, Verified);

    // The cursor's move at the end of the core's frame, by what the tool's
    // frame allowed. A cursor that snaps to objects moves from where it
    // was before the snap, and is put on the own cell of the object it
    // lands on.
    void moveToolCursor(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        if (!editor->cursorMoves)
            return;
        if (editor->cursorSnapsToObjects)
        {
            editor->cursorColumn = editor->unsnappedColumn;
            editor->cursorRow = editor->unsnappedRow;
        }
        moveCursorByPad(editor, changed, held, static_cast<uint8_t>(editor->cursorStretches), static_cast<uint8_t>(editor->selectionFollows));
        editor->unsnappedColumn = editor->cursorColumn;
        editor->unsnappedRow = editor->cursorRow;
        if (!editor->cursorSnapsToObjects)
            return;
        const ee::Ptr<PlacedObject> object = parkCell(editor, editor->cursorColumn, editor->cursorRow)->object;
        if (object)
        {
            editor->cursorColumn = static_cast<int8_t>(object->column);
            editor->cursorRow = static_cast<int8_t>(object->row);
        }
    }
    READABLE_FUNCTION(moveToolCursor, Verified);

    // The view moved by the sticks: the left one turns and tilts it, the
    // right one's up and down zooms. A stick within kStickDeadZone of its
    // rest does nothing.
    void moveEditViewByPad(ee::Ptr<ParkEditor> editor)
    {
        const ee::Float turn = editor->pad->levels[zge::kLevelLeftX];
        const ee::Float zoom = editor->pad->levels[zge::kLevelRightY];
        const ee::Float tilt = editor->pad->levels[zge::kLevelLeftY];
        if (stickMoved(turn))
            turnEditView(editor, turn * kViewStickRate);
        if (stickMoved(tilt))
            tiltEditView(editor, tilt * kViewStickRate);
        if (stickMoved(zoom))
            game::tail(zoomEditView, editor, zoom * kViewStickRate);
    }
    READABLE_FUNCTION(moveEditViewByPad, Verified);

    // The buttons that change the tool. Select goes back to the first tool
    // entry, Start opens the pause menu, R1 and L1 roll to the next and the
    // previous enabled entry, and Triangle takes up the copy tool from the
    // terrain, piece or rail tool and puts it down again.
    void toolButtonsFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        ee::Local<ee::Float> angle;
        if (editor->tool == kEditToolTerrain)
            editor->terrainInHand = 1;
        if ((held & zge::kPadSelect) && (changed & zge::kPadSelect))
        {
            *angle = kRollStartAngle;
            useEditToolEntry(firstToolEntry());
            editor->toolStep = 0;
            const ee::Ptr<zge::zgui::Node> roll = zge::zgui::findNode(*g_uiRoot, kIconRollPath, 0);
            zge::zgui::setVariable(roll, kTextRollAngle, angle);
            resetSelection(editor);
            return;
        }
        if ((held & zge::kPadStart) && (changed & zge::kPadStart))
        {
            markStopFrameOpen();
            const ee::Ptr<zge::zgui::Node> menu = zge::zgui::findNode(*g_uiRoot, kPauseMenuPath, 0);
            const ee::Ptr<zge::zgui::ListedAction> init = widgetAction(menu, kTextInit);
            zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, init, 1);
            return;
        }

        ee::Ptr<const char> scroll;
        if ((held & zge::kPadR1) && (changed & zge::kPadR1))
        {
            ee::Ptr<EditToolEntry> entry = *g_currentToolEntry;
            do
            {
                entry = nextToolEntry(entry);
                *g_currentToolEntry = entry;
                if (!entry)
                {
                    entry = firstToolEntry();
                    *g_currentToolEntry = entry;
                }
            } while (!entry->enabled);
            scroll = kTextScrollLeft;
        }
        else if ((held & zge::kPadL1) && (changed & zge::kPadL1))
        {
            ee::Ptr<EditToolEntry> entry = *g_currentToolEntry;
            do
            {
                entry = previousToolEntry(entry);
                *g_currentToolEntry = entry;
                if (!entry)
                {
                    entry = lastToolEntry();
                    *g_currentToolEntry = entry;
                }
            } while (!entry->enabled);
            scroll = kTextScrollRight;
        }
        if (scroll)
        {
            const ee::Ptr<zge::zgui::Node> header = zge::zgui::findNode(*g_uiRoot, kTopHeaderPath, 0);
            const ee::Ptr<zge::zgui::ListedAction> action = widgetAction(header, scroll);
            zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, action, 1);
            markStopFrameOpen();
            selectEditToolEntry();
            zge::zgui::g_zgui->moveSound();
            selectHeldObjectCells(editor);
            return;
        }

        const uint8_t tool = editor->tool;
        if (tool < kEditToolStart)
        {
            if (!(changed & zge::kPadTriangle) || !(held & zge::kPadTriangle))
                return;
            *g_toolBeforeCopy = editTool();
            editor->cursorStretches = 1;
            hideHeldObject();
            setEditTool(kEditToolCopy);
            editor->toolStep = 0;
            return;
        }
        if (editor->toolStep != 1 || tool != kEditToolCopy)
            return;
        if (!(changed & zge::kPadTriangle) || !(held & zge::kPadTriangle))
            return;
        editor->selectionFollows = 0;
        setEditTool(*g_toolBeforeCopy);
        selectEditToolEntry();
        resetSelection(editor);
    }
    READABLE_FUNCTION(toolButtonsFrame, Verified);

    // The terrain tool's frame. Cross held stretches the selection; let go,
    // the tape frame opens and Up and Down raise and lower the selected
    // cells (once when pressed, then again and again while held); Circle
    // ends that, Cross goes back to selecting. Square removes what stands
    // on the selected cells.
    void terrainToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        ee::Local<TerrainScratch> scratch;
        scratch->found = 1;
        editor->selectionIsHeldObject = 0;
        switch (editor->toolStep)
        {
        case kTerrainIdle:
            if ((changed & zge::kPadCross) && (held & zge::kPadCross))
            {
                editor->cursorStretches = 1;
                editor->toolStep = kTerrainSelecting;
                *g_inputTaken = 1;
            }
            break;
        case kTerrainSelecting:
            editor->cursorStretches = 1;
            if ((changed & zge::kPadCross) && !(held & zge::kPadCross))
            {
                if (editor->cursorState != kCursorBlocked)
                {
                    editor->toolStep = kTerrainLevelling;
                    playUiAction(kTapeFramePath, kTextInit);
                }
                else
                {
                    editor->toolStep = kTerrainIdle;
                }
                *g_inputTaken = 1;
            }
            break;
        case kTerrainLevelling:
            editor->cursorMoves = 0;
            if (held & zge::kPadUp)
                changeSelectionLevel(editor, scratch, changed & zge::kPadUp, 1, sym::noteCellAtTopLevel);
            if (held & zge::kPadDown)
                changeSelectionLevel(editor, scratch, changed & zge::kPadDown, static_cast<uint32_t>(-1), sym::noteCellAtGroundLevel);

            scratch->tapeDistance = ee::toFloat(parkCell(editor, editor->cursorColumn, editor->cursorRow)->level);
            scratch->tapeDistance = scratch->tapeDistance - ee::toFloat(themeOffset(editor->theme));
            zge::zgui::setVariable(zge::zgui::findNode(*g_uiRoot, kTapePath, 0), kTextDistance, &scratch->tapeDistance);
            if ((held & zge::kPadCircle) && (changed & zge::kPadCircle))
            {
                editor->toolStep = kTerrainIdle;
                resetSelection(editor);
                playUiAction(kTapeFramePath, kTextDeInit);
            }
            if ((changed & zge::kPadCross) && (held & zge::kPadCross))
            {
                editor->toolStep = kTerrainSelecting;
                editor->cursorStretches = 1;
                playUiAction(kTapeFramePath, kTextDeInit);
            }
            break;
        }

        // The selection grows over every object it touches.
        scratch->found = 1;
        do
        {
            scratch->found = 0;
            forEachSelectedCell(editor, sym::growSelectionOverCellObject, &scratch->found);
        } while (scratch->found);

        if ((held & zge::kPadSquare) && (changed & zge::kPadSquare) && !*g_inputTaken)
        {
            forEachSelectedCell(editor, sym::eraseCellContents, 0);
            zge::zgui::g_zgui->blockedSound();
            *g_inputTaken = 1;
        }
    }
    READABLE_FUNCTION(terrainToolFrame, Verified);

    // The piece tool's frame. R2 and L2 hold the category's next and
    // previous object, Circle turns the held object until it fits (16
    // turns at most), Cross places it, Square removes what stands on the
    // selected cells. After Cross or Square the cursor is copied aside for
    // the core's next frame.
    void pieceToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        ee::Local<uint32_t> fits;
        bool cursorUsed = false;
        if ((held & zge::kPadR2) && (changed & zge::kPadR2))
        {
            (*g_currentToolEntry)->heldObject = holdNextObject();
            playUiAction(kTopToolPath, kTextChangePieceRight);
            zge::zgui::g_zgui->changeSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadL2) && (changed & zge::kPadL2))
        {
            (*g_currentToolEntry)->heldObject = holdPreviousObject();
            playUiAction(kTopToolPath, kTextChangePieceLeft);
            zge::zgui::g_zgui->changeSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadCircle) && (changed & zge::kPadCircle))
        {
            *fits = 0;
            uint32_t turns = 0;
            do
            {
                *fits = 1;
                rotateHeldObject();
                ++turns;
                forEachSelectedCell(editor, sym::requireBuildableCell, fits);
            } while (!*fits && turns < kTurnsTried);
            zge::zgui::g_zgui->cancelSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadCross) && (changed & zge::kPadCross))
        {
            placeHeldObject(editor);
            cursorUsed = true;
            zge::zgui::g_zgui->selectSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadSquare) && (changed & zge::kPadSquare))
        {
            forEachSelectedCell(editor, sym::eraseCellContents, 0);
            cursorUsed = true;
            zge::zgui::g_zgui->blockedSound();
            *g_inputTaken = 1;
        }
        selectHeldObjectCells(editor);
        if (cursorUsed)
        {
            zge::memoryCopy(&*g_savedCursor, cursorBlock(editor), sizeof(CursorBlock));
            *g_cursorSaved = 1;
        }
    }
    READABLE_FUNCTION(pieceToolFrame, Verified);

    // The rail tool's frame. The cursor lands on objects' own cells. Circle
    // goes to the cell's next rail slot, Cross places a rail, Square
    // removes what stands on the selected cells, R2 and L2 take the next
    // and the previous kind of rail.
    void railToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        editor->selectionIsHeldObject = 0;
        editor->cursorSnapsToObjects = 1;
        if ((held & zge::kPadCircle) && (changed & zge::kPadCircle))
        {
            nextRailSlot(editor);
            zge::zgui::g_zgui->cancelSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadCross) && (changed & zge::kPadCross))
        {
            placeRailAtCursor(editor);
            zge::zgui::g_zgui->selectSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadSquare) && (changed & zge::kPadSquare))
        {
            forEachSelectedCell(editor, sym::eraseCellContents, 0);
            zge::zgui::g_zgui->blockedSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadR2) && (changed & zge::kPadR2))
        {
            nextRailType();
            playUiAction(kTopToolPath, kTextChangePieceRight);
            zge::zgui::g_zgui->changeSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadL2) && (changed & zge::kPadL2))
        {
            previousRailType();
            playUiAction(kTopToolPath, kTextChangePieceLeft);
            zge::zgui::g_zgui->changeSound();
            *g_inputTaken = 1;
        }
    }
    READABLE_FUNCTION(railToolFrame, Verified);

    // The start tool's frame. Circle turns the marker a quarter, Cross
    // places the start where it fits (the blocked sound where it does
    // not), and R2 or L2 makes the cursor's marker again.
    void startToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        if ((held & zge::kPadCircle) && (changed & zge::kPadCircle))
        {
            turnStartMarker();
            zge::zgui::g_zgui->cancelSound();
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadCross) && (changed & zge::kPadCross))
        {
            if (startFitsAtCursor(editor))
            {
                placeStart(editor);
                zge::zgui::g_zgui->selectSound();
            }
            else
            {
                zge::zgui::g_zgui->blockedSound();
            }
            *g_inputTaken = 1;
        }
        else if ((held & zge::kPadR2) && (changed & zge::kPadR2))
        {
            renewStartMarker(editor);
            *g_inputTaken = 1;
        }
        if ((held & zge::kPadL2) && (changed & zge::kPadL2))
        {
            renewStartMarker(editor);
            *g_inputTaken = 1;
        }
    }
    READABLE_FUNCTION(startToolFrame, Verified);

    // The gap tool's steps (ParkEditor::toolStep).
    namespace
    {
        constexpr uint8_t kGapPlacingStart = 0;
        constexpr uint8_t kGapPlacingEnd = 1;
        constexpr uint8_t kGapNaming = 2;
    }

    // The gap tool's frame. Cross places the gap's start on a free cell
    // (the stop frame says "Too many Gaps!" at the limit), then its end;
    // then the keyboard opens on the gap's name. Square removes the gaps
    // that end on the cursor's cell; while the end is being placed,
    // Triangle or Square goes back to the start.
    void gapToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        switch (editor->toolStep)
        {
        case kGapPlacingStart:
            if ((held & zge::kPadCross) && (changed & zge::kPadCross) && cursorFree(editor))
            {
                if (*g_gapCount < *g_maxGaps)
                {
                    editor->toolStep = kGapPlacingEnd;
                    setNewGapStart(editor->cursorColumn, editor->cursorRow);
                    newGapEndMarker(editor);
                    zge::zgui::g_zgui->selectSound();
                }
                else
                {
                    const ee::Ptr<zge::zgui::Node> title = zge::zgui::findNode(*g_uiRoot, kStopFrameTitlePath, 0);
                    zge::zgui::setVariable(title, kTextText, kTextTooManyGaps);
                    playUiAction(kStopFramePath, kTextInit);
                    markStopFrameOpen();
                }
                *g_inputTaken = 1;
            }
            else if ((held & zge::kPadSquare) && (changed & zge::kPadSquare))
            {
                removeGapsAtCursor(editor);
                zge::zgui::g_zgui->blockedSound();
                *g_inputTaken = 1;
            }
            break;
        case kGapPlacingEnd:
            if ((held & zge::kPadCross) && (changed & zge::kPadCross) && cursorFree(editor))
            {
                setNewGapEnd(editor->cursorColumn, editor->cursorRow);
                editor->toolStep = kGapNaming;
                zge::zgui::g_zgui->selectSound();
                *g_inputTaken = 1;
            }
            else if (((held & zge::kPadTriangle) && (changed & zge::kPadTriangle)) || ((held & zge::kPadSquare) && (changed & zge::kPadSquare)))
            {
                moveCursorToNewGapStart(editor);
                setNewGapStart(kGapNoCell, kGapNoCell);
                releaseNewGapEndMarker();
                editor->toolStep = kGapPlacingStart;
                zge::zgui::g_zgui->blockedSound();
                *g_inputTaken = 1;
            }
            break;
        case kGapNaming:
        {
            markStopFrameOpen();
            const ee::Ptr<zge::zgui::Node> keyboard = zge::zgui::findNode(*g_uiRoot, kKeyboardPath, 0);
            zge::zgui::setVariable(keyboard, kTextString, newGapName());
            const ee::Ptr<zge::zgui::Node> mainUi = zge::zgui::findNode(*g_uiRoot, kMainUiPath, 0);
            const ee::Ptr<zge::zgui::ListedAction> nameGap = widgetAction(mainUi, kTextNameGap);
            game::tail(zge::sendNewMessage, 0, *g_uiRoot, kPlayMessage, nameGap, 1);
            break;
        }
        }
    }
    READABLE_FUNCTION(gapToolFrame, Verified);

    // The copy tool's steps (ParkEditor::toolStep).
    namespace
    {
        constexpr uint8_t kCopySelecting = 0;
        constexpr uint8_t kCopyPasting = 1;
    }

    // The copy tool's frame. While Triangle is held the cursor stretches
    // the selection, which grows over every object it touches; let go, the
    // selected cells are copied. Then the copy follows the cursor: Cross
    // pastes it where the cursor is free, Square removes what stands on
    // the selected cells, Circle turns the copy.
    void copyToolFrame(ee::Ptr<ParkEditor> editor, uint32_t held, uint32_t changed)
    {
        ee::Local<uint32_t> grew;
        switch (editor->toolStep)
        {
        case kCopySelecting:
            editor->selectionIsHeldObject = 0;
            *grew = 1;
            editor->cursorStretches = 1;
            do
            {
                *grew = 0;
                forEachSelectedCell(editor, sym::growSelectionOverCellObject, grew);
            } while (*grew);
            if ((changed & zge::kPadTriangle) && !(held & zge::kPadTriangle))
            {
                editor->toolStep = kCopyPasting;
                copySelectedCells(editor);
                *g_inputTaken = 1;
            }
            break;
        case kCopyPasting:
            editor->selectionFollows = 1;
            if ((changed & zge::kPadCross) && (held & zge::kPadCross))
            {
                if (editor->cursorState != kCursorFree)
                    return;
                pasteCopiedCells(editor);
                zge::zgui::g_zgui->selectSound();
                *g_inputTaken = 1;
            }
            else if ((changed & zge::kPadSquare) && (held & zge::kPadSquare))
            {
                forEachSelectedCell(editor, sym::eraseCellContents, 0);
                zge::zgui::g_zgui->blockedSound();
                *g_inputTaken = 1;
            }
            else if ((changed & zge::kPadCircle) && (held & zge::kPadCircle))
            {
                turnCopiedCells(editor);
                zge::zgui::g_zgui->cancelSound();
                *g_inputTaken = 1;
            }
            break;
        }
    }
    READABLE_FUNCTION(copyToolFrame, Verified);

    // The core's frame: the tool in hand runs, then the view's sticks; and
    // unless the tool took the pad's buttons, the buttons that change the
    // tool and then the cursor's move. The first frame only takes up the
    // first tool entry.
    void updateEditorCore(ee::Ptr<ParkEditor> editor)
    {
        if (!*g_currentToolEntry)
        {
            game::tail(useEditToolEntry, firstToolEntry());
            return;
        }
        const uint32_t changed = editor->pad->changed;
        const uint32_t held = editor->pad->buttons;
        *g_inputTaken = 0;
        if (*g_cursorSaved)
        {
            // The cursor is copied aside, replaced by the saved one and
            // then by the copy again: it ends as it was (the original's).
            zge::memoryCopy(&*g_cursorScratch, cursorBlock(editor), sizeof(CursorBlock));
            zge::memoryCopy(cursorBlock(editor), &*g_savedCursor, sizeof(CursorBlock));
            zge::memoryCopy(cursorBlock(editor), &*g_cursorScratch, sizeof(CursorBlock));
            *g_cursorSaved = 0;
        }
        resetCursorRules(editor);
        switch (editTool())
        {
        case kEditToolTerrain:
            terrainToolFrame(editor, held, changed);
            break;
        case kEditToolPiece:
            pieceToolFrame(editor, held, changed);
            break;
        case kEditToolRail:
            railToolFrame(editor, held, changed);
            break;
        case kEditToolStart:
            startToolFrame(editor, held, changed);
            break;
        case kEditToolGap:
            gapToolFrame(editor, held, changed);
            break;
        case kEditToolCopy:
            copyToolFrame(editor, held, changed);
            break;
        }
        moveEditViewByPad(editor);
        if (*g_inputTaken)
            return;
        toolButtonsFrame(editor, held, changed);
        if (*g_inputTaken)
            return;
        moveToolCursor(editor, held, changed);
    }
    READABLE_FUNCTION(updateEditorCore, Verified);

    // A visit: the cell's ground raised (or lowered) by `by` levels, kept
    // within 0 .. kTopLevel; its object and rails follow. The original also
    // hands the new level to movePlacedObjectToCell, which takes no third
    // word.
    void changeCellLevel(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, uint32_t by)
    {
        ee::Local<zge::Vec4> position;
        if (!cell)
            return;
        cell->level = static_cast<int8_t>(static_cast<uint8_t>(cell->level) + by);
        if (cell->level < 0)
            cell->level = 0;
        if (!(cell->level < kTopLevel))
            cell->level = kTopLevel;
        cellWorldPosition(editor, cell->column, cell->row, position);
        if (cell->object)
            movePlacedObjectToCell(editor, cell->object);
        updateCellRails(editor, cell->column, cell->row);
    }
    READABLE_FUNCTION(changeCellLevel, Verified);

    // A visit: `found` set when a cell next to this one, outside the
    // selection, stands at this cell's level. Each neighbour that can be
    // built on also frees the cursor's state.
    void noteLevelNeighbourOutside(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell, ee::Ptr<uint8_t> found)
    {
        if (!cell)
            return;
        for (int32_t side = 0; side < 4; ++side)
        {
            int8_t column = static_cast<int8_t>(cell->column);
            int8_t row = static_cast<int8_t>(cell->row);
            switch (side)
            {
            case 0:
                column = static_cast<int8_t>(column + 1);
                break;
            case 1:
                row = static_cast<int8_t>(row + 1);
                break;
            case 2:
                column = static_cast<int8_t>(column - 1);
                break;
            case 3:
                row = static_cast<int8_t>(row - 1);
                break;
            }
            if (!cellCanBeBuiltOn(editor, column, row))
                continue;
            editor->cursorState = kCursorFree;
            editor->cursorStateLocked = 0;
            if (cellInSelection(editor, static_cast<uint8_t>(column), static_cast<uint8_t>(row)))
                continue;
            if (cell->level == parkCell(editor, column, row)->level)
                *found = 1;
        }
    }
    READABLE_FUNCTION(noteLevelNeighbourOutside, Verified);

    // A visit: the cell's rails removed, or with no rail its object, whose
    // cost goes back to the park's capacity.
    void eraseCellContents(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkCell> cell)
    {
        // Scaffolding (frame-exact): three words the original clears and
        // never reads.
        ee::Local<UnusedWords> unused;
        ee::Local<zge::Vec4> position;
        if (!cell)
            return;
        if (cellHasRail(editor, cell->column, cell->row))
        {
            removeCellRails(editor, cell->column, cell->row);
            return;
        }
        if (!cell->object)
            return;
        unused->words[0] = 0;
        unused->words[1] = 0;
        unused->words[2] = 0;
        cellWorldPosition(editor, cell->column, cell->row, position);
        const uint32_t cost = placedObjectCost(cell->object);
        *g_parkCapacityUsed = *g_parkCapacityUsed - unsignedToFloat(cost);
        removePlacedObject(editor, &cell->object);
    }
    READABLE_FUNCTION(eraseCellContents, Verified);

    // The first tool entry taken, the top header's icon roll put at its
    // start and the selection reset.
    void rewindToolRoll(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<ee::Float> angle;
        *angle = kRollStartAngle;
        useEditToolEntry(firstToolEntry());
        editor->toolStep = 0;
        const ee::Ptr<zge::zgui::Node> roll = zge::zgui::findNode(*g_uiRoot, kIconRollPath, 0);
        zge::zgui::setVariable(roll, kTextRollAngle, angle);
        resetSelection(editor);
    }
    READABLE_FUNCTION(rewindToolRoll, Draft);

    // 1 after opening the stop frame with "Too many Lights!" (a light, with
    // kMaxParkLights placed) or "Too many Objects!" (the park's capacity
    // used up); 0 when the object can still be placed.
    uint32_t refuseWhenParkFull(uint32_t object)
    {
        if (editObjectIsLight(object) && !(g_parkLights->count < kMaxParkLights))
        {
            const ee::Ptr<zge::zgui::Node> title = zge::zgui::findNode(*g_uiRoot, kStopFrameTitlePath, 0);
            zge::zgui::setVariable(title, kTextText, kTextTooManyLights);
        }
        else if (*g_parkCapacity <= *g_parkCapacityUsed)
        {
            const ee::Ptr<zge::zgui::Node> title = zge::zgui::findNode(*g_uiRoot, kStopFrameTitlePath, 0);
            zge::zgui::setVariable(title, kTextText, kTextTooManyObjects);
        }
        else
        {
            return 0;
        }
        const ee::Ptr<zge::zgui::Node> frame = zge::zgui::findNode(*g_uiRoot, kStopFramePath, 0);
        const ee::Ptr<zge::zgui::ListedAction> init = widgetAction(frame, kTextInit);
        zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, init, 1);
        markStopFrameOpen();
        return 1;
    }
    READABLE_FUNCTION(refuseWhenParkFull, Verified);

    uint32_t heldPiecesFit(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<uint32_t> allBuildable;
        *allBuildable = 1;
        forEachSelectedCell(editor, sym::requireBuildableCell, allBuildable);
        return *allBuildable;
    }
    READABLE_FUNCTION(heldPiecesFit, Verified);

    uint32_t cursorFree(ee::Ptr<ParkEditor> editor)
    {
        return editor->cursorState == kCursorFree ? 1 : 0;
    }
    READABLE_FUNCTION(cursorFree, Verified);

    // No test for no entry in use (the original's).
    uint8_t currentToolCategory()
    {
        return (*g_currentToolEntry)->category;
    }
    READABLE_FUNCTION(currentToolCategory, Verified);

    // An empty function in this build: called when the held object is swapped.
    void heldObjectChanged(uint8_t object, uint8_t rotation)
    {
        static_cast<void>(object);
        static_cast<void>(rotation);
    }
    READABLE_FUNCTION(heldObjectChanged, Verified);

    ee::Ptr<ParkStream> newParkStream(uint32_t bytes)
    {
        const ee::Ptr<ParkStream> stream(zge::heapAllocate(sizeof(ParkStream)));
        stream->bytes = ee::Ptr<uint8_t>(zge::heapAllocate(bytes));
        stream->size = bytes;
        stream->position = 0;
        return stream;
    }
    READABLE_FUNCTION(newParkStream, Verified);

    void freeParkStream(ee::Ptr<ParkStream> stream)
    {
        zge::heapFree(stream->bytes);
        game::tail(zge::heapFree, stream);
    }
    READABLE_FUNCTION(freeParkStream, Verified);

    void parkStreamWrite(ee::Address stream, ee::Address from, uint32_t count)
    {
        const ee::Ptr<ParkStream> park(stream);
        const ee::Ptr<const uint8_t> bytes(from);
        for (uint32_t i = 0; i < count; ++i)
        {
            park->bytes[park->position] = bytes[i];
            ++park->position;
        }
    }
    READABLE_FUNCTION(parkStreamWrite, Verified);

    void parkStreamRead(ee::Address stream, ee::Address to, uint32_t count)
    {
        const ee::Ptr<ParkStream> park(stream);
        const ee::Ptr<uint8_t> bytes(to);
        for (uint32_t i = 0; i < count; ++i)
        {
            bytes[i] = park->bytes[park->position];
            ++park->position;
        }
    }
    READABLE_FUNCTION(parkStreamRead, Verified);

    namespace
    {
        struct LoadParkFrame
        {
            zge::Vec4 position; // +0x00 the cell's centre: asked for and not used
            uint8_t theme;      // +0x10
            uint8_t hasObject;  // +0x11
            uint8_t _12[2];
            uint32_t version;   // +0x14 read and not looked at
        };
        static_assert(offsetof(LoadParkFrame, theme) == 0x10 && offsetof(LoadParkFrame, version) == 0x14);

        struct SaveParkFrame
        {
            uint8_t hasObject; // +0x00
            uint8_t _01[3];
            uint32_t version;  // +0x04
        };
        static_assert(offsetof(SaveParkFrame, version) == 4);
    }

    // The park read from its stream: the version, the theme (which sets
    // the grid up), the start, then every cell by row: its level, its
    // object if it is the object's own cell, its rails; then the gaps, the
    // high scores and the lights' brightness.
    void loadPark(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        ee::Local<LoadParkFrame> frame;
        parkStreamRead(stream, &frame->version, 4);
        parkStreamRead(stream, &frame->theme, 1);
        *g_parkThemePosition = frame->theme;
        setUpParkTheme(editor, frame->theme);
        loadParkStart(editor, stream);
        for (uint8_t row = 0; row < editor->rows; ++row)
        {
            for (uint8_t column = 0; column < editor->columns; ++column)
            {
                const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
                parkStreamRead(stream, cell, 1);
                cellWorldPosition(editor, column, row, &frame->position);
                parkStreamRead(stream, &frame->hasObject, 1);
                if (frame->hasObject)
                    loadPlacedObject(editor, stream, &cell->object);
                loadCellRails(editor, stream, column, row);
            }
        }
        loadGaps(editor, stream);
        readParkScores(editor, stream);
        parkStreamRead(stream, &g_parkLights->brightness, 4);
        showBrightness();
        fitThemeTowers(editor);
    }
    READABLE_FUNCTION(loadPark, Verified);

    // The park written to its stream, as loadPark reads it. A cell an
    // object covers that is not the object's own writes two zero bytes: no
    // object, and no rails.
    void savePark(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        ee::Local<SaveParkFrame> frame;
        frame->version = kParkStreamVersion;
        parkStreamWrite(stream, &frame->version, 4);
        parkStreamWrite(stream, &editor->theme, 1);
        saveParkStart(editor, stream);
        for (uint8_t row = 0; row < editor->rows; ++row)
        {
            for (uint8_t column = 0; column < editor->columns; ++column)
            {
                const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
                parkStreamWrite(stream, cell, 1);
                const ee::Ptr<PlacedObject> object = cell->object;
                if (!object)
                {
                    frame->hasObject = 0;
                    parkStreamWrite(stream, &frame->hasObject, 1);
                    saveCellRails(editor, stream, column, row);
                }
                else if (object->column != column || object->row != row)
                {
                    frame->hasObject = 0;
                    parkStreamWrite(stream, &frame->hasObject, 1);
                    parkStreamWrite(stream, &frame->hasObject, 1);
                }
                else
                {
                    frame->hasObject = 1;
                    parkStreamWrite(stream, &frame->hasObject, 1);
                    savePlacedObject(stream, cell->object);
                    saveCellRails(editor, stream, column, row);
                }
            }
        }
        saveGaps(editor, stream);
        writeParkScores(editor, stream);
        parkStreamWrite(stream, &g_parkLights->brightness, 4);
    }
    READABLE_FUNCTION(savePark, Verified);
}
