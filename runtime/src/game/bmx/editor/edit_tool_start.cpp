// The park editor's start tool: the selection holds the player's start object,
// which the pad moves and turns; placing it takes away the one placed before.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tool_start.cpp.

#include "bmx/editor/edit_tool_start.h"
#include "zge/editor/edit_tool_start.h"
#include "bmx/editor/edit_records.h"
#include "bmx/editor/edit_core.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/edit_tools.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tool_start.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditStartToolClass = sym::g_editStartToolName; // "EditStartTool"
        constexpr uint32_t kEditToolClass = sym::g_startToolEditToolName;  // "EditTool"
    }

    // The move buttons: a cell left of, right of, along and against the camera's
    // heading, with the move's sound (BMX XXX).
    int32_t startToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        startToolMove(tool, ee::Float(90.0f));
        return static_cast<int32_t>(calls::soundEventTail(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(startToolMoveLeft, Verified);

    int32_t startToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        startToolMove(tool, ee::Float(-90.0f));
        return static_cast<int32_t>(calls::soundEventTail(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(startToolMoveRight, Draft);

    int32_t startToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        startToolMove(tool, ee::Float(0.0f));
        return static_cast<int32_t>(calls::soundEventTail(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(startToolMoveForward, Verified);

    int32_t startToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        startToolMove(tool, ee::Float(180.0f));
        return static_cast<int32_t>(calls::soundEventTail(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(startToolMoveBack, Verified);

    // A quarter turn, and the turn's sound (BMX XXX), taken or not.
    int32_t startToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        ee::Local<zge::EulerAngles> turn;
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        rotateSelection(tool->selection, turn.address());
        return static_cast<int32_t>(soundEvent(kRotateSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(startToolRotate, Verified);

    // Where the start object fits, the one already in the park goes first; placed, the
    // place sound, and where it does not fit the refusal's (BMX XXX).
    uint32_t startToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditStartToolPart> start(editObjectPart(button->data, kEditStartToolClass));
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        if (canPlaceHeldPieces(tool->selection) == 0)
            return calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
        const ee::Ptr<EditObjectPart> placed = findParkPiece(toolSetPark(tool->toolSet), start->pieceDef);
        if (placed != 0)
        {
            detachFromAnchor(editObjectParentAnchor(placed));
            destroyEditPiece(placed);
        }
        placeHeldPieces(tool->selection);
        return calls::soundEventTail(kPlaceSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(startToolPlace, Verified);

    // The park's start object shown or hidden, if the park has one (BMX XXX: its game code
    // hides it for a run and shows it again after).
    void showParkStartObject(uint32_t shown)
    {
        const ee::Ptr<EditObjectPart> start = findParkStartObject();
        if (start != 0)
            game::tail(showEditObjectDisplay, start, shown);
    }
    READABLE_FUNCTION_WAITS(showParkStartObject, Verified);
}
