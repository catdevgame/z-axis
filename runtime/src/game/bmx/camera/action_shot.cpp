// BMX XXX's action cameras (action_camera.h): the action shots, the views
// that watch the rider from a place of their own
// (./src/com/action/camera.c). BMX XXX's own code.
//
// A shot has a number (ActionCamera::shot, 0 .. 14) and runs through four
// stages (the mode's second level): 0 and 1 for their seconds, 2 held for
// its seconds or until the rider is too far from the eye, 3 for its
// seconds; then the next shot is asked for. What a shot's view is:
//   0                   round the rider, a little further each frame
//   1, 4, 7, 10, 12-14  from a place on the level's faces ahead of the
//                       rider, found as the shot starts
//   2, 3, 5, 6, 8, 9    from the settings' offset beside the moving rider
//   11                  through the rider's eyes

#include "action_camera.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/physics/physics.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "bmx/action/camera_state.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/camera/action_shot.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::Rider;
    using rider::vec4;
    using rider::physics::upperHalf;

    namespace
    {
        constexpr uint32_t kShots = 15;
        inline ee::Global<CameraSettings[kShots], sym::g_shotCameraSettings> g_shotCameraSettings;

        // A shot's times, in seconds, and what ends its held stage early.
        struct ShotTiming
        {
            ee::Float leaveDistance; // +0x00 stage 2 ends when the rider is further than this from the eye (0: never)
            ee::Float heldSeconds;   // +0x04 stage 2's (0: until the rider is that far)
            ee::Float stage0Seconds; // +0x08
            ee::Float stage1Seconds; // +0x0C
            ee::Float stage3Seconds; // +0x10
            ee::Float fov;           // +0x14 the field of view stages 1 and 2 want, degrees (0: as it is)
        };
        static_assert(sizeof(ShotTiming) == 0x18);
        inline ee::Global<ShotTiming[kShots], sym::g_shotTimings> g_shotTimings;

        constexpr uint32_t kLastStage = 3;
        constexpr uint32_t kHeldStage = 2;
        constexpr ee::Float kFramesASecond = ee::Float(60.0f);
        constexpr uint32_t kBlurLeadFrames = 60; // the blur starts pulsing up this long before stage 3 ends
        // The last shot a held camera steps on from; past it the shots start over.
        constexpr uint32_t kLastSteppedShot = 11;
        constexpr uint32_t kShotRiderEyes = 11;

        // The rider's action state (level 0) in which the shots stop
        // leading the rider (update.cpp: a blocked follow camera stays
        // where it is in it).
        constexpr uint32_t kActionStateKeepsCamera = 8;

        constexpr ee::Float kWatchedLift = ee::Float(0.2f);  // the point looked at above the drawn rider, along its up
        constexpr ee::Float kDrawnPull = ee::Float(0.33f);   // the share of the way the point goes to the drawn body
        // A placed shot's eye is on the face under a point this many frames
        // of the rider's velocity ahead (a standing rider: this far along
        // its forward row), on a grid of 4 units, at the cells' middles.
        constexpr ee::Float kPlaceLeadFrames = ee::Float(25.0f);
        constexpr ee::Float kPlaceLeadStanding = ee::Float(2.5f);
        constexpr int32_t kPlaceGrid = 4;
        constexpr uint32_t kPlaceFaceKind = 2; // (what findSpawnFace is asked for here; the spawn asks 3)
        // The game camera's state (bmx.c's) in which a new shot leaves the blur alone.
        constexpr uint32_t kCameraStateKeepsBlur = 1;

        // How far above its face (y grows downwards) a placed shot's eye is.
        ee::Float placedShotLift(uint32_t shot)
        {
            switch (shot)
            {
            case 1:
                return ee::Float(-3.0f);
            case 10:
                return ee::Float(-1.2f);
            case 14:
                return ee::Float(-8.0f);
            default:
                return ee::Float(-5.0f);
            }
        }

        bool shotIsPlaced(uint32_t shot)
        {
            return shot == 1 || shot == 4 || shot == 7 || shot == 10 || shot == 12 || shot == 13 || shot == 14;
        }

        // A length across the ground to the grid: the cell's middle.
        int32_t gridMiddle(int32_t length) { return length / kPlaceGrid * kPlaceGrid + kPlaceGrid / 2; }
    }

    // setCameraShot: the camera's action shot, from its first stage, cut
    // to. Leaving the rider's eyes puts the eye back among the walls and
    // off the rider; taking them does the reverse. A placed shot finds its
    // eye here. A blur past its pulse is stopped, and a pulse going up
    // turns down (not for shots 13 and 14, nor while the game camera is in
    // state 1). The shot is no longer held or blocked.
    void setCameraShot(ActionCamera *camera, uint32_t shot)
    {
        ee::Local<Vec4> point;
        ee::Local<Matrix> faceRows;
        const Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
        camera->stageFrames = 0;
        setCameraMode(camera, 1, 0);
        camera->easing = CameraEasing::Cut;
        if (camera->shot == kShotRiderEyes)
            camera->flags = static_cast<uint8_t>((camera->flags | kCameraKeepsOffRider) & ~kCameraThroughWalls);
        if (static_cast<uint8_t>(camera->blurPhase) >= static_cast<uint8_t>(CameraBlur::Steady))
            camera->blurPhase = CameraBlur::None;
        if (shot == kShotRiderEyes)
        {
            camera->flags = static_cast<uint8_t>((camera->flags & ~kCameraKeepsOffRider) | kCameraThroughWalls);
        }
        else if (shotIsPlaced(shot))
        {
            int32_t x, z;
            if (!(rider->speed == ee::Float(0.0f)))
            {
                x = ee::toInt(rider->position[0]) + ee::toInt(rider->velocity[0] * kPlaceLeadFrames);
                z = ee::toInt(rider->position[2]) + ee::toInt(rider->velocity[2] * kPlaceLeadFrames);
            }
            else
            {
                x = ee::toInt(rider->position[0]) + ee::toInt(rider->forward[0] * kPlaceLeadStanding);
                z = ee::toInt(rider->position[2]) + ee::toInt(rider->forward[2] * kPlaceLeadStanding);
            }
            z = gridMiddle(z);
            x = gridMiddle(x);
            point->x = ee::toFloat(x);
            point->y = rider->position[1];
            point->z = ee::toFloat(z);
            const rider::collision::FaceRef face = rider::findSpawnFace(&point->x, kPlaceFaceKind, faceRows);
            rider::collision::faceCentre(face, &camera->wantedEye.x);
            camera->wantedEye.y = camera->wantedEye.y + placedShotLift(shot);
            if (cameraState() != kCameraStateKeepsBlur && shot < 13 && camera->blurPhase == CameraBlur::PulseUp)
            {
                camera->blurFrames = 0;
                camera->blurPhase = CameraBlur::PulseDown;
            }
        }
        camera->shot = static_cast<uint8_t>(shot);
        camera->flags &= static_cast<uint8_t>(~(kCameraShotHeld | kCameraShotBlocked));
    }
    READABLE_FUNCTION(setCameraShot, Verified);

    // actionShotCameraMode: the action shot mode's frame. A blocked shot
    // (the frame's update found the level between the eye and the rider,
    // or the shot's last stage ran out) is replaced first: a held camera
    // steps to the next shot (after shot 11, shot 0) and stays held, any
    // other takes its own shot again. Then the stage is moved on when its
    // time is up, stages 1 and 2 ask for the shot's field of view, stage 3
    // starts the blur's pulse a second before it ends, and the shot sets
    // what the camera wants.
    uint32_t actionShotCameraMode(ActionCamera *camera)
    {
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> offset;
        ee::Local<Matrix> basis;
        Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
        *offset = (*g_shotCameraSettings)[camera->shot].offset;

        Vec4 &lookAt = camera->wantedLookAt;
        lookAt.x = rider->position[0] + rider->rootOffset[0] + rider->up[0] * kWatchedLift;
        lookAt.y = rider->position[1] + rider->rootOffset[1] + rider->up[1] * kWatchedLift;
        lookAt.z = rider->position[2] + rider->rootOffset[2] + rider->up[2] * kWatchedLift;
        if (rider::actionState(rider, rider::kLevelAction) != kActionStateKeepsCamera)
        {
            lookAt.x = lookAt.x + (rider->drawnPosition[0] - lookAt.x) * kDrawnPull;
            lookAt.y = lookAt.y + (rider->drawnPosition[1] - lookAt.y) * kDrawnPull;
            lookAt.z = lookAt.z + (rider->drawnPosition[2] - lookAt.z) * kDrawnPull;
        }

        if (camera->flags & kCameraShotBlocked)
        {
            if (camera->flags & kCameraShotHeld)
            {
                if (camera->shot <= kLastSteppedShot)
                    setCameraShot(camera, camera->shot + 1u);
                else
                    setCameraShot(camera, 0);
                camera->flags |= kCameraShotHeld;
            }
            else
            {
                setCameraShot(camera, camera->shot);
            }
            camera->easing = CameraEasing::Cut;
            camera->flags &= static_cast<uint8_t>(~kCameraShotBlocked);
        }

        const ShotTiming &timing = (*g_shotTimings)[camera->shot];
        ee::Float seconds;
        for (;;)
        {
            const uint32_t stage = cameraMode(camera, 1);
            if (stage == 1)
                seconds = timing.stage1Seconds;
            else if (stage == 0)
                seconds = timing.stage0Seconds;
            else if (stage == kHeldStage)
            {
                seconds = timing.heldSeconds;
                if (ee::Float(0.0f) < timing.leaveDistance)
                {
                    *scratch = camera->wantedLookAt - camera->wantedEye;
                    if (timing.leaveDistance * timing.leaveDistance < squaredLength(*scratch))
                    {
                        setCameraMode(camera, 1, kLastStage);
                        camera->stageFrames = 0;
                        seconds = timing.stage3Seconds;
                    }
                }
            }
            else if (stage == kLastStage)
                seconds = timing.stage3Seconds;
            else
                seconds = ee::Float(-1.0f);

            // (A time under zero never runs out: the count is unsigned.)
            if (camera->stageFrames < static_cast<uint32_t>(ee::toInt(seconds * kFramesASecond)))
                break;
            if (!(ee::Float(0.0f) < seconds) && cameraMode(camera, 1) == kHeldStage)
                break;
            if (!(cameraMode(camera, 1) < kLastStage))
            {
                camera->flags |= kCameraShotBlocked;
                break;
            }
            setCameraMode(camera, 1, cameraMode(camera, 1) + 1u);
            camera->stageFrames = 0;
        }

        const uint32_t stage = cameraMode(camera, 1);
        if (stage == 1 || stage == kHeldStage)
        {
            if (!(timing.fov == ee::Float(0.0f)))
                camera->wantedFov = timing.fov;
        }
        else if (stage == kLastStage)
        {
            if (camera->stageFrames == static_cast<uint32_t>(ee::toInt(seconds * kFramesASecond)) - kBlurLeadFrames)
            {
                camera->blurFrames = 0;
                camera->blurPhase = CameraBlur::PulseUp;
            }
        }

        const uint32_t shot = camera->shot;
        if (shot == 0)
        {
            // Round the rider: three up, five out, looking a quarter of a
            // frame's move ahead of it.
            *scratch = *vec4(rider->velocity);
            *scratch = scaledCarrying(*scratch, ee::Float(0.25f), upperHalf(*scratch));
            lookAt = lookAt + *scratch;
            camera->wantedEye.z = ee::Float(0.0f);
            camera->wantedEye.x = ee::Float(5.0f);
            camera->wantedEye.y = ee::Float(-3.0f);
            camera->orbitAngle = static_cast<Angle>(camera->orbitAngle + 300);
            vec3RotateY(&camera->wantedEye.xyz(), &camera->wantedEye.xyz(), camera->orbitAngle);
            camera->wantedEye = camera->wantedEye + *vec4(rider->position);
        }
        else if (shot == kShotRiderEyes)
        {
            riderViewCamera(camera, rider);
            camera->flags |= kCameraModePlacedAxes;
        }
        else if (shot == 12 || shot == 13)
        {
            camera->wantedFov = ee::Float(75.0f);
        }
        else if (shotIsPlaced(shot))
        {
            // From its place, looking a frame and a half ahead of the
            // rider. Shot 10 sinks slowly; shot 14 looks closer.
            *scratch = *vec4(rider->velocity);
            *scratch = scaledCarrying(*scratch, ee::Float(1.5f), upperHalf(*scratch));
            lookAt = lookAt + *scratch;
            if (shot == 10)
                camera->wantedEye.y = camera->wantedEye.y - ee::Float(0.05f);
            else if (shot == 14)
                camera->wantedFov = ee::Float(30.0f);
        }
        else if (rider::actionState(rider, rider::kLevelAction) != kActionStateKeepsCamera)
        {
            // Beside the rider: the settings' offset in the rider's axes
            // at the drawn rider, with their forward row along the velocity
            // (and the right row squared to it) while the rider moves. The
            // point looked at leads by some frames of the velocity.
            *scratch = *vec4(rider->velocity);
            *scratch = scaledCarrying(*scratch, shot == 6 ? ee::Float(7.5f) : ee::Float(1.75f), upperHalf(*scratch));
            lookAt = lookAt + *scratch;
            *basis = *rider::matrix(rider->right);
            if (!(rider->speed == ee::Float(0.0f)))
            {
                basis->row(2) = *vec4(rider->velocity);
                basis->row(2) = unit(basis->row(2));
                basis->row(0) = cross(basis->row(1), basis->row(2));
            }
            basis->row(3) = *vec4(rider->position);
            basis->row(3) = basis->row(3) + *vec4(rider->rootOffset);
            camera->wantedEye = transformed(*basis, *offset);
        }
        else
        {
            // In that action state the eye backs off instead: three units
            // from the rider's position on the side it is on now and seven
            // up (five along x when it is not off the rider on both x and z).
            *scratch = *vec4(rider->position);
            vec3Negate(&scratch->xyz(), &scratch->xyz());
            *scratch = *scratch + camera->wantedEye;
            scratch->y = ee::Float(0.0f);
            if (!(scratch->x == ee::Float(0.0f)) && !(scratch->z == ee::Float(0.0f)))
            {
                *scratch = unit(*scratch);
                *scratch = scaledAfterMeasure(*scratch, ee::Float(3.0f));
            }
            else
            {
                scratch->x = ee::Float(5.0f);
            }
            scratch->y = ee::Float(-7.0f);
            *scratch = *scratch + *vec4(rider->position);
            camera->wantedEye = *scratch;
        }
        ++camera->stageFrames;
        return 1;
    }
    READABLE_FUNCTION(actionShotCameraMode, Verified);
}
