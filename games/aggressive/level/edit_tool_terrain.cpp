// The park editor's terrain tool: the select button, held, drags a rectangle of
// cells out from the cursor; let go, the rectangle is chosen and the left and
// right buttons raise and lower its ground a step at a time, a tape measure
// showing its height in feet. Reselect goes back to moving the cursor.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tool_terrain.cpp.

#include "aggressive/level/edit_tool_terrain.h"
#include "zge/editor/edit_tool_terrain.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"
#include "aggressive/level/edit_tools.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_terrain.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditTerrainToolClass = sym::g_editTerrainToolName; // "EditTerrainTool"
        constexpr uint32_t kEditToolClass = sym::g_terrainToolEditToolName;   // "EditTool"

        // A button row's states (park_ui.cpp).
        constexpr uint32_t kPressed = 1, kReleased = 4;
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3, kOnPressHoldOrRelease = 7;

        // EditTerrainToolPart::mode
        constexpr int32_t kTerrainMoving = 0, kTerrainSelecting = 1, kTerrainChosen = 2;
        // A raise or lower: a foot, in metres.
        constexpr ee::Float kTerrainStep = ee::Float(0.30476668f);
        constexpr ee::Float kFeetPerMetre = ee::Float(3.280484f);
        // The left and right buttons repeat every this many frames moving the cursor,
        // and faster raising and lowering.
        constexpr uint32_t kMoveRepeat = 10, kShapeRepeat = 4;

        // One cell of the cursor along a heading (degrees) from the camera's.
        int32_t moveCursor(ee::Ptr<EditToolPart> tool, ee::Float heading)
        {
            ee::Local<zge::Vec3> step;
            headingStep(ee::Ptr<zge::Vec3>(step), heading);
            return moveSelection(tool->selection, step.address());
        }

        // The chosen ground goes up (a negative rise: y is down) or down a step,
        // and the tape shows its new height.
        int32_t shapeGround(ee::Ptr<EditToolPart> tool, ee::Float rise)
        {
            ee::Local<zge::Vec4> offset;
            ee::Local<ee::Float> feet;
            offset->x = ee::Float(0.0f);
            offset->z = ee::Float(0.0f);
            offset->y = rise;
            *feet = selectionTopHeight(tool->selection, offset.address()) * kFeetPerMetre;
            return zgui::setNodeValueByPath(sym::g_textRootParkEditorMainUITapeTape, feet.address());
        }
    }

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
        terrain->mode = kTerrainMoving;
        terrain->step = kTerrainStep;
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

    // Put away with ground chosen, the tape goes too.
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
        if (terrain->mode != kTerrainChosen)
            return kTerrainChosen;
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
    // right repeat faster, and the tape comes up.
    int32_t terrainToolSelect(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(terrain, kEditToolClass));
        const uint32_t state = button->state;
        if (state == kPressed)
        {
            terrain->mode = kTerrainSelecting;
            return game::tail(beginSelectionRectangle, tool->selection);
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
        return game::tail(playTapeInitScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(terrainToolSelect, Verified);

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
        terrain->mode = kTerrainMoving;
        setUiButtonRepeat(button->ui, 12, kMoveRepeat);
        game::tail(setUiButtonRepeat, button->ui, 13, kMoveRepeat);
    }
    READABLE_FUNCTION_WAITS(terrainToolReselect, Verified);

    uint32_t terrainToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(deleteSelectedPieces, tool->selection);
    }
    READABLE_FUNCTION_WAITS(terrainToolDelete, Verified);
}
