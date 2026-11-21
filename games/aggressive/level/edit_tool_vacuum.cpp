// The park editor's vacuum tool: the hangar's overhead crane (a beam, "Terrain_
// HangerCraneB002", that runs along z, and a trolley on it, "B001", that runs
// along x) carries six vacuum heads over the selection, and the pieces there are
// lifted, carried and put down again.

#include "aggressive/level/edit_tool_vacuum.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_selection.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_vacuum.cpp.h"

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditVacuumToolClass = sym::g_editVacuumToolName; // "EditVacuumTool"
        constexpr uint32_t kEditToolClass = sym::g_vacuumToolEditToolName;   // "EditTool"

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // What the heads hold hangs this far above where it sits (y is down).
        constexpr ee::Float kHeldLift = ee::Float(-3.0f);

        // setEditCameraTarget, and what it leaves in the result register (the tool's methods return it).
        int32_t setEditCameraTargetKeepingResult(ee::Ptr<EditCamera> camera, uint32_t target) { return ee::tailCall<int32_t>(sym::setEditCameraTarget, camera, target); }
        uint32_t findWorldNode(uint32_t name) { return ee::Address(zge::findWorldNodeByHash(zge::nameHash(ee::Ptr<const char>(name)), 0, 1)); }
    }

    int32_t registerEditVacuumToolClass()
    {
        return game::tail(registerEditClass, kEditVacuumToolClass, sizeof(EditVacuumToolPart), kEditToolClass, sym::constructEditVacuumTool, sym::destructEditVacuumTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditVacuumToolClass, Verified);

    // The crane's nodes, and their local matrices as they stand, to be put back
    // when the tool goes.
    void constructEditVacuumTool(uint32_t data)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        const ee::Ptr<EditVacuumToolPart> vacuum(editObjectPart(data, kEditVacuumToolClass));
        setEditToolMethods(tool, sym::g_vacuumToolMethods);
        setEditToolName(tool, sym::g_textVacuumTool);
        vacuum->craneNodeB2 = findWorldNode(sym::g_textTerrainHangerCraneB002);
        vacuum->craneNodeB1 = findWorldNode(sym::g_textTerrainHangerCraneB001);
        vacuum->vacuumNodes[0] = findWorldNode(sym::g_textTerrainHangerVac001);
        vacuum->vacuumNodes[1] = findWorldNode(sym::g_textTerrainHangerVac002);
        vacuum->vacuumNodes[2] = findWorldNode(sym::g_textTerrainHangerVac003);
        vacuum->vacuumNodes[3] = findWorldNode(sym::g_textTerrainHangerVac004);
        vacuum->vacuumNodes[4] = findWorldNode(sym::g_textTerrainHangerVac005);
        vacuum->vacuumNodes[5] = findWorldNode(sym::g_textTerrainHangerVac006);
        vacuum->craneB2RestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB2), 0);
        vacuum->craneB1RestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB1), 0);
        for (int i = 0; i < kVacuumHeads; ++i)
            vacuum->vacuumRestMatrices[i] = *zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->vacuumNodes[i]), 0);
    }
    READABLE_FUNCTION_WAITS(constructEditVacuumTool, Verified);

    void destructEditVacuumTool(uint32_t data)
    {
        const ee::Ptr<EditVacuumToolPart> vacuum(editObjectPart(data, kEditVacuumToolClass));
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB2), &vacuum->craneB2RestMatrix);
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB1), &vacuum->craneB1RestMatrix);
        for (int i = 0; i < kVacuumHeads; ++i)
            zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->vacuumNodes[i]), &vacuum->vacuumRestMatrices[i]);
    }
    READABLE_FUNCTION_WAITS(destructEditVacuumTool, Verified);

    int32_t vacuumToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditVacuumToolPart> vacuum(editObjectPart(tool, kEditVacuumToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        pushUiButtonPage(ui);
        setCameraButtons(tool->toolSet);
        setUiButton(ui, 12, kOnPressOrHold, sym::vacuumToolMoveLeft, vacuum, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::vacuumToolMoveRight, vacuum, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::vacuumToolMoveForward, vacuum, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::vacuumToolMoveBack, vacuum, 10, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::vacuumToolPickOrRelease, vacuum, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::vacuumToolRotate, vacuum, 1, noDeadzone);
        setUiButton(ui, 0, kOnPress, sym::vacuumToolPause, vacuum, 1, noDeadzone);
        setInfoButtonText(ui, 1, sym::g_textPick2);
        setInfoButtonText(ui, 2, sym::g_textRotate4);
        setInfoButtonText(ui, 3, sym::g_textNa5);
        setInfoButtonText(ui, 4, sym::g_textNa5);
        setInfoButtonText(ui, 5, sym::g_textMove5);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textVacuumTool2));
        setInfoHelpText(ui, sym::g_textMovePiecesUsingTheVacuum);
        return setEditCameraTargetKeepingResult(tool->editCamera, selectionTarget(tool->selection));
    }
    READABLE_FUNCTION_WAITS(vacuumToolActivate, Verified);

    // Each frame the beam goes to the selection's z, and the trolley and the
    // heads to its x and z.
    void vacuumToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float /*seconds*/)
    {
        const ee::Ptr<EditVacuumToolPart> vacuum(editObjectPart(tool, kEditVacuumToolClass));
        ee::Local<zge::Vec4> selected;
        updateSelectionCursor(tool->selection); // the frame's seconds go along too, unread
        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(selected));
        const ee::Ptr<zge::Matrix> beam = zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB2), 1);
        beam->translation().z = selected->z;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB2), beam);
        const ee::Ptr<zge::Matrix> trolley = zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB1), 1);
        trolley->translation().x = selected->x;
        trolley->translation().z = selected->z;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->craneNodeB1), trolley);
        for (int i = 0; i < kVacuumHeads; ++i)
        {
            const ee::Ptr<zge::Matrix> head = zge::nodeMatrix(ee::Ptr<zge::Node>(vacuum->vacuumNodes[i]), 1);
            head->translation().x = selected->x;
            head->translation().z = selected->z;
            zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(vacuum->vacuumNodes[i]), head);
        }
    }
    READABLE_FUNCTION_WAITS(vacuumToolUpdate, Verified);

    int32_t vacuumToolDraw(ee::Ptr<EditToolPart> tool)
    {
        return game::tail(drawSelectionInColour, tool->selection);
    }
    READABLE_FUNCTION_WAITS(vacuumToolDraw, Verified);

    void vacuumToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 5);
        clearUiButton(ui, 0);
        clearCameraButtons(tool->toolSet);
        game::tail(popUiButtonPage, ui);
    }
    READABLE_FUNCTION_WAITS(vacuumToolDeactivate, Verified);

    // One cell of the selection along a heading (degrees) from the camera's.
    int32_t vacuumToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec3> step;
        headingStep(ee::Ptr<zge::Vec3>(step), editCameraHeading(tool->editCamera) + heading);
        return moveSelection(tool->selection, step.address());
    }
    READABLE_FUNCTION_WAITS(vacuumToolMove, Verified);

    // The move buttons: their row's data is the vacuum part.
    int32_t vacuumToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(vacuumToolMove, tool, ee::Float(90.0f));
    }
    READABLE_FUNCTION_WAITS(vacuumToolMoveLeft, Verified);

    int32_t vacuumToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(vacuumToolMove, tool, ee::Float(-90.0f));
    }
    READABLE_FUNCTION_WAITS(vacuumToolMoveRight, Verified);

    int32_t vacuumToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(vacuumToolMove, tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(vacuumToolMoveForward, Verified);

    int32_t vacuumToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(vacuumToolMove, tool, ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(vacuumToolMoveBack, Verified);

    // With nothing held, the pieces at the selection are lifted. With something
    // held it is set down and placed; if it cannot be placed there, it is lifted
    // again and stays held.
    int32_t vacuumToolPickOrRelease(ee::Ptr<EditUiButton> button)
    {
        ee::Local<CellIndex> cell;
        ee::Local<zge::Vec4> down;
        ee::Local<zge::Vec4> lifted;
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        const ee::Ptr<EditObjectPart> held = firstHeldPiece(tool->selection);
        if (held != 0)
        {
            down->x = ee::Float(0.0f);
            down->y = ee::Float(0.0f);
            down->z = ee::Float(0.0f);
            setEditObjectPosition(held, down);
            cursorCell(tool->selection, ee::Ptr<CellIndex>(cell));
            if (placeHeldPieces(tool->selection) != 0)
            {
                clearSelection(tool->selection);
                clearSelectionRotation(tool->selection);
            }
            else
            {
                down->x = ee::Float(0.0f);
                down->y = kHeldLift;
                down->z = ee::Float(0.0f);
                setEditObjectPosition(held, down);
            }
            return setInfoButtonText(button->ui, 1, sym::g_textPick2);
        }
        clearSelectionRotation(tool->selection);
        pickUpSelectedPieces(tool->selection);
        const ee::Ptr<EditObjectPart> picked = firstHeldPiece(tool->selection);
        if (picked != 0)
        {
            lifted->x = ee::Float(0.0f);
            lifted->y = kHeldLift;
            lifted->z = ee::Float(0.0f);
            setEditObjectPosition(picked, lifted);
        }
        return setInfoButtonText(button->ui, 1, sym::g_textRelease2);
    }
    READABLE_FUNCTION_WAITS(vacuumToolPickOrRelease, Verified);

    // A quarter turn about y, when anything is selected.
    int32_t vacuumToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        const int32_t empty = selectionEmpty(tool->selection);
        if (empty != 0)
            return empty;
        ee::Local<zge::EulerAngles> turn;
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        return static_cast<int32_t>(rotateSelection(tool->selection, turn.address()));
    }
    READABLE_FUNCTION_WAITS(vacuumToolRotate, Verified);

    int32_t vacuumToolPause(ee::Ptr<EditUiButton> button)
    {
        return game::tail(playInGameEditPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(vacuumToolPause, Verified);
}
