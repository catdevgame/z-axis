// The park editor's object tools: one per catalog category. The selection holds
// a piece of the category, which the pad moves, turns, places and deletes, and
// two buttons step through the category's unlocked pieces.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tool_object.cpp.

#include "bmx/editor/edit_tool_object.h"
#include "zge/editor/edit_tool_object.h"
#include "bmx/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_messages.h"
#include "bmx/editor/edit_tools.h"
#include "bmx/editor/park_cells.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tool_object.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditToolClass = sym::g_objectToolEditToolName;   // "EditTool"

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

    // BMX XXX's delete button says what it would do: Delete while the pieces held cannot
    // be placed (they stand over the park's), None while they can. Every move, place and
    // delete brings it up to date.
    int32_t updateDeleteButtonText(ee::Ptr<EditToolPart> tool)
    {
        const uint32_t ui = toolSetUi(tool->toolSet);
        if (canPlaceHeldPieces(tool->selection) != 0)
            return game::tail(setInfoButtonText, ui, 4, sym::g_textNa7);
        return game::tail(setInfoButtonText, ui, 4, sym::g_textDelete3);
    }
    READABLE_FUNCTION_WAITS(updateDeleteButtonText, Verified);

    // The four moves step the selection a cell along the camera's heading, bring the
    // delete button up to date and sound the move (BMX XXX; Aggressive Inline's only step).
    int32_t objectToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + ee::Float(90.0f));
        moveSelection(tool->selection, step.address());
        updateDeleteButtonText(tool);
        return static_cast<int32_t>(soundEvent(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(objectToolMoveLeft, Verified);

    int32_t objectToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) - ee::Float(90.0f));
        moveSelection(tool->selection, step.address());
        updateDeleteButtonText(tool);
        return static_cast<int32_t>(soundEvent(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(objectToolMoveRight, Verified);

    int32_t objectToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera));
        moveSelection(tool->selection, step.address());
        updateDeleteButtonText(tool);
        return static_cast<int32_t>(soundEvent(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(objectToolMoveForward, Verified);

    int32_t objectToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        ee::Local<zge::Vec3> step;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + ee::Float(180.0f));
        moveSelection(tool->selection, step.address());
        updateDeleteButtonText(tool);
        return static_cast<int32_t>(soundEvent(kMoveSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(objectToolMoveBack, Verified);

    // The piece held is deleted, and the selection holds the next unlocked piece
    // of the category (from the last, the first); the change sounds (BMX XXX).
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
        holdPiece(tool->selection, object);
        return calls::soundEventTail(kChangePieceSound, 0, 0, 0, 0);
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
        holdPiece(tool->selection, object);
        return calls::soundEventTail(kChangePieceSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(objectToolPreviousPiece, Verified);

    // The selection turned a quarter, or the first of a half and three quarters it will
    // take; then the turn's sound (BMX XXX), whichever happened.
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
        return static_cast<int32_t>(soundEvent(kRotateSound, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(objectToolRotate, Verified);

    // Placed: the place sound and the delete button brought up to date; nothing placed, the
    // refusal's sound (BMX XXX; Aggressive Inline's places and returns).
    uint32_t objectToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        if (placeHeldPieces(tool->selection) == 0)
            return calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
        soundEvent(kPlaceSound, 0, 0, 0, 0);
        return game::tail(updateDeleteButtonText, tool);
    }
    READABLE_FUNCTION_WAITS(objectToolPlace, Verified);

    // Delete: refused while the pieces held could be placed (nothing of the park's is
    // selected), else the delete sound; the selected pieces go either way, and the delete
    // button is brought up to date (BMX XXX).
    uint32_t objectToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        if (canPlaceHeldPieces(tool->selection) != 0)
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        else
            soundEvent(kDeleteSound, 0, 0, 0, 0);
        deleteSelectedPieces(tool->selection);
        return game::tail(updateDeleteButtonText, tool);
    }
    READABLE_FUNCTION_WAITS(objectToolDelete, Verified);

    // Whether the object tool offers a piece: BMX XXX's pieces with a secret id (0 and up) are
    // never offered, the others (-1) always. (Aggressive Inline asks its unlocked secrets.)
    uint32_t pieceUnlocked(ee::Ptr<PieceDef> piece)
    {
        return static_cast<int32_t>(pieceDefSecretId(ee::Address(piece))) < 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(pieceUnlocked, Verified);
}
