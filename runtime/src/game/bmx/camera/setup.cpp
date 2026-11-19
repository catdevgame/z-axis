// BMX XXX's action cameras (action_camera.h): a level's set-up, and the idle
// mode's view from the level's "idle" camera points
// (./src/com/action/camera.c).

#include "action_camera.h"

#include "bmx/game/level_globals.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/tasks.h"
#include "zge/render/material.h"
#include "zge/scene/locators.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/camera/setup.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        inline ee::Global<ActionCamera[kActionCameraCount], sym::g_actionCameras> g_actionCameras;
        inline ee::Global<uint8_t, sym::g_shownCameraCount> g_shownCameraCount;
        inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
        inline ee::Global<ee::Ptr<Task>, sym::g_cameraTask> g_cameraTask;
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraShadeMaterial> g_cameraShadeMaterial;
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraTintMaterial> g_cameraTintMaterial;
        inline ee::Global<uint8_t[4], sym::g_cameraClearColour> g_cameraClearColour;
        inline ee::Global<uint8_t[4], sym::g_cameraFogColour> g_cameraFogColour;
        inline ee::Global<ee::Float, sym::g_cameraFogDepth> g_cameraFogDepth;
        inline ee::Global<ee::Float, sym::g_cameraFarClip> g_cameraFarClip;
        // How many "idle" camera points the level has, and the one the
        // idle mode looks from.
        inline ee::Global<uint8_t, sym::g_idlePointCount> g_idlePointCount;
        inline ee::Global<uint8_t, sym::g_idlePoint> g_idlePoint;

        // The level's camera points (a scene point's kind) and the name the
        // idle ones have ("idle").
        constexpr uint32_t kCameraPointKind = zge::nameHashOf("CameraPos"); // the "CameraPos" scene points (camera.c's warnings name them)
        constexpr uint32_t kIdlePointNameHash = 0x0004D905;
        constexpr uint32_t kIdlePointName = sym::bmx::g_idlePointName; // "idle"
        constexpr uint32_t kVanPointName = sym::bmx::g_vanPointName;   // "van"
        // The modes' motion values, from mode 2 on (update.cpp reads the rest).
        inline ee::Global<CameraSettings[8], sym::g_modeCameraSettings> g_modeCameraSettings;
        // The orbit mode's eye from the rider before it is turned, and its turn a frame.
        constexpr ee::Float kOrbitAway = ee::Float(5.0f);
        constexpr ee::Float kOrbitAbove = ee::Float(-7.0f);
        constexpr Angle kOrbitTurn = 0x12C;

        // The fog starts this far before the far clip and the far clip is
        // this far, unless the level says otherwise; the far clip is never
        // nearer than the least.
        constexpr ee::Float kDefaultFogDepth = ee::Float(20.0f);
        constexpr ee::Float kDefaultFarClip = ee::Float(200.0f);
        constexpr ee::Float kLeastFarClip = ee::Float(11.0f);

        // The fifteen settings of the two materials the camera task draws
        // with, in the engine's order.
        constexpr uint32_t kMaterialSettings = 15;
        constexpr uint8_t kShadeMaterial[kMaterialSettings] = {1, 0, 1, 0, 0, 4, 0, 0, 0, 0, 1, 1, 0, 0, 0};
        constexpr uint8_t kTintMaterial[kMaterialSettings] = {1, 0, 1, 1, 1, 3, 0, 0, 0, 0, 1, 1, 0, 0, 0};

        // The NPC the game is showing: modes 6 and 7 watch its rider.
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;
        // What the game knows of an NPC, as its NPC code hands it out.
        // Scaffolding (unnamed-field): only the rider's index is read here.
        struct NpcFacts
        {
            uint8_t _00[0x0E];
            uint8_t rider; // +0x0E the NPC's rider (riderOf's index)
            uint8_t _0f;
        };
        static_assert(offsetof(NpcFacts, rider) == 0x0E && sizeof(NpcFacts) == 0x10);
        inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
        // The rider's flags for its model and its bike's being drawn (update.cpp).
        constexpr rider::RiderFlag kFlagRiderDrawn{0, 0x04};
        constexpr rider::RiderFlag kFlagBikeDrawn{0, 0x08};
        constexpr ee::Float kWatchedLift = ee::Float(0.2f); // the point looked at above the drawn rider, along its up
        constexpr ee::Float kDrawnPull = ee::Float(0.33f);  // the share of the way the point goes to the drawn body

        constexpr uint32_t kCameraTaskStage = 1;
        constexpr uint32_t kCameraTaskSourceLine = 0x1C1;

        namespace calls
        {
            // Scaffolding (address-call): docs/SCAFFOLDING.md (game.c's NPC code: npcFacts is a Draft)
            // An NPC's facts copied out.
            void npcFacts(uint32_t npc, NpcFacts *facts) { ee::game(sym::npcFacts, npc, facts); }
        }
    }

    // initActionCameras: the cameras' set-up for a level: the level's clear
    // colour (black), fog and far clip; the two materials the camera task
    // draws with; the level's idle points counted; every camera record
    // cleared; the game's own camera made, in the idle mode, and run for a
    // frame while shown; and the camera task started.
    void initActionCameras()
    {
        ee::Local<CameraSetup> setup;
        ee::Local<LocatorList> points;

        (*g_cameraClearColour)[0] = 0;
        (*g_cameraClearColour)[3] = 0xFF;
        (*g_cameraClearColour)[1] = 0;
        (*g_cameraClearColour)[2] = 0;
        levelFogColour(&(*g_cameraFogColour)[0]);
        *g_cameraFogDepth = kDefaultFogDepth;
        levelFogDepth(&*g_cameraFogDepth);
        *g_cameraFarClip = kDefaultFarClip;
        levelFarClip(&*g_cameraFarClip);
        *g_cameraFarClip = ee::max(*g_cameraFarClip, kLeastFarClip);

        *g_cameraShadeMaterial = newMaterial();
        for (uint32_t setting = 0; setting < kMaterialSettings; ++setting)
            zgui::rebuildMaterialRegisters(*g_cameraShadeMaterial, 0, setting, kShadeMaterial[setting]);
        *g_cameraTintMaterial = newMaterial();
        for (uint32_t setting = 0; setting < kMaterialSettings; ++setting)
            zgui::rebuildMaterialRegisters(*g_cameraTintMaterial, 0, setting, kTintMaterial[setting]);

        *g_idlePointCount = 0;
        findLocators(points.address(), kCameraPointKind, 0);
        for (ee::Ptr<Locator> locator = points->head; locator != 0; locator = locator->next)
        {
            if (ee::Ptr<ScenePoint>(locator->point)->nameHash == kIdlePointNameHash)
                *g_idlePointCount = static_cast<uint8_t>(*g_idlePointCount + 1);
        }
        freeLocators(points.address());

        memoryFill(&(*g_actionCameras)[0], 0, sizeof(ActionCamera) * kActionCameraCount);
        *g_shownCameraCount = 0;
        *setup = CameraSetup{};
        *g_gameCamera = newActionCamera(setup);
        setCameraMode(*g_gameCamera, 0, static_cast<uint32_t>(CameraMode::Idle));
        showCamera(*g_gameCamera, 1);
        updateCameras();
        showCamera(*g_gameCamera, 0);
        *g_cameraTask = newTask(kCameraTaskStage, sym::cameraTask, 0, 1, sym::g_cameraSourceFile, kCameraTaskSourceLine);
    }
    READABLE_FUNCTION(initActionCameras, Verified);

    // idleCameraMode: the idle mode's frame: the view from one of the
    // level's "idle" camera points, the next one each time the low byte of
    // the mode's frame count is 0 (the mode's first frame, and every 256th
    // after). The point's first tag byte is its field of view. A level
    // with none looks from its origin at 60 degrees.
    uint32_t idleCameraMode(ActionCamera *camera)
    {
        ee::Local<char[0x40]> name;
        if (static_cast<uint8_t>(camera->modeFrames[0]) == 0)
        {
            if (*g_idlePointCount != 0)
                *g_idlePoint = static_cast<uint8_t>((*g_idlePoint + 1) % *g_idlePointCount);
            else
                *g_idlePoint = 0;
            formatString(name, kIdlePointName);
            const uint32_t hash = nameHash(name);
            const ee::Ptr<ScenePoint> point = findScenePoint(kCameraPointKind, hash, *g_idlePoint + 1u);
            if (point != 0)
            {
                camera->wantedFov = ee::toFloat(*zge::tagFieldAs<uint8_t>(point->tags, 0));
                camera->wantedEye = point->position;
                quaternionToMatrix(&camera->axes, &point->orientation);
            }
            else
            {
                camera->wantedFov = ee::Float(60.0f);
                camera->wantedEye = kZero;
                matrixIdentity(&camera->axes);
            }
            vec3Negate(&camera->axes.row(1).xyz(), &camera->axes.row(1).xyz());
            vec3Negate(&camera->axes.row(0).xyz(), &camera->axes.row(0).xyz());
        }
        camera->easing = CameraEasing::Cut;
        camera->flags |= kCameraModePlacedAxes;
        return 1;
    }
    READABLE_FUNCTION(idleCameraMode, Verified);

    // vanCameraMode: the van mode's frame: on the mode's first frame the
    // view goes to the level's "van" camera point with the mode's field of
    // view (a level with none: its origin, 60 degrees) and the blur pulses
    // down. The view cuts there and the mode sets the axes itself.
    uint32_t vanCameraMode(ActionCamera *camera)
    {
        ee::Local<char[0x40]> name;
        if (camera->modeFrames[0] == 0)
        {
            formatString(name, kVanPointName);
            const uint32_t hash = nameHash(name);
            const ee::Ptr<ScenePoint> point = findScenePoint(kCameraPointKind, hash, 1);
            if (point != 0)
            {
                camera->wantedFov = (*g_modeCameraSettings)[cameraMode(camera, 0) - 2].fov;
                camera->wantedEye = point->position;
                quaternionToMatrix(&camera->axes, &point->orientation);
            }
            else
            {
                camera->wantedFov = ee::Float(60.0f);
                camera->wantedEye = kZero;
                matrixIdentity(&camera->axes);
            }
            vec3Negate(&camera->axes.row(1).xyz(), &camera->axes.row(1).xyz());
            vec3Negate(&camera->axes.row(0).xyz(), &camera->axes.row(0).xyz());
            camera->blurPhase = CameraBlur::PulseDown;
        }
        camera->easing = CameraEasing::Cut;
        camera->flags |= kCameraModePlacedAxes;
        return 1;
    }
    READABLE_FUNCTION(vanCameraMode, Draft);

    // orbitCameraMode: mode 3's frame: the camera looks at the rider from a
    // point five units out and seven up that goes round the rider, a
    // little further each frame.
    uint32_t orbitCameraMode(ActionCamera *camera)
    {
        const rider::Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
        camera->wantedLookAt.x = rider->position[0] + rider->rootOffset[0];
        camera->wantedLookAt.y = rider->position[1] + rider->rootOffset[1];
        camera->wantedLookAt.z = rider->position[2] + rider->rootOffset[2];
        camera->wantedEye.z = ee::Float(0.0f);
        camera->wantedEye.x = kOrbitAway;
        camera->wantedEye.y = kOrbitAbove;
        camera->orbitAngle = static_cast<Angle>(camera->orbitAngle + kOrbitTurn);
        vec3RotateY(&camera->wantedEye.xyz(), &camera->wantedEye.xyz(), camera->orbitAngle);
        camera->wantedEye = camera->wantedEye + *rider::vec4(rider->position);
        return 1;
    }
    READABLE_FUNCTION(orbitCameraMode, Draft);

    namespace
    {
        // The view of the NPC the game is showing: the point looked at is a
        // little above its drawn rider and a third of the way to the drawn
        // body; the eye is at the mode's settings' offset in the rider's
        // axes at the drawn rider.
        void watchShownNpc(ActionCamera *camera, NpcFacts *facts, Vec4 &offset, Matrix &basis)
        {
            calls::npcFacts(*g_shownNpc, facts);
            const rider::Rider *rider = rider::riderOf(facts->rider);
            offset = (*g_modeCameraSettings)[cameraMode(camera, 0) - 2].offset;
            Vec4 &lookAt = camera->wantedLookAt;
            lookAt.x = rider->position[0] + rider->rootOffset[0] + rider->up[0] * kWatchedLift;
            lookAt.y = rider->position[1] + rider->rootOffset[1] + rider->up[1] * kWatchedLift;
            lookAt.z = rider->position[2] + rider->rootOffset[2] + rider->up[2] * kWatchedLift;
            lookAt.x = lookAt.x + (rider->drawnPosition[0] - lookAt.x) * kDrawnPull;
            lookAt.y = lookAt.y + (rider->drawnPosition[1] - lookAt.y) * kDrawnPull;
            lookAt.z = lookAt.z + (rider->drawnPosition[2] - lookAt.z) * kDrawnPull;
            basis = *rider::matrix(rider->right);
            basis.row(3) = *rider::vec4(rider->position);
            basis.row(3) = basis.row(3) + *rider::vec4(rider->rootOffset);
            camera->wantedEye = transformed(basis, offset);
        }
    }

    // npcAloneCameraMode: mode 6's frame: the shown NPC watched, with every
    // player's rider and bike not drawn.
    uint32_t npcAloneCameraMode(ActionCamera *camera)
    {
        ee::Local<NpcFacts> facts;
        ee::Local<Vec4> offset;
        ee::Local<Matrix> basis;
        watchShownNpc(camera, facts, *offset, *basis);
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            rider::Rider *rider = rider::riderOf(riderIndexOf(player));
            rider::clearFlag(rider, kFlagRiderDrawn);
            rider::clearFlag(rider, kFlagBikeDrawn);
        }
        return 1;
    }
    READABLE_FUNCTION(npcAloneCameraMode, Verified);

    // npcCameraMode: mode 7's frame: the shown NPC watched.
    uint32_t npcCameraMode(ActionCamera *camera)
    {
        ee::Local<NpcFacts> facts;
        ee::Local<Vec4> offset;
        ee::Local<Matrix> basis;
        watchShownNpc(camera, facts, *offset, *basis);
        return 1;
    }
    READABLE_FUNCTION(npcCameraMode, Draft);
}
