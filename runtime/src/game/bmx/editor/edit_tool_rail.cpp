// BMX XXX's park editor (../GameTools/ParkEditor, linked after its game's own sources, as in
// Aggressive Inline): Aggressive Inline's game/aggressive/level/edit_tool_rail.cpp, copied for BMX XXX.
// The park editor's rail tool: the pad moves the selection over the park, and
// the object under it is the target; a preview of the rail chosen hangs at the
// target's anchor that takes it, with a pointer over that anchor. Buttons place
// a copy of the rail there, delete the rail an anchor holds, step to the next
// anchor and to the next rail that fits.

#include "bmx/editor/edit_tool_rail.h"
#include "bmx/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
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
#include "sym/bmx/editor/edit_tool_rail.cpp.h"

namespace game::bmx::editor
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditRailToolClass = sym::g_editRailToolName;   // "EditRailTool"
        constexpr uint32_t kEditToolClass = sym::g_railToolEditToolName;   // "EditTool"

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

        constexpr uint32_t kEditAnchorClass = sym::g_railToolEditAnchorName; // "EditAnchor"

        // The tool set catalog's category of rails.
        constexpr uint32_t kRailsCategory = 13;

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        constexpr uint32_t kTokenBytes = 0x100;

        // The rail places table, read from assets\editor\anchor.txt when the tool is made.
        inline ee::Global<ee::Ptr<RailPlacesRow>, sym::g_railPlaces> g_railPlaces;
        inline ee::Global<uint32_t, sym::g_railPlacesRowCount> g_railPlacesRowCount;

        // A piece turned in the park has its rail places turned with it. By the place as it lies in the park, and
        // the quarter turns of the piece: the piece's own place there. The four sides go round; the two centre
        // lines swap with each quarter turn, and so do the two diagonals.
        //   top:    top, left, bottom, right        centre horizontal: horizontal, vertical, horizontal, vertical
        //   right:  right, top, left, bottom        centre vertical:   vertical, horizontal, vertical, horizontal
        //   bottom: bottom, right, top, left        diagonal one:      one, two, one, two
        //   left:   left, bottom, right, top        diagonal two:      two, one, two, one
        constexpr uint32_t kQuarterTurns = 4;
        uint32_t turnedRailPlace(uint32_t place, uint32_t quarterTurns)
        {
            return ee::Ptr<uint8_t>(sym::g_railPlaceTurns)[quarterTurns + place * kQuarterTurns];
        }

        // How many quarter turns the target object's parent anchor is turned about the vertical.
        uint32_t targetQuarterTurns(ee::Ptr<EditRailToolPart> rail)
        {
            const ee::Ptr<zge::EulerAngles> angles(static_cast<uint32_t>(anchorAngles(editObjectParentAnchor(rail->targetObject))));
            return static_cast<uint16_t>(angles->y) >> 14;
        }
    }

    void railToolUpdatePointer(ee::Ptr<EditRailToolPart> rail);

    void railToolShowTarget(ee::Ptr<EditRailToolPart> rail);
    void railToolMovePreview(ee::Ptr<EditRailToolPart> rail);
    void loadRailPlaces(ee::Ptr<EditRailToolPart> rail);
    int32_t railPlacesRowOf(ee::Ptr<EditObjectPart> object);
    uint32_t railPlaceOfAnchor(int32_t row, int32_t anchorIndex);
    int32_t railPlaceAnchor(int32_t row, uint32_t place);
    uint32_t firstRailPlaceAnchor(int32_t row, ee::Ptr<uint32_t> place);

    int32_t registerEditRailToolClass()
    {
        return game::tail(registerEditClass, kEditRailToolClass, sizeof(EditRailToolPart), kEditToolClass, sym::constructEditRailTool, sym::destructEditRailTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditRailToolClass, Verified);

    void railToolFindTarget(ee::Ptr<EditRailToolPart> rail);
    void railToolUpdatePointer(ee::Ptr<EditRailToolPart> rail);
    void railToolNextFittingRail(ee::Ptr<EditRailToolPart> rail);
    void railToolNextAnchor(ee::Ptr<EditRailToolPart> rail);
    void railToolNoop();

    // Each frame: the target under the cursor, and the selection kept up with
    // the cursor.
    void railToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float /*seconds*/)
    {
        railToolFindTarget(ee::Ptr<EditRailToolPart>(editObjectPart(tool, kEditRailToolClass)));
        game::tail(updateSelectionCursor, tool->selection); // the frame's seconds go along too, unread
    }
    READABLE_FUNCTION_WAITS(railToolUpdate, Verified);

    // Drawn: the selection (red where the target anchor already holds a rail), then
    // a marker on each of the target object's anchors, the target's in the held
    // piece's colour and the others grey.
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

    // One cell of the selection along a heading (degrees) from the camera's, and the
    // target shown again.
    void railToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        moveSelection(tool->selection, step.address());
        railToolShowTarget(ee::Ptr<EditRailToolPart>(editObjectPart(tool, kEditRailToolClass)));
    }
    READABLE_FUNCTION_WAITS(railToolMove, Verified);

    // The move buttons: one cell of the selection, left of, right of, along and
    // against the camera's heading, with the move's sound (BMX XXX). Their row's
    // data is the rail tool part.
    uint32_t railToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        railToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(90.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(railToolMoveLeft, Verified);

    uint32_t railToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        railToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(-90.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(railToolMoveRight, Verified);

    uint32_t railToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        railToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(0.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(railToolMoveForward, Verified);

    uint32_t railToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        railToolMove(ee::Ptr<EditToolPart>(editObjectPart(button->data, kEditToolClass)), ee::Float(180.0f));
        return calls::soundEventTail(kMoveSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(railToolMoveBack, Verified);

    // The rail goes to the target object's next anchor, with the turn's sound (BMX XXX).
    uint32_t railToolChangeAnchor(ee::Ptr<EditUiButton> button)
    {
        railToolNextAnchor(ee::Ptr<EditRailToolPart>(editObjectPart(button->data, kEditRailToolClass)));
        return calls::soundEventTail(kRotateSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(railToolChangeAnchor, Verified);

    // The preview shows while there is a target anchor, and targetFree says whether that
    // anchor holds no rail yet; BMX XXX's delete button says None over a free anchor and
    // Delete over a taken one (it has no pointer display: Aggressive Inline moves one there).
    void railToolUpdatePointer(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(rail, kEditToolClass));
        const uint32_t ui = toolSetUi(tool->toolSet);
        if (rail->targetObject == 0 || rail->targetAnchor == 0)
        {
            showEditObjectDisplay(rail->preview, 0);
            rail->targetFree = 0;
            return;
        }
        showEditObjectDisplay(rail->preview, 1);
        if (anchorAttachedObject(rail->targetAnchor) != 0)
        {
            rail->targetFree = 0;
            game::tail(setInfoButtonText, ui, 4, sym::g_textRailToolDelete);
            return;
        }
        rail->targetFree = 1;
        game::tail(setInfoButtonText, ui, 4, sym::g_textRailToolNone);
    }
    READABLE_FUNCTION_WAITS(railToolUpdatePointer, Verified);

    // Place: refused while the park's cost allows nothing more, or with no preview or no
    // target anchor (the refusal's sound each time); otherwise a copy of the preview goes
    // on the target anchor with the place sound, or the refusal's if it holds a rail already.
    void railToolPlace(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(button->data, kEditRailToolClass));
        if (parkCostAllowsMore() == 0)
        {
            editRefused();
            calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
            return;
        }
        if (rail->preview == 0 || rail->targetAnchor == 0)
        {
            calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
            return;
        }
        if (anchorAttachedObject(rail->targetAnchor) == 0)
        {
            attachToAnchor(rail->targetAnchor, cloneEditObject(rail->preview));
            soundEvent(kPlaceSound, 0, 0, 0, 0);
        }
        else
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolPlace, Verified);

    // Made with the tool: its name and key, no target, the first rail of the catalog's rails
    // chosen, an anchor of the tool's own for the preview to hang from, and the rail places
    // table read.
    void constructEditRailTool(uint32_t object, ee::Ptr<EditToolSet> toolSet)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(object, kEditToolClass));
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(object, kEditRailToolClass));
        setEditToolMethods(tool, sym::g_railToolMethods);
        setEditToolName(tool, sym::g_textRailTool);
        tool->iconChar = 'L';
        rail->targetAnchorIndex = 0;
        rail->place = kRailTop;
        rail->placesRow = 0;
        rail->targetAnchor = 0;
        rail->targetObject = {};
        rail->catalog = toolSetCatalog(toolSet);
        rail->category = static_cast<uint32_t>(catalogCategory(rail->catalog, kRailsCategory));
        rail->pieceDef = categoryFirstPiece(rail->category);
        rail->previewAnchor = newEditObject(kEditAnchorClass, 0);
        game::tail(loadRailPlaces, rail);
    }
    READABLE_FUNCTION_WAITS(constructEditRailTool, Verified);

    // Unmade: its anchor goes, and the rail places table.
    void destructEditRailTool(uint32_t object)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(object, kEditRailToolClass));
        deleteAnchor(rail->previewAnchor);
        game::tail(zge::heapFree, *g_railPlaces);
    }
    READABLE_FUNCTION_WAITS(destructEditRailTool, Verified);

    // Picked: the pad moves the selection, buttons place, change the anchor and delete; the
    // selection starts afresh, and a preview of the rail chosen is made and hung from the
    // tool's anchor. (BMX XXX has no buttons for the next and previous rail: the rail is
    // whichever fits the anchor.)
    void railToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
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
        const uint32_t selection = toolSetSelection(tool->toolSet);
        tool->selection = selection;
        resetSelection(selection);
        clearSelectionRotation(tool->selection);
        refreshToolSelection(tool);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        setInfoButtonText(ui, 1, sym::g_textRailToolPlace);
        setInfoButtonText(ui, 2, sym::g_textRailToolChange);
        setInfoButtonText(ui, 3, sym::g_textRailToolNone);
        setInfoButtonText(ui, 4, sym::g_textRailToolDelete);
        setInfoButtonText(ui, 5, sym::g_textRailToolMove);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textRailTool));
        setInfoHelpText(ui, sym::g_textAddRailsToObjects);
        const ee::Ptr<EditObjectPart> preview(newEditObject(pieceDefClassName(rail->pieceDef), rail->pieceDef));
        rail->preview = preview;
        makeEditObjectDisplay(preview, 0);
        attachToAnchor(rail->previewAnchor, rail->preview);
        game::tail(railToolShowTarget, rail);
    }
    READABLE_FUNCTION_WAITS(railToolActivate, Verified);

    // Put away: the buttons go, and so do the preview (deleted) and the target.
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
        if (rail->preview != 0)
        {
            detachFromAnchor(rail->previewAnchor);
            broadcastEditMessage(rail->preview, sym::makeDeleteMessage, 0);
            rail->preview = {};
        }
        rail->targetObject = {};
    }
    READABLE_FUNCTION_WAITS(railToolDeactivate, Verified);

    // Delete: the rail the target anchor holds comes off and is deleted, with the delete
    // sound; with no target anchor, or none on it, the refusal's sound.
    void railToolDelete(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditRailToolPart> rail(editObjectPart(button->data, kEditRailToolClass));
        if (rail->targetAnchor == 0)
        {
            calls::soundEventTail(kRefusedSound, 0, 0, 0, 0);
            return;
        }
        const ee::Ptr<EditObjectPart> placed = anchorAttachedObject(rail->targetAnchor);
        if (placed != 0)
        {
            detachFromAnchor(rail->targetAnchor);
            broadcastEditMessage(placed, sym::makeDeleteMessage, 0);
            soundEvent(kDeleteSound, 0, 0, 0, 0);
        }
        else
            soundEvent(kRefusedSound, 0, 0, 0, 0);
        game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolDelete, Verified);

    namespace
    {
        // The target object's anchor at the target anchor's index, counted along its list.
        uint32_t targetAnchorAtIndex(ee::Ptr<EditRailToolPart> rail)
        {
            uint32_t anchor = firstAnchor(editObjectAnchors(rail->targetObject));
            for (int32_t index = 0; index != rail->targetAnchorIndex; ++index)
                anchor = nextAnchor(anchor);
            return anchor;
        }
    }

    // The rail goes to the next place of the target that has an anchor: round the places from
    // the one after the present, as they lie in the park, to the first one the target's row
    // of the table gives an anchor (the present one again when it is the only one). If that
    // anchor does not take the rail chosen, the next rail of the category that it takes is
    // chosen, and the preview made again from it; the preview then moves there.
    void railToolNextAnchor(ee::Ptr<EditRailToolPart> rail)
    {
        if (rail->targetObject == 0 || rail->targetAnchor == 0)
            return;
        const ee::Ptr<RailPlacesRow> row = *g_railPlaces + rail->placesRow;
        const uint32_t startPlace = rail->place & 0xFF;
        uint32_t place = startPlace + 1;
        bool found = false;
        do
        {
            if (place == kRailPlaces)
                place = kRailTop;
            const int32_t anchorIndex = ee::Ptr<int8_t>(&row->anchorIndex[0])[turnedRailPlace(place, targetQuarterTurns(rail))];
            if (anchorIndex == -1)
                ++place;
            else
            {
                rail->place = place;
                found = true;
                rail->targetAnchorIndex = anchorIndex;
            }
        } while (place != startPlace && !found);
        if (!found)
            return;
        const uint32_t anchor = targetAnchorAtIndex(rail);
        if (anchorAccepts(anchor, rail->preview) != 0)
        {
            rail->targetAnchor = anchor;
            game::tail(railToolMovePreview, rail);
            return;
        }
        ee::Ptr<PieceDef> piece = rail->pieceDef;
        do
        {
            piece = categoryNextPiece(rail->category, piece);
            if (!piece)
                piece = categoryFirstPiece(rail->category);
            const ee::Ptr<EditObjectPart> tried(newEditObject(pieceDefClassName(piece), piece));
            if (anchorAccepts(anchor, tried) != 0)
            {
                rail->pieceDef = piece;
                if (anchorAttachedObject(rail->previewAnchor) == rail->preview)
                {
                    detachFromAnchor(rail->previewAnchor);
                    deleteEditObject(rail->preview);
                    rail->preview = tried;
                    makeEditObjectDisplay(tried, 0);
                    attachToAnchor(rail->previewAnchor, rail->preview);
                }
                else
                {
                    deleteEditObject(rail->preview);
                    rail->preview = tried;
                    makeEditObjectDisplay(tried, 0);
                }
            }
            else
                deleteEditObject(tried);
        } while (piece != rail->pieceDef);
        rail->targetAnchor = anchor;
        game::tail(railToolMovePreview, rail);
    }
    READABLE_FUNCTION_WAITS(railToolNextAnchor, Verified);

    // The object under the cursor that rails go on. A piece standing on the cursor's anchor is
    // the target unless it is itself a rail. Otherwise the anchor's owner is, unless one of the
    // owner's anchors holds something that is not a rail. With none, there is no target anchor
    // either, and the preview goes.
    void railToolFindTarget(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(rail, kEditToolClass));
        const uint32_t anchor = cursorAnchor(tool->selection);
        const ee::Ptr<EditObjectPart> standing = anchorAttachedObject(anchor);
        if (standing != 0 && pieceDefCategory(editObjectDef(standing)) != kRailsCategory)
        {
            rail->targetObject = standing;
            return;
        }
        if (anchorOwner(anchor) != 0)
        {
            const ee::Ptr<EditObjectPart> owner = anchorOwner(anchor);
            bool holdsOtherThanRail = false;
            for (uint32_t held = firstAnchor(editObjectAnchors(owner)); held != 0; held = nextAnchor(held))
            {
                const ee::Ptr<EditObjectPart> attached = anchorAttachedObject(held);
                if (attached != 0 && pieceDefCategory(editObjectDef(attached)) != kRailsCategory)
                {
                    holdsOtherThanRail = true;
                    break;
                }
            }
            if (!holdsOtherThanRail)
            {
                rail->targetObject = owner;
                return;
            }
        }
        rail->targetObject = {};
        rail->targetAnchor = 0;
        game::tail(railToolUpdatePointer, rail);
    }
    READABLE_FUNCTION_WAITS(railToolFindTarget, Verified);

    // The target and the anchor of it the rail goes on. The target's row of the rail places
    // table gives the anchor at the tool's place, turned as the target is; where that place
    // has none, the row's first place that has one is taken instead. If the anchor does not
    // take the rail chosen, the next rail of the category that it takes is chosen and the
    // preview made again. The buttons' texts say whether the target takes a rail, and the
    // preview moves to the anchor.
    void railToolShowTarget(ee::Ptr<EditRailToolPart> rail)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(rail, kEditToolClass));
        const uint32_t ui = toolSetUi(tool->toolSet);
        railToolFindTarget(rail);
        if (!rail->targetObject)
        {
            setInfoButtonText(ui, 1, sym::g_textRailToolNone);
            setInfoButtonText(ui, 2, sym::g_textRailToolNone);
            setInfoButtonText(ui, 3, sym::g_textRailToolNone);
            setInfoButtonText(ui, 4, sym::g_textRailToolNone);
            setInfoButtonText(ui, 5, sym::g_textRailToolMove);
            game::tail(setInfoHelpText, ui, sym::g_textRailToolNoHelp);
            return;
        }
        rail->targetAnchor = 0;
        uint32_t chosen = 0;
        rail->placesRow = railPlacesRowOf(rail->targetObject);
        if (rail->placesRow != -1)
        {
            const uint32_t quarterTurns = targetQuarterTurns(rail);
            rail->targetAnchorIndex = railPlaceAnchor(rail->placesRow, turnedRailPlace(rail->place, quarterTurns));
            if (rail->targetAnchorIndex == -1)
            {
                rail->targetAnchorIndex = static_cast<int32_t>(firstRailPlaceAnchor(rail->placesRow, ee::Ptr<uint32_t>(&rail->place)));
                rail->place = railPlaceOfAnchor(rail->placesRow, rail->targetAnchorIndex);
                rail->place = turnedRailPlace(rail->place, quarterTurns);
            }
            const uint32_t anchor = targetAnchorAtIndex(rail);
            chosen = anchor;
            if (anchorAccepts(chosen, rail->preview) == 0)
            {
                ee::Ptr<PieceDef> piece = rail->pieceDef;
                do
                {
                    piece = categoryNextPiece(rail->category, piece);
                    if (!piece)
                        piece = categoryFirstPiece(rail->category);
                    const ee::Ptr<EditObjectPart> tried(newEditObject(pieceDefClassName(piece), piece));
                    if (anchorAccepts(anchor, tried) != 0)
                    {
                        rail->pieceDef = piece;
                        if (rail->previewAnchor != 0 && rail->preview == anchorAttachedObject(rail->previewAnchor))
                            detachFromAnchor(rail->previewAnchor);
                        deleteEditObject(rail->preview);
                        rail->preview = tried;
                        makeEditObjectDisplay(tried, 0);
                        attachToAnchor(rail->previewAnchor, rail->preview);
                    }
                    else
                    {
                        if (rail->previewAnchor != 0 && tried == anchorAttachedObject(rail->previewAnchor))
                            detachFromAnchor(rail->previewAnchor);
                        deleteEditObject(tried);
                    }
                } while (piece != rail->pieceDef);
            }
        }
        if (chosen != 0)
        {
            rail->targetAnchor = chosen;
            setInfoButtonText(ui, 1, sym::g_textRailToolPlace);
            setInfoButtonText(ui, 2, sym::g_textRailToolChange);
            setInfoButtonText(ui, 3, sym::g_textRailToolNone);
            setInfoButtonText(ui, 4, sym::g_textRailToolDelete);
            setInfoButtonText(ui, 5, sym::g_textRailToolMove);
            setInfoHelpText(ui, sym::g_textRailToolTakesRailsHelp);
        }
        else
        {
            setInfoButtonText(ui, 1, sym::g_textRailToolNone);
            setInfoButtonText(ui, 2, sym::g_textRailToolNone);
            setInfoButtonText(ui, 3, sym::g_textRailToolNone);
            setInfoButtonText(ui, 4, sym::g_textRailToolNone);
            setInfoButtonText(ui, 5, sym::g_textRailToolMove);
            setInfoHelpText(ui, sym::g_textRailToolNoRailsHelp);
        }
        game::tail(railToolMovePreview, rail);
    }
    READABLE_FUNCTION_WAITS(railToolShowTarget, Verified);

    // The preview goes to the target anchor: the tool's own anchor takes the target anchor's
    // place and turn, with the preview hung from it again.
    void railToolMovePreview(ee::Ptr<EditRailToolPart> rail)
    {
        ee::Local<zge::EulerAngles> angles;
        if (rail->targetObject != 0 && rail->targetAnchor != 0)
        {
            detachFromAnchor(rail->previewAnchor);
            const ee::Ptr<const zge::Matrix> place(static_cast<uint32_t>(anchorMatrix(rail->targetAnchor)));
            zge::matrixToEuler(angles, place);
            setAnchorPosition(rail->previewAnchor, &place->translation());
            setAnchorAngles(rail->previewAnchor, angles);
            attachToAnchor(rail->previewAnchor, rail->preview);
        }
        railToolUpdatePointer(rail);
    }
    READABLE_FUNCTION_WAITS(railToolMovePreview, Verified);

    // The rail places table, from assets\editor\anchor.txt: the number of rows, then for each
    // its piece's index in the catalog and eight anchor names, one for each RailPlace, or
    // "none". A name becomes the anchor's place in the piece's list of anchors. (A name none
    // of the piece's anchors has is not looked for: the walk runs off the list's end. A text
    // saying the piece has no anchors is made for every row and shown nowhere.)
    void loadRailPlaces(ee::Ptr<EditRailToolPart> rail)
    {
        ee::Local<uint8_t[0x100]> token;
        ee::Local<uint8_t[0x100]> message;
        ee::Local<uint32_t> bytes;
        ee::Local<uint32_t> pieceIndex;
        const ee::Ptr<zge::ZFile> file = zge::openFile(sym::g_railPlacesFileName, zge::kFileModeRead, 0).file();
        zge::fileSize(file, bytes, 0);
        const uint32_t text = zge::heapAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memoryFile = zge::openMemoryFile(text, *bytes);
        zge::beginScript(zge::filePath(memoryFile));
        zge::readNumberToken(&*g_railPlacesRowCount);
        *g_railPlaces = ee::Ptr<RailPlacesRow>(zge::heapAllocate(*g_railPlacesRowCount * static_cast<uint32_t>(sizeof(RailPlacesRow))));
        for (uint32_t index = 0; index < *g_railPlacesRowCount; ++index)
        {
            const ee::Ptr<RailPlacesRow> row = *g_railPlaces + index;
            zge::readNumberToken(pieceIndex.address());
            row->pieceIndex = *pieceIndex;
            const uint32_t piece = catalogPieceDef(rail->catalog, *pieceIndex);
            for (uint32_t place = 0; place < kRailPlaces; ++place)
            {
                zge::nextToken(token, kTokenBytes);
                if (zge::stringCompareNoCase(token, sym::g_railPlacesNone) == 0)
                {
                    row->anchorIndex[place] = -1;
                    continue;
                }
                const uint32_t hash = zge::nameHash(ee::Ptr<const char>(token.address()));
                int32_t anchorIndex = 0;
                uint32_t anchor = firstAnchor(pieceDefAnchors(piece));
                while (anchorNameHash(anchor) != hash)
                {
                    anchor = nextAnchor(anchor);
                    ++anchorIndex;
                }
                row->anchorIndex[place] = static_cast<int8_t>(anchorIndex);
            }
            zge::formatString(message, sym::bmx::g_railPlacesNoAnchorsFormat, row->pieceIndex);
        }
        zge::endScript();
        zge::closeFile(memoryFile, 0);
        zge::heapFree(text);
    }
    READABLE_FUNCTION_WAITS(loadRailPlaces, Verified);

    // The row of the rail places table for an object's piece, -1 when the table has none.
    int32_t railPlacesRowOf(ee::Ptr<EditObjectPart> object)
    {
        const uint32_t pieceIndex = pieceDefIndex(editObjectDef(object));
        for (uint32_t row = 0; row < *g_railPlacesRowCount; ++row)
        {
            if ((*g_railPlaces)[row].pieceIndex == pieceIndex)
                return static_cast<int32_t>(row);
        }
        return -1;
    }
    READABLE_FUNCTION_WAITS(railPlacesRowOf, Verified);

    // The place of a row that has the anchor, 0xFF for none.
    uint32_t railPlaceOfAnchor(int32_t row, int32_t anchorIndex)
    {
        const ee::Ptr<RailPlacesRow> places = *g_railPlaces + row;
        for (uint32_t place = 0; place < kRailPlaces; ++place)
        {
            if (places->anchorIndex[place] == anchorIndex)
                return place;
        }
        return 0xFF;
    }
    READABLE_FUNCTION(railPlaceOfAnchor, Verified);

    // The anchor at a place of a row, -1 for none.
    int32_t railPlaceAnchor(int32_t row, uint32_t place)
    {
        const ee::Ptr<RailPlacesRow> places = *g_railPlaces + row;
        return ee::Ptr<int8_t>(&places->anchorIndex[0])[place & 0xFF];
    }
    READABLE_FUNCTION(railPlaceAnchor, Verified);

    // A row's first place that has an anchor: the place goes to *place and the anchor is the
    // result, as the unsigned byte it is in the table. With none, 0, and *place is left.
    uint32_t firstRailPlaceAnchor(int32_t row, ee::Ptr<uint32_t> place)
    {
        const ee::Ptr<RailPlacesRow> places = *g_railPlaces + row;
        for (uint32_t index = 0; index < kRailPlaces; ++index)
        {
            if (places->anchorIndex[index] != -1)
            {
                *place = index;
                return static_cast<uint8_t>(places->anchorIndex[index]);
            }
        }
        return 0;
    }
    READABLE_FUNCTION(firstRailPlaceAnchor, Verified);
}
