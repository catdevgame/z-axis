// DM2's park editor: the editor's own record, as the editor's modules share
// it. The record belongs to the editor core (zgeif/zoverlay.c in DM2's
// build); the files of this folder (editor/editGui.c and what links after
// it) read these fields of it.
#pragma once

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/input/pads.h"
#include "zge/math/vec.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // The tool in the editor's hands (ParkEditor::tool): the names are the
    // titles the editor's top header shows for each.
    constexpr uint8_t kEditToolTerrain = 0; // "Terrain Tool"
    constexpr uint8_t kEditToolPiece = 1;   // the title is the piece category's name
    constexpr uint8_t kEditToolRail = 2;    // "Rail Tool"
    constexpr uint8_t kEditToolStart = 3;   // "Start Tool"
    constexpr uint8_t kEditToolGap = 4;     // "Gap Tool"
    constexpr uint8_t kEditToolCopy = 7;    // "Copy Tool"
    // The memory card's save or load screen has the pad (editSetModeSavePark,
    // editSetModeLoadPark; the editor's frame runs uiFrameUpdate).
    constexpr uint8_t kEditToolCardFrame = 8;
    // No tool: the editor is left (editBackToMain, editTestLevel).
    constexpr uint8_t kEditToolNone = 9;

    struct CellRails;

    // An object placed on the park's grid (0x1C bytes of the engine's heap):
    // which object of the catalog (edit_objects.h), how it is turned, the cell
    // it stands on, and its node in the scene. It may cover several cells:
    // each of them points at it.
    constexpr uint32_t kPieceUnderCursor = 1u << 0; // the cursor's cells cover it this frame
    constexpr uint32_t kPieceTinted = 1u << 1;      // drawn in the cursor's colour: relit when the cursor leaves
    struct PlacedObject
    {
        // The first two words are not written when the record is made.
        uint8_t _00[8];
        uint8_t object;           // +0x08 its index in the catalog
        uint8_t unused;           // +0x09 made 0 with the record and saved with the park; a load reads it and drops it
        uint8_t rotation;         // +0x0A quarter turns, 0 .. 3
        uint8_t column;           // +0x0B its own cell
        uint8_t row;              // +0x0C
        uint8_t _0d[3];
        ee::Ptr<zge::Node> node;  // +0x10 its shown node
        ee::Ptr<CellRails> rails; // +0x14 the rails placed on it (rail_tool.h), 0: none
        uint32_t flags;           // +0x18 kPiece...
    };
    static_assert(offsetof(PlacedObject, object) == 0x08 && offsetof(PlacedObject, unused) == 0x09 && offsetof(PlacedObject, rotation) == 0x0A);
    static_assert(offsetof(PlacedObject, column) == 0x0B && offsetof(PlacedObject, node) == 0x10);
    static_assert(offsetof(PlacedObject, rails) == 0x14 && offsetof(PlacedObject, flags) == 0x18 && sizeof(PlacedObject) == 0x1C);

    // One cell of the park's grid.
    struct ParkCell
    {
        int8_t level;                 // +0x00 the ground's level; the ground's height is its negative
        uint8_t _01[3];
        ee::Ptr<PlacedObject> object; // +0x04 the object standing on or covering the cell, 0: bare ground
        ee::Ptr<CellRails> rails;     // +0x08 the rails placed on the bare ground (rail_tool.h), 0: none
        uint8_t column, row;          // +0x0C the cell's own place
        uint8_t _0e[2];
        // +0x10 the ground tower the cell is part of: one node for four
        // cells (ground_towers.h).
        ee::Ptr<zge::Node> tower;
    };
    static_assert(sizeof(ParkCell) == 0x14 && offsetof(ParkCell, object) == 0x04 && offsetof(ParkCell, column) == 0x0C);
    static_assert(offsetof(ParkCell, tower) == 0x10);

    // A rectangle of the grid's cells, both ends included, with its levels
    // (only the cursor's box has levels).
    struct CellBox
    {
        int32_t firstColumn, firstRow; // +0x00
        int32_t top;                   // +0x08 the ground's height at the cursor (the cell's level, negated)
        int32_t lastColumn, lastRow;   // +0x0C
        int32_t bottom;                // +0x14 top less the held object's highest corner
    };
    static_assert(sizeof(CellBox) == 0x18 && offsetof(CellBox, lastColumn) == 0x0C);

    // What the cursor's action would do where it stands.
    constexpr uint8_t kCursorFree = 0;
    constexpr uint8_t kCursorBlocked = 2; // the red cursor, the "N/A" texts

    constexpr uint32_t kParkMaxColumns = 64; // the grid's cells each way, as the record holds them
    constexpr uint32_t kParkMaxRows = 64;

    struct ParkEditor
    {
        ee::Ptr<zge::Pad> pad;                  // +0x000 the pad that works the editor
        int8_t cursorColumn, cursorRow;         // +0x004 the cell the cursor is on (a theme's CURSOR to start with)
        // The selected rectangle: the corner that follows the cursor, and
        // how many cells it reaches back from that corner each way (1: the
        // corner's cell alone; 0 or less: it reaches forwards, to corner -
        // extent - 1).
        int8_t selectionColumns, selectionRows; // +0x006
        int8_t selectionColumn, selectionRow;   // +0x008
        int8_t unsnappedColumn, unsnappedRow;   // +0x00A where the cursor was before it snapped to an object's own cell
        uint8_t stretching;                     // +0x00C 1 while the cursor stretches the selection from its corner
        // +0x00D 1 while the selection is the held object's cells
        // (selectHeldObjectCells; a selection's reset and the terrain, rail
        // and copy tools' frames clear it): the cursor's moves keep the
        // selection so, and its cross is not drawn.
        uint8_t selectionIsHeldObject;
        uint8_t cursorState;                    // +0x00E kCursor...
        uint8_t selectionFollowing;             // +0x00F the last move's selectionFollows: the selection does not grow while set
        // What the tool's frame says of the cursor's move that follows it
        // (resetCursorRules before each frame).
        uint32_t cursorMoves;                   // +0x010 the pad's directions move the cursor
        uint32_t cursorStretches;               // +0x014 a move stretches the selection
        uint32_t selectionFollows;              // +0x018 a move takes the selection along
        uint32_t cursorStateLocked;             // +0x01C the cursor was blocked this frame and stays so
        uint32_t cursorSnapsToObjects;          // +0x020 the cursor lands on the own cell of the object it moves onto
        // +0x024 1 when the frame's tool buttons (toolButtonsFrame) find the
        // terrain tool in hand; cleared with the rules. Nothing reads it.
        uint32_t terrainInHand;
        int8_t levelRepeat;                     // +0x028 the terrain tool: frames until held Up or Down changes the level again
        int8_t padRepeat;                       // +0x029 frames until a held direction moves the cursor again
        // The step the tool in hand has reached in its own sequence (the
        // terrain tool: idle, selecting, levelling). The cursor is yellow,
        // blue or dark blue for 0, 1 and 2, and the gap tool offers
        // "Backup" while it is not 0.
        uint8_t toolStep;                       // +0x02A
        uint8_t _02b;
        // 1 while the editor's GUI has the player (markStopFrameOpen sets
        // it when the stop frame or a scroll of the tool roll is played; the
        // script function editUnlockUser clears it): the editor's frame then
        // neither moves the cursor nor the view.
        uint32_t userLocked;
        uint8_t _030[8];
        // The view (edit_camera.h): its camera, where it stands and what it
        // looks at in the world.
        ee::Ptr<zge::SceneObject> viewCamera;   // +0x038
        uint8_t _03c[4];
        zge::Vec4 viewEye;                      // +0x040
        zge::Vec4 viewTarget;                   // +0x050
        // What the player has asked for: the camera's heading and pitch in
        // degrees and its distance.
        ee::Float viewHeading;                  // +0x060 -180 .. 180
        // +0x064 the distance the view is set up with (kStartDistance, as
        // viewDistance's). Nothing reads it (PS2 and GameCube).
        ee::Float startDistance;
        ee::Float viewDistance;                 // +0x068 15 .. 100
        ee::Float viewPitch;                    // +0x06C 5 .. 85
        // The cell the view looks at and its ground, each easing towards
        // the cursor's.
        ee::Float viewColumn;                   // +0x070
        ee::Float viewGround;                   // +0x074
        ee::Float viewRow;                      // +0x078
        // +0x07C the fourth word of the three as a quadword: updateEditView
        // keeps the four in its frame; nothing sets or reads it alone.
        uint32_t viewCellW;
        uint8_t viewChanged;                    // +0x080 1 once the player has moved the view
        uint8_t _081[0x0F];
        // +0x090 1 once a theme's grid is set up (setUpParkTheme), 0 when the
        // park's cells or objects are cleared. Nothing reads it.
        uint8_t themeSetUp;
        uint8_t theme;                          // +0x091 the park's theme (edit_themes.h), by its index
        uint8_t rows;                           // +0x092 the park's size in cells
        uint8_t columns;                        // +0x093
        ParkCell cells[kParkMaxRows][kParkMaxColumns]; // +0x094 by row, then column
        // +0x14094 the park's start (start_tool.h): its cell, its heading
        // in quarter turns, and 1 once it is placed.
        int8_t startColumn, startRow;
        int8_t startTurns;
        int8_t startPlaced;
        // +0x14098 the park's high-score tables (park_scores.h), as the park's file carries them
        uint8_t scores[0x21C];
        uint8_t tool;                           // +0x142B4 kEditTool...
    };
    static_assert(offsetof(ParkEditor, cursorColumn) == 0x04 && offsetof(ParkEditor, selectionColumn) == 0x08);
    static_assert(offsetof(ParkEditor, cursorState) == 0x0E && offsetof(ParkEditor, cursorMoves) == 0x10);
    static_assert(offsetof(ParkEditor, cursorSnapsToObjects) == 0x20 && offsetof(ParkEditor, levelRepeat) == 0x28 && offsetof(ParkEditor, userLocked) == 0x2C);
    static_assert(offsetof(ParkEditor, toolStep) == 0x2A && offsetof(ParkEditor, selectionIsHeldObject) == 0x0D);
    static_assert(offsetof(ParkEditor, terrainInHand) == 0x24 && offsetof(ParkEditor, startDistance) == 0x64);
    static_assert(offsetof(ParkEditor, viewCellW) == 0x7C);
    static_assert(offsetof(ParkEditor, viewCamera) == 0x38 && offsetof(ParkEditor, viewEye) == 0x40 && offsetof(ParkEditor, viewColumn) == 0x70);
    static_assert(offsetof(ParkEditor, viewHeading) == 0x60 && offsetof(ParkEditor, viewPitch) == 0x6C && offsetof(ParkEditor, viewChanged) == 0x80);
    static_assert(offsetof(ParkEditor, themeSetUp) == 0x90 && offsetof(ParkEditor, theme) == 0x91);
    static_assert(offsetof(ParkEditor, rows) == 0x92);
    static_assert(offsetof(ParkEditor, scores) == 0x14098 && offsetof(ParkEditor, startColumn) == 0x14094);
    static_assert(offsetof(ParkEditor, cells) == 0x94);
    static_assert(offsetof(ParkEditor, tool) == 0x142B4);

    // The cell at (column, row): the address the original computes, whatever
    // the numbers (a cell outside the park is never read: the callers ask
    // cellInPark or cellCanBeBuiltOn first).
    inline ee::Ptr<ParkCell> parkCell(ee::Ptr<ParkEditor> editor, int32_t column, int32_t row)
    {
        const ee::Ptr<ParkCell> cells(&editor->cells[0][0]);
        return cells + (row * static_cast<int32_t>(kParkMaxColumns) + column);
    }

    // Whether a node's collision frame is there to read. The original tests
    // the address 8 bytes into the frame (a field's, never the frame's own),
    // so only a frame pointer of -8 fails: a node with no frame reads on.
    constexpr uint32_t kCollisionFrameTestedAt = 8;
    inline bool hasCollisionFrame(ee::Ptr<zge::Node> node)
    {
        return ee::Address(node->lods) + kCollisionFrameTestedAt != 0;
    }
}
