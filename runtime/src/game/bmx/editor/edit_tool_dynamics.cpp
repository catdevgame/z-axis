// BMX XXX's park editor (../GameTools/ParkEditor): the dynamics tool, which Aggressive Inline does not have.
// It works as the rail tool does: the pad moves the selection over the park, the object under it is the
// target, and a preview of the piece chosen stands at the target's first anchor that takes dynamics, turned
// as the player turns it. Buttons place a copy there, delete what the anchor holds, turn the preview and
// step through the catalog's dynamics.

#include "bmx/editor/edit_tool_dynamics.h"
#include "bmx/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/text/strings.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_display.h"
#include "bmx/editor/park_messages.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/edit_tools.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/edit_tool_dynamics.cpp.h"

namespace game::bmx::editor
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditDynamicsToolClass = sym::g_editDynamicsToolName;  // "EditDynamicsTool"
        constexpr uint32_t kEditToolClass = sym::g_dynamicsToolEditToolName;      // "EditTool"
        constexpr uint32_t kEditAnchorClass = sym::g_dynamicsToolEditAnchorName;  // "EditAnchor"

        // The tool set catalog's category of dynamics.
        constexpr uint32_t kDynamicsCategory = 12;

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // The tool's icon in the tool bar's font.
        constexpr int8_t kDynamicsToolIcon = 'J';

        constexpr Colour kAnchorTaken = {154, 28, 33, 128}; // red
        constexpr Colour kOtherAnchor = {160, 160, 160, 255}; // grey

        // Where an anchor stands in the world: its world matrix's translation.
        ee::Ptr<zge::Vec4> anchorPosition(uint32_t anchor)
        {
            return ee::Ptr<zge::Vec4>(static_cast<uint32_t>(anchorWorldPosition(anchor)));
        }

        // A colour as one word, as drawAnchorMarker takes it.
        uint32_t colourWord(const Colour &colour)
        {
            uint32_t word;
            std::memcpy(&word, &colour, sizeof(word));
            return word;
        }
    }

    void dynamicsToolFindTarget(ee::Ptr<EditDynamicsToolPart> dynamics);
    void dynamicsToolShowTarget(ee::Ptr<EditDynamicsToolPart> dynamics);
    void dynamicsToolMovePreview(ee::Ptr<EditDynamicsToolPart> dynamics);
    void dynamicsToolUpdatePointer(ee::Ptr<EditDynamicsToolPart> dynamics);
    void dynamicsToolRemakePreview(ee::Ptr<EditDynamicsToolPart> dynamics, ee::Ptr<EditUiButton> button);
    void dynamicsToolChooseNextPiece(ee::Ptr<EditDynamicsToolPart> dynamics);
    void dynamicsToolChoosePreviousPiece(ee::Ptr<EditDynamicsToolPart> dynamics);

    int32_t registerEditDynamicsToolClass()
    {
        return game::tail(registerEditClass, kEditDynamicsToolClass, sizeof(EditDynamicsToolPart), kEditToolClass, sym::constructEditDynamicsTool, sym::destructEditDynamicsTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditDynamicsToolClass, Verified);

    // Made with the tool: its key (it is given no name), no target, the first piece of the
    // catalog's dynamics chosen, and an anchor of the tool's own for the preview to hang from.
    void constructEditDynamicsTool(uint32_t object, ee::Ptr<EditToolSet> toolSet)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(object, kEditToolClass));
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(object, kEditDynamicsToolClass));
        setEditToolMethods(tool, sym::g_dynamicsToolMethods);
        tool->iconChar = kDynamicsToolIcon;
        dynamics->targetAnchorIndex = 0;
        dynamics->madeZero[0] = 0;
        dynamics->madeZero[1] = 0;
        dynamics->targetAnchor = 0;
        dynamics->targetObject = {};
        dynamics->catalog = toolSetCatalog(toolSet);
        dynamics->category = static_cast<uint32_t>(catalogCategory(dynamics->catalog, kDynamicsCategory));
        dynamics->pieceDef = categoryFirstPiece(dynamics->category);
        dynamics->previewAnchor = newEditObject(kEditAnchorClass, 0);
    }
    READABLE_FUNCTION_WAITS(constructEditDynamicsTool, Verified);

    void destructEditDynamicsTool(uint32_t object)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(object, kEditDynamicsToolClass));
        game::tail(deleteAnchor, dynamics->previewAnchor);
    }
    READABLE_FUNCTION_WAITS(destructEditDynamicsTool, Verified);

    // Picked: the pad moves the selection, buttons step to the previous and the next piece,
    // place, turn and delete; the selection starts afresh, a preview of the piece chosen is
    // made and hung from the tool's anchor, and an object of the category's first piece is
    // made to try anchors with.
    void dynamicsToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(tool, kEditDynamicsToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::dynamicsToolMoveLeft, dynamics, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::dynamicsToolMoveRight, dynamics, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::dynamicsToolMoveForward, dynamics, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::dynamicsToolMoveBack, dynamics, 10, noDeadzone);
        setUiButton(ui, 7, kOnPress, sym::dynamicsToolPreviousPiece, dynamics, 1, noDeadzone);
        setUiButton(ui, 10, kOnPress, sym::dynamicsToolNextPiece, dynamics, 1, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::dynamicsToolPlace, dynamics, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::dynamicsToolRotate, dynamics, 1, noDeadzone);
        setUiButton(ui, 3, kOnPress, sym::dynamicsToolDelete, dynamics, 1, noDeadzone);
        const uint32_t selection = toolSetSelection(tool->toolSet);
        tool->selection = selection;
        resetSelection(selection);
        clearSelectionRotation(tool->selection);
        refreshToolSelection(tool);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        setInfoButtonText(ui, 1, sym::g_textDynamicsToolPlace);
        setInfoButtonText(ui, 2, sym::g_textDynamicsToolRotate);
        setInfoButtonText(ui, 3, sym::g_textDynamicsToolNone);
        setInfoButtonText(ui, 4, sym::g_textDynamicsToolDelete);
        setInfoButtonText(ui, 5, sym::g_textDynamicsToolMove);
        setObjectSelectionText(ui, pieceDefName(dynamics->pieceDef));
        setInfoHelpText(ui, sym::g_textDynamicsToolHelp);
        setInfoHelpText(ui, sym::g_textDynamicsToolNoHelp);
        const ee::Ptr<EditObjectPart> preview(newEditObject(pieceDefClassName(dynamics->pieceDef), dynamics->pieceDef));
        dynamics->preview = preview;
        makeEditObjectDisplay(preview, 0);
        const ee::Ptr<PieceDef> firstPiece = categoryFirstPiece(dynamics->category);
        dynamics->trial = ee::Ptr<EditObjectPart>(newEditObject(pieceDefClassName(firstPiece), firstPiece));
        attachToAnchor(dynamics->previewAnchor, dynamics->preview);
        game::tail(dynamicsToolShowTarget, dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolActivate, Verified);

    // Each frame: the target under the cursor, and the selection kept up with the cursor.
    void dynamicsToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float /*seconds*/)
    {
        dynamicsToolFindTarget(ee::Ptr<EditDynamicsToolPart>(editObjectPart(tool, kEditDynamicsToolClass)));
        game::tail(updateSelectionCursor, tool->selection); // the frame's seconds go along too, unread
    }
    READABLE_FUNCTION_WAITS(dynamicsToolUpdate, Verified);

    // Drawn: the selection (red where the target anchor already holds something), then a
    // marker on each of the target object's anchors, the target's in the held piece's colour
    // and the others grey. (The original first sets the box orange, 176, 142, 21, which the
    // copy of the selection's colours then overwrites: the box is never orange.)
    void dynamicsToolDraw(ee::Ptr<EditToolPart> tool)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(tool, kEditDynamicsToolClass));
        ee::Local<SelectionColours> colours;
        selectionColour(tool->selection, ee::Ptr<SelectionColours>(colours));
        if (dynamics->targetFree == 0)
        {
            colours->heldFit = kAnchorTaken;
            colours->box = kAnchorTaken;
        }
        drawSelection(tool->selection, colours.address());
        dynamicsToolFindTarget(dynamics);
        if (dynamics->targetObject == 0)
            return;
        const uint32_t anchors = editObjectAnchors(dynamics->targetObject);
        if (anchors == 0)
            return;
        for (uint32_t anchor = firstAnchor(anchors); anchor != 0; anchor = nextAnchor(anchor))
        {
            if (anchor == dynamics->targetAnchor)
                drawAnchorMarker(anchorPosition(anchor), colourWord(colours->heldFit));
            else
                drawAnchorMarker(anchorPosition(anchor), colourWord(kOtherAnchor));
        }
    }
    READABLE_FUNCTION_WAITS(dynamicsToolDraw, Verified);

    // Put away: the buttons go, and so do the preview and the trial object (deleted) and the
    // target.
    void dynamicsToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(tool, kEditDynamicsToolClass));
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 7);
        clearUiButton(ui, 10);
        clearUiButton(ui, 5);
        clearUiButton(ui, 4);
        clearUiButton(ui, 3);
        if (dynamics->preview != 0)
        {
            detachFromAnchor(dynamics->previewAnchor);
            broadcastEditMessage(dynamics->preview, sym::makeDeleteMessage, 0);
            dynamics->preview = {};
        }
        if (dynamics->trial != 0)
        {
            broadcastEditMessage(dynamics->trial, sym::makeDeleteMessage, 0);
            dynamics->trial = {};
        }
        dynamics->targetObject = {};
    }
    READABLE_FUNCTION_WAITS(dynamicsToolDeactivate, Verified);

    // One cell of the selection along a heading (degrees) from the camera's, and the target
    // shown again.
    void dynamicsToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        moveSelection(tool->selection, step.address());
        dynamicsToolShowTarget(ee::Ptr<EditDynamicsToolPart>(editObjectPart(tool, kEditDynamicsToolClass)));
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMove, Verified);

    // The move buttons: one cell of the selection, left of, right of, along and against the
    // camera's heading, with the move's sound. Their row's data is the dynamics tool part.
    uint32_t dynamicsToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(90.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMoveLeft, Verified);

    uint32_t dynamicsToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(-90.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMoveRight, Verified);

    uint32_t dynamicsToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(0.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMoveForward, Verified);

    uint32_t dynamicsToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(180.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMoveBack, Verified);

    // The next piece of the category, and its preview; the UI's next-object script plays.
    void dynamicsToolNextPiece(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolChooseNextPiece(ee::Ptr<EditDynamicsToolPart>(button->data));
        dynamicsToolRemakePreview(ee::Ptr<EditDynamicsToolPart>(button->data), button);
        game::tail(playNextObjectScript); // the button's UI goes along, unread
    }
    READABLE_FUNCTION_WAITS(dynamicsToolNextPiece, Verified);

    // The previous piece of the category, and its preview; the UI's previous-object script plays.
    void dynamicsToolPreviousPiece(ee::Ptr<EditUiButton> button)
    {
        dynamicsToolChoosePreviousPiece(ee::Ptr<EditDynamicsToolPart>(button->data));
        dynamicsToolRemakePreview(ee::Ptr<EditDynamicsToolPart>(button->data), button);
        game::tail(playPreviousObjectScript); // the button's UI goes along, unread
    }
    READABLE_FUNCTION_WAITS(dynamicsToolPreviousPiece, Verified);

    // Place: refused while the park's cost allows nothing more; nothing with no preview or no
    // target anchor. Otherwise a copy of the preview goes on the target anchor, turned as the
    // preview is, with the place sound; or the refusal's sound if the anchor holds something.
    void dynamicsToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(button->data, kEditDynamicsToolClass));
        if (parkCostAllowsMore() == 0)
        {
            game::tail(editRefused);
            return;
        }
        if (dynamics->preview == 0 || dynamics->targetAnchor == 0)
            return;
        if (anchorAttachedObject(dynamics->targetAnchor) == 0)
        {
            attachToAnchor(dynamics->targetAnchor, cloneEditObject(dynamics->preview));
            soundEvent(kPlaceSound, 0, 0, 0, 0);
            setAnchorAngles(dynamics->targetAnchor, static_cast<uint32_t>(anchorAngles(dynamics->previewAnchor)));
        }
        else
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        game::tail(dynamicsToolUpdatePointer, dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolPlace, Verified);

    // Delete: what the target anchor holds comes off and is deleted, with the delete sound;
    // with no target anchor, or nothing on it, the refusal's sound.
    void dynamicsToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(button->data, kEditDynamicsToolClass));
        if (dynamics->targetAnchor == 0)
        {
            calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
            return;
        }
        const ee::Ptr<EditObjectPart> placed = anchorAttachedObject(dynamics->targetAnchor);
        if (placed != 0)
        {
            detachFromAnchor(dynamics->targetAnchor);
            broadcastEditMessage(placed, sym::makeDeleteMessage, 0);
            soundEvent(kDeleteSound, 0, 0, 0, 0);
        }
        else
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        game::tail(dynamicsToolUpdatePointer, dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolDelete, Verified);

    // Rotate: with a target anchor, the preview's anchor turns a quarter about the vertical,
    // with the turn's sound; with none, the refusal's sound.
    uint32_t dynamicsToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditDynamicsToolPart> dynamics(editObjectPart(button->data, kEditDynamicsToolClass));
        ee::Local<zge::EulerAngles> turn;
        if (dynamics->targetAnchor == 0)
            return soundEvent(kRefusedSound, 0, 0, 0, 0);
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        rotateAnchor(dynamics->previewAnchor, turn.address());
        return soundEvent(kRotateSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolRotate, Verified);

    // A new preview of the piece chosen: made with its display and moved to the target, then
    // the old one deleted; the UI shows the piece's name, and the change-piece sound plays.
    void dynamicsToolRemakePreview(ee::Ptr<EditDynamicsToolPart> dynamics, ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditObjectPart> old = dynamics->preview;
        const ee::Ptr<EditObjectPart> preview(newEditObject(pieceDefClassName(dynamics->pieceDef), dynamics->pieceDef));
        dynamics->preview = preview;
        makeEditObjectDisplay(preview, 0);
        dynamicsToolMovePreview(dynamics);
        if (old != 0)
            broadcastEditMessage(old, sym::makeDeleteMessage, 0);
        setObjectSelectionText(button->ui, pieceDefName(dynamics->pieceDef));
        calls::soundEventTail(kChangePieceSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolRemakePreview, Verified);

    // The object under the cursor that dynamics go on. A piece standing on the cursor's anchor
    // is the target unless it is itself one of the dynamics. Otherwise the anchor's owner is,
    // unless one of the owner's anchors holds something that is not one of the dynamics. With
    // none, there is no target anchor either, and the preview goes.
    void dynamicsToolFindTarget(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(dynamics, kEditToolClass));
        const uint32_t anchor = cursorAnchor(tool->selection);
        const ee::Ptr<EditObjectPart> standing = anchorAttachedObject(anchor);
        if (standing != 0 && pieceDefCategory(editObjectDef(standing)) != kDynamicsCategory)
        {
            dynamics->targetObject = standing;
            return;
        }
        if (anchorOwner(anchor) != 0)
        {
            const ee::Ptr<EditObjectPart> owner = anchorOwner(anchor);
            bool holdsOtherThanDynamics = false;
            for (uint32_t held = firstAnchor(editObjectAnchors(owner)); held != 0; held = nextAnchor(held))
            {
                const ee::Ptr<EditObjectPart> attached = anchorAttachedObject(held);
                if (attached != 0 && pieceDefCategory(editObjectDef(attached)) != kDynamicsCategory)
                {
                    holdsOtherThanDynamics = true;
                    break;
                }
            }
            if (!holdsOtherThanDynamics)
            {
                dynamics->targetObject = owner;
                return;
            }
        }
        dynamics->targetObject = {};
        dynamics->targetAnchor = 0;
        game::tail(dynamicsToolUpdatePointer, dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolFindTarget, Verified);

    // The target and the anchor of it the piece goes on: the first of its anchors that takes
    // the trial object. The buttons' texts say whether the target takes dynamics, and the
    // preview moves to the anchor.
    void dynamicsToolShowTarget(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(dynamics, kEditToolClass));
        const uint32_t ui = toolSetUi(tool->toolSet);
        dynamicsToolFindTarget(dynamics);
        if (!dynamics->targetObject)
        {
            setInfoButtonText(ui, 1, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 2, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 3, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 4, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 5, sym::g_textDynamicsToolMove);
            game::tail(setInfoHelpText, ui, sym::g_textDynamicsToolNoHelp);
            return;
        }
        dynamics->targetAnchor = 0;
        uint32_t chosen = 0;
        int32_t chosenIndex = 0;
        int32_t index = 0;
        for (uint32_t anchor = firstAnchor(editObjectAnchors(dynamics->targetObject)); anchor != 0; anchor = nextAnchor(anchor), ++index)
        {
            if (anchorAccepts(anchor, dynamics->trial) != 0)
            {
                chosen = anchor;
                chosenIndex = index;
                break;
            }
        }
        if (chosen != 0)
        {
            dynamics->targetAnchorIndex = chosenIndex;
            dynamics->targetAnchor = chosen;
            setInfoButtonText(ui, 1, sym::g_textDynamicsToolPlace);
            setInfoButtonText(ui, 2, sym::g_textDynamicsToolRotate);
            setInfoButtonText(ui, 3, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 4, sym::g_textDynamicsToolDelete);
            setInfoButtonText(ui, 5, sym::g_textDynamicsToolMove);
            setInfoHelpText(ui, sym::g_textDynamicsToolNoHelp);
        }
        else
        {
            setInfoButtonText(ui, 1, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 2, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 3, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 4, sym::g_textDynamicsToolNone);
            setInfoButtonText(ui, 5, sym::g_textDynamicsToolMove);
            setInfoHelpText(ui, sym::g_textDynamicsToolNoDynamicsHelp);
        }
        game::tail(dynamicsToolMovePreview, dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolShowTarget, Verified);

    // The category's next piece is chosen, round to the first after the last (an edit object
    // of it is made and deleted at once). Nothing with no target anchor.
    void dynamicsToolChooseNextPiece(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        if (dynamics->targetAnchor == 0)
            return;
        ee::Ptr<PieceDef> piece = categoryNextPiece(dynamics->category, dynamics->pieceDef);
        if (!piece)
            piece = categoryFirstPiece(dynamics->category);
        const uint32_t made = newEditObject(pieceDefClassName(piece), piece);
        dynamics->pieceDef = piece;
        game::tail(deleteEditObject, made);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolChooseNextPiece, Verified);

    // The category's previous piece is chosen, round to the last before the first (an edit
    // object of it is made and deleted at once). Nothing with no target anchor.
    void dynamicsToolChoosePreviousPiece(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        if (dynamics->targetAnchor == 0)
            return;
        ee::Ptr<PieceDef> piece = categoryPreviousPiece(dynamics->category, dynamics->pieceDef);
        if (!piece)
            piece = categoryLastPiece(dynamics->category);
        const uint32_t made = newEditObject(pieceDefClassName(piece), piece);
        dynamics->pieceDef = piece;
        game::tail(deleteEditObject, made);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolChoosePreviousPiece, Verified);

    // The preview goes to the target anchor: the tool's own anchor takes the target anchor's
    // place (its turn stays the player's), with the preview hung from it again. (The target
    // anchor's angles are worked out and not used.)
    void dynamicsToolMovePreview(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        ee::Local<zge::EulerAngles> angles;
        if (dynamics->targetObject != 0 && dynamics->targetAnchor != 0)
        {
            detachFromAnchor(dynamics->previewAnchor);
            const ee::Ptr<const zge::Matrix> place(static_cast<uint32_t>(anchorMatrix(dynamics->targetAnchor)));
            zge::matrixToEuler(ee::Ptr<zge::EulerAngles>(angles), place);
            setAnchorPosition(dynamics->previewAnchor, &place->translation());
            attachToAnchor(dynamics->previewAnchor, dynamics->preview);
        }
        dynamicsToolUpdatePointer(dynamics);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolMovePreview, Verified);

    // The preview shows while there is a target anchor, and targetFree says whether that
    // anchor holds nothing yet; the delete button says None over a free anchor and Delete
    // over a taken one.
    void dynamicsToolUpdatePointer(ee::Ptr<EditDynamicsToolPart> dynamics)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(dynamics, kEditToolClass));
        const uint32_t ui = toolSetUi(tool->toolSet);
        if (dynamics->targetObject == 0 || dynamics->targetAnchor == 0)
        {
            showEditObjectDisplay(dynamics->preview, 0);
            dynamics->targetFree = 0;
            return;
        }
        showEditObjectDisplay(dynamics->preview, 1);
        if (anchorAttachedObject(dynamics->targetAnchor) != 0)
        {
            dynamics->targetFree = 0;
            game::tail(setInfoButtonText, ui, 4, sym::g_textDynamicsToolDelete);
            return;
        }
        dynamics->targetFree = 1;
        game::tail(setInfoButtonText, ui, 4, sym::g_textDynamicsToolNone);
    }
    READABLE_FUNCTION_WAITS(dynamicsToolUpdatePointer, Verified);
}
