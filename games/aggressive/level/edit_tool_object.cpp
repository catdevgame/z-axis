// The park editor's object tools: one per catalog category. The selection holds
// a piece of the category, which the pad moves, turns, places and deletes, and
// two buttons step through the category's unlocked pieces.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tool_object.cpp.

#include "aggressive/level/edit_tool_object.h"
#include "zge/editor/edit_tool_object.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_messages.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_object.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditToolClass = sym::g_objectToolEditToolName;   // "EditTool"

        // 16-bit angle units in a degree.
        constexpr ee::Float kAngleUnitsPerDegree = ee::Float(182.04445f);

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

    // The move buttons: one cell of the selection, left of, right of, along and
    // against the camera's heading. Their row's data is the object tool part.
    int32_t objectToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + ee::Float(90.0f));
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(objectToolMoveLeft, Verified);

    int32_t objectToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) - ee::Float(90.0f));
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(objectToolMoveRight, Verified);

    int32_t objectToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera));
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(objectToolMoveForward, Verified);

    int32_t objectToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + ee::Float(180.0f));
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(objectToolMoveBack, Verified);

    // The piece held is deleted, and the selection holds the next unlocked piece
    // of the category (from the last, the first).
    uint32_t objectToolNextPiece(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditObjectToolPart> objectTool(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(objectTool, kEditToolClass));
        broadcastEditMessage(takeHeldPiece(tool->selection), sym::makeDeleteMessage, 0);
        do
        {
            ee::Ptr<PieceDef> piece = categoryNextPiece(objectTool->category, objectTool->pieceDef);
            if (!piece)
                piece = categoryFirstPiece(objectTool->category);
            objectTool->pieceDef = piece;
        } while (pieceUnlocked(objectTool->pieceDef) == 0);
        const ee::Ptr<EditObjectPart> object = newPieceObject(objectTool);
        playNextObjectScript();
        showPiece(button->ui, objectTool);
        return game::tail(holdPiece, tool->selection, object);
    }
    READABLE_FUNCTION_WAITS(objectToolNextPiece, Verified);

    uint32_t objectToolPreviousPiece(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditObjectToolPart> objectTool(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(objectTool, kEditToolClass));
        broadcastEditMessage(takeHeldPiece(tool->selection), sym::makeDeleteMessage, 0);
        do
        {
            ee::Ptr<PieceDef> piece = categoryPreviousPiece(objectTool->category, objectTool->pieceDef);
            if (!piece)
                piece = categoryLastPiece(objectTool->category);
            objectTool->pieceDef = piece;
        } while (pieceUnlocked(objectTool->pieceDef) == 0);
        const ee::Ptr<EditObjectPart> object = newPieceObject(objectTool);
        playPreviousObjectScript();
        showPiece(button->ui, objectTool);
        return game::tail(holdPiece, tool->selection, object);
    }
    READABLE_FUNCTION_WAITS(objectToolPreviousPiece, Verified);

    // A quarter turn about y; if the selection cannot turn so far, a half and
    // then three quarters are tried.
    int32_t objectToolRotate(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::EulerAngles> turn;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        for (int32_t quarters = 1; quarters < 4; ++quarters)
        {
            turn->x = 0;
            turn->z = 0;
            turn->y = static_cast<zge::Angle>(ee::toInt(ee::toFloat(quarters) * ee::Float(90.0f) * kAngleUnitsPerDegree));
            if (rotateSelection(tool->selection, turn.address()) != 0)
                break;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(objectToolRotate, Verified);

    uint32_t objectToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(placeHeldPieces, tool->selection);
    }
    READABLE_FUNCTION_WAITS(objectToolPlace, Verified);

    uint32_t objectToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(deleteSelectedPieces, tool->selection);
    }
    READABLE_FUNCTION_WAITS(objectToolDelete, Verified);
}
