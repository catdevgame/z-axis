// The park editor's start tool: the selection holds the player's start object,
// which the pad moves and turns; placing it takes away the one placed before.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tool_start.cpp.

#include "aggressive/level/edit_tool_start.h"
#include "zge/editor/edit_tool_start.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_start.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditStartToolClass = sym::g_editStartToolName; // "EditStartTool"
        constexpr uint32_t kEditToolClass = sym::g_startToolEditToolName;  // "EditTool"
    }

    // The move buttons: their row's data is the start tool part.
    int32_t startToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(startToolMove, tool, ee::Float(90.0f));
    }
    READABLE_FUNCTION_WAITS(startToolMoveLeft, Verified);

    int32_t startToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(startToolMove, tool, ee::Float(-90.0f));
    }
    READABLE_FUNCTION_WAITS(startToolMoveRight, Verified);

    int32_t startToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(startToolMove, tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(startToolMoveForward, Verified);

    int32_t startToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(startToolMove, tool, ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(startToolMoveBack, Verified);

    // A quarter turn about y.
    int32_t startToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        ee::Local<zge::EulerAngles> turn;
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        return static_cast<int32_t>(rotateSelection(tool->selection, turn.address()));
    }
    READABLE_FUNCTION_WAITS(startToolRotate, Verified);

    // Where the start object fits, the one already in the park goes first.
    uint32_t startToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditStartToolPart> start(editObjectPart(button->data, kEditStartToolClass));
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        const int32_t fits = canPlaceHeldPieces(tool->selection);
        if (fits == 0)
            return 0;
        const ee::Ptr<EditObjectPart> placed = findParkPiece(static_cast<uint32_t>(toolSetPark(tool->toolSet)), start->pieceDef);
        if (placed != 0)
        {
            detachFromAnchor(editObjectParentAnchor(placed));
            destroyEditPiece(placed);
        }
        return game::tail(placeHeldPieces, tool->selection);
    }
    READABLE_FUNCTION_WAITS(startToolPlace, Verified);
}
