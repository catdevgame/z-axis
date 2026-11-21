// The park editor's start tool: the selection holds the player's start object,
// which the pad moves and turns; placing it takes away the one placed before.

#include "zge/editor/edit_tool_start.h"
#include "zge/editor/game.h"
#include "zge/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tool_start.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditStartToolClass = sym::g_editStartToolName; // "EditStartTool"
        constexpr uint32_t kEditToolClass = sym::g_startToolEditToolName;  // "EditTool"

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // The tool's icon in the tool bar's font.
        constexpr int8_t kStartToolIcon = 'P';
    }

    int32_t registerEditStartToolClass()
    {
        return game::tail(registerEditClass, kEditStartToolClass, sizeof(EditStartToolPart), kEditToolClass, sym::constructEditStartTool, sym::destructEditStartTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditStartToolClass, Verified);

    void constructEditStartTool(uint32_t data)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        setEditToolMethods(tool, sym::g_startToolMethods);
        setEditToolName(tool, sym::g_textStartTool);
        tool->iconChar = kStartToolIcon;
    }
    READABLE_FUNCTION(constructEditStartTool, Verified);

    void destructEditStartTool()
    {
    }
    READABLE_FUNCTION(destructEditStartTool, Verified);

    // Picked: the selection holds a new start object.
    int32_t startToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditStartToolPart> start(editObjectPart(tool, kEditStartToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::startToolMoveLeft, start, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::startToolMoveRight, start, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::startToolMoveForward, start, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::startToolMoveBack, start, 10, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::startToolPlace, start, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::startToolRotate, start, 1, noDeadzone);
        const uint32_t pieceDef = findPieceDef(static_cast<uint32_t>(toolSetCatalog(tool->toolSet)), sym::g_textStartObject2);
        start->pieceDef = pieceDef;
        const ee::Ptr<EditObjectPart> object(newEditObject(sym::g_startToolEditObjectName, pieceDef));
        makeEditObjectDisplay(object, 0);
        holdPiece(tool->selection, object);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        setInfoButtonText(ui, 1, sym::g_textPlace4);
        setInfoButtonText(ui, 2, sym::g_textRotate5);
        setInfoButtonText(ui, 3, sym::g_textNa6);
        setInfoButtonText(ui, 4, sym::g_textNa6);
        setInfoButtonText(ui, 5, sym::g_textMove6);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textStartTool));
        return game::tail(setInfoHelpText, ui, sym::g_textPlaceThePlayersStartingPosition);
    }
    READABLE_FUNCTION_WAITS(startToolActivate, Verified);

    void startToolUpdate(ee::Ptr<EditToolPart> tool)
    {
        game::tail(updateSelectionCursor, tool->selection);
    }
    READABLE_FUNCTION_WAITS(startToolUpdate, Verified);

    int32_t startToolDraw(ee::Ptr<EditToolPart> tool)
    {
        return game::tail(drawSelectionInColour, tool->selection);
    }
    READABLE_FUNCTION_WAITS(startToolDraw, Verified);

    int32_t startToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 5);
        clearUiButton(ui, 4);
        return game::tail(clearSelection, tool->selection);
    }
    READABLE_FUNCTION_WAITS(startToolDeactivate, Verified);

    // One cell of the selection along a heading (degrees) from the camera's.
    int32_t startToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(startToolMove, Verified);
}
