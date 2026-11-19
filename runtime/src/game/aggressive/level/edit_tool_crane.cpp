// The park editor's crane tool: the level's crane ("Terrain_Crane", its arm,
// two bolts and its chain) turns and reaches to stand over the selection, picks
// the pieces there up on its hook, and puts them down again, or drops them in a
// corner of the park to be destroyed.

#include "aggressive/level/edit_tool_crane.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/edit_tools.h"
#include "aggressive/level/park_selection.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include "zge/render/primitives.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_crane.cpp.h"

namespace aggressive::level
{
    using namespace zge::editor; // (the park editor this part of the game drives)

    namespace
    {
        constexpr uint32_t kEditCraneToolClass = sym::g_editCraneToolName; // "EditCraneTool"
        constexpr uint32_t kEditToolClass = sym::g_craneToolEditToolName;  // "EditTool"

        // A button row's states (park_ui.cpp): pressed, or pressed and held.
        constexpr uint32_t kOnPress = 1, kOnPressOrHold = 3;

        // playToolSound's sounds.
        constexpr uint32_t kCraneMoveSound = 142, kCranePickSound = 143, kCraneReleaseSound = 146;

        // One park cell, in metres.
        constexpr ee::Float kCell = ee::Float(4.57f);
        // The chain hangs this far above the selection.
        constexpr ee::Float kChainLift = ee::Float(-10.19676f);
        // A piece picked up hangs under the hook: its height less this, and these.
        constexpr ee::Float kHookDrop = ee::Float(4.572f), kHookGap = ee::Float(1.0f), kHookEye = ee::Float(0.85f);

        // What drawLines takes: a material and two vertices.
        struct LineVertex
        {
            zge::Vec4 position;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(LineVertex) == 0x20);
        struct LineRecord
        {
            uint32_t material;
            ee::Ptr<LineVertex> first, second;
            uint32_t _0c;
            LineVertex vertices[2]; // +0x10
        };
        static_assert(sizeof(LineRecord) == 0x50 && offsetof(LineRecord, vertices) == 0x10);
        constexpr uint32_t kWorldLines = 0x10; // drawLines' kind: lines in the world

        inline ee::Global<int32_t, sym::g_uiDefaultMaterial> g_uiDefaultMaterial;

        uint32_t findWorldNode(uint32_t name) { return ee::Address(zge::findWorldNodeByHash(zge::nameHash(ee::Ptr<const char>(name)), 0, 1)); }
    }

    int32_t registerEditCraneToolClass()
    {
        return game::tail(registerEditClass, kEditCraneToolClass, sizeof(EditCraneToolPart), kEditToolClass, sym::constructEditCraneTool, sym::destructEditCraneTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditCraneToolClass, Verified);

    // The crane's nodes, and their local matrices as they stand, to be put back
    // when the tool goes.
    void constructEditCraneTool(uint32_t data)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        const ee::Ptr<EditCraneToolPart> crane(editObjectPart(data, kEditCraneToolClass));
        setEditToolMethods(tool, sym::g_craneToolMethods);
        setEditToolName(tool, sym::g_textCraneTool);
        crane->craneNode = findWorldNode(sym::g_textTerrainCrane);
        crane->armNode = findWorldNode(sym::g_textTerrainCranearmA);
        crane->boltANode = findWorldNode(sym::g_textTerrainCraneBoltA);
        crane->boltBNode = findWorldNode(sym::g_textTerrainCraneBoltB);
        crane->chainNode = findWorldNode(sym::g_textTerrainCranechain);
        crane->armRestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->armNode), 0);
        crane->craneRestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->craneNode), 0);
        crane->boltARestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->boltANode), 0);
        crane->boltBRestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->boltBNode), 0);
        crane->chainRestMatrix = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->chainNode), 0);
    }
    READABLE_FUNCTION_WAITS(constructEditCraneTool, Verified);

    void destructEditCraneTool(uint32_t data)
    {
        const ee::Ptr<EditCraneToolPart> crane(editObjectPart(data, kEditCraneToolClass));
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->armNode), &crane->armRestMatrix);
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->craneNode), &crane->craneRestMatrix);
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->boltANode), &crane->boltARestMatrix);
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->boltBNode), &crane->boltBRestMatrix);
        game::tail(zge::setNodeLocalMatrix, ee::Ptr<zge::Node>(crane->chainNode), &crane->chainRestMatrix);
    }
    READABLE_FUNCTION_WAITS(destructEditCraneTool, Verified);

    // Picked: the pad moves the selection, a button picks up or puts down, one
    // turns what is held; the crane's rope is hidden while the tool is on.
    int32_t craneToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const ee::Ptr<EditCraneToolPart> crane(editObjectPart(tool, kEditCraneToolClass));
        const ee::Float noDeadzone = ee::Float(0.0f);
        pushUiButtonPage(ui);
        setCameraButtons(tool->toolSet);
        setUiButton(ui, 12, kOnPressOrHold, sym::craneToolMoveLeft, crane, 10, noDeadzone);
        setUiButton(ui, 13, kOnPressOrHold, sym::craneToolMoveRight, crane, 10, noDeadzone);
        setUiButton(ui, 14, kOnPressOrHold, sym::craneToolMoveForward, crane, 10, noDeadzone);
        setUiButton(ui, 15, kOnPressOrHold, sym::craneToolMoveBack, crane, 10, noDeadzone);
        setUiButton(ui, 5, kOnPress, sym::craneToolPickOrRelease, crane, 1, noDeadzone);
        setUiButton(ui, 4, kOnPress, sym::craneToolRotate, crane, 1, noDeadzone);
        setUiButton(ui, 0, kOnPress, sym::craneToolPause, crane, 1, noDeadzone);
        setEditCameraTarget(tool->editCamera, ee::Ptr<const zge::Vec4>(selectionTarget(tool->selection)));
        zge::setNodeShown(ee::Ptr<zge::Node>(findWorldNode(sym::g_textTerrainCranerope)), 0, 1);
        setInfoButtonText(ui, 1, sym::g_textPick);
        setInfoButtonText(ui, 2, sym::g_textNa2);
        setInfoButtonText(ui, 3, sym::g_textNa2);
        setInfoButtonText(ui, 4, sym::g_textNa2);
        setInfoButtonText(ui, 5, sym::g_textMove4);
        setObjectSelectionText(ui, ee::Ptr<const char>(sym::g_textCraneTool2));
        setInfoHelpText(ui, sym::g_textUseTheCraneToDestroyPieces);
        return game::tail(showEditHelpPage, ui, 0);
    }
    READABLE_FUNCTION_WAITS(craneToolActivate, Verified);

    // Each frame the crane turns about y to face the selection, its arm tilts to
    // reach it (straight out when it is out of reach), and the chain hangs over it.
    void craneToolUpdate(ee::Ptr<EditToolPart> tool, ee::Float /*seconds*/)
    {
        const ee::Ptr<EditCraneToolPart> crane(editObjectPart(tool, kEditCraneToolClass));
        ee::Local<zge::Vec4> selected;
        ee::Local<zge::Matrix> turned;
        ee::Local<zge::Vec4> arm;

        updateSelectionCursor(tool->selection); // the frame's seconds go along too, unread
        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(selected));
        const ee::Ptr<zge::Matrix> craneWorld = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->craneNode), 1);
        *selected = *selected - craneWorld->translation();
        selected->y = ee::Float(0.0f);
        const ee::Float distance = zge::vec3Length(&selected->xyz());
        zge::Angle heading = zge::angleAcos(selected->x / distance);
        if (ee::Float(0.0f) < selected->z)
            heading = static_cast<zge::Angle>(-heading);
        heading = static_cast<zge::Angle>(heading - 0x8000);
        zge::matrixIdentity(turned);
        zge::matrixRotateY(turned, turned, heading);
        turned->translation() = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->craneNode), 0)->translation();
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->craneNode), turned);

        const ee::Ptr<zge::Matrix> armWorld = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->armNode), 1);
        const ee::Ptr<zge::Matrix> boltWorld = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->boltANode), 1);
        const ee::Ptr<zge::Matrix> chainWorld = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->chainNode), 1);
        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(selected));
        *selected = *selected - armWorld->translation();
        selected->y = ee::Float(0.0f);
        const ee::Float reach = zge::vec3Length(&selected->xyz());
        *arm = boltWorld->translation() - armWorld->translation();
        const ee::Float armLength = zge::vec3Length(&arm->xyz());
        zge::Angle tilt = 0;
        if (!(armLength < reach))
            tilt = zge::angleAcos(reach / armLength);
        const ee::Ptr<zge::Matrix> armLocal = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->armNode), 0);
        zge::matrixIdentity(turned);
        zge::matrixRotateZ(turned, turned, tilt);
        turned->translation() = armLocal->translation();
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->armNode), turned);

        selectionPosition(tool->selection, ee::Ptr<zge::Vec4>(selected));
        firstHeldPiece(tool->selection); // its answer is not used
        zge::Vec4 &chainAt = chainWorld->translation();
        chainAt.x = selected->x;
        chainAt.y = selected->y + kChainLift;
        chainAt.z = selected->z;
        *turned = *zge::nodeMatrix(ee::Ptr<zge::Node>(crane->chainNode), 1);
        turned->translation() = chainAt;
        zge::setNodeLocalMatrix(ee::Ptr<zge::Node>(crane->chainNode), turned);
    }
    READABLE_FUNCTION_WAITS(craneToolUpdate, Verified);

    // The cable, black: from the chain to bolt A, and from bolt B to bolt A; then
    // the selection, unless the crane holds something.
    int32_t craneToolDraw(ee::Ptr<EditToolPart> tool)
    {
        const ee::Ptr<EditCraneToolPart> crane(editObjectPart(tool, kEditCraneToolClass));
        const ee::Ptr<zge::Matrix> boltA = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->boltANode), 1);
        const ee::Ptr<zge::Matrix> boltB = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->boltBNode), 1);
        const ee::Ptr<zge::Matrix> chain = zge::nodeMatrix(ee::Ptr<zge::Node>(crane->chainNode), 1);
        ee::Local<LineRecord> line;
        line->first = &line->vertices[0];
        line->material = *g_uiDefaultMaterial;
        line->second = &line->vertices[1];
        for (LineVertex &vertex : line->vertices)
        {
            vertex.colour[0] = 0;
            vertex.colour[1] = 0;
            vertex.colour[2] = 0;
            vertex.colour[3] = 255;
        }
        line->vertices[0].position = chain->translation();
        line->vertices[1].position = boltA->translation();
        zge::drawLines(kWorldLines, line.address(), 1, 0);
        line->vertices[0].position = boltB->translation();
        zge::drawLines(kWorldLines, line.address(), 1, 0);
        const ee::Address held = firstHeldPiece(tool->selection);
        if (held != 0)
            return held;
        return drawSelectionInColour(tool->selection);
    }
    READABLE_FUNCTION_WAITS(craneToolDraw, Verified);

    // One cell along a heading (degrees) from the camera's. A cursor that ends on
    // the cell diagonally in from a corner of the park steps once more, diagonally
    // into the corner (what it holds is dropped there: craneToolPickOrRelease).
    int32_t craneToolMove(ee::Ptr<EditToolPart> tool, ee::Float heading)
    {
        ee::Local<zge::Vec4> step;
        ee::Local<CellIndex> cell;
        headingStep(ee::Ptr<zge::Vec3>(&step->xyz()), editCameraHeading(tool->editCamera) + heading);
        moveSelection(tool->selection, step.address());
        playToolSound(tool, kCraneMoveSound);
        cursorCell(tool->selection, ee::Ptr<CellIndex>(cell));
        const int32_t lastX = static_cast<int32_t>(parkWidth(toolSetPark(tool->toolSet))) - 2;
        const int32_t lastZ = static_cast<int32_t>(parkDepth(toolSetPark(tool->toolSet))) - 2;
        const auto stepInto = [&](ee::Float x, ee::Float z)
        {
            step->x = x;
            step->z = z;
            step->y = ee::Float(0.0f);
            return moveSelection(tool->selection, step.address());
        };
        if (cell->x == 1)
        {
            if (cell->z == 1)
                return stepInto(-kCell, -kCell);
            if (cell->z == lastZ)
                return stepInto(-kCell, kCell);
        }
        if (cell->x != lastX)
            return lastX;
        if (cell->z == lastZ)
            return stepInto(kCell, kCell);
        if (cell->z == 1)
            return stepInto(kCell, -kCell);
        return 1;
    }
    READABLE_FUNCTION_WAITS(craneToolMove, Verified);

    // The move buttons: their row's data is the crane part.
    int32_t craneToolMoveLeft(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(craneToolMove, tool, ee::Float(90.0f));
    }
    READABLE_FUNCTION_WAITS(craneToolMoveLeft, Verified);

    int32_t craneToolMoveRight(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(craneToolMove, tool, ee::Float(-90.0f));
    }
    READABLE_FUNCTION_WAITS(craneToolMoveRight, Verified);

    int32_t craneToolMoveForward(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(craneToolMove, tool, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(craneToolMoveForward, Verified);

    int32_t craneToolMoveBack(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        return game::tail(craneToolMove, tool, ee::Float(180.0f));
    }
    READABLE_FUNCTION_WAITS(craneToolMoveBack, Verified);

    // With nothing held, the pieces at the selection go up on the hook, keeping
    // their turn, and hang under it. With something held it comes down: in one
    // of the park's four corner cells the crusher there ("ScriptObj_ParkEditor")
    // hears ParkEditDestroyObject and what was held is not placed; elsewhere it is
    // placed, if it fits there.
    int32_t craneToolPickOrRelease(ee::Ptr<EditUiButton> button)
    {
        ee::Local<CellIndex> cell;
        ee::Local<zge::Vec4> offset;
        ee::Local<AngleWords> angles;
        ee::Local<zge::Box> box;
        ee::Local<uint32_t> names;
        ee::Local<zge::EngineMessage> message;

        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        const uint32_t park = static_cast<uint32_t>(toolSetPark(tool->toolSet));
        const int32_t width = static_cast<int32_t>(parkWidth(park));
        const int32_t depth = static_cast<int32_t>(parkDepth(park));
        if (firstHeldPiece(tool->selection) != 0)
        {
            if (canPlaceHeldPieces(tool->selection) == 0)
                return 0;
            offset->x = ee::Float(0.0f);
            offset->y = ee::Float(0.0f);
            offset->z = ee::Float(0.0f);
            setSelectionOffset(tool->selection, ee::Ptr<zge::Vec4>(offset));
            cursorCell(tool->selection, ee::Ptr<CellIndex>(cell));
            const bool cornerX = cell->x == 0 || cell->x == width - 1;
            const bool cornerZ = cell->z == 0 || cell->z == depth - 1;
            if (cornerX && cornerZ)
            {
                *names = zge::nameHash(ee::Ptr<const char>(sym::g_textScriptObjParkEditor));
                const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(names, 1);
                if (found && *found)
                {
                    message->target = ee::Address(*found); // the message's word
                    message->sender = 0;
                    message->type = zge::nameHash(ee::Ptr<const char>(sym::g_textParkEditDestroyObject));
                    message->flags = zge::kMessageNow;
                    message->data = 0;
                    zge::sendEngineMessage(message);
                }
            }
            else
            {
                placeHeldPieces(tool->selection);
                playToolSound(tool, kCraneReleaseSound);
            }
            clearSelection(tool->selection);
            clearSelectionRotation(tool->selection);
            setInfoButtonText(button->ui, 1, sym::g_textPick);
            return setInfoButtonText(button->ui, 2, sym::g_textNa2);
        }

        cursorCellAngles(tool->selection, ee::Ptr<AngleWords>(angles));
        pickUpSelectedPieces(tool->selection);
        clearSelectionRotation(tool->selection);
        rotateSelection(tool->selection, angles.address());
        const ee::Ptr<EditObjectPart> held = firstHeldPiece(tool->selection);
        if (!held)
            return 0;
        editDisplayBox(editObjectDisplay(held), ee::Ptr<zge::Box>(box));
        const ee::Float height = box->high.y - box->low.y;
        offset->x = ee::Float(0.0f);
        offset->z = ee::Float(0.0f);
        offset->y = -(kHookDrop - height + kHookGap + kHookEye);
        setSelectionOffset(tool->selection, ee::Ptr<zge::Vec4>(offset));
        playToolSound(button->data, kCranePickSound);
        setInfoButtonText(button->ui, 1, sym::g_textRelease);
        return setInfoButtonText(button->ui, 2, sym::g_textRotate3);
    }
    READABLE_FUNCTION_WAITS(craneToolPickOrRelease, Verified);

    // A quarter turn about y of what is held.
    int32_t craneToolRotate(ee::Ptr<EditUiButton> button)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(button->data, kEditToolClass));
        ee::Local<zge::EulerAngles> turn;
        turn->x = 0;
        turn->y = 0x4000;
        turn->z = 0;
        return static_cast<int32_t>(rotateSelection(tool->selection, turn.address()));
    }
    READABLE_FUNCTION_WAITS(craneToolRotate, Verified);

    // Put away: the buttons go, and the rope shows again.
    int32_t craneToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        clearUiButton(ui, 12);
        clearUiButton(ui, 13);
        clearUiButton(ui, 14);
        clearUiButton(ui, 15);
        clearUiButton(ui, 5);
        clearUiButton(ui, 0);
        clearCameraButtons(tool->toolSet);
        popUiButtonPage(ui);
        zge::setNodeShown(ee::Ptr<zge::Node>(findWorldNode(sym::g_textTerrainCranerope)), 1, 1);
        return game::tail(hideEditHelpPage, ui, 0);
    }
    READABLE_FUNCTION_WAITS(craneToolDeactivate, Verified);

    int32_t craneToolPause(ee::Ptr<EditUiButton> button)
    {
        return game::tail(playInGameEditPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(craneToolPause, Verified);
}
