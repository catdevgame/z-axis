// DM2's park editor: the view (edit_camera.h). DM2's own code, after
// ./src/com/editor/editGui.c in its build.

#include "edit_camera.h"

#include "editor_state.h"
#include "dm2/camera_far_clip.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"

#include <cstddef>

#include <cstdint>
#include "sym/dm2/editor/edit_camera.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Float kPitchLowest = 5.0f;
        constexpr ee::Float kPitchHighest = 85.0f;
        constexpr ee::Float kHalfTurn = 180.0f;
        constexpr ee::Float kFullTurn = 360.0f;
        constexpr ee::Float kDistanceNearest = 15.0f;
        constexpr ee::Float kDistanceFarthest = 100.0f;
    }

    void tiltEditView(ee::Ptr<ParkEditor> editor, ee::Float change)
    {
        const ee::Float pitch = editor->viewPitch + change;
        editor->viewPitch = pitch;
        if (pitch < kPitchLowest)
            editor->viewPitch = kPitchLowest;
        else if (kPitchHighest < pitch)
            editor->viewPitch = kPitchHighest;
        editor->viewChanged = 1;
    }
    READABLE_FUNCTION(tiltEditView, Verified);

    void turnEditView(ee::Ptr<ParkEditor> editor, ee::Float change)
    {
        const ee::Float heading = editor->viewHeading + change;
        editor->viewHeading = heading;
        if (kHalfTurn < heading)
            editor->viewHeading = heading - kFullTurn;
        if (editor->viewHeading < -kHalfTurn)
            editor->viewHeading = editor->viewHeading + kFullTurn;
        editor->viewChanged = 1;
    }
    READABLE_FUNCTION(turnEditView, Verified);

    void zoomEditView(ee::Ptr<ParkEditor> editor, ee::Float change)
    {
        const ee::Float distance = editor->viewDistance + change;
        editor->viewDistance = distance;
        if (distance < kDistanceNearest)
            editor->viewDistance = kDistanceNearest;
        else if (kDistanceFarthest < distance)
            editor->viewDistance = kDistanceFarthest;
        editor->viewChanged = 1;
    }
    READABLE_FUNCTION(zoomEditView, Verified);

    namespace
    {
        // The menus' scene camera, shared with the game's menus.
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_menuSceneCamera> g_menuSceneCamera;
        // Where the eye starts before the first frame works it out.
        inline ee::Global<zge::Vec4, sym::editViewStartEye> g_startEye;
        // A degree in the engine's angles (65536 a turn).
        inline ee::Global<ee::Float, sym::editViewAngleADegree> g_angleADegree;
        // The target stands under the ground by this much of the distance.
        inline ee::Global<uint64_t, sym::editViewTargetDrop> g_targetDrop;

        constexpr ee::Float kParkDrawDistance = 500.0f;
        constexpr ee::Float kStartDistance = 40.0f;
        constexpr ee::Float kStartPitch = 60.0f;
        constexpr ee::Float kCellSize = 5.0f;
        constexpr ee::Float kHalfCell = 2.5f;
        // The looked-at cell eases this many cells a second for every cell
        // it is away (one at least); its ground this many units a second.
        constexpr ee::Float kCellEaseRate = 5.0f;
        constexpr ee::Float kGroundEaseRate = 50.0f;
        constexpr ee::Float kLeastEaseGap = 1.0f;
        // 3.0 as a double: the target's further drop.
        constexpr uint64_t kTargetSink = 0x4008000000000000ull;
        constexpr uint32_t kNewCameraFlags = 1;

        // Scaffolding (address-call): libc's memset and the engine's sine
        // and cosine tables are called where they are.
        void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game(sym::memset, destination, byte, bytes); }
        ee::Float sinTurnOf(int32_t angle) { return ee::game<ee::Float>(sym::sinTurnOf, angle); }
        ee::Float cosTurnOf(int32_t angle) { return ee::game<ee::Float>(sym::cosTurnOf, angle); }

        int32_t angleOf(ee::Float degrees)
        {
            return static_cast<int16_t>(ee::toInt(degrees * *g_angleADegree));
        }

        // An unsigned number as a float, the way the compiler wrote it.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1) | (value >> 1)));
            return half + half;
        }

        // A cell number of the view one frame nearer the cursor's; 1 if it
        // moved.
        bool easeCell(ee::Float &current, int32_t cell, ee::Float seconds)
        {
            const ee::Float goal = unsignedToFloat(static_cast<uint32_t>(cell));
            const ee::Float from = current;
            if (from == goal)
                return false;
            ee::Float gap = ee::abs(from - goal);
            if (seconds * kCellEaseRate < gap)
            {
                if (gap < kLeastEaseGap)
                    gap = kLeastEaseGap;
                const ee::Float step = seconds * (gap * kCellEaseRate);
                current = goal < from ? from - step : from + step;
            }
            else
            {
                current = goal;
            }
            return true;
        }

        struct SetUpViewFrame
        {
            zge::Vec4 startEye; // +0x00
            zge::Vec4 zero;     // +0x10
        };
        // Scaffolding (frame-exact): the copies of the target and of the
        // looked-at cell updateEditView's compiler left in its frame.
        struct UpdateViewFrame
        {
            zge::Vec4 keptTarget;                 // +0x00
            ee::Float keptColumn, keptGround, keptRow; // +0x10
            uint32_t keptCellW;
        };
        static_assert(offsetof(UpdateViewFrame, keptColumn) == 0x10);
    }

    void setUpEditView(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<SetUpViewFrame> frame;
        frame->startEye = *g_startEye;
        memset(&frame->zero, 0, sizeof(zge::Vec4));
        const ee::Ptr<ParkCell> cell = parkCell(editor, editor->cursorColumn, editor->cursorRow);
        if (*g_menuSceneCamera)
        {
            editor->viewCamera = *g_menuSceneCamera;
            setSceneFarClip(*g_menuSceneCamera, kParkDrawDistance);
        }
        else
        {
            const ee::Ptr<zge::SceneObject> camera(zge::newSceneCamera(kNewCameraFlags));
            editor->viewCamera = camera;
            setSceneFarClip(camera, kParkDrawDistance);
            *g_menuSceneCamera = editor->viewCamera;
        }
        editor->viewEye = frame->startEye;
        editor->viewTarget = frame->zero;
        editor->viewDistance = kStartDistance;
        editor->viewPitch = kStartPitch;
        editor->viewColumn = ee::toFloat(editor->cursorColumn);
        editor->viewRow = ee::toFloat(editor->cursorRow);
        editor->viewHeading = 0.0f;
        editor->startDistance = kStartDistance;
        editor->viewChanged = 0;
        editor->viewGround = -ee::toFloat(cell->level);
    }
    READABLE_FUNCTION(setUpEditView, Verified);

    void endEditView(ee::Ptr<ParkEditor> editor, uint32_t keepCamera)
    {
        ee::Local<zge::Vec4> zero;
        if (keepCamera)
        {
            memset(zero, 0, sizeof(zge::Vec4));
            editor->viewEye = *zero;
            editor->viewTarget = *zero;
            return;
        }
        zge::releaseSceneCamera(editor->viewCamera);
        *g_menuSceneCamera = ee::Ptr<zge::SceneObject>();
    }
    READABLE_FUNCTION(endEditView, Verified);

    void updateEditView(ee::Ptr<ParkEditor> editor, ee::Float seconds)
    {
        ee::Local<UpdateViewFrame> frame;
        int32_t column, row;
        if (editor->selectionFollowing)
        {
            column = editor->selectionColumn;
            row = editor->selectionRow;
        }
        else if (editTool() != kEditToolTerrain)
        {
            column = editor->unsnappedColumn;
            row = editor->unsnappedRow;
        }
        else
        {
            // The middle of the selection (the halves round towards 0).
            column = editor->unsnappedColumn + editor->selectionColumns / 2;
            row = editor->unsnappedRow + editor->selectionRows / 2;
        }
        const ee::Float ground = -ee::toFloat(parkCell(editor, column, row)->level);
        frame->keptTarget = editor->viewTarget;
        frame->keptColumn = editor->viewColumn;
        frame->keptGround = editor->viewGround;
        frame->keptRow = editor->viewRow;
        frame->keptCellW = editor->viewCellW;

        if (easeCell(editor->viewColumn, column, seconds))
            editor->viewChanged = 1;
        if (easeCell(editor->viewRow, row, seconds))
            editor->viewChanged = 1;
        const ee::Float groundNow = editor->viewGround;
        if (!(groundNow == ground))
        {
            const ee::Float step = seconds * kGroundEaseRate;
            if (step < ee::abs(groundNow - ground))
                editor->viewGround = ground < groundNow ? groundNow - step : groundNow + step;
            else
                editor->viewGround = ground;
            editor->viewChanged = 1;
        }

        // The target: the looked-at cell's centre, under its ground by
        // 3 and a part of the distance (worked out in doubles).
        const ee::Float columnsHalf = ee::toFloat(editor->columns) * kHalfCell;
        const ee::Float rowsHalf = ee::toFloat(editor->rows) * kHalfCell;
        editor->viewTarget.x = editor->viewColumn * kCellSize - columnsHalf + kHalfCell;
        editor->viewTarget.z = editor->viewRow * kCellSize - rowsHalf + kHalfCell;
        const uint64_t groundDouble = sce::libgcc::fptodp(editor->viewGround);
        const uint64_t drop = sce::libgcc::dpmul(sce::libgcc::fptodp(editor->viewDistance), *g_targetDrop);
        editor->viewTarget.y = sce::libgcc::dptofp(sce::libgcc::dpsub(sce::libgcc::dpsub(groundDouble, drop), kTargetSink));

        // The eye: from the target, the distance out along the pitch and
        // the heading.
        editor->viewEye = editor->viewTarget;
        const ee::Float pitchSine = sinTurnOf(angleOf(editor->viewPitch));
        const ee::Float headingSine = sinTurnOf(angleOf(editor->viewHeading));
        editor->viewEye.x = editor->viewEye.x + editor->viewDistance * pitchSine * headingSine;
        const ee::Float pitchCosine = cosTurnOf(angleOf(editor->viewPitch));
        editor->viewEye.y = editor->viewEye.y - editor->viewDistance * pitchCosine;
        const ee::Float pitchSineAgain = sinTurnOf(angleOf(editor->viewPitch));
        const ee::Float headingCosine = cosTurnOf(angleOf(editor->viewHeading));
        editor->viewEye.z = editor->viewEye.z + editor->viewDistance * pitchSineAgain * headingCosine;

        // The eye stays over the park and one cell around it; held back,
        // the distance becomes what is left.
        const ee::Float columnsHalfNow = ee::toFloat(editor->columns) * kHalfCell;
        const ee::Float rowsHalfNow = ee::toFloat(editor->rows) * kHalfCell;
        const ee::Float leastX = -kCellSize - columnsHalfNow + kHalfCell;
        const ee::Float mostX = ee::toFloat(editor->columns + 1) * kCellSize - columnsHalfNow + kHalfCell;
        const ee::Float mostZ = ee::toFloat(editor->rows + 1) * kCellSize - rowsHalfNow + kHalfCell;
        const ee::Float leastZ = -kCellSize - rowsHalfNow + kHalfCell;
        bool heldBack = false;
        const ee::Float x = editor->viewEye.x;
        if (x < leastX)
        {
            editor->viewEye.x = leastX;
            heldBack = true;
        }
        else if (mostX < x)
        {
            editor->viewEye.x = mostX;
            heldBack = true;
        }
        const ee::Float z = editor->viewEye.z;
        if (z < leastZ)
        {
            editor->viewEye.z = leastZ;
            heldBack = true;
        }
        else if (mostZ < z)
        {
            editor->viewEye.z = mostZ;
            heldBack = true;
        }
        if (heldBack)
        {
            const ee::Float dx = editor->viewEye.x - editor->viewTarget.x;
            const ee::Float dy = editor->viewEye.y - editor->viewTarget.y;
            const ee::Float dz = editor->viewEye.z - editor->viewTarget.z;
            editor->viewDistance = ee::sqrt(dx * dx + dy * dy + dz * dz);
        }
        zge::lookFromAt(editor->viewCamera, &editor->viewEye, &editor->viewTarget, 0);
    }
    READABLE_FUNCTION(updateEditView, Verified);
}
