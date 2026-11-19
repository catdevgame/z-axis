// BMX XXX's cameras (action_camera.h): the records, their scene cameras, their
// rectangles of the screen, their modes and what the sound engine hears from
// them. BMX XXX's own code (./src/com/action/camera.c).

#include "action_camera.h"

#include "scene_camera.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/fx_manager.h"
#include "zge/render/material.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/camera.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene_database.h"
#include "zge/scene/scene.h"
#include "zge/sound/sound.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/camera/action_camera.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using namespace game::bmx::rider::collision;

    namespace
    {
        inline ee::Global<ActionCamera[kActionCameraCount], sym::g_actionCameras> g_actionCameras;
        // How many cameras are shown, counted by showCamera.
        inline ee::Global<uint8_t, sym::g_shownCameraCount> g_shownCameraCount;
        // The game's own camera (the first made) and the task that draws
        // over the cameras' rectangles.
        inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
        inline ee::Global<ee::Ptr<Task>, sym::g_cameraTask> g_cameraTask;
        // The two materials that task draws with.
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraShadeMaterial> g_cameraShadeMaterial;
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraTintMaterial> g_cameraTintMaterial;

        // The level's settings every camera takes: the colour its frame is
        // cleared to, the fog's colour (red, green, blue, alpha bytes), how
        // far before the far clip the fog starts, and the far clip.
        inline ee::Global<uint8_t[4], sym::g_cameraClearColour> g_cameraClearColour;
        inline ee::Global<uint8_t[4], sym::g_cameraFogColour> g_cameraFogColour;
        inline ee::Global<ee::Float, sym::g_cameraFogDepth> g_cameraFogDepth;
        inline ee::Global<ee::Float, sym::g_cameraFarClip> g_cameraFarClip;

        // The level being played: a row of the 13-row tables below (12 with
        // no level loaded).
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        // Each level's far clip in split screen.
        struct SplitFarClip
        {
            ee::Float twoPlayers;
            ee::Float fourPlayers;
        };
        inline ee::Global<SplitFarClip[13], sym::g_splitFarClips> g_splitFarClips;

        // The motion blur effect's request (startFullScreenEffect's; the
        // engine's, as Aggressive Inline's camera fills it).
        struct MotionBlurRequest
        {
            uint8_t _00[4];
            uint8_t strength;      // +0x04
            uint8_t _05[3];
            uint32_t colour;       // +0x08
            ee::Float scale[2];    // +0x0C
            // +0x14 two small values the swelling blur swings (what the
            // effect does with them is the engine's).
            ee::Float drift[2];
            ee::Ptr<Scene> camera; // +0x1C
        };
        static_assert(sizeof(MotionBlurRequest) == 0x20 && offsetof(MotionBlurRequest, camera) == 0x1C);
        // The blur's colour: white, alpha 0 (red, green, blue, alpha bytes).
        constexpr uint32_t kMotionBlurColour = 0xFFu | 0xFFu << 8 | 0xFFu << 16;
        // The request updateCameraBlur starts each frame from (the same values).
        inline ee::Global<MotionBlurRequest, sym::g_cameraBlurFrameDefaults> g_cameraBlurFrameDefaults;
        inline ee::Global<MotionBlurRequest, sym::g_cameraMotionBlurDefaults> g_cameraMotionBlurDefaults;
        inline ee::Global<char, sym::g_cameraMotionBlurName> g_cameraMotionBlurName; // "FSMotionBlur"

        // The second effect's values (ActionCamera::secondEffect).
        // showCamera fills one and hands it to nothing.
        struct SecondEffectRequest
        {
            uint32_t bytes;        // +0x00 its own size
            uint8_t strength;      // +0x04
            uint8_t _05[3];
            ee::Float values[2];   // +0x08 7 and 15 in the blur's phase 5
            ee::Ptr<Scene> camera; // +0x10
        };
        static_assert(sizeof(SecondEffectRequest) == 0x14 && offsetof(SecondEffectRequest, camera) == 0x10);

        // How far the eye is kept from the rider (across the ground) and
        // from the level's faces.
        inline ee::Global<ee::Float, sym::g_cameraRiderGap> g_cameraRiderGap;
        inline ee::Global<ee::Float, sym::g_cameraWallRadius> g_cameraWallRadius;


        // What cameraMoved hands the sound engine: the scene objects sounds
        // are heard from.
        struct SoundListeners
        {
            int32_t count;
            ee::Ptr<SceneObject> objects[4];
        };
        static_assert(sizeof(SoundListeners) == 0x14);

        // The sound event a lone listener's frame sends from its eye.
        constexpr uint32_t kListenerSoundEvent = 0x35;

        namespace calls
        {
            // Scaffolding (address-call): docs/SCAFFOLDING.md (libc's memset: a library function, never rewritten)
            void memset(ee::Address to, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, to, byte, bytes); }
        }
        namespace calls
        {
            // setSceneClears (camera.c's own wrapper, beside scene_camera.cpp's)
            // waits for the engine's setCameraClears to have a body.
            void setSceneClears(ee::Ptr<SceneObject> object, uint32_t clears) { ee::game(sym::setSceneClears, object, clears); }
        }

        // The view's axes as the scene camera takes them: the first two
        // turned round, the eye as the translation.
        void placeSceneCamera(ActionCamera *camera, const CameraView *view, ee::Ptr<Matrix> axes)
        {
            *axes = view->axes;
            vec3Negate(&axes->row(1).xyz(), &axes->row(1).xyz());
            vec3Negate(&axes->row(0).xyz(), &axes->row(0).xyz());
            axes->translation() = view->eye;
            setObjectPosition(camera->object, ee::Ptr<Vec4>(ee::Address(&view->eye)));
            setObjectRotationMatrix(camera->object, axes);
            applyCameraLens(camera);
        }
    }

    // ---- what a camera is

    uint8_t cameraType(const ActionCamera *camera)
    {
        return camera->type;
    }
    READABLE_FUNCTION(cameraType, Verified);

    // cameraMode: the mode at a level (0 the camera's mode, 1 the mode
    // within it).
    uint8_t cameraMode(const ActionCamera *camera, uint32_t level)
    {
        return camera->modes[level];
    }
    READABLE_FUNCTION(cameraMode, Verified);

    uint8_t cameraShot(const ActionCamera *camera)
    {
        return camera->shot;
    }
    READABLE_FUNCTION(cameraShot, Verified);

    uint8_t cameraViewportLayout(const ActionCamera *camera)
    {
        return static_cast<uint8_t>(camera->viewportLayout);
    }
    READABLE_FUNCTION(cameraViewportLayout, Verified);

    ScreenRectangle *cameraScreenRectangle(ActionCamera *camera)
    {
        return &camera->viewport;
    }
    READABLE_FUNCTION(cameraScreenRectangle, Verified);

    void setCameraHearing(ActionCamera *camera, uint32_t hearing)
    {
        camera->hearing = static_cast<CameraHearing>(hearing);
    }
    READABLE_FUNCTION(setCameraHearing, Verified);

    ee::Ptr<SceneObject> cameraSceneObject(const ActionCamera *camera)
    {
        return camera->object;
    }
    READABLE_FUNCTION(cameraSceneObject, Verified);

    bool cameraShown(const ActionCamera *camera)
    {
        return (camera->flags & kCameraShown) != 0;
    }
    READABLE_FUNCTION(cameraShown, Draft);

    // ---- the modes with nothing to do

    // noCameraMode: mode 2's frame: nothing moves the view.
    uint32_t noCameraMode(ActionCamera *)
    {
        return 1;
    }
    READABLE_FUNCTION(noCameraMode, Draft);

    // heldCameraMode: mode 9's frame: other code places the scene camera, so
    // the frame's update leaves it alone.
    uint32_t heldCameraMode(ActionCamera *camera)
    {
        camera->flags |= kCameraModePlacedView;
        return 1;
    }
    READABLE_FUNCTION(heldCameraMode, Draft);

    // ---- making and showing

    // newActionCamera: the first free record set up from `setup`: a 60 degree
    // view looking along z, on the set-up's rectangle, not shown yet. 0 when
    // all eight are taken.
    ee::Ptr<ActionCamera> newActionCamera(const CameraSetup *setup)
    {
        for (uint32_t i = 0; i < kActionCameraCount; ++i)
        {
            ActionCamera *camera = &(*g_actionCameras)[i];
            if ((camera->flags & kCameraInUse) != 0)
                continue;
            memoryFill(camera, 0, sizeof(ActionCamera));
            camera->kind = setup->kind;
            camera->view.lookAt.z = ee::Float(1.0f);
            camera->rider = setup->rider;
            camera->view.fov = 0x2AAA;
            matrixIdentity(&camera->view.axes);
            camera->flags = static_cast<uint8_t>((camera->flags & ~kCameraShown) | kCameraInUse);
            setCameraViewportLayout(camera, setup->viewportLayout);
            camera->motionBlur = ee::Ptr<RunningEffect>();
            camera->secondEffect = ee::Ptr<RunningEffect>();
            return camera;
        }
        return ee::Ptr<ActionCamera>();
    }
    READABLE_FUNCTION(newActionCamera, Verified);

    // showCamera: a camera given its scene camera (on its rectangle, cleared
    // to the level's colour, placed from its view, with the full-screen
    // motion blur started on it) or its scene camera and effects taken away.
    void showCamera(ActionCamera *camera, uint32_t shown)
    {
        ee::Local<Matrix> axes;
        ee::Local<MotionBlurRequest> request;
        ee::Local<SecondEffectRequest> unused;
        *request = *g_cameraMotionBlurDefaults;
        calls::memset(unused, 0, sizeof(SecondEffectRequest));
        unused->bytes = sizeof(SecondEffectRequest);
        *g_shownCameraCount = 0;
        for (uint32_t i = 0; i < kActionCameraCount; ++i)
        {
            if (((*g_actionCameras)[i].flags & kCameraShown) != 0)
                *g_shownCameraCount = static_cast<uint8_t>(*g_shownCameraCount + 1);
        }
        if (shown != 0)
        {
            if ((camera->flags & kCameraShown) == 0)
            {
                camera->object = ee::Ptr<SceneObject>(newSceneCamera(1));
                createObjectAnimator(camera->object);
                calls::setSceneClears(camera->object, 1);
                swapCameraViewport(objectCamera(camera->object), &camera->viewport);
                setSceneClearColour(camera->object, &*g_cameraClearColour);
                placeSceneCamera(camera, &camera->view, axes);
                request->camera = sceneOf(camera->object);
                unused->camera = sceneOf(camera->object);
                camera->motionBlur = startFullScreenEffect(nameHash(&*g_cameraMotionBlurName), request);
                // No effect is all ones here: kept as none.
                if (ee::Address(camera->motionBlur) == ~0u)
                    camera->motionBlur = ee::Ptr<RunningEffect>();
                camera->secondEffect = ee::Ptr<RunningEffect>();
            }
            camera->flags |= kCameraShown;
        }
        else
        {
            if ((camera->flags & kCameraShown) != 0)
            {
                if (camera->motionBlur != 0)
                {
                    stopFullScreenEffect(camera->motionBlur);
                    camera->motionBlur = ee::Ptr<RunningEffect>();
                }
                if (camera->secondEffect != 0)
                {
                    stopFullScreenEffect(camera->secondEffect);
                    camera->secondEffect = ee::Ptr<RunningEffect>();
                }
                deleteSceneObject(camera->object);
            }
            camera->flags &= static_cast<uint8_t>(~kCameraShown);
        }
    }
    READABLE_FUNCTION(showCamera, Verified);

    // freeActionCamera: a camera hidden and its record given back.
    void freeActionCamera(ActionCamera *camera)
    {
        showCamera(camera, 0);
        camera->flags &= static_cast<uint8_t>(~kCameraInUse);
    }
    READABLE_FUNCTION(freeActionCamera, Verified);

    // freeActionCameras: the camera code's shutdown: its task, the game's own
    // camera and the two materials.
    void freeActionCameras()
    {
        deleteTask(*g_cameraTask);
        freeActionCamera(*g_gameCamera);
        releaseMaterial(*g_cameraTintMaterial);
        game::tail(releaseMaterial, *g_cameraShadeMaterial);
    }
    READABLE_FUNCTION(freeActionCameras, Verified);

    // ---- the rectangle of the screen

    // setCameraViewportLayout: the camera's rectangle of the 512 by 448
    // screen for a layout; a number past the layouts keeps the rectangle.
    // A shown camera's scene camera takes it at once.
    void setCameraViewportLayout(ActionCamera *camera, uint32_t layout)
    {
        ScreenRectangle &viewport = camera->viewport;
        constexpr float kHalfWidth = 254.0f, kHalfHeight = 222.0f, kRightHalf = 258.0f, kBottomHalf = 226.0f;
        switch (static_cast<ViewportLayout>(layout))
        {
        case ViewportLayout::Full:
            viewport.width = ee::Float(512.0f);
            viewport.height = ee::Float(448.0f);
            viewport.x = ee::Float(0.0f);
            viewport.y = ee::Float(0.0f);
            break;
        case ViewportLayout::Letterbox:
            viewport.y = ee::Float(44.8f);
            viewport.width = ee::Float(512.0f);
            viewport.height = ee::Float(336.0f);
            viewport.x = ee::Float(0.0f);
            break;
        case ViewportLayout::Top:
        case ViewportLayout::Bottom:
            viewport.x = ee::Float(0.0f);
            viewport.y = ee::Float(layout == static_cast<uint32_t>(ViewportLayout::Top) ? 0.0f : kBottomHalf);
            viewport.width = ee::Float(512.0f);
            viewport.height = ee::Float(kHalfHeight);
            break;
        case ViewportLayout::Left:
        case ViewportLayout::Right:
            viewport.x = ee::Float(layout == static_cast<uint32_t>(ViewportLayout::Left) ? 0.0f : kRightHalf);
            viewport.width = ee::Float(kHalfWidth);
            viewport.height = ee::Float(448.0f);
            viewport.y = ee::Float(0.0f);
            break;
        case ViewportLayout::Quarter0:
        case ViewportLayout::Quarter1:
        case ViewportLayout::Quarter2:
        case ViewportLayout::Quarter3:
            // The odd ones are on the left, the last two at the bottom.
            viewport.x = ee::Float((layout & 1) != 0 ? 0.0f : kRightHalf);
            viewport.y = ee::Float(layout < static_cast<uint32_t>(ViewportLayout::Quarter2) ? 0.0f : kBottomHalf);
            viewport.width = ee::Float(kHalfWidth);
            viewport.height = ee::Float(kHalfHeight);
            break;
        case ViewportLayout::Inset:
            viewport.x = ee::Float::fromBits(0x439AAAAAu);      // 309.33
            viewport.y = ee::Float(28.0f);
            viewport.width = ee::Float::fromBits(0x432AAAABu);  // 170.67
            viewport.height = ee::Float::fromBits(0x43155556u); // 149.33
            break;
        default:
            break;
        }
        if ((camera->flags & kCameraShown) != 0)
            swapCameraViewport(objectCamera(camera->object), &camera->viewport);
        camera->viewportLayout = static_cast<ViewportLayout>(layout);
    }
    READABLE_FUNCTION(setCameraViewportLayout, Verified);

    // ---- the view

    // setCameraView: the camera shows `view`; a shown camera's scene camera
    // is placed from it.
    void setCameraView(ActionCamera *camera, const CameraView *view)
    {
        ee::Local<Matrix> axes;
        memoryCopy(&camera->view, view, sizeof(CameraView));
        if ((camera->flags & kCameraShown) != 0)
            placeSceneCamera(camera, view, axes);
    }
    READABLE_FUNCTION(setCameraView, Verified);

    // cameraView: a copy of what the camera shows, with its scene object.
    void cameraView(const ActionCamera *camera, CameraView *out)
    {
        memoryCopy(out, &camera->view, sizeof(CameraView));
        out->object = camera->object;
    }
    READABLE_FUNCTION(cameraView, Draft);

    // applyCameraLens: the scene camera's fog, clip range and field of view
    // for the camera's rectangle. A split screen draws less far: a part of
    // the fog's depth, the level's split-screen far clip where that is
    // nearer, and a thicker fog. A narrow field of view sees further: the
    // far clip grows as the view narrows from 35 degrees, up to twice.
    void applyCameraLens(ActionCamera *camera)
    {
        ee::Local<uint8_t[4]> fogColour;
        ee::Float fogDepth = *g_cameraFogDepth;
        ee::Float farClip = *g_cameraFarClip;
        for (uint32_t i = 0; i < 4; ++i)
            fogColour[i] = (*g_cameraFogColour)[i];
        const CameraView *view = &camera->view;
        const uint32_t layout = static_cast<uint32_t>(camera->viewportLayout);
        ee::Float splitFarClip = farClip;
        ee::Float splitFogDepth = fogDepth;
        if (layout >= static_cast<uint32_t>(ViewportLayout::Top) && layout < static_cast<uint32_t>(ViewportLayout::Inset))
        {
            const SplitFarClip &level = (*g_splitFarClips)[*g_currentLevel];
            ee::Float alpha;
            if (layout < static_cast<uint32_t>(ViewportLayout::Quarter0))
            {
                splitFogDepth = farClip * ee::Float(0.15f);
                splitFarClip = level.twoPlayers;
                alpha = ee::toFloat(fogColour[3]) * ee::Float(1.15f);
            }
            else
            {
                splitFogDepth = farClip * ee::Float(0.25f);
                splitFarClip = level.fourPlayers;
                alpha = ee::toFloat(fogColour[3]) * ee::Float(1.35f);
            }
            fogColour[3] = static_cast<uint8_t>(ee::toInt(alpha));
        }
        fogDepth = ee::max(splitFogDepth, fogDepth);
        farClip = ee::min(splitFarClip, farClip);
        // The field of view as a share of a full turn; 0.0972 of a turn is
        // 35 degrees.
        const ee::Float turn = ee::toFloat(view->fov) * ee::Float(1.0f / 65536.0f);
        ee::Float reach = ee::Float::fromBits(0x3DC71800u) / turn;
        reach = ee::max(reach, ee::Float(1.0f));
        reach = ee::min(reach, ee::Float(2.0f));
        farClip = farClip * reach;
        setSceneFogColour(camera->object, &*fogColour);
        moveCameraClipRange(objectCamera(camera->object), fogDepth);
        setSceneFarClip(camera->object, farClip);
        setFov(camera->object, view->fov);
    }
    READABLE_FUNCTION(applyCameraLens, Verified);

    // ---- the modes

    // setCameraMode: the mode at a level, with every level below it back to
    // mode 0 and its frame count to 0. A change of the camera's mode (level
    // 0) cuts to the new view, frees the animation an animated camera was
    // playing and sets the new mode up; a change within the follow mode
    // (level 1, while level 0 is 0) eases, or cuts when the old mode within
    // was 3 to 5 or the new one is 5.
    void setCameraMode(ActionCamera *camera, uint32_t level, uint32_t mode)
    {
        const auto throughWalls = [&] {
            camera->flags = static_cast<uint8_t>((camera->flags & ~kCameraKeepsOffRider) | kCameraThroughWalls);
        };
        camera->flags = static_cast<uint8_t>((camera->flags | kCameraKeepsOffRider) & ~kCameraThroughWalls);
        if (level == 0)
        {
            const uint8_t old = camera->modes[0];
            camera->easing = CameraEasing::Cut;
            if (old == static_cast<uint8_t>(CameraMode::Animated))
                zge::freeAnimatorOf(camera->object);
            switch (mode)
            {
            case 4:
            case 5:
            case 6:
            case 7:
                throughWalls();
                break;
            case 1:
                setCameraShot(camera, 7);
                camera->flags = static_cast<uint8_t>((camera->flags | kCameraShotHeld) & ~kCameraShotBlocked);
                break;
            default:
                break;
            }
            camera->blurFrames = 0;
            camera->blurPhase = CameraBlur::Steady;
        }
        else if (level == 1 && camera->modes[0] == 0)
        {
            const uint8_t old = camera->modes[1];
            camera->easing = (old >= 3 && old < 6) ? CameraEasing::Cut : CameraEasing::Ease;
            if (mode == 5)
            {
                camera->easing = CameraEasing::Cut;
                throughWalls();
            }
        }
        // A level past the last writes past the record's modes, as the
        // original does: no caller passes one.
        const ee::Ptr<uint8_t> modes(camera->modes);
        const ee::Ptr<uint32_t> frames(camera->modeFrames);
        modes[level] = static_cast<uint8_t>(mode);
        frames[level] = 0;
        for (uint32_t below = level + 1; below < kCameraModeLevels; ++below)
        {
            modes[below] = 0;
            frames[below] = 0;
        }
    }
    READABLE_FUNCTION(setCameraMode, Verified);

    // setCameraType: the player's camera type. Type 4 is the one the rider
    // is not followed from behind in: going to it or from it while the
    // camera follows the rider cuts, and turns the eye's two keep-outs
    // (the rider, the walls) the other way round. A change that leaves the
    // camera in its follow mode restarts that mode's frame count.
    void setCameraType(ActionCamera *camera, uint32_t type)
    {
        constexpr uint32_t kFixedType = 4;
        rider::riderOf(static_cast<uint8_t>(camera->rider));
        if (camera->type == type)
            return;
        camera->easing = CameraEasing::Ease;
        if (camera->type == kFixedType && cameraMode(camera, 0) == 0)
        {
            camera->easing = CameraEasing::Cut;
            camera->flags = static_cast<uint8_t>((camera->flags | kCameraKeepsOffRider) & ~kCameraThroughWalls);
        }
        if (type == kFixedType && cameraMode(camera, 0) == 0)
        {
            camera->easing = CameraEasing::Cut;
            camera->flags = static_cast<uint8_t>((camera->flags & ~kCameraKeepsOffRider) | kCameraThroughWalls);
        }
        camera->type = static_cast<uint8_t>(type);
        if (cameraMode(camera, 0) == 0)
            camera->modeFrames[0] = 0;
    }
    READABLE_FUNCTION(setCameraType, Verified);

    // ---- where the eye may be, and where the camera looks

    // keepEyeOffRider: the eye kept the rider gap away from the drawn rider
    // across the ground (x and z; the height is left alone): an eye nearer
    // than that is moved out to the gap along its own direction from the
    // rider, or along the rider's forward axis when it is exactly on the
    // rider. Returns whether it moved.
    uint32_t keepEyeOffRider(ActionCamera *, const rider::Rider *rider, Vec4 *eye)
    {
        ee::Local<Vec4> offset;
        const ee::Float riderX = rider->position[0] + rider->rootOffset[0];
        const ee::Float riderZ = rider->position[2] + rider->rootOffset[2];
        const ee::Float riderY = rider->position[1] + rider->rootOffset[1];
        offset->x = eye->x - riderX;
        offset->z = eye->z - riderZ;
        offset->y = eye->y - riderY;
        const ee::Float across = offset->x * offset->x + offset->z * offset->z;
        if (!(across < *g_cameraRiderGap * *g_cameraRiderGap))
            return 0;
        offset->y = ee::Float(0.0f);
        if (ee::Float(0.0f) < across)
        {
            *offset = unit(*offset);
        }
        else
        {
            *offset = *rider::vec4(rider->forward);
            offset->y = ee::Float(0.0f);
        }
        *offset = *offset * *g_cameraRiderGap;
        eye->x = offset->x + rider->position[0] + rider->rootOffset[0];
        eye->z = offset->z + rider->position[2] + rider->rootOffset[2];
        return 1;
    }
    READABLE_FUNCTION(keepEyeOffRider, Verified);

    // pushEyeOutOfWalls: the eye kept the camera's radius off the level's
    // faces: each face within one and a half radii that faces the eye and is
    // nearer than the radius pushes the eye straight off it. Faces of
    // surface kind 12 do not count. Returns whether any did.
    uint32_t pushEyeOutOfWalls(ActionCamera *, Vec4 *eye)
    {
        constexpr uint32_t kSurfaceIgnored = 12;
        ee::Local<Vec4> toFace;
        ee::Local<Vec4> normal;
        uint32_t pushed = 0;
        SphereHits *hits = sphereQuery(&eye->x, *g_cameraWallRadius * ee::Float(1.5f));
        if (hits == nullptr)
            return 0;
        while (const SphereFace *face = nextSphereFace(hits))
        {
            if (faceSurfaceKind(faceRefAt(&face->face)) == kSurfaceIgnored)
                continue;
            faceNormal(faceRefAt(&face->face), &normal->x);
            closestOnFace(face, &toFace->x);
            toFace->x = toFace->x - eye->x;
            toFace->y = toFace->y - eye->y;
            toFace->z = toFace->z - eye->z;
            if (!(dot(*toFace, *normal) <= ee::Float(0.0f)))
                continue;
            if (!(squaredLength(*toFace) < *g_cameraWallRadius * *g_cameraWallRadius))
                continue;
            const ee::Float distance = vec3Length(&toFace->xyz());
            *toFace = *toFace * (ee::Float(1.0f) / distance);
            pushed = 1;
            const ee::Float gap = *g_cameraWallRadius - distance;
            const ee::Float z = eye->z - toFace->z * gap;
            const ee::Float x = eye->x - toFace->x * gap;
            const ee::Float y = eye->y - toFace->y * gap;
            eye->z = z;
            eye->x = x;
            eye->y = y;
        }
        return pushed;
    }
    READABLE_FUNCTION(pushEyeOutOfWalls, Verified);

    // aimCamera: the camera's axes from its eye and the point it looks at:
    // forward to the point, upright unless it looks nearly straight up or
    // down (then the axis kept from last frame, or the level's z when that
    // is no better).
    void aimCamera(ActionCamera *camera)
    {
        // A straight-down vector the original builds and never reads.
        ee::Local<Vec4> down;
        Vec4 &right = camera->axes.row(0);
        Vec4 &up = camera->axes.row(1);
        Vec4 &forward = camera->axes.row(2);
        forward = camera->lookAt - camera->eye;
        forward = unit(forward);
        down->x = ee::Float(0.0f);
        down->y = ee::Float(-1.0f);
        down->z = ee::Float(0.0f);
        const ee::Float nearlyVertical = ee::Float::fromBits(0x3F7FBE77u); // 0.999
        if (ee::abs(forward.y) < nearlyVertical)
        {
            up.y = ee::Float(-1.0f);
            up.x = ee::Float(0.0f);
            up.z = ee::Float(0.0f);
        }
        else if (nearlyVertical < ee::abs(dot(forward, up)))
        {
            up.z = ee::Float(-1.0f);
            up.x = ee::Float(0.0f);
            up.y = ee::Float(0.0f);
        }
        right = cross(up, forward);
        right = unit(right);
        up = cross(forward, right);
    }
    READABLE_FUNCTION(aimCamera, Verified);

    // ---- the motion blur

    // updateCameraBlur: the frame's values for the camera's full-screen
    // effects, by the blur's phase: a pulse up to full strength over a
    // second and back down over the next, a steady light blur, or a blur
    // that swells and drifts; phase 5 sets the second effect instead.
    void updateCameraBlur(ActionCamera *camera)
    {
        constexpr uint32_t kPulseFrames = 60;
        ee::Local<MotionBlurRequest> blur;
        ee::Local<SecondEffectRequest> second;
        *blur = *g_cameraBlurFrameDefaults;
        calls::memset(second, 0, sizeof(SecondEffectRequest));
        second->bytes = sizeof(SecondEffectRequest);
        blur->camera = sceneOf(camera->object);
        second->camera = sceneOf(camera->object);
        if (camera->blurPhase != CameraBlur::None)
        {
            camera->blurFrames = camera->blurFrames + 1;
            switch (camera->blurPhase)
            {
            case CameraBlur::PulseUp:
                if (camera->blurFrames < kPulseFrames)
                {
                    const ee::Float strength = ee::toFloat(static_cast<int32_t>(camera->blurFrames * 255)) / ee::toFloat(kPulseFrames);
                    blur->strength = static_cast<uint8_t>(ee::toInt(strength));
                }
                else
                {
                    blur->strength = 0xFF;
                    camera->blurPhase = CameraBlur::PulseDown;
                    camera->blurFrames = 0;
                }
                break;
            case CameraBlur::PulseDown:
                if (camera->blurFrames < kPulseFrames)
                {
                    const uint32_t left = kPulseFrames - camera->blurFrames;
                    const ee::Float strength = ee::toFloat(static_cast<int32_t>(left * 255)) / ee::toFloat(kPulseFrames);
                    blur->strength = static_cast<uint8_t>(ee::toInt(strength));
                }
                else
                {
                    camera->blurPhase = CameraBlur::None;
                    camera->blurFrames = 0;
                }
                break;
            case CameraBlur::Steady:
                blur->strength = 0x41;
                break;
            case CameraBlur::Swell:
            {
                blur->strength = 0x80;
                blur->colour = kMotionBlurColour;
                // Three slow waves off the frame count: a turn every 180,
                // 96 and 105 frames.
                const ee::Float fullTurn(65536.0f);
                const auto turns = [&](uint32_t perFrameBits) {
                    const ee::Float angle = ee::toFloat(static_cast<int32_t>(camera->blurFrames)) * ee::Float::fromBits(perFrameBits) * fullTurn;
                    return static_cast<Angle>(ee::toInt(angle));
                };
                const ee::Float scale = angleSin(turns(0x3BB60B61u)) * ee::Float(0.15f) + ee::Float(1.15f);
                blur->scale[1] = scale;
                blur->scale[0] = scale;
                blur->drift[0] = angleCos(turns(0x3C2AAAABu)) * ee::Float(0.05f);
                blur->drift[1] = angleSin(turns(0x3C1C09C1u)) * ee::Float(0.03f);
                break;
            }
            case CameraBlur::Second:
                second->strength = 0xFF;
                second->values[0] = ee::Float(7.0f);
                second->values[1] = ee::Float(15.0f);
                break;
            default:
                break;
            }
        }
        if (camera->motionBlur != 0)
            setEffectValue(camera->motionBlur, blur);
        if (camera->secondEffect != 0)
            setEffectValue(camera->secondEffect, second);
    }
    READABLE_FUNCTION(updateCameraBlur, Verified);

    // ---- the modes that place the view themselves

    // animatedCameraMode: mode 8's frame: the scene camera's animation
    // places it. While it plays, its first bone gives the eye, the
    // direction (the look-at point a unit ahead) and, as its scale, the
    // lens: x times 62 degrees (0 counts as 1, and is made 1), y times the
    // level's far clip, z times its fog depth.
    uint32_t animatedCameraMode(ActionCamera *camera)
    {
        constexpr uint32_t kCameraBone = 1;
        ee::Local<Matrix> axes;
        ee::Local<ee::Float> time;
        if (zge::objectHasAnimator(camera->object))
        {
            *time = ee::Float(0.0f);
            animationTime(camera->object, time);
            if (animationDone(camera->object, *time) == 0)
            {
                const ee::Ptr<AnimationChannel> bone = findBoneChannel(sceneObjectAnimator(camera->object), kCameraBone);
                const ee::Ptr<Vec4> position = boneTranslation(bone);
                if (position != 0)
                {
                    camera->wantedEye = *position;
                    setObjectPosition(camera->object, position);
                }
                const ee::Ptr<Quaternion> rotation = boneRotation(bone);
                if (rotation != 0)
                {
                    quaternionToMatrix(&*axes, &*rotation);
                    camera->wantedLookAt = axes->row(2) * ee::Float(1.0f);
                    camera->wantedLookAt = camera->wantedLookAt + camera->wantedEye;
                    setObjectOrientation(camera->object, rotation);
                }
                const ee::Ptr<Vec4> lens = boneScale(bone);
                if (lens != 0)
                {
                    if (lens->x == ee::Float(0.0f))
                        lens->x = ee::Float(1.0f);
                    const ee::Float degrees = lens->x * ee::Float(62.0f);
                    camera->wantedFov = degrees;
                    const ee::Float angle = degrees * ee::Float::fromBits(0x43360B61u); // 65536 / 360
                    setFov(camera->object, static_cast<int16_t>(ee::toInt(angle)));
                    setSceneFarClip(camera->object, lens->y * *g_cameraFarClip);
                    const ee::Float fogDepth = lens->z * *g_cameraFogDepth;
                    moveCameraClipRange(objectCamera(camera->object), fogDepth);
                }
                camera->easing = CameraEasing::Cut;
            }
        }
        camera->flags |= kCameraModePlacedView;
        return 1;
    }
    READABLE_FUNCTION(animatedCameraMode, Verified);

    // ---- what the sound engine hears

    // updateCameraListeners: the shown cameras the sound engine hears from.
    // A camera that hears alone is the one listener, and a sound event goes
    // out from its eye; otherwise the split-screen cameras are the
    // listeners.
    void updateCameraListeners()
    {
        ee::Local<SoundListeners> listeners;
        uint32_t count = 0;
        for (uint32_t i = 0; i < kActionCameraCount; ++i)
        {
            ActionCamera *camera = &(*g_actionCameras)[i];
            if ((camera->flags & kCameraShown) == 0)
                continue;
            switch (camera->hearing)
            {
            case CameraHearing::Deaf:
                break;
            case CameraHearing::Alone:
                listeners->count = 1;
                listeners->objects[0] = camera->object;
                cameraMoved(listeners);
                soundEvent(kListenerSoundEvent, &camera->view.eye, 0, 0, 0);
                return;
            case CameraHearing::SplitFirst:
            case CameraHearing::SplitSecond:
                // Kept as it is: the slot is one past half the count so far,
                // so two cameras both land in the second slot and the first
                // slot holds what the stack held.
                listeners->objects[(count >> 1) + 1] = camera->object;
                ++count;
                break;
            default:
                ++count;
                break;
            }
        }
        listeners->count = static_cast<int32_t>(count);
        cameraMoved(listeners);
    }
    READABLE_FUNCTION(updateCameraListeners, Verified);
}
