// DM2's park editor GUI (./src/com/editor/editGui.c): the editor's zgui
// widgets (the info texts, the capacity bar, the tool's title and help) and
// the task that draws the editor's cursor.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/render/part.h"
#include "zge/memory/dmm.h"
#include "zge/memory/heaps.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // A widget as the scene object it is (a zgui node is one in ZGE2), and
    // the ACTION `name` of a widget, or 0.
    inline ee::Ptr<zge::SceneObject> widgetObject(ee::Ptr<zge::zgui::Node> widget)
    {
        return widget.cast<zge::SceneObject>();
    }
    inline ee::Ptr<zge::zgui::ListedAction> widgetAction(ee::Ptr<zge::zgui::Node> widget, ee::Ptr<const char> name)
    {
        return zge::zgui::findActionByPath(widgetObject(widget), name);
    }
    namespace zgui
    {
        // ZGE2's zgui keeps its memory in a DMM region where ZGE3's keeps a heap: the region, for its compaction.
        inline ee::Ptr<zge::DmmRegion> heapRegion(ee::Ptr<zge::Heap> heap) { return heap.cast<zge::DmmRegion>(); }
    }

    // The widgets of "ParkEditor.MainUI" the editor writes to, found once by
    // findEditGuiWidgets.
    constexpr uint32_t kEditInfoTextCount = 5; // BottomHeader.InfoA .. InfoE
    struct EditGuiWidgets
    {
        uint8_t _00[4];
        ee::Ptr<zge::zgui::Node> info[kEditInfoTextCount]; // +0x04 BottomHeader.InfoA .. InfoE
        ee::Ptr<zge::zgui::Node> capacityBar;              // +0x18 BottomHeader.CapaBar
        ee::Ptr<zge::zgui::Node> capacityEstimate;         // +0x1C BottomHeader.CapaBar.CapaEst
        uint8_t _20[8];
        ee::Ptr<zge::zgui::Node> selection;                // +0x28 TopHeader.TopTool.Selection
        uint8_t _2c[8];
        ee::Ptr<zge::zgui::Node> brightnessBar;            // +0x34 LightMenu.Menu.Brightness.Bar
    };
    static_assert(offsetof(EditGuiWidgets, capacityBar) == 0x18);
    static_assert(offsetof(EditGuiWidgets, selection) == 0x28);
    static_assert(offsetof(EditGuiWidgets, brightnessBar) == 0x34);

    // The GUI's task made (it draws the cursor every frame) and the tape's
    // text format set; the task deleted.
    int32_t startEditGui(ee::Ptr<ParkEditor> editor);
    void stopEditGui();

    // Info text `slot` (0 .. 4: InfoA .. InfoE) set; the result is zgui's, or
    // 0 for another slot.
    int32_t setInfoText(uint32_t slot, ee::Ptr<const char> text);

    void findEditGuiWidgets();

    // The info texts, the title and the help of the editor's tool, then the
    // capacity bar.
    void showEditToolTexts(ee::Ptr<ParkEditor> editor);

    // The cursor's colour now: red where its action is blocked, else yellow,
    // blue or dark blue, the first two pulsing.
    void cursorColour(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::VertexColour> colour);

    // Every piece marked kPieceUnderCursor takes the cursor's colour (the
    // terrain tool's free cursor only) and the mark becomes kPieceTinted; a
    // tinted piece the cursor has left is lit again.
    void tintMarkedPieces(ee::Ptr<ParkEditor> editor);

    // kPieceUnderCursor on every piece whose own cell is in the cursor's box.
    void markCursorPieces(ee::Ptr<ParkEditor> editor);

    // The cursor's cells as quads in the cursor's colour, their alpha pulsing;
    // the cursor's cross: four lines each way around its cell, fading out at
    // their ends.
    void drawCursorCells(ee::Ptr<ParkEditor> editor);
    void drawCursorCross(ee::Ptr<ParkEditor> editor);

    // The held object's box over the cursor's cells, as twelve lines: red
    // where the object does not fit, else blue. Nothing with no object held.
    void drawHeldObjectBox(ee::Ptr<ParkEditor> editor);

    // The GUI task's function: the tool's cursor drawn. Always 1.
    uint32_t drawEditGui(ee::Ptr<ParkEditor> editor);

    // The held object's name as the top header's Selection text and its help
    // as the bottom header's, each when there is one.
    int32_t showHeldObjectTexts();

    // The rail tool's draw while its cursor is not free: nothing.
    void drawRailToolStub(ee::Ptr<ParkEditor> editor);

    // The capacity bar's width from how much of the park's capacity is used,
    // and the estimate's width and place on it.
    void updateCapacityBar();
}
