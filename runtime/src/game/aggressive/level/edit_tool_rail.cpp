// The park editor's rail tool: the pad moves the selection over the park, and
// the object under it is the target; a preview of the rail chosen hangs at the
// target's anchor that takes it, with a pointer over that anchor. Buttons place
// a copy of the rail there, delete the rail an anchor holds, step to the next
// anchor and to the next rail that fits.

#include "aggressive/level/edit_tool_rail.h"
#include "aggressive/level/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/text/strings.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include "aggressive/level/park_messages.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/edit_tools.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/edit_tool_rail.cpp.h"

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditRailToolClass = sym::g_editRailToolName;   // "EditRailTool"
        constexpr uint32_t kEditToolClass = sym::g_railToolEditToolName;   // "EditTool"
        constexpr uint32_t kEditAnchorClass = sym::g_railToolEditAnchorName; // "EditAnchor"

        // The tool set catalog's category of rails.
        constexpr uint32_t kRailsCategory = 12;

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

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

        int32_t showEditDisplayTail(uint32_t display, uint32_t shown) { return ee::tailCall<int32_t>(sym::showEditDisplay, display, shown); }
    }

    int32_t railToolShowTarget(ee::Ptr<EditRailToolPart> rail);
    ee::Address railToolFindTarget(ee::Ptr<EditRailToolPart> rail);
    int32_t railToolRemakePreview(ee::Ptr<EditRailToolPart> rail);
    void railToolNextFittingRail(ee::Ptr<EditRailToolPart> rail);
    void railToolNextAnchor(ee::Ptr<EditRailToolPart> rail);
    int32_t railToolMovePreview(ee::Ptr<EditRailToolPart> rail);
    int32_t railToolUpdatePointer(ee::Ptr<EditRailToolPart> rail);
    void railToolNoop();

    int32_t registerEditRailToolClass()
    {
        return game::tail(registerEditClass, kEditRailToolClass, sizeof(EditRailToolPart), kEditToolClass, sym::constructEditRailTool, sym::destructEditRailTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditRailToolClass, Verified);

    // Made with the tool: its name and key, no target, the first rail of the
    // catalog's rails chosen, an anchor of the tool's own for the preview to
    // hang from, and the pointer, hidden.
    void constructEditRailTool(uint32_t object, ee::Ptr<EditToolSet> toolSet)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(object, kEditToolClass));
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(object, kEditRailToolClass));
        setEditToolMethods(tool, sym::g_railToolMethods);
        setEditToolName(tool, sym::g_textRailTool);
        tool->iconChar = 'L';
        rail->targetAnchorIndex = 0;
        rail->unused = 0;
        rail->targetAnchor = 0;
        rail->targetObject = 0;
        rail->category = catalogCategory(toolSetCatalog(toolSet), kRailsCategory);
        rail->pieceDef = categoryFirstPiece(rail->category);
        rail->previewAnchor = newEditObject(kEditAnchorClass, 0);
        rail->pointerDisplay = newEditDisplay(zge::nameHash(ee::Ptr<const char>(sym::g_anchorPointerDisplayName)), 0);
        showEditDisplayTail(rail->pointerDisplay, 0);
    }
    READABLE_FUNCTION_WAITS(constructEditRailTool, Verified);

    void destructEditRailTool(uint32_t object)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(object, kEditRailToolClass));
        deleteAnchor(rail->previewAnchor);
        game::tail(freeShownDisplay, rail->pointerDisplay);
    }
    READABLE_FUNCTION_WAITS(destructEditRailTool, Verified);

    // Picked: the pad moves the selection, buttons place, change the anchor,
    // delete, and step the rail; the selection starts afresh, and a preview of
    // the rail chosen is made and hung from the tool's anchor.
    int32_t railToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(tool, kEditRailToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        setUiButton(ui, 12, kOnPressOrHold, sym::railToolMoveLeft, rail, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::railToolMoveRight, rail, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::railToolMoveForward, rail, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::railToolMoveBack, rail, 10, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::railToolPlace, rail, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::railToolChangeAnchor, rail, 1, noDeadzone);
        setUiButton(ui, 3, kOnPress, sym::railToolDelete, rail, 1, noDeadzone);
        setUiButton(ui, 7, kOnPress, sym::railToolPreviousRail, rail, 1, noDeadzone);
        setUiButton(ui, 10, kOnPress, sym::railToolNextRail, rail, 1, noDeadzone);
        const uint32_t selection = toolSetSelection(tool->toolSet);
        tool->selection = selection;
        resetSelection(selection);
        clearSelectionRotation(tool->selection);
        refreshToolSelection(tool);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        setInfoButtonText(ui, 1, sym::g_textPlace);
        setInfoButtonText(ui, 2, sym::g_textChange);
        setInfoButtonText(ui, 3, sym::g_textNa);
        setInfoButtonText(ui, 4, sym::g_textDelete2);
        setInfoButtonText(ui, 5, sym::g_textMove);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textRailTool));
        setInfoHelpText(ui, sym::g_textAddRailsToObjects);
        const ee::Ptr<EditObjectPart> preview(newEditObject(pieceDefClassName(rail->pieceDef), rail->pieceDef));
        rail->preview = preview;
        makeEditObjectDisplay(preview, 0);
        attachToAnchor(rail->previewAnchor, rail->preview);
        return game::tail(railToolShowTarget, rail);
    }
    READABLE_FUNCTION_WAITS(railToolActivate, Verified);

    // Each frame: the target under the cursor, and the selection kept up with
    // the cursor.
    void railToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float /*seconds*/)
    {
        railToolFindTarget(ee::Ptr<EditRailToolPart>(editObjectPart(tool, kEditRailToolClass)));
        game::tail(updateSelectionCursor, tool->selection); // the frame's seconds go along too, unread
    }
    READABLE_FUNCTION_WAITS(railToolUpdate, Verified);

    // The selection in its colours, the box and the fitting colour red while the
    // target anchor already holds a rail; then a marker on each anchor of the
    // target object, the target anchor's in the fitting colour, the rest grey.
    // (The original first sets the box orange, 176, 142, 21, which the copy of
    // the selection's colours then overwrites: the box is never orange.)
    void railToolDraw(ee::Ptr<EditToolPart> tool)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(tool, kEditRailToolClass));
        ee::Local<SelectionColours> colours;
        selectionColour(tool->selection, ee::Ptr<SelectionColours>(colours));
        if (rail->targetFree == 0)
        {
            colours->heldFit = kAnchorTaken;
            colours->box = kAnchorTaken;
        }
        drawSelection(tool->selection, colours.address());
        railToolFindTarget(rail);
        if (rail->targetObject == 0)
            return;
        const uint32_t anchors = editObjectAnchors(rail->targetObject);
        if (anchors == 0)
            return;
        for (uint32_t anchor = firstAnchor(anchors); anchor != 0; anchor = nextAnchor(anchor))
        {
            if (anchor == rail->targetAnchor)
                drawAnchorMarker(anchorPosition(anchor), colourWord(colours->heldFit));
            else
                drawAnchorMarker(anchorPosition(anchor), colourWord(kOtherAnchor));
        }
    }
    READABLE_FUNCTION_WAITS(railToolDraw, Verified);

    // Put away: the buttons go, and so does the preview (deleted), the target
    // and the pointer.
    void railToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(tool, kEditRailToolClass));
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 5);
        clearUiButton(ui, 4);
        clearUiButton(ui, 3);
        clearUiButton(ui, 7);
        clearUiButton(ui, 10);
        if (rail->preview != 0)
        {
            detachFromAnchor(rail->previewAnchor);
            broadcastEditMessage(rail->preview, sym::makeDeleteMessage, 0);
            rail->preview = 0;
        }
        rail->targetObject = 0;
        showEditDisplayTail(rail->pointerDisplay, 0);
    }
    READABLE_FUNCTION_WAITS(railToolDeactivate, Verified);

    // One cell of the selection along a heading (degrees) from the camera's, and
    // the target shown again (railToolShowTarget's result).
    int32_t railToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        moveSelection(tool->selection, step.address());
        return railToolShowTarget(ee::Ptr<EditRailToolPart>(editObjectPart(tool, kEditRailToolClass)));
    }
    READABLE_FUNCTION_WAITS(railToolMove, Verified);

    // The move buttons: one cell of the selection, left of, right of, along and
    // against the camera's heading. Their row's data is the rail tool part.
    int32_t railToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(railToolMove, tool, ee::Float(90.0f));
    }
    READABLE_FUNCTION_WAITS(railToolMoveLeft, Verified);

    int32_t railToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(railToolMove, tool, ee::Float(-90.0f));
    }
    READABLE_FUNCTION_WAITS(railToolMoveRight, Verified);

    int32_t railToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(railToolMove, tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(railToolMoveForward, Verified);

    int32_t railToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(railToolMove, tool, ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(railToolMoveBack, Verified);

    // The next rail that fits, and its preview.
    int32_t railToolNextRail(ee::Ptr<EditUiButton> button)
    {
        railToolNextFittingRail(ee::Ptr<EditRailToolPart>(button->data));
        return game::tail(railToolRemakePreview, ee::Ptr<EditRailToolPart>(button->data));
    }
    READABLE_FUNCTION_WAITS(railToolNextRail, Verified);

    // The previous rail: an empty step (this build has none), and the preview
    // made again.
    int32_t railToolPreviousRail(ee::Ptr<EditUiButton> button)
    {
        railToolNoop();
        return game::tail(railToolRemakePreview, ee::Ptr<EditRailToolPart>(button->data));
    }
    READABLE_FUNCTION_WAITS(railToolPreviousRail, Verified);

    void railToolChangeAnchor(ee::Ptr<EditUiButton> button)
    {
        game::tail(railToolNextAnchor, ee::Ptr<EditRailToolPart>(editObjectPart(button->data, kEditRailToolClass)));
    }
    READABLE_FUNCTION_WAITS(railToolChangeAnchor, Verified);

    // Place: refused while the park's cost allows nothing more; otherwise a copy
    // of the preview goes on the target anchor, if it holds no rail yet.
    void railToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(button->data, kEditRailToolClass));
        if (parkCostAllowsMore() == 0)
        {
            game::tail(editRefused);
            return;
        }
        if (rail->preview == 0 || rail->targetAnchor == 0)
            return;
        if (anchorAttachedObject(rail->targetAnchor) == 0)
            attachToAnchor(rail->targetAnchor, cloneEditObject(rail->preview));
        game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolPlace, Verified);

    // Delete: the rail the target anchor holds comes off and is deleted.
    void railToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(button->data, kEditRailToolClass));
        if (rail->targetAnchor == 0)
            return;
        const ee::Ptr<EditObjectPart> placed = anchorAttachedObject(rail->targetAnchor);
        if (placed != 0)
        {
            detachFromAnchor(rail->targetAnchor);
            broadcastEditMessage(placed, sym::makeDeleteMessage, 0);
        }
        game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolDelete, Verified);

    // The target object's next anchor that takes the preview (nextAnchorFor
    // steps the index), and the preview moves there.
    void railToolNextAnchor(ee::Ptr<EditRailToolPart> rail)
    {
        if (rail->targetObject == 0 || rail->targetAnchor == 0)
            return;
        rail->targetAnchor = nextAnchorFor(rail->targetObject, rail->preview, &rail->targetAnchorIndex);
        game::tail(railToolMovePreview, rail);
    }
    READABLE_FUNCTION_WAITS(railToolNextAnchor, Verified);

    // A new preview of the rail chosen: the old one deleted, the new one made
    // with its display and moved to the target.
    int32_t railToolRemakePreview(ee::Ptr<EditRailToolPart> rail)
    {
        if (rail->preview != 0)
            broadcastEditMessage(rail->preview, sym::makeDeleteMessage, 0);
        const ee::Ptr<EditObjectPart> preview(newEditObject(pieceDefClassName(rail->pieceDef), rail->pieceDef));
        rail->preview = preview;
        makeEditObjectDisplay(preview, 0);
        return game::tail(railToolMovePreview, rail);
    }
    READABLE_FUNCTION_WAITS(railToolRemakePreview, Verified);

    // The object under the cursor: the one hanging from the cursor's anchor, or
    // else the anchor's owner. With none, there is no target anchor either, and
    // the pointer and preview go.
    ee::Address railToolFindTarget(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(rail, kEditToolClass));
        const uint32_t anchor = cursorAnchor(tool->selection);
        if (anchorAttachedObject(anchor) != 0)
        {
            const ee::Ptr<EditObjectPart> object = anchorAttachedObject(anchor);
            rail->targetObject = object;
            return object;
        }
        if (anchorOwner(anchor) != 0)
        {
            const ee::Ptr<EditObjectPart> object = anchorOwner(anchor);
            rail->targetObject = object;
            return object;
        }
        rail->targetObject = {};
        rail->targetAnchor = 0;
        return game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolFindTarget, Verified);

    // The target and the anchor of it the preview goes on: the one at the place
    // chosen last time if it still takes the preview, otherwise the last of its
    // anchors that does. The test meant to keep the first one found reads the
    // target anchor, cleared just before, so each later one replaces it (kept).
    // The buttons' texts say whether the target takes a rail.
    int32_t railToolShowTarget(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(rail, kEditToolClass));
        const uint32_t ui = toolSetUi(tool->toolSet);
        const ee::Address found = railToolFindTarget(rail);
        if (!rail->targetObject)
            return found;
        rail->targetAnchor = 0;
        uint32_t chosen = 0;
        int32_t chosenIndex = 0;
        int32_t index = 0;
        for (uint32_t anchor = firstAnchor(editObjectAnchors(rail->targetObject)); anchor != 0; anchor = nextAnchor(anchor), ++index)
        {
            if (anchorAccepts(anchor, rail->preview) == 0)
                continue;
            if (index == rail->targetAnchorIndex)
            {
                chosen = anchor;
                chosenIndex = index;
                break;
            }
            if (rail->targetAnchor == 0)
            {
                chosen = anchor;
                chosenIndex = index;
            }
        }
        if (chosen != 0)
        {
            rail->targetAnchorIndex = chosenIndex;
            rail->targetAnchor = chosen;
            setInfoButtonText(ui, 1, sym::g_textPlace);
            setInfoButtonText(ui, 2, sym::g_textChange);
            setInfoButtonText(ui, 3, sym::g_textNa);
            setInfoButtonText(ui, 4, sym::g_textDelete2);
            setInfoButtonText(ui, 5, sym::g_textMove);
            setInfoHelpText(ui, sym::g_textAddRailsToThisObject);
        }
        else
        {
            setInfoButtonText(ui, 1, sym::g_textNa);
            setInfoButtonText(ui, 2, sym::g_textNa);
            setInfoButtonText(ui, 3, sym::g_textNa);
            setInfoButtonText(ui, 4, sym::g_textNa);
            setInfoButtonText(ui, 5, sym::g_textMove);
            setInfoHelpText(ui, sym::g_textThisObjectDoesNotAcceptRails);
        }
        return game::tail(railToolMovePreview, rail);
    }
    READABLE_FUNCTION_WAITS(railToolShowTarget, Verified);

    // The rail after the one chosen in the category, round to the first, that
    // the target anchor takes: each is made to be tried and deleted again. Back
    // at the rail it started from, it stops. Nothing without a target anchor.
    void railToolNextFittingRail(ee::Ptr<EditRailToolPart> rail)
    {
        ee::Ptr<PieceDef> piece = rail->pieceDef;
        if (rail->targetAnchor == 0)
            return;
        do
        {
            piece = categoryNextPiece(rail->category, piece);
            if (!piece)
                piece = categoryFirstPiece(rail->category);
            const ee::Ptr<EditObjectPart> tried(newEditObject(pieceDefClassName(piece), piece));
            if (anchorAccepts(rail->targetAnchor, tried) != 0)
                rail->pieceDef = piece;
            deleteEditObject(tried);
        } while (piece != rail->pieceDef);
    }
    READABLE_FUNCTION_WAITS(railToolNextFittingRail, Verified);

    // The empty step of the previous-rail button.
    void railToolNoop()
    {
    }
    READABLE_FUNCTION(railToolNoop, Verified);

    // With a target anchor, the tool's anchor takes its place and angles, and
    // the preview hangs from it there.
    int32_t railToolMovePreview(ee::Ptr<EditRailToolPart> rail)
    {
        ee::Local<zge::EulerAngles> angles;
        if (rail->targetObject != 0 && rail->targetAnchor != 0)
        {
            detachFromAnchor(rail->previewAnchor);
            const ee::Ptr<const zge::Matrix> place(static_cast<uint32_t>(anchorMatrix(rail->targetAnchor)));
            zge::matrixToEuler(ee::Ptr<zge::EulerAngles>(angles), place);
            setAnchorPosition(rail->previewAnchor, &place->translation());
            setAnchorAngles(rail->previewAnchor, angles.address());
            attachToAnchor(rail->previewAnchor, rail->preview);
        }
        return railToolUpdatePointer(rail);
    }
    READABLE_FUNCTION_WAITS(railToolMovePreview, Verified);

    // The preview and the pointer show while there is a target anchor; the
    // pointer stands at it, and targetFree says whether it holds no rail yet.
    int32_t railToolUpdatePointer(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditObjectPart> preview = rail->preview;
        if (rail->targetObject != 0 && rail->targetAnchor != 0)
        {
            showEditObjectDisplay(preview, 1);
            rail->targetFree = anchorAttachedObject(rail->targetAnchor) == 0 ? 1 : 0;
            showEditDisplay(rail->pointerDisplay, 1);
            return game::tail(setDisplayPosition, rail->pointerDisplay, anchorWorldPosition(rail->targetAnchor));
        }
        showEditObjectDisplay(preview, 0);
        rail->targetFree = 0;
        return showEditDisplayTail(rail->pointerDisplay, 0);
    }
    READABLE_FUNCTION_WAITS(railToolUpdatePointer, Verified);
}
