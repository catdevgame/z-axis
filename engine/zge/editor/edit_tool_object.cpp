// The park editor's object tools: one per catalog category. The selection holds
// a piece of the category, which the pad moves, turns, places and deletes, and
// two buttons step through the category's unlocked pieces.

#include "zge/editor/edit_tool_object.h"
#include "zge/editor/game.h"
#include "zge/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_messages.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tool_object.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectToolClass = sym::g_editObjectToolName; // "EditObjectTool"
        constexpr uint32_t kEditToolClass = sym::g_objectToolEditToolName;   // "EditTool"

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // setEditCameraTarget, and what it leaves in the result register (the tool's methods return it).
        int32_t setEditCameraTargetKeepingResult(ee::Ptr<EditCamera> camera, uint32_t target) { return ee::tailCall<int32_t>(sym::setEditCameraTarget, camera, target); }

        // A new edit object of the tool's piece, with its display.
        ee::Ptr<EditObjectPart> newPieceObject(ee::Ptr<EditObjectToolPart> objectTool)
        {
            const ee::Ptr<EditObjectPart> object(newEditObject(pieceDefClassName(objectTool->pieceDef), objectTool->pieceDef));
            makeEditObjectDisplay(object, 0);
            return object;
        }

        // The piece's name, and the help text every piece shows.
        void showPiece(uint32_t ui, ee::Ptr<EditObjectToolPart> objectTool)
        {
            setObjectSelectionText(ui, pieceDefName(objectTool->pieceDef));
            setInfoHelpText(ui, pieceDefZero());
        }
    }

    int32_t registerEditObjectToolClass()
    {
        return game::tail(registerEditClass, kEditObjectToolClass, sizeof(EditObjectToolPart), kEditToolClass, sym::constructEditObjectTool, sym::destructEditObjectTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditObjectToolClass, Verified);

    // The tool takes its category's icon and name, and starts on its first piece.
    ee::Ptr<PieceDef> constructEditObjectTool(uint32_t data, uint32_t category)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        const ee::Ptr<EditObjectToolPart> objectTool(editObjectPart(data, kEditObjectToolClass));
        objectTool->category = category;
        tool->iconChar = static_cast<int8_t>(categoryIconChar(category));
        setEditToolMethods(tool, sym::g_objectToolMethods);
        setEditToolName(tool, categoryName(objectTool->category));
        const ee::Ptr<PieceDef> first = categoryFirstPiece(objectTool->category);
        objectTool->pieceDef = first;
        return first;
    }
    READABLE_FUNCTION(constructEditObjectTool, Verified);

    void destructEditObjectTool()
    {
    }
    READABLE_FUNCTION(destructEditObjectTool, Verified);

    // Picked: the selection holds a new piece of the tool's kind.
    int32_t objectToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditObjectToolPart> objectTool(editObjectPart(tool, kEditObjectToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::objectToolMoveLeft, objectTool, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::objectToolMoveRight, objectTool, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::objectToolMoveForward, objectTool, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::objectToolMoveBack, objectTool, 10, noDeadzone);
        setUiButton(ui, 7, kOnPress, sym::objectToolPreviousPiece, objectTool, 1, noDeadzone);
        setUiButton(ui, 10, kOnPress, sym::objectToolNextPiece, objectTool, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::objectToolRotate, objectTool, 1, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::objectToolPlace, objectTool, 1, noDeadzone);
        setUiButton(ui, 3, kOnPress, sym::objectToolDelete, objectTool, 1, noDeadzone);
        tool->selection = toolSetSelection(tool->toolSet);
        resetSelection(tool->selection);
        holdPiece(tool->selection, newPieceObject(objectTool));
        showPiece(ui, objectTool);
        setInfoButtonText(ui, 1, sym::g_textPlace2);
        setInfoButtonText(ui, 2, sym::g_textRotate);
        setInfoButtonText(ui, 3, sym::g_textNa7);
        setInfoButtonText(ui, 4, sym::g_textDelete3);
        setInfoButtonText(ui, 5, sym::g_textMove2);
        return setEditCameraTargetKeepingResult(tool->editCamera, selectionTarget(tool->selection));
    }
    READABLE_FUNCTION_WAITS(objectToolActivate, Verified);

    void objectToolUpdate(ee::Ptr<EditToolPart> tool)
    {
        game::tail(updateSelectionCursor, tool->selection);
    }
    READABLE_FUNCTION_WAITS(objectToolUpdate, Verified);

    int32_t objectToolDraw(ee::Ptr<EditToolPart> tool)
    {
        return game::tail(drawSelectionInColour, tool->selection);
    }
    READABLE_FUNCTION_WAITS(objectToolDraw, Verified);

    int32_t objectToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 7);
        clearUiButton(ui, 10);
        clearUiButton(ui, 3);
        clearUiButton(ui, 4);
        clearUiButton(ui, 5);
        return game::tail(clearSelection, tool->selection);
    }
    READABLE_FUNCTION_WAITS(objectToolDeactivate, Verified);
}
