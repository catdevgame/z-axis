// The park editor's terrain tool: the select button, held, drags a rectangle of
// cells out from the cursor; let go, the rectangle is chosen and the left and
// right buttons raise and lower its ground a step at a time (the park's height
// increment), a tape measure showing its height in metres. Reselect goes back to
// moving the cursor. BMX XXX's plays a sound at each move, choice and reselect.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tool_terrain.cpp.

#include "bmx/editor/edit_tool_terrain.h"
#include "zge/editor/edit_tool_terrain.h"
#include "bmx/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/zgui/widget.h"
#include "bmx/editor/edit_tools.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tool_terrain.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditTerrainToolClass = sym::g_editTerrainToolName; // "EditTerrainTool"
        constexpr uint32_t kEditToolClass = sym::g_terrainToolEditToolName;   // "EditTool"
        constexpr uint32_t kTextTapeDistance = sym::g_textRootParkEditorMainUITapeTapeDistance; // "Root.ParkEditor.MainUI.Tape.Tape.Distance"

        // A button row's state as its handler sees it.
        constexpr uint32_t kPressed = 1, kReleased = 4;
        // When a row's handler runs.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3, kOnPressHoldOrRelease = 7;

        // EditTerrainToolPart::mode
        constexpr int32_t kTerrainMoving = 0, kTerrainSelecting = 1, kTerrainChosen = 2;

        // Frames between a held button's repeats: moving the cursor, shaping the chosen ground.
        constexpr uint32_t kMoveRepeat = 10, kShapeRepeat = 4;

        // The cursor a cell along the heading, with the move's sound.
        int32_t moveCursor(ee::Ptr<EditToolPart> tool, ee::Float heading)
        {
            ee::Local<zge::Vec3> step;
            headingStep(ee::Ptr<zge::Vec3>(step), heading);
            moveSelection(tool->selection, step.address());
            return static_cast<int32_t>(soundEvent(kMoveSound, 0, 0, 0, 0));
        }

        // The chosen ground goes up (a negative rise: y is down) or down a step, and
        // the tape shows its new height in metres (Aggressive Inline's shows feet).
        int32_t shapeGround(ee::Ptr<EditToolPart> tool, ee::Float rise)
        {
            ee::Local<zge::Vec4> offset;
            ee::Local<ee::Float> height;
            offset->x = ee::Float(0.0f);
            offset->z = ee::Float(0.0f);
            offset->y = rise;
            *height = selectionTopHeight(tool->selection, offset.address());
            return zge::zgui::setNodeValueByPath(kTextTapeDistance, height.address());
        }
    }

    // Switched to: its buttons, the camera on the selection, the first help page, the
    // step (the park's height increment; Aggressive Inline's is a foot) and the texts.
    int32_t terrainToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(editObjectPart(tool, kEditTerrainToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::terrainToolLeft, terrain, kMoveRepeat, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::terrainToolRight, terrain, kMoveRepeat, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::terrainToolForward, terrain, kMoveRepeat, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::terrainToolBack, terrain, kMoveRepeat, noDeadzone);
        setUiButton(ui, 5, kOnPressHoldOrRelease, sym::terrainToolSelect, terrain, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::terrainToolReselect, terrain, 1, noDeadzone);
        setUiButton(ui, 3, kOnPress, sym::terrainToolDelete, terrain, 1, noDeadzone);
        tool->selection = toolSetSelection(tool->toolSet);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        showEditHelpPage(ui, 0);
        const ee::Float step = parkHeightIncrement(toolSetPark(tool->toolSet));
        terrain->mode = kTerrainMoving;
        terrain->step = step;
        setInfoButtonText(ui, 1, sym::g_textSelect);
        setInfoButtonText(ui, 2, sym::g_textNa3);
        setInfoButtonText(ui, 3, sym::g_textNa3);
        setInfoButtonText(ui, 4, sym::g_textDelete);
        setInfoButtonText(ui, 5, sym::g_textMove7);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textTerrainTool));
        setInfoHelpText(ui, sym::g_textRaiseOrLowerTerrainUsingThe);
        return game::tail(refreshToolSelection, tool);
    }
    READABLE_FUNCTION_WAITS(terrainToolActivate, Verified);

    // Put away: its buttons go, and a selection in progress or chosen takes the tape
    // down with it (Aggressive Inline's only a chosen one). Nothing to take down: 0.
    int32_t terrainToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(editObjectPart(tool, kEditTerrainToolClass));
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 5);
        clearUiButton(ui, 4);
        clearUiButton(ui, 3);
        if (terrain->mode != kTerrainSelecting && terrain->mode != kTerrainChosen)
            return 0;
        const int32_t result = playTapeShutdownScript(ui);
        terrain->mode = kTerrainMoving;
        return result;
    }
    READABLE_FUNCTION_WAITS(terrainToolDeactivate, Verified);

    // Left and right move the cursor, or with ground chosen raise and lower it;
    // forward and back only move the cursor. Their row's data is the terrain part.
    int32_t terrainToolLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        if (terrain->mode != kTerrainChosen)
            return moveCursor(tool, editCameraHeading(tool->editCamera) + ee::Float(90.0f));
        return shapeGround(tool, -terrain->step);
    }
    READABLE_FUNCTION_WAITS(terrainToolLeft, Verified);

    int32_t terrainToolRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        if (terrain->mode != kTerrainChosen)
            return moveCursor(tool, editCameraHeading(tool->editCamera) - ee::Float(90.0f));
        return shapeGround(tool, terrain->step);
    }
    READABLE_FUNCTION_WAITS(terrainToolRight, Verified);

    int32_t terrainToolForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        if (terrain->mode == kTerrainChosen)
            return kTerrainChosen;
        return moveCursor(tool, editCameraHeading(tool->editCamera));
    }
    READABLE_FUNCTION_WAITS(terrainToolForward, Verified);

    int32_t terrainToolBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        if (terrain->mode == kTerrainChosen)
            return kTerrainChosen;
        return moveCursor(tool, editCameraHeading(tool->editCamera) + ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(terrainToolBack, Verified);

    // Pressed: the rectangle starts at the cursor. Let go: it is chosen, left and
    // right repeat faster, the tape comes up with the ground's height, and the
    // choice sounds.
    int32_t terrainToolSelect(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        const uint32_t state = button->state;
        if (state == kPressed)
        {
            terrain->mode = kTerrainSelecting;
            return beginSelectionRectangle(tool->selection);
        }
        if (state != kReleased)
            return kReleased;
        endSelectionRectangle(tool->selection);
        terrain->mode = kTerrainChosen;
        setUiButtonRepeat(button->ui, 12, kShapeRepeat);
        setUiButtonRepeat(button->ui, 13, kShapeRepeat);
        setInfoButtonText(button->ui, 1, sym::g_textReselect);
        setInfoButtonText(button->ui, 2, sym::g_textDrop);
        setInfoButtonText(button->ui, 3, sym::g_textNa3);
        setInfoButtonText(button->ui, 4, sym::g_textDelete);
        playTapeInitScript(button->ui);
        soundEvent(kPlaceSound, 0, 0, 0, 0);
        return shapeGround(tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(terrainToolSelect, Verified);

    // Reselect: the selection cleared and, with ground chosen or being chosen, the
    // texts back to selecting and the tape down; the turn sound, and the cursor moves again.
    void terrainToolReselect(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        clearSelection(tool->selection);
        refreshToolSelection(tool);
        if (terrain->mode != kTerrainMoving)
        {
            setInfoButtonText(button->ui, 1, sym::g_textSelect);
            setInfoButtonText(button->ui, 2, sym::g_textNa3);
            setInfoButtonText(button->ui, 3, sym::g_textNa3);
            setInfoButtonText(button->ui, 4, sym::g_textDelete);
            playTapeShutdownScript(button->ui);
        }
        soundEvent(kRotateSound, 0, 0, 0, 0);
        terrain->mode = kTerrainMoving;
        setUiButtonRepeat(button->ui, 12, kMoveRepeat);
        game::tail(setUiButtonRepeat, button->ui, 13, kMoveRepeat);
    }
    READABLE_FUNCTION_WAITS(terrainToolReselect, Verified);

    // Delete: refused (its sound) while the pieces held could be placed, else the delete
    // sound; the selected pieces go either way (BMX XXX; Aggressive Inline's only deletes).
    uint32_t terrainToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        if (canPlaceHeldPieces(tool->selection) != 0)
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        else
            soundEvent(kDeleteSound, 0, 0, 0, 0);
        return game::tail(deleteSelectedPieces, tool->selection);
    }
    READABLE_FUNCTION_WAITS(terrainToolDelete, Verified);
}
