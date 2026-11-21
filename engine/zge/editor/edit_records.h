#pragma once

// Records the adopted functions read: their layout and names are the work database's (readable_names_layout.tsv);
// each has only the fields code reads so far.

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/math/box.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge { struct Task; } // (zge/platform/tasks.h)
namespace zge::editor { struct EditCamera; } // (zge/editor/park_camera.h)

namespace zge::editor
{
    // A piece of the park editor's catalog (ObjectDefBegin .. ObjectDefEnd in
    // the piece file, level/park_catalog.cpp): what the object, rail and
    // pisser tools hold.
    struct PieceDef
    {
        zge::ListNode link;    // +0x00 in its category
        char name[0x20];       // +0x08 "Name"
        char className[0x40];  // +0x28 "ClassName": its edit object's class
        uint32_t objectHash;   // +0x68 nameHash("MaxObjectName"): its world object
        uint32_t category;     // +0x6C "CategoryIndex"
        uint32_t secretId;     // +0x70 "SecretID"
        uint32_t cellsX;       // +0x74
        uint32_t cellsZ;       // +0x78
        uint32_t cellsY;       // +0x7C
        int32_t originCellX;   // +0x80 from its first cell to its origin's
        int32_t originCellY;   // +0x84
        int32_t originCellZ;   // +0x88
        uint8_t _8c[4];
        zge::Box bounds;       // +0x90 in park space, about its origin's cell
        uint32_t anchors;      // +0xB0 an edit list
        uint8_t _b4[4];
        uint32_t index;        // +0xB8 in the catalog
        uint8_t _bc[4];
    };
    static_assert(sizeof(PieceDef) == 0xC0 && offsetof(PieceDef, bounds) == 0x90 && offsetof(PieceDef, index) == 0xB8);

    // An edit object's display (level/park_display.cpp): the scene node it shows.
    struct EditDisplay
    {
        uint32_t node;
    };
    static_assert(sizeof(EditDisplay) == 4);

    // An edit object's own part (class "EditObject", level/edit_object.cpp): where
    // it stands, its display, anchors and piece, and its kind's methods.
    struct EditObjectPart
    {
        zge::Matrix localMatrix;  // +0x00 under its parent anchor; its translation is the object's position
        zge::Matrix matrix;       // +0x40 in the world
        uint32_t owner;           // +0x80 the object it belongs to (setEditObjectOwner)
        int32_t display;          // +0x84
        int32_t flags[4];         // +0x88 by EditObjectFlagIndex (the kind's flagAt): a plain piece's 0, 1, 1, 1,
                                  //       a tower's 1, 0, 0, 0
        int32_t cost;             // +0x98 what it adds to the park's cost (0 for a tower)
        int32_t anchors;          // +0x9C
        int32_t parentAnchor;     // +0xA0
        int32_t pieceDef;         // +0xA4
        int32_t methods;          // +0xA8 its kind's EditObjectMethods
        uint8_t _ac[0x4];
    };
    static_assert(sizeof(EditObjectPart) == 0xB0 && offsetof(EditObjectPart, matrix) == 0x40);
    // EditObjectPart::flags' indices (editObjectFlag): a tower's height counts, it
    // goes with the remove-flagged message, the selection shows its box, it snaps.
    enum EditObjectFlagIndex : uint32_t
    {
        kObjectHasHeight = 0,
        kObjectRemovable = 1,
        kObjectShowsBox = 2,
        kObjectSnaps = 3,
    };
    static_assert(offsetof(EditObjectPart, owner) == 0x80 && offsetof(EditObjectPart, display) == 0x84);
    static_assert(offsetof(EditObjectPart, flags) == 0x88);
    static_assert(offsetof(EditObjectPart, cost) == 0x98);
    static_assert(offsetof(EditObjectPart, anchors) == 0x9C);
    static_assert(offsetof(EditObjectPart, parentAnchor) == 0xA0);
    static_assert(offsetof(EditObjectPart, pieceDef) == 0xA4);
    static_assert(offsetof(EditObjectPart, methods) == 0xA8);

    // A tower's own part (class "EditTowerObject", level/edit_tower.cpp). Four park
    // cells make one tower: the display's vertices are tagged by corner, and each
    // corner's piece raises its own.
    struct EditTowerObjectPart
    {
        int32_t corner;   // +0x00 1 to 4 (1 on even x and z, the one holding the shared display), -1 when made
        ee::Float minY;   // +0x04 setTowerBottom's height, negated: a move below it is undone
        ee::Float maxY;   // +0x08 setTowerTop's, negated: a move above it is undone
    };
    static_assert(sizeof(EditTowerObjectPart) == 0xC);

    struct EditToolSet;
    // The "EditTool" part of a tool (level/park_anchors.cpp makes it).
    struct EditToolPart
    {
        uint32_t next;      // +0x00 the tool set's tools are a list of these parts
        uint32_t previous;  // +0x04
        uint32_t word08;    // +0x08 1 when made
        char name[0x20];    // +0x0C
        int8_t iconChar;    // +0x2C
        uint8_t _2d[3];
        uint32_t methods;   // +0x30 its kind's EditToolMethods
        ee::Ptr<EditToolSet> toolSet;          // +0x34
        ee::Ptr<EditCamera> editCamera; // +0x38
        int32_t selection;  // +0x3C
    };
    static_assert(sizeof(EditToolPart) == 0x40 && offsetof(EditToolPart, methods) == 0x30);
    static_assert(offsetof(EditToolPart, iconChar) == 0x2C);
    static_assert(offsetof(EditToolPart, toolSet) == 0x34);
    static_assert(offsetof(EditToolPart, editCamera) == 0x38);
    static_assert(offsetof(EditToolPart, selection) == 0x3C);

    // A list as zge's list functions keep it (zge::List), its records by address.
    struct EditList
    {
        uint32_t first;  // +0x00
        uint32_t last;   // +0x04
        int32_t count;   // +0x08
    };
    static_assert(sizeof(EditList) == 0xC);

    // The park editor's tool set (newEditToolSet, level/edit_tools.cpp): its tools,
    // the one in use and those put aside under it, and what they work with.
    struct EditToolSet
    {
        uint32_t built;          // +0x00 1, newEditToolSet's last step; the tool set's own code does not read it
        uint32_t cameraActive;   // +0x04 the camera follows the sticks while set
        int32_t hasUiScript;     // +0x08 the theme's UI script is attached
        EditList tools;          // +0x0C each tool's EditTool part, in the order made
        ee::Ptr<zge::Task> task; // +0x18 the task that draws the editor
        EditList toolStack;     // +0x1C nodes holding the tools pushEditorTool put aside, the last on top
        char toolKeys[0x20];     // +0x28 each tool's key, the last made first (the UI's tool roll)
        int32_t currentTool;     // +0x48
        ee::Ptr<EditCamera> editCamera;      // +0x4C
        int32_t park;            // +0x50
        int32_t catalog;         // +0x54
        int32_t ui;              // +0x58
        int32_t selection;       // +0x5C
    };
    static_assert(sizeof(EditToolSet) == 0x60);
    static_assert(offsetof(EditToolSet, hasUiScript) == 0x8);
    static_assert(offsetof(EditToolSet, tools) == 0xC && offsetof(EditToolSet, toolStack) == 0x1C);
    static_assert(offsetof(EditToolSet, task) == 0x18 && offsetof(EditToolSet, toolKeys) == 0x28);
    static_assert(offsetof(EditToolSet, currentTool) == 0x48);
    static_assert(offsetof(EditToolSet, editCamera) == 0x4C);
    static_assert(offsetof(EditToolSet, park) == 0x50);
    static_assert(offsetof(EditToolSet, catalog) == 0x54);
    static_assert(offsetof(EditToolSet, ui) == 0x58);
    static_assert(offsetof(EditToolSet, selection) == 0x5C);

    // A node of the tool set's stack of tools put aside.
    struct ToolStackNode
    {
        uint32_t next, previous; // +0x00 the list's links
        int32_t tool;            // +0x08
    };
    static_assert(sizeof(ToolStackNode) == 0xC);

    // What a button's handler gets.
    struct EditUiButton
    {
        uint32_t state;   // +0x00 the state the handler runs for: kPressed, kHeld, kReleased (park_ui.cpp)
        ee::Float rate;   // +0x04
        uint8_t _08[0x8];
        int32_t data;
        int32_t ui;
    };
    static_assert(offsetof(EditUiButton, rate) == 0x4 && offsetof(EditUiButton, data) == 0x10);
    static_assert(offsetof(EditUiButton, ui) == 0x14);

    struct ParkEditor
    {
        uint8_t _00[0x4];
        int32_t park;
        uint8_t _08[0x4];
        int32_t parkProjectTheme;
        int32_t pieceCatalog;
    };
    static_assert(offsetof(ParkEditor, park) == 0x4);
    static_assert(offsetof(ParkEditor, parkProjectTheme) == 0xC);
    static_assert(offsetof(ParkEditor, pieceCatalog) == 0x10);
    // Where the park editor's camera looks from, and how: what
    // startEditCameraTransition slides between.
    struct EditCameraView
    {
        zge::Vec4 lookAt;     // +0x00
        ee::Float heading;    // +0x10
        ee::Float tilt;       // +0x14
        ee::Float distance;   // +0x18
        uint32_t _1c = 0;     // nothing reads it; a view the camera makes has 0 here
    };
    static_assert(sizeof(EditCameraView) == 0x20 && offsetof(EditCameraView, distance) == 0x18);

    // The pisser tool's own part (level/edit_tool_pisser.cpp).
    struct EditPisserToolPart
    {
        int32_t mode;                 // +0x00 moving the selection or choosing a piece
        uint32_t pisserNode;          // +0x04 "Terrain_Pisser"
        uint8_t _08[0x8];
        zge::Vec4 pisserRestPosition; // +0x10 where it stood when the tool was made
        uint32_t sliderNode;          // +0x20 "Terrain_Slider"
        uint8_t _24[0xC];
        zge::Vec4 sliderRestPosition; // +0x30
        ee::Ptr<PieceDef> pieceDef;   // +0x40 the piece chosen
        uint32_t category;            // +0x44 of the tool set's catalog: the pisser's pieces
        uint32_t stand;               // +0x48 an anchor: the piece chosen stands on it in choose mode
        uint8_t _4c[0x4];
        EditCameraView moveView;      // +0x50
        EditCameraView chooseView;    // +0x70
        int32_t action;               // +0x90 a make or delete under way
        uint32_t ui;                  // +0x94 whose buttons it holds meanwhile
        ee::Float actionTime;         // +0x98 seconds since the make or delete started
        uint8_t _9c[0x4];
    };
    static_assert(sizeof(EditPisserToolPart) == 0xA0);
    static_assert(offsetof(EditPisserToolPart, sliderNode) == 0x20 && offsetof(EditPisserToolPart, pieceDef) == 0x40);
    static_assert(offsetof(EditPisserToolPart, moveView) == 0x50 && offsetof(EditPisserToolPart, chooseView) == 0x70);
    static_assert(offsetof(EditPisserToolPart, action) == 0x90 && offsetof(EditPisserToolPart, actionTime) == 0x98);

    // The park selection's records (park_selection.cpp), for the edit tools that ask it.
    struct CellIndex
    {
        int32_t x, y, z;
    };
    static_assert(sizeof(CellIndex) == 0x0C);

    // The crane tool's own part (level/edit_tool_crane.cpp): the crane's nodes and
    // their local matrices as they stood when the tool was made.
    struct EditCraneToolPart
    {
        uint8_t _00[0x4];
        uint32_t armNode;              // +0x04 "Terrain_CranearmA"
        uint32_t craneNode;            // +0x08 "Terrain_Crane"
        uint32_t boltANode;            // +0x0C "Terrain_CraneBoltA"
        uint32_t boltBNode;            // +0x10 "Terrain_CraneBoltB"
        uint32_t chainNode;            // +0x14 "Terrain_Cranechain"
        uint8_t _18[0x8];
        zge::Matrix armRestMatrix;     // +0x20
        zge::Matrix craneRestMatrix;   // +0x60
        zge::Matrix boltARestMatrix;   // +0xA0
        zge::Matrix boltBRestMatrix;   // +0xE0
        zge::Matrix chainRestMatrix;   // +0x120
    };
    static_assert(sizeof(EditCraneToolPart) == 0x160 && offsetof(EditCraneToolPart, chainNode) == 0x14);
    static_assert(offsetof(EditCraneToolPart, armRestMatrix) == 0x20 && offsetof(EditCraneToolPart, chainRestMatrix) == 0x120);
    // The vacuum tool's own part (level/edit_tool_vacuum.cpp): the hangar crane's
    // nodes and their local matrices as they stood when the tool was made.
    constexpr int kVacuumHeads = 6;
    struct EditVacuumToolPart
    {
        uint32_t craneNodeB2;                           // +0x00 "Terrain_HangerCraneB002": the beam
        uint32_t craneNodeB1;                           // +0x04 "Terrain_HangerCraneB001": the trolley
        uint8_t _08[0x8];
        zge::Matrix craneB2RestMatrix;                  // +0x10
        zge::Matrix craneB1RestMatrix;                  // +0x50
        uint32_t vacuumNodes[kVacuumHeads];             // +0x90 "Terrain_HangerVac001".."006"
        uint8_t _a8[0x8];
        zge::Matrix vacuumRestMatrices[kVacuumHeads];   // +0xB0
    };
    static_assert(sizeof(EditVacuumToolPart) == 0x230 && offsetof(EditVacuumToolPart, craneB1RestMatrix) == 0x50);
    static_assert(offsetof(EditVacuumToolPart, vacuumNodes) == 0x90 && offsetof(EditVacuumToolPart, vacuumRestMatrices) == 0xB0);
    struct AngleWords
    {
        uint16_t x, y, z;
    };
    static_assert(sizeof(AngleWords) == 6);

    struct Colour
    {
        uint8_t r, g, b, a;
    };

    // What the selection draws in.
    struct SelectionColours
    {
        Colour cell, box;
        Colour heldMisfit, heldFit; // a held piece's box, by whether it fits
    };
    static_assert(sizeof(SelectionColours) == 0x10);

}
