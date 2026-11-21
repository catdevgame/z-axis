// BMX XXX's action cameras (action_camera.h): the frame's update and the
// task that draws over the views (./src/com/action/camera.c).

#include "action_camera.h"

#include "bmx/profile/players.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/physics/physics.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/material.h"
#include "zge/render/primitives.h"
#include "zge/scene/scene.h"
#include "zge/sound/sound.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/camera/update.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::Rider;

    namespace
    {
        inline ee::Global<ActionCamera[kActionCameraCount], sym::g_actionCameras> g_actionCameras;

        constexpr uint32_t kCameraTypes = 5;
        constexpr uint32_t kLayoutGroups = 4;
        // The follow mode's, by how the screen is shared and the player's camera type.
        inline ee::Global<CameraSettings[kLayoutGroups][kCameraTypes], sym::g_followCameraSettings> g_followCameraSettings;
        // The follow mode's while its second level is 3 or 4, by how the screen is shared.
        inline ee::Global<CameraSettings[kLayoutGroups], sym::g_followSpecialCameraSettings> g_followSpecialCameraSettings;
        // The follow mode's while its second level is 5.
        inline ee::Global<CameraSettings, sym::g_followFifthCameraSettings> g_followFifthCameraSettings;
        // Each action shot's.
        inline ee::Global<CameraSettings[15], sym::g_shotCameraSettings> g_shotCameraSettings;
        // The other modes', from mode 2 on.
        inline ee::Global<CameraSettings[8], sym::g_modeCameraSettings> g_modeCameraSettings;
        // How the screen is shared, for each of the first ten viewport
        // layouts: 0 one view, 1 two views one above the other, 2 two side
        // by side, 3 four.
        inline ee::Global<uint8_t[10], sym::g_layoutGroups> g_layoutGroups;

        // Each mode's frame: it sets what the camera wants (or places the
        // view itself) and answers 0 to have the frame's mode run again (it
        // changed the mode).
        using CameraModeFrame = ee::FunctionPtr<uint32_t(ee::Ptr<ActionCamera>)>;
        inline ee::Global<CameraModeFrame[10], sym::g_cameraModeFrames> g_cameraModeFrames;

        // A field of view nearer its wanted value than this goes there.
        constexpr ee::Float kFovStep = std::bit_cast<float>(0x3DCCCCCDu); // 0.1
        constexpr ee::Float kAnglesADegree = std::bit_cast<float>(0x43360B61u); // 65536 / 360
        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);
        // A rider's camera whose view of the rider the level has blocked
        // for more frames than this does something about it.
        constexpr uint32_t kMostBlockedFrames = 40;
        // The follow mode's second level whose view hides the rider, the
        // camera type that does and the action shot that does.
        constexpr uint32_t kFollowHidesRider = 5;
        constexpr uint32_t kTypeHidesRider = 4;
        constexpr uint32_t kShotHidesRider = 11;
        // The rider's action state (level 0) in which a blocked follow
        // camera stays where it is.
        constexpr uint32_t kActionStateKeepsCamera = 8;

        // The rider's flags the cameras use: whether the rider's model and
        // the bike's are drawn (the first-person views clear both), and one
        // that keeps the bike's shadow off.
        constexpr rider::RiderFlag kFlagRiderDrawn{0, 0x04};
        constexpr rider::RiderFlag kFlagBikeDrawn{0, 0x08};

        // The two materials the task draws over the views with, and the tint's colour.
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraShadeMaterial> g_cameraShadeMaterial;
        inline ee::Global<ee::Ptr<Material>, sym::g_cameraTintMaterial> g_cameraTintMaterial;
        inline ee::Global<uint8_t[4], sym::g_cameraTintColour> g_cameraTintColour;

        // A quad over a camera's rectangle of the screen, as the engine's
        // 2D drawing takes it.
        struct ViewQuad
        {
            ee::Ptr<Material> material;                // +0x00
            ee::Ptr<PrimitiveVertex> vertices[4];      // +0x04
            uint8_t _14[0x20 - 0x14];
            PrimitiveVertex corners[4];                // +0x20
        };
        static_assert(offsetof(ViewQuad, corners) == 0x20 && sizeof(ViewQuad) == 0xA0);
        constexpr uint32_t kScreenQuads = 0x02000002;

        // The quad's corners in turn (left top, right top, right bottom,
        // left bottom), listed backwards. Only a corner's place and colour
        // are set: the rest of it is what the stack held.
        void drawOverView(ViewQuad *quad, const ActionCamera *camera, ee::Ptr<Material> material, const uint8_t *colour)
        {
            quad->material = material;
            for (uint32_t i = 0; i < 4; ++i)
            {
                PrimitiveVertex &corner = quad->corners[i];
                corner.x = ((i + 1) & 2) ? camera->viewport.x + camera->viewport.width : camera->viewport.x;
                corner.y = (i & 2) ? camera->viewport.y + camera->viewport.height : camera->viewport.y;
                corner.z = ee::Float(1.0f);
                for (uint32_t c = 0; c < 4; ++c)
                    corner.rgba[c] = colour[c];
                quad->vertices[i] = &quad->corners[3 - i];
            }
            drawPrimitiveQuads(kScreenQuads, quad, 1, 0);
        }

        // The view's angle under which the shade starts, degrees, the
        // shade's grey, and its alpha for each degree under.
        constexpr ee::Float kShadeFov = ee::Float(35.0f);
        constexpr uint8_t kShadeGrey = 0x98;
        constexpr ee::Float kShadeAlphaADegree = std::bit_cast<float>(0x4036DB6Eu); // 100 / 35
        constexpr ee::Float kDegreesAnAngle = std::bit_cast<float>(0x3BB40000u);    // 360 / 65536
        // The rider's night vision goggles (rider::kFlagNightVision,
        // riderModelReady's) tint the first-person views; SavedGame::featuresOn's
        // bit 3, "NIGHT VISION MODE" (STRING.ZAL's features, docs/CHEATS.md),
        // keeps the tint off.
        constexpr uint16_t kFeatureNightVisionMode = 0x0008;


        // One of the motion values the camera uses this frame. (Each is
        // looked up on its own, mode and all, as the original does.)
        ee::Float motionValue(const ActionCamera *camera, ee::Float CameraSettings::*value)
        {
            if (cameraMode(camera, 0) != static_cast<uint32_t>(CameraMode::Follow))
            {
                if (cameraMode(camera, 0) == static_cast<uint32_t>(CameraMode::ActionShot))
                    return (*g_shotCameraSettings)[camera->shot].*value;
                return (*g_modeCameraSettings)[cameraMode(camera, 0) - 2].*value;
            }
            const uint32_t second = cameraMode(camera, 1);
            if (second == 3 || second == 4)
                return (*g_followSpecialCameraSettings)[(*g_layoutGroups)[static_cast<uint8_t>(camera->viewportLayout)]].*value;
            if (second == kFollowHidesRider)
                return (*g_followFifthCameraSettings).*value;
            return (*g_followCameraSettings)[(*g_layoutGroups)[static_cast<uint8_t>(camera->viewportLayout)]][camera->type].*value;
        }

        // The field of view a step nearer the wanted one. Answers whether
        // it was there already.
        bool easeFov(ActionCamera *camera, ee::Float share)
        {
            const ee::Float fov = camera->fov;
            const ee::Float wanted = camera->wantedFov;
            if (fov == wanted)
                return true;
            const ee::Float gap = wanted - fov;
            if (kFovStep < ee::abs(gap))
                camera->fov = fov + gap * share;
            else
                camera->fov = wanted;
            return false;
        }

        // The eye kept off the rider (a rider's camera that asks for it)
        // and out of the walls (unless it goes through them).
        void keepEyeClear(ActionCamera *camera, Vec4 *eye)
        {
            if ((camera->flags & kCameraKeepsOffRider) && camera->kind == 1)
                keepEyeOffRider(camera, rider::riderOf(static_cast<uint8_t>(camera->rider)), eye);
            if ((camera->flags & kCameraThroughWalls) == 0)
                pushEyeOutOfWalls(camera, eye);
        }

        // A rider's camera that has had the level between it and its rider
        // for too long: the follow mode cuts to where it wants to be
        // (unless the rider is in action state 8), and an action shot is
        // set up again, or marked blocked when it is held.
        void watchForBlockedView(ActionCamera *camera, Vec4 *riderAt)
        {
            const Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
            riderAt->x = rider->position[0] + rider->rootOffset[0];
            riderAt->y = rider->position[1] + rider->rootOffset[1];
            riderAt->z = rider->position[2] + rider->rootOffset[2];
            if (rider::collision::rayCast(&camera->eye.x, &riderAt->x) == nullptr)
                return;
            ++camera->blockedFrames;
            if (camera->blockedFrames <= kMostBlockedFrames)
                return;
            const uint32_t mode = cameraMode(camera, 0);
            if (mode == static_cast<uint32_t>(CameraMode::Follow))
            {
                if (rider::actionState(rider, 0) != kActionStateKeepsCamera)
                    camera->easing = CameraEasing::Cut;
            }
            else if (mode == static_cast<uint32_t>(CameraMode::ActionShot))
            {
                if (camera->flags & kCameraShotHeld)
                    camera->flags |= kCameraShotBlocked;
                else
                    setCameraShot(camera, camera->shot);
            }
        }

        // A rider's own full-screen camera decides whether the rider and
        // the bike are drawn: not in the follow mode's first-person views
        // nor in the action shot from the rider's eyes. In a shared screen
        // they stay as they are, and the bike casts no shadow.
        void showRiderFor(ActionCamera *camera)
        {
            const uint32_t layout = cameraViewportLayout(camera);
            const bool shared = layout < static_cast<uint32_t>(ViewportLayout::Inset) && layout >= static_cast<uint32_t>(ViewportLayout::Top);
            Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
            if (!shared)
            {
                bool hidden = false;
                switch (cameraMode(camera, 0))
                {
                case static_cast<uint32_t>(CameraMode::Follow):
                    if (cameraType(camera) == kTypeHidesRider)
                    {
                        rider::clearFlag(rider, kFlagRiderDrawn);
                        rider::clearFlag(rider, kFlagBikeDrawn);
                    }
                    hidden = cameraMode(camera, 1) == kFollowHidesRider;
                    break;
                case static_cast<uint32_t>(CameraMode::ActionShot):
                    hidden = cameraShot(camera) == kShotHidesRider;
                    break;
                default:
                    break;
                }
                if (hidden)
                {
                    rider::clearFlag(rider, kFlagRiderDrawn);
                    rider::clearFlag(rider, kFlagBikeDrawn);
                }
            }
            setSceneObjectShown(rider->messenger, rider->flags[0] & kFlagRiderDrawn.mask);
            setSceneObjectShown(rider->bikeModel, rider->flags[0] & kFlagBikeDrawn.mask);
            const bool shadow = rider::hasFlag(rider, kFlagBikeDrawn) && !rider::hasFlag(rider, rider::kFlagNoPlayer) && !shared;
            setObjectCastsShadow(rider->bikeModel, shadow ? 1 : 0);
        }
    }

    // updateCameras: a frame of every shown camera.
    //
    // The camera's mode runs first: it says where the eye and the point
    // looked at should be (or places the view itself, and then nothing more
    // is done). The view goes there as the camera's easing says:
    //
    //   Ease   the point looked at and an eased eye go a share of the way;
    //          the eye itself is on a spring to the eased eye, damped, kept
    //          off the rider and out of the walls. A rider's camera also
    //          watches for the level blocking its view of the rider.
    //   Snap   there at once; only the field of view eases.
    //   Swing  a share of the way that grows as a sine over 30 frames.
    //   Cut    there at once, kept off the rider and out of the walls, and
    //          the sound engine told not to bend pitches for the jump.
    //
    // Then the view's axes are aimed (unless the mode set them), a rider's
    // camera shows or hides its rider, the blur runs and the view is handed
    // to the scene camera. At the end the sound engine's listeners follow.
    void updateCameras()
    {
        ee::Local<Vec4> offset;
        ee::Local<Vec4> newEye;

        for (uint32_t index = 0; index < kActionCameraCount; ++index)
        {
            ActionCamera *camera = &(*g_actionCameras)[index];
            if ((camera->flags & kCameraShown) == 0)
                continue;

            CameraSettings motion;
            motion.distance = motionValue(camera, &CameraSettings::distance);
            motion.spring = motionValue(camera, &CameraSettings::spring);
            motion.damping = motionValue(camera, &CameraSettings::damping);
            motion.eyeShare = motionValue(camera, &CameraSettings::eyeShare);
            motion.lookAtShare = motionValue(camera, &CameraSettings::lookAtShare);
            motion.fovShare = motionValue(camera, &CameraSettings::fovShare);
            motion.fov = motionValue(camera, &CameraSettings::fov);
            if (cameraMode(camera, 0) != static_cast<uint32_t>(CameraMode::Idle) && cameraMode(camera, 0) != static_cast<uint32_t>(CameraMode::Animated))
                camera->wantedFov = motion.fov;

            camera->flags &= static_cast<uint8_t>(~(kCameraModePlacedAxes | kCameraModePlacedView));
            while ((*g_cameraModeFrames)[camera->modes[0]](camera) == 0)
            {
            }
            if (camera->flags & kCameraModePlacedView)
                continue;

            switch (camera->easing)
            {
            case CameraEasing::Ease:
            {
                camera->lookAt.x = camera->lookAt.x + (camera->wantedLookAt.x - camera->lookAt.x) * motion.lookAtShare;
                camera->lookAt.y = camera->lookAt.y + (camera->wantedLookAt.y - camera->lookAt.y) * motion.lookAtShare;
                camera->lookAt.z = camera->lookAt.z + (camera->wantedLookAt.z - camera->lookAt.z) * motion.lookAtShare;
                *newEye = camera->wantedEye;
                camera->easedEye.x = camera->easedEye.x + (newEye->x - camera->easedEye.x) * motion.eyeShare;
                camera->easedEye.y = camera->easedEye.y + (newEye->y - camera->easedEye.y) * motion.eyeShare;
                camera->easedEye.z = camera->easedEye.z + (newEye->z - camera->easedEye.z) * motion.eyeShare;
                camera->eyeVelocity.z = camera->eyeVelocity.z - camera->eyeVelocity.z * motion.damping;
                camera->eyeVelocity.x = camera->eyeVelocity.x - camera->eyeVelocity.x * motion.damping;
                camera->eyeVelocity.y = camera->eyeVelocity.y - camera->eyeVelocity.y * motion.damping;
                offset->x = camera->easedEye.x - camera->eye.x;
                offset->y = camera->easedEye.y - camera->eye.y;
                offset->z = camera->easedEye.z - camera->eye.z;
                const ee::Float pull = vec3Length(&offset->xyz()) * motion.spring;
                if (ee::Float(0.0f) < pull)
                {
                    ee::Float length;
                    *offset = unit(*offset, length);
                    camera->eyeVelocity.x = camera->eyeVelocity.x + offset->x * pull;
                    camera->eyeVelocity.y = camera->eyeVelocity.y + offset->y * pull;
                    camera->eyeVelocity.z = camera->eyeVelocity.z + offset->z * pull;
                }
                newEye->x = camera->eye.x + camera->eyeVelocity.x;
                newEye->y = camera->eye.y + camera->eyeVelocity.y;
                newEye->z = camera->eye.z + camera->eyeVelocity.z;
                keepEyeClear(camera, newEye);
                camera->eyeVelocity = *newEye - camera->eye;
                camera->eye = *newEye;
                if (camera->kind == 1)
                    watchForBlockedView(camera, offset);
                easeFov(camera, motion.fovShare);
                break;
            }
            case CameraEasing::Snap:
                camera->lookAt = camera->wantedLookAt;
                *newEye = camera->wantedEye;
                camera->eyeVelocity = *newEye - camera->eye;
                camera->eye = *newEye;
                camera->easedEye = *newEye;
                easeFov(camera, motion.fovShare);
                camera->easing = CameraEasing::Ease;
                break;
            case CameraEasing::Swing:
            {
                constexpr ee::Float kSwingFrames = std::bit_cast<float>(0x3D088889u); // 1 / 30
                const ee::Float turn = ee::Float(0.25f) * (unsignedToFloat(camera->modeFrames[1] + 1) * kSwingFrames);
                const ee::Float share = angleSin(static_cast<Angle>(ee::toInt(turn * kAnglesATurn)));
                camera->easedEye = camera->wantedEye;
                const uint64_t carried = rider::physics::upperHalf(camera->wantedEye);
                *offset = camera->wantedLookAt - camera->lookAt;
                *offset = scaledCarrying(*offset, share, carried);
                camera->lookAt = camera->lookAt + *offset;
                *offset = camera->wantedEye - camera->eye;
                camera->eyeVelocity = scaledCarrying(*offset, share, carried);
                camera->eye = camera->eye + camera->eyeVelocity;
                easeFov(camera, motion.fovShare);
                break;
            }
            case CameraEasing::Cut:
                camera->lookAt = camera->wantedLookAt;
                *newEye = camera->wantedEye;
                keepEyeClear(camera, newEye);
                camera->easedEye = *newEye;
                camera->eye = *newEye;
                camera->eyeVelocity = kZero;
                camera->easing = CameraEasing::Ease;
                camera->blockedFrames = 0;
                camera->fov = camera->wantedFov;
                disableDoppler();
                break;
            default:
                break;
            }

            if ((camera->flags & kCameraModePlacedAxes) == 0)
                aimCamera(camera);
            if (camera->kind == 1)
                showRiderFor(camera);
            updateCameraBlur(camera);
            camera->view.fov = static_cast<Angle>(ee::toInt(camera->fov * kAnglesADegree));
            camera->view.eye = camera->eye;
            camera->view.lookAt = camera->lookAt;
            camera->view.axes = camera->axes;
            setCameraView(camera, &camera->view);
            ++camera->modeFrames[0];
            ++camera->modeFrames[1];
        }
        updateCameraListeners();
    }
    READABLE_FUNCTION(updateCameras, Verified);

    // cameraTask: what is drawn over each shown camera's rectangle of the
    // screen every frame.
    //
    // A grey shade once the view is zoomed in under 35 degrees, stronger
    // the further in. And a tint over a rider's first-person view (the
    // follow mode with camera type 4, or with its second level 5) while
    // the rider has the flag for it, unless the saved game's flag keeps
    // it off.
    uint32_t cameraTask()
    {
        ee::Local<ViewQuad> quad;
        ee::Local<uint8_t[4]> shade;

        for (uint32_t index = 0; index < kActionCameraCount; ++index)
        {
            ActionCamera *camera = &(*g_actionCameras)[index];
            if ((camera->flags & kCameraShown) == 0)
                continue;

            const ee::Float degrees = ee::toFloat(camera->view.fov) * kDegreesAnAngle;
            if (degrees < kShadeFov)
            {
                shade[0] = kShadeGrey;
                shade[1] = kShadeGrey;
                shade[2] = kShadeGrey;
                shade[3] = static_cast<uint8_t>(ee::toInt((kShadeFov - degrees) * kShadeAlphaADegree));
                drawOverView(quad, camera, *g_cameraShadeMaterial, &shade[0]);
            }

            bool tinted = false;
            if ((g_savedGame->featuresOn & kFeatureNightVisionMode) == 0 && camera->kind == 1 &&
                rider::hasFlag(rider::riderOf(static_cast<uint8_t>(camera->rider)), rider::kFlagNightVision) &&
                cameraMode(camera, 0) == static_cast<uint32_t>(CameraMode::Follow))
            {
                tinted = cameraType(camera) == kTypeHidesRider || cameraMode(camera, 1) == kFollowHidesRider;
            }
            if (tinted)
                drawOverView(quad, camera, *g_cameraTintMaterial, &(*g_cameraTintColour)[0]);
        }
        return 1;
    }
    READABLE_FUNCTION(cameraTask, Verified);
}
