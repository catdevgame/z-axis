// The park editor's pisser tool: a machine in the level (the "Terrain_Pisser"
// node on its "Terrain_Slider") that follows the selection and makes or
// removes pieces there with a puff. In choose mode the piece it will make
// turns on a stand while the camera looks at it.

#include "aggressive/level/edit_tool_pisser.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "aggressive/fx/library.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_pisser.cpp.h"

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditPisserToolClass = sym::g_editPisserToolName; // "EditPisserTool"
        constexpr uint32_t kEditToolClass = sym::g_pisserToolEditToolName;   // "EditTool"
        constexpr uint32_t kEditAnchorClass = sym::g_pisserToolEditAnchorName;

        // EditPisserToolPart::mode
        constexpr int32_t kPisserMoveMode = 0, kPisserChooseMode = 1;
        // EditPisserToolPart::action: a piece is removed or made half a second after
        // its puff starts, and the buttons come back a second after it started.
        constexpr int32_t kPisserSettling = 0, kPisserDeleting = 1, kPisserCreating = 2, kPisserIdle = 3;

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // Where the piece to choose stands, turning, in choose mode.
        constexpr ee::Float kStandX = ee::Float(178.32956f), kStandY = ee::Float(-3.5427387f), kStandZ = ee::Float(-160.65317f);
        // How far the piece on the stand turns each frame, in 16-bit angle units.
        constexpr uint16_t kStandTurn = 100;

        inline ee::Global<uint16_t, sym::g_pisserToolAngle> g_standHeading; // the y of g_pisserAnchorAngles
    }

    int32_t registerEditPisserToolClass()
    {
        return game::tail(registerEditClass, kEditPisserToolClass, sizeof(EditPisserToolPart), kEditToolClass, sym::constructEditPisserTool, sym::destructEditPisserTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditPisserToolClass, Verified);

    // The machine's two nodes keep where they stood, to be put back when the
    // tool goes; the piece to make is the category's first; the stand is made.
    int32_t constructEditPisserTool(uint32_t data, ee::Ptr<EditToolSet> toolSet)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(data, kEditPisserToolClass));
        setEditToolMethods(tool, sym::g_pisserToolMethods);
        setEditToolName(tool, sym::g_textPisserTool);
        pisser->category = catalogCategory(toolSetCatalog(toolSet), 0);

        pisser->pisserNode = ee::Address(zge::findWorldNodeByHash(zge::nameHash(ee::Ptr<const char>(sym::g_textTerrainPisser)), 0, 0));
        pisser->pisserRestPosition = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), 1)->translation();
        pisser->sliderNode = ee::Address(zge::findWorldNodeByHash(zge::nameHash(ee::Ptr<const char>(sym::g_textTerrainSlider)), 0, 0));
        pisser->sliderRestPosition = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->sliderNode), 1)->translation();

        pisser->pieceDef = categoryFirstPiece(pisser->category);
        pisser->stand = newEditObject(kEditAnchorClass, 0);
        pisser->action = kPisserIdle;
        ee::Local<zge::Vec4> standPosition; // its w is whatever the stack held, as in the original
        standPosition->x = kStandX;
        standPosition->y = kStandY;
        standPosition->z = kStandZ;
        return setAnchorPosition(pisser->stand, standPosition);
    }
    READABLE_FUNCTION_WAITS(constructEditPisserTool, Verified);

    // The machine goes back where it stood; the stand goes.
    int32_t destructEditPisserTool(uint32_t data)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(data, kEditPisserToolClass));
        const ee::Ptr<zge::Matrix> pisserMatrix = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), 1);
        pisserMatrix->translation() = pisser->pisserRestPosition;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), pisserMatrix);
        const ee::Ptr<zge::Matrix> sliderMatrix = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->sliderNode), 1);
        sliderMatrix->translation() = pisser->sliderRestPosition;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(pisser->sliderNode), sliderMatrix);
        return game::tail(deleteEditObject, pisser->stand);
    }
    READABLE_FUNCTION_WAITS(destructEditPisserTool, Verified);

    int32_t pisserToolEnterChooseMode(ee::Ptr<EditToolPart> tool, uint32_t ui);
    int32_t pisserToolEnterMoveMode(ee::Ptr<EditToolPart> tool, uint32_t ui);
    void pisserToolLeaveChooseMode(ee::Ptr<EditToolPart> tool, uint32_t ui);
    int32_t pisserToolLeaveMoveMode(ee::Ptr<EditToolPart> tool, uint32_t ui);

    // Picked: the move view starts as the camera stands, looking at the
    // selection; the tool opens in choose mode.
    int32_t pisserToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        pushUiButtonPage(ui);
        tool->selection = toolSetSelection(tool->toolSet);
        resetSelection(tool->selection);
        editCameraView(tool->editCamera, &pisser->moveView);
        pisser->moveView.lookAt = *ee::Ptr<zge::Vec4>(selectionTarget(tool->selection));
        return game::tail(pisserToolEnterChooseMode, tool, ui);
    }
    READABLE_FUNCTION_WAITS(pisserToolActivate, Verified);

    // Move mode: the pad moves the selection, which holds a piece of the
    // chosen kind; buttons make, delete, turn, and go to choose mode.
    int32_t pisserToolEnterMoveMode(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::pisserToolMoveLeft, pisser, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::pisserToolMoveRight, pisser, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::pisserToolMoveForward, pisser, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::pisserToolMoveBack, pisser, 10, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::pisserToolRotate, pisser, 1, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::pisserToolStartCreate, pisser, 1, noDeadzone);
        setUiButton(ui, 3, kOnPress, sym::pisserToolStartDelete, pisser, 1, noDeadzone);
        setUiButton(ui, 2, kOnPress, sym::pisserToolChoosePiece, pisser, 1, noDeadzone);
        setCameraButtons(tool->toolSet);
        setUiButton(ui, 0, kOnPress, sym::pisserToolPause, pisser, 1, noDeadzone);
        pisser->mode = kPisserMoveMode;
        resetSelection(tool->selection);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        holdPiece(tool->selection, newEditObjectOfDef(pisser->pieceDef));
        setObjectSelectionText(ui, pieceDefName(pisser->pieceDef));
        setInfoButtonText(ui, 1, sym::g_textCreate);
        setInfoButtonText(ui, 2, sym::g_textRotate2);
        setInfoButtonText(ui, 3, sym::g_textSelection);
        setInfoButtonText(ui, 4, sym::g_textDelete4);
        setInfoButtonText(ui, 5, sym::g_textMove3);
        return game::tail(showEditHelpPage, ui, 0);
    }
    READABLE_FUNCTION_WAITS(pisserToolEnterMoveMode, Verified);

    // Choose mode: the chosen piece stands on the stand, the camera looks at it
    // from above, and the pad steps through the category.
    int32_t pisserToolEnterChooseMode(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 14, kOnPress, sym::pisserToolPreviousPiece, pisser, 1, noDeadzone);
        setUiButton(ui, 15, kOnPress, sym::pisserToolNextPiece, pisser, 1, noDeadzone);
        setUiButton(ui, 2, kOnPress, sym::pisserToolBackToMove, pisser, 1, noDeadzone);
        setUiButton(ui, 0, kOnPress, sym::pisserToolPause, pisser, 1, noDeadzone);
        const ee::Ptr<PieceDef> piece = pisser->pieceDef;
        pisser->mode = kPisserChooseMode;
        attachToAnchor(pisser->stand, newEditObjectOfDef(piece));
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(static_cast<uint32_t>(anchorLocalPosition(pisser->stand))));
        setEditCameraTilt(tool->editCamera, ee::Float(80.0f));
        setEditCameraDistance(tool->editCamera, ee::Float(10.0f));
        setEditCameraHeading(tool->editCamera, ee::Float(0.0f));
        setObjectSelectionText(ui, pieceDefName(pisser->pieceDef));
        setInfoButtonText(ui, 1, sym::g_textNa4);
        setInfoButtonText(ui, 2, sym::g_textNa4);
        setInfoButtonText(ui, 3, sym::g_textPlace3);
        setInfoButtonText(ui, 4, sym::g_textNa4);
        setInfoButtonText(ui, 5, sym::g_textChange2);
        return game::tail(setInfoHelpText, ui, sym::g_textChoosePieceUsingLeftAndRight);
    }
    READABLE_FUNCTION_WAITS(pisserToolEnterChooseMode, Verified);

    // The machine stands over the selection: the pisser at its x and z, the
    // slider at its x.
    void pisserToolFollowSelection(ee::Ptr<EditPisserToolPart> pisser, ee::Float seconds)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        updateSelectionCursor(tool->selection); // the frame's seconds go along too, unread
        const ee::Ptr<zge::Matrix> pisserMatrix = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), 1);
        ee::Local<zge::Vec4> selected;
        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(selected));
        pisserMatrix->translation().x = selected->x;
        pisserMatrix->translation().z = selected->z;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), pisserMatrix);
        const ee::Ptr<zge::Matrix> sliderMatrix = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->sliderNode), 1);
        sliderMatrix->translation().x = selected->x;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(pisser->sliderNode), sliderMatrix);
    }
    READABLE_FUNCTION_WAITS(pisserToolFollowSelection, Verified);

    // The piece is made where the selection is: the selection's held piece is
    // put aside, the new one placed, and the old one held again. A piece def
    // begins with its link in the category, and the piece made is the one after
    // the piece chosen: the category lists each choice followed by what it makes.
    uint32_t pisserToolCreatePiece(ee::Ptr<EditPisserToolPart> pisser)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        const ee::Ptr<EditObjectPart> heldPiece = takeHeldPiece(tool->selection);
        holdPiece(tool->selection, newEditObjectOfDef(pisser->pieceDef->link.next));
        placeHeldPieces(tool->selection);
        clearSelection(tool->selection);
        return game::tail(holdPiece, tool->selection, heldPiece);
    }
    READABLE_FUNCTION_WAITS(pisserToolCreatePiece, Verified);

    void releasePisserButtons(ee::Ptr<EditPisserToolPart> pisser, uint32_t ui);

    // Each frame. In move mode the machine follows the selection, and a make or
    // delete under way runs its course; in choose mode the stand turns.
    void pisserToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float seconds)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        if (pisser->mode == kPisserChooseMode)
        {
            setAnchorAngles(pisser->stand, sym::g_pisserAnchorAngles);
            *g_standHeading = static_cast<uint16_t>(*g_standHeading + kStandTurn);
            return;
        }
        if (pisser->mode != kPisserMoveMode)
            return;

        pisserToolFollowSelection(pisser, seconds);
        const int32_t action = pisser->action;
        const ee::Float elapsed = pisser->actionTime + seconds;
        pisser->actionTime = elapsed;
        if (action == kPisserDeleting)
        {
            if (ee::Float(0.5f) < elapsed)
            {
                deleteSelectedPieces(tool->selection);
                pisser->action = kPisserSettling;
            }
            return;
        }
        if (action == kPisserCreating)
        {
            if (ee::Float(0.5f) < elapsed)
            {
                pisserToolCreatePiece(pisser);
                pisser->action = kPisserSettling;
            }
            return;
        }
        if (action != kPisserSettling || !(ee::Float(1.0f) < elapsed))
            return;
        pisser->action = kPisserIdle;
        game::tail(releasePisserButtons, pisser, pisser->ui);
    }
    READABLE_FUNCTION_WAITS(pisserToolUpdate, Verified);

    void pisserToolDraw()
    {
    }
    READABLE_FUNCTION(pisserToolDraw, Verified);

    void pisserToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        if (pisser->mode == kPisserChooseMode)
            pisserToolLeaveChooseMode(tool, ui);
        else
            pisserToolLeaveMoveMode(tool, ui);
        game::tail(popUiButtonPage, ui);
    }
    READABLE_FUNCTION_WAITS(pisserToolDeactivate, Verified);

    // The move view is kept for the way back.
    int32_t pisserToolLeaveMoveMode(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        clearSelection(tool->selection);
        editCameraView(tool->editCamera, &pisser->moveView);
        clearCameraButtons(tool->toolSet);
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 4);
        clearUiButton(ui, 3);
        clearUiButton(ui, 5);
        clearUiButton(ui, 2);
        clearUiButton(ui, 0);
        return game::tail(hideEditHelpPage, ui, 0);
    }
    READABLE_FUNCTION_WAITS(pisserToolLeaveMoveMode, Verified);

    // The choose view is kept for next time; the piece on the stand goes.
    void pisserToolLeaveChooseMode(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditPisserToolPart> pisser(editObjectPart(tool, kEditPisserToolClass));
        editCameraView(tool->editCamera, &pisser->chooseView);
        deleteEditObject(detachFromAnchor(pisser->stand));
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 2);
        game::tail(clearUiButton, ui, 0);
    }
    READABLE_FUNCTION_WAITS(pisserToolLeaveChooseMode, Verified);

    // One cell of the selection along a heading (degrees) from the camera's.
    int32_t pisserToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        return moveSelection(tool->selection, step);
    }
    READABLE_FUNCTION_WAITS(pisserToolMove, Verified);

    // The move buttons: their row's data is the pisser part.
    int32_t pisserToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(pisserToolMove, tool, ee::Float(90.0f));
    }
    READABLE_FUNCTION_WAITS(pisserToolMoveLeft, Verified);

    int32_t pisserToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(pisserToolMove, tool, ee::Float(-90.0f));
    }
    READABLE_FUNCTION_WAITS(pisserToolMoveRight, Verified);

    int32_t pisserToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(pisserToolMove, tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(pisserToolMoveForward, Verified);

    int32_t pisserToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(pisserToolMove, tool, ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(pisserToolMoveBack, Verified);

    // A quarter turn about y.
    int32_t pisserToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        ee::Local<zge::EulerAngles> turn;
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        return static_cast<int32_t>(rotateSelection(tool->selection, turn.address()));
    }
    READABLE_FUNCTION_WAITS(pisserToolRotate, Verified);

    // While a make or delete runs, its buttons do nothing.
    void holdPisserButtons(ee::Ptr<EditPisserToolPart> pisser, uint32_t ui)
    {
        pisser->ui = ui;
        disableUiButton(ui, 14);
        disableUiButton(ui, 15);
        disableUiButton(ui, 12);
        disableUiButton(ui, 13);
        disableUiButton(ui, 2);
        disableUiButton(ui, 3);
        disableUiButton(ui, 4);
        game::tail(disableUiButton, ui, 5);
    }
    READABLE_FUNCTION(holdPisserButtons, Verified);

    void releasePisserButtons(ee::Ptr<EditPisserToolPart> /*pisser*/, uint32_t ui)
    {
        enableUiButton(ui, 14);
        enableUiButton(ui, 15);
        enableUiButton(ui, 12);
        enableUiButton(ui, 13);
        enableUiButton(ui, 2);
        enableUiButton(ui, 3);
        enableUiButton(ui, 4);
        game::tail(enableUiButton, ui, 5);
    }
    READABLE_FUNCTION(releasePisserButtons, Verified);

    // Make or delete: unless the selection is still sliding, a puff starts
    // above the pisser and the buttons are held until it is done.
    void pisserToolStartCreate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        if (selectionMoved(tool->selection) != 0)
            return;
        ee::Local<zge::Vec4> puffAt;
        *puffAt = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), 1)->translation();
        puffAt->y = puffAt->y + ee::Float(5.0f);
        aggressive::fx::startPisserCreatePuff(puffAt.address());
        pisser->actionTime = ee::Float(0.0f);
        pisser->action = kPisserCreating;
        holdPisserButtons(pisser, button->ui);
    }
    READABLE_FUNCTION(pisserToolStartCreate, Verified);

    void pisserToolStartDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        if (selectionMoved(tool->selection) != 0)
            return;
        ee::Local<zge::Vec4> puffAt;
        *puffAt = zge::nodeMatrix(ee::Ptr<zge::Node>(pisser->pisserNode), 1)->translation();
        puffAt->y = puffAt->y + ee::Float(5.0f);
        aggressive::fx::startPisserDeletePuff(puffAt.address());
        pisser->actionTime = ee::Float(0.0f);
        pisser->action = kPisserDeleting;
        holdPisserButtons(pisser, button->ui);
    }
    READABLE_FUNCTION(pisserToolStartDelete, Verified);

    // Between the modes the camera slides from one mode's view to the other's.
    int32_t pisserToolChoosePiece(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        pisserToolLeaveMoveMode(tool, button->ui);
        pisserToolEnterChooseMode(tool, button->ui);
        return static_cast<int32_t>(game::tail(startEditCameraTransition, tool->editCamera, &pisser->moveView, &pisser->chooseView, ee::Float(0.5f)));
    }
    READABLE_FUNCTION_WAITS(pisserToolChoosePiece, Verified);

    int32_t pisserToolBackToMove(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        const ee::Ptr<EditToolPart> tool(editObjectPart(pisser, kEditToolClass));
        pisserToolLeaveChooseMode(tool, button->ui);
        pisserToolEnterMoveMode(tool, button->ui);
        return static_cast<int32_t>(game::tail(startEditCameraTransition, tool->editCamera, &pisser->chooseView, &pisser->moveView, ee::Float(0.5f)));
    }
    READABLE_FUNCTION_WAITS(pisserToolBackToMove, Verified);

    // The pieces to choose from step two at a time, each choice being followed
    // in the category by the piece it makes (pisserToolCreatePiece); past either
    // end they wrap.
    int32_t pisserToolPreviousPiece(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        deleteEditObject(detachFromAnchor(pisser->stand));
        ee::Ptr<PieceDef> piece = categoryPreviousPiece(pisser->category, pisser->pieceDef);
        if (!piece)
            piece = categoryLastPiece(pisser->category);
        piece = categoryPreviousPiece(pisser->category, piece);
        pisser->pieceDef = piece;
        attachToAnchor(pisser->stand, newEditObjectOfDef(piece));
        return game::tail(setObjectSelectionText, button->ui, pieceDefName(piece));
    }
    READABLE_FUNCTION_WAITS(pisserToolPreviousPiece, Verified);

    int32_t pisserToolNextPiece(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditPisserToolPart> pisser(button->data);
        deleteEditObject(detachFromAnchor(pisser->stand));
        ee::Ptr<PieceDef> piece = categoryNextPiece(pisser->category, pisser->pieceDef);
        piece = categoryNextPiece(pisser->category, piece);
        if (!piece)
            piece = categoryFirstPiece(pisser->category);
        pisser->pieceDef = piece;
        attachToAnchor(pisser->stand, newEditObjectOfDef(piece));
        return game::tail(setObjectSelectionText, button->ui, pieceDefName(piece));
    }
    READABLE_FUNCTION_WAITS(pisserToolNextPiece, Verified);

    int32_t pisserToolPause(ee::Ptr<EditUiButton> button)
    {
        return game::tail(playInGameEditPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(pisserToolPause, Verified);
}
