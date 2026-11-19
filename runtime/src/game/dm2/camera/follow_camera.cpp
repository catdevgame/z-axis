// DM2's follow camera (follow_camera.h). DM2's own code (camera.c).
//
// Each frame, for the player the camera shows: the settings row of what the
// camera is doing, then where it wants to be and to look (by its level-0
// mode, and under it the rider's mode, the view or the ground camera's
// kind), then its move there (at once at a cut; else eased, kept off the
// rider and out of the walls), and last the camera state: the place, the
// point looked at, the rows and the field of view, applied to the camera
// node.

#include "follow_camera.h"

#include "camera.h"

#include "dm2/game/npcs.h"
#include "dm2/game/players.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"
#include "zge/sound/sound.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/camera/follow_camera.cpp.h"

namespace game::dm2
{
    using rider::Rider;
    using rider::vec4;

    namespace
    {
        // The settings table: a row for each ground camera kind, then the
        // rider's modes 1 and 2 while the camera follows, then the game's
        // modes from 2, then the follow views.
        constexpr uint32_t kRiderModeRows = 12; // + the rider's (level-1) mode
        constexpr uint32_t kGameModeRows = 13;  // + the game's (level-0) mode
        constexpr uint32_t kViewRows = 22;      // + the view
        ee::Ptr<CameraSettings> cameraSettings(uint32_t row)
        {
            return ee::Ptr<CameraSettings>(sym::g_cameraSettings) + static_cast<int32_t>(row);
        }

        // The game's (level-0) modes camera.h does not name.
        // Scaffolding (unnamed-field): named by value; what each does here is
        // in followPlayer.
        constexpr uint8_t kCameraMode2 = 2, kCameraMode3 = 3, kCameraMode5 = 5, kCameraMode6 = 6, kCameraMode7 = 7, kCameraMode8 = 8;
        // The rider's (level-1) modes while the camera follows; a ground
        // camera that stands counts its stages in the same byte.
        constexpr uint8_t kRiderRiding = 0, kRiderBailStarts = 1;
        constexpr uint8_t kStageNew = 0, kStageWatching = 1, kStageLeaving = 2;
        // The follow views with their own code: 2 looks further ahead.
        constexpr uint8_t kViewFarAhead = 2;

        // The NPC the game is showing: the camera's modes 6 and 7 follow its rider.
        inline ee::Global<uint8_t, sym::g_shownNpc> g_shownNpc;

        // The level's camera points (five a level: levelCameraPoint) and which
        // one a level-point camera takes next.
        constexpr uint32_t kLevelCameraPoints = 5;
        inline ee::Global<uint8_t, sym::g_levelCameraPoint> g_levelCameraPoint;
        constexpr ee::Float kLevelPointLift = -5.0f; // (y grows downwards)
        // Whether the pad's zoom is held.
        inline ee::Global<uint32_t, sym::g_cameraZoomHeld> g_cameraZoomHeld;

        // The camera's own numbers (the game keeps them as variables).
        // How many frames of the rider's velocity the point looked at leads by.
        inline ee::Global<ee::Float, sym::g_targetLead> g_targetLead;
        // The nearest the camera comes to the rider, across the ground.
        inline ee::Global<ee::Float, sym::g_leastRiderDistance> g_leastRiderDistance;
        // The camera's radius against the walls.
        inline ee::Global<ee::Float, sym::g_cameraRadius> g_cameraRadius;
        // How far above the rider the follow camera stands while the rider is off the ground.
        inline ee::Global<ee::Float, sym::g_offGroundHeight> g_offGroundHeight;
        // Scaffolding (unnamed-field): where mode 8 puts the camera and its
        // target (10000 below the world's origin); what mode 8 is for is not read.
        inline ee::Global<zge::Vec4, sym::g_cameraMode8Place> g_cameraMode8Place;

        constexpr ee::Float kTurn = 65536.0f;           // an angle's units a turn
        constexpr ee::Float kUnitsPerDegree = 182.04445f; // 0x10000 / 360
        constexpr ee::Float kDrawnPull = 0.33f;         // the share of the way the target goes to the drawn body
        constexpr ee::Float kSeatLift = 0.8f;           // the target above a coasting rider's position, along its up
        constexpr ee::Float kChestLift = 0.2f;          // the target above the rider for the cameras that watch it
        constexpr ee::Float kFallAllowance = 0.15f;     // an airborne rider's fall speed the target's lead leaves out
        constexpr ee::Float kStandingLead = 0.01f;      // a standing rider leads the far view along its forward row
        constexpr ee::Float kNoDirection = 0.00001f;    // a direction shorter than this on an axis is none
        constexpr ee::Float kEyesAhead = 5.0f;          // the rider's eyes look this far along their forward row
        constexpr ee::Float kCoastHeight = 0.2f;        // the follow camera's height behind a coasting rider
        constexpr ee::Float kContactHeight = 1.5f;      // and off the ground with a contact of kind 1 or 3
        constexpr ee::Float kBailTargetLift = -0.25f;
        constexpr ee::Float kFieldOfViewStep = 0.1f;    // nearer than this to the wanted field of view it is taken at once
        constexpr ee::Float kNearVertical = 0.999f;     // a view this close to straight up or down has no (0, -1, 0) up
        constexpr ee::Float kWallReach = 1.5f;          // walls are looked for within this many camera radii
        constexpr uint32_t kBlockedLimit = 41;        // frames of a blocked view before the camera gives up its place
        constexpr int32_t kCircleStep = 300;            // the circling cameras' turn a frame
        constexpr int32_t kLevelPointStep = -0x32;      // the level-point camera's

        // The frame's working values, which the steps below share.
        struct Follow
        {
            uint32_t player;
            PlayerCamera &camera;
            ee::Ptr<Rider> rider;   // none for a level-point camera
            zge::Vec4 &scratch;
            zge::Vec4 &eye;         // where the camera goes this frame
            zge::Vec4 &offset;      // the settings' offset (a view changes its height)
            zge::Matrix &basis;     // the rider's space the offset is in
            zge::Matrix &turn;
            FollowRates &rates;
            zge::Vec4 &normal;      // a wall face's
            NpcInfo &npc;
            uint32_t &rowsSet;      // 1: the camera's rows are the rider's own, not worked out from the target
        };

        // v scaled as the engine's macro does it.
        // Scaffolding (t0-leftover): the upper half the last call left.
        zge::Vec4 scaled(const zge::Vec4 &v, ee::Float factor)
        {
            return zge::scaledCarrying(v, factor, zge::scaleLeftover(zge::LeftBy::lastCall));
        }

        // x, y and z of `v` moved `share` of the way to `toward`.
        void easeToward(zge::Vec4 &v, const ee::Float *toward, ee::Float share)
        {
            v.x = v.x + (toward[0] - v.x) * share;
            v.y = v.y + (toward[1] - v.y) * share;
            v.z = v.z + (toward[2] - v.z) * share;
        }

        // Where the rider is drawn: its physics position and the motion's root offset.
        zge::Vec3 drawnPlace(const Rider *rider)
        {
            return {rider->position[0] + rider->rootOffset[0], rider->position[1] + rider->rootOffset[1],
                    rider->position[2] + rider->rootOffset[2]};
        }
        // The same, `lift` along the rider's up row.
        zge::Vec3 drawnPlaceLifted(const Rider *rider, ee::Float lift)
        {
            zge::Vec3 place = drawnPlace(rider);
            place.x = place.x + rider->up[0] * lift;
            place.y = place.y + rider->up[1] * lift;
            place.z = place.z + rider->up[2] * lift;
            return place;
        }
        // The rider's physics position, `lift` along its up row.
        zge::Vec3 positionLifted(const Rider *rider, ee::Float lift)
        {
            return {rider->position[0] + rider->up[0] * lift, rider->position[1] + rider->up[1] * lift,
                    rider->position[2] + rider->up[2] * lift};
        }

        // How far the camera's wanted place is from the drawn rider across
        // the ground, squared (the offset's x and z are left in `scratch`).
        ee::Float groundDistanceSquared(Follow &f)
        {
            const PlayerCamera &camera = f.camera;
            const ee::Float x = camera.wantedPosition.x - (f.rider->position[0] + f.rider->rootOffset[0]);
            f.scratch.x = x;
            const ee::Float z = camera.wantedPosition.z - (f.rider->position[2] + f.rider->rootOffset[2]);
            f.scratch.z = z;
            return x * x + z * z;
        }

        // Riding along with no motion running over the coast: the rider sits still on the bike.
        bool coasting(const Rider *rider)
        {
            return rider::actionState(rider, rider::kLevelAction) == rider::kActionRiding &&
                   rider::actionState(rider, rider::kLevelSubState) == rider::kRidingCoast &&
                   rider::actionState(rider, rider::kLevelPhase) == rider::kPhaseHeld;
        }

        // The camera's place: the settings' offset taken from the rider's space to the world.
        void placeByOffset(Follow &f)
        {
            zge::transformPoint(ee::Ptr<zge::Vec4>(&f.camera.wantedPosition), ee::Ptr<const zge::Matrix>(&f.basis),
                                ee::Ptr<const zge::Vec4>(&f.offset));
        }

        // The settings for what the player's camera is doing: its row's six
        // rates and its offset. The game reads them rate by rate, asking for
        // the camera's modes again for each rate (and, in a game mode, once
        // more for the rate's row and once more for the offset's), and copies
        // the row's offset on every turn.
        void readSettings(Follow &f)
        {
            const int32_t player = static_cast<int32_t>(f.player);
            ee::Float *const rates = &f.rates.pull; // the six rates in their order
            constexpr uint32_t kRates = sizeof(FollowRates) / sizeof(ee::Float);
            for (uint32_t rate = 0; rate < kRates; ++rate)
            {
                if (cameraMode(player, 0) == kCameraFollows)
                {
                    const uint8_t riderMode = cameraMode(player, 1);
                    const uint32_t row = riderMode == kRiderBailStarts || riderMode == kRiderResting
                                             ? kRiderModeRows + riderMode
                                             : kViewRows + f.camera.view;
                    rates[rate] = (&cameraSettings(row)->rates.pull)[rate];
                    f.offset = cameraSettings(row)->offset;
                }
                else if (cameraMode(player, 0) == kCameraOnGround)
                {
                    rates[rate] = (&cameraSettings(f.camera.groundKind)->rates.pull)[rate];
                    f.offset = cameraSettings(f.camera.groundKind)->offset;
                }
                else
                {
                    rates[rate] = (&cameraSettings(kGameModeRows + cameraMode(player, 0))->rates.pull)[rate];
                    f.offset = cameraSettings(kGameModeRows + cameraMode(player, 0))->offset;
                }
            }
        }

        // Behind a riding rider. The point looked at is the drawn rider (or,
        // coasting, the seat), pulled toward the drawn body off the ground or
        // moving, and led by the velocity as the view says. The camera stands
        // at the settings' offset in a space that faces along the velocity,
        // level: forward flat, up straight, at the drawn rider.
        // Kept from the original: with no direction to face, the forward row
        // is taken and then overwritten by the rider's up row negated.
        void followRiding(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            const ee::Ptr<Rider> rider = f.rider;
            if (coasting(rider))
                camera.wantedTarget.xyz() = positionLifted(rider, kSeatLift);
            else
            {
                camera.wantedTarget.xyz() = drawnPlace(rider);
                if (rider::physicsState(rider) != rider::kPhysicsGround || !(rider->speed == 0.0f))
                    easeToward(camera.wantedTarget, rider->drawnPosition, kDrawnPull);
            }

            if (camera.view == kViewFarAhead)
            {
                // Twice the lead, level; a standing rider leads along its forward row.
                if (rider->speed == 0.0f)
                    f.scratch = scaled(*vec4(rider->forward), kStandingLead);
                else
                    f.scratch = *vec4(rider->velocity);
                const ee::Float lead = *g_targetLead;
                f.scratch.y = 0.0f;
                f.scratch = scaled(f.scratch, lead + lead);
                camera.wantedTarget = camera.wantedTarget + f.scratch;
            }
            else if (camera.view == kRiderEyesView)
            {
                // The camera's rows are the rider's, turned by the bars' lean
                // (as riderViewCamera's); it looks ahead along them.
                zge::copyRotationAndTranslation(&f.basis, rider->right);
                f.rowsSet = 1;
                zge::matrixIdentity(&f.turn);
                zge::matrixRotateY(&f.turn, &f.turn, static_cast<zge::Angle>(ee::toInt(rider->barLean[1] * kTurn)));
                rider::physics::engine::matrixProduct(&f.basis, &f.basis, &f.turn);
                zge::matrixIdentity(&f.turn);
                zge::matrixRotateX(&f.turn, &f.turn, static_cast<zge::Angle>(ee::toInt(rider->barLean[0] * kTurn)));
                rider::physics::engine::matrixProduct(&f.basis, &f.basis, &f.turn);
                const zge::Vec4 &ahead = f.basis.row(2);
                camera.wantedTarget.x = camera.wantedTarget.x + (rider->up[0] * kSeatLift + ahead.x * kEyesAhead);
                camera.wantedTarget.y = camera.wantedTarget.y + (rider->up[1] * kSeatLift + ahead.y * kEyesAhead);
                camera.wantedTarget.z = camera.wantedTarget.z + (rider->up[2] * kSeatLift + ahead.z * kEyesAhead);
                zge::copyRotationAndTranslation(&camera.rows, &f.basis);
            }
            else
            {
                // The lead is level but for an airborne rider's fall past the allowance.
                f.scratch = *vec4(rider->velocity);
                if (rider::physicsState(rider) == rider::kPhysicsAir && kFallAllowance < rider->velocity[1])
                    f.scratch.y = f.scratch.y - kFallAllowance;
                else
                    f.scratch.y = 0.0f;
                f.scratch = scaled(f.scratch, *g_targetLead);
                camera.wantedTarget = camera.wantedTarget + f.scratch;
            }

            // The space the offset is in.
            zge::copyRotationAndTranslation(&f.basis, rider->right);
            zge::Vec4 &right = f.basis.row(0);
            zge::Vec4 &up = f.basis.row(1);
            zge::Vec4 &forward = f.basis.row(2);
            if (!(rider->speed == 0.0f))
                forward = *vec4(rider->velocity);
            forward.y = 0.0f;
            if (ee::abs(forward.x) < kNoDirection && ee::abs(forward.z) < kNoDirection)
            {
                forward = *vec4(rider->forward);
                forward.y = 0.0f;
                zge::vec3Negate(&forward.xyz(), &rider::vec4(rider->up)->xyz());
            }
            forward = zge::unit(forward);
            up.x = 0.0f;
            up.y = -1.0f;
            up.z = 0.0f;
            right = zge::cross(up, forward);
            right = zge::unit(right);
            forward = zge::cross(right, up);
            zge::Vec4 &origin = f.basis.row(3);
            if (coasting(rider))
                origin.xyz() = positionLifted(rider, kSeatLift);
            else
            {
                origin = *vec4(rider->position);
                origin = origin + *vec4(rider->rootOffset);
            }

            if (camera.view == kRiderEyesView)
            {
                zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), 0);
                zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->bikeModel), 0);
            }
            else if (camera.view != kViewFarAhead)
            {
                // The views behind the rider change their height: low behind
                // a coasting rider, high off the ground (less high with a
                // contact of kind 1 or 3 in the air and on the approaches).
                if (coasting(rider))
                    f.offset.y = kCoastHeight;
                const uint8_t state = rider::physicsState(rider);
                if (state == rider::kPhysicsAir || (state >= rider::kPhysicsGrindApproach && state <= rider::kPhysicsStall))
                {
                    f.offset.y = *g_offGroundHeight;
                    const uint8_t now = rider::physicsState(rider);
                    if ((now == rider::kPhysicsAir || now == rider::kPhysicsGrindApproach || now == rider::kPhysicsStallApproach) &&
                        (rider->contactKind == 1 || rider->contactKind == 3))
                        f.offset.y = kContactHeight;
                }
            }
            placeByOffset(f);
        }

        // As a bail starts: the camera looks at the rider's position, a
        // little up, from the settings' offset in a level space that faces
        // from the camera to the rider.
        // Kept from the original: that direction is used only when it runs
        // straight along x (no z at all); any other gets the rider's forward
        // row, as a camera right above the rider does.
        void followBail(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            const ee::Ptr<Rider> rider = f.rider;
            camera.wantedTarget = *vec4(rider->position);
            camera.wantedTarget.y = camera.wantedTarget.y + kBailTargetLift;
            zge::copyRotationAndTranslation(&f.basis, rider->right);
            zge::Vec4 &right = f.basis.row(0);
            zge::Vec4 &up = f.basis.row(1);
            zge::Vec4 &forward = f.basis.row(2);
            forward = *vec4(rider->position) - camera.wantedPosition;
            forward.y = 0.0f;
            if (!(forward.x == forward.y) && forward.z == forward.y)
                forward = zge::unit(forward);
            else
            {
                forward = *vec4(rider->forward);
                forward.y = 0.0f;
                forward = zge::unit(forward);
            }
            up.x = 0.0f;
            up.y = -1.0f;
            up.z = 0.0f;
            right = zge::cross(up, forward);
            right = zge::unit(right);
            f.basis.row(3) = *vec4(rider->position);
            placeByOffset(f);
        }

        // The game's mode 0: behind the rider, as the rider's mode says.
        void followRider(Follow &f)
        {
            switch (cameraMode(static_cast<int32_t>(f.player), 1))
            {
            case kRiderRiding:
                followRiding(f);
                break;
            case kRiderBailStarts:
                followBail(f);
                break;
            case kRiderResting:
                riderViewCamera(&f.camera, f.rider);
                f.rowsSet = 1;
                break;
            default:
                break;
            }
        }

        // The eye a fixed distance from a point, turned a step further round it each frame.
        void circle(PlayerCamera &camera, zge::Vec4 &eye, ee::Float away, ee::Float lift, int32_t step)
        {
            camera.angle = static_cast<int16_t>(static_cast<uint16_t>(camera.angle) + step);
            eye.x = away;
            eye.y = lift;
            eye.z = 0.0f;
            zge::vec3RotateY(&eye.xyz(), &eye.xyz(), camera.angle);
        }

        // A ground camera asks for a new place: at once (the same kind
        // again) when a mode change did not place it, else flagged for the
        // next frame's kind.
        void groundCameraMoves(Follow &f, uint32_t kind)
        {
            if (f.camera.flags & kFlagGroundPlaced)
                f.camera.flags |= kFlagGroundMoves;
            else
                placeGroundCamera(f.player, kind);
        }

        // Ground kind 0: circles the rider, close and low, looking a little
        // ahead of it; after 360 frames it wants a new place.
        void groundCircles(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            f.scratch = *vec4(f.rider->velocity);
            f.scratch = scaled(f.scratch, ee::Float(0.25f));
            camera.wantedTarget = camera.wantedTarget + f.scratch;
            circle(camera, camera.wantedPosition, ee::Float(5.0f), ee::Float(-3.0f), kCircleStep);
            camera.wantedPosition = camera.wantedPosition + *vec4(f.rider->position);
            if (!(camera.frames < 361))
                camera.flags |= kFlagGroundMoves;
        }

        // Ground kinds 2, 3, 5, 7 and 8 (and any past 12): rides along at the
        // settings' offset in the rider's space, facing along the velocity
        // (not levelled), looking ahead of the rider (kind 5 far ahead).
        // Beside a bailed rider it stays 3 away across the ground on the
        // side it is on (straight on an axis from the rider: 5 along x), 7 up.
        // After 300 frames it wants a new place.
        void groundRidesAlong(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            const ee::Ptr<Rider> rider = f.rider;
            if (rider::physicsState(rider) != rider::kPhysicsBail)
            {
                f.scratch = *vec4(rider->velocity);
                f.scratch = scaled(f.scratch, camera.groundKind == 5 ? ee::Float(7.5f) : ee::Float(1.75f));
                camera.wantedTarget = camera.wantedTarget + f.scratch;
                zge::copyRotationAndTranslation(&f.basis, rider->right);
                if (!(rider->speed == 0.0f))
                {
                    zge::Vec4 &forward = f.basis.row(2);
                    forward = *vec4(rider->velocity);
                    forward = zge::unit(forward);
                    f.basis.row(0) = zge::cross(f.basis.row(1), forward);
                }
                zge::Vec4 &origin = f.basis.row(3);
                origin = *vec4(rider->position);
                origin = origin + *vec4(rider->rootOffset);
                placeByOffset(f);
            }
            else
            {
                zge::Vec4 &away = f.scratch;
                away = *vec4(rider->position);
                zge::vec3Negate(&away.xyz(), &away.xyz());
                away = away + camera.wantedPosition;
                away.y = 0.0f;
                if (away.x == away.y || away.z == away.y)
                    away.x = 5.0f;
                else
                {
                    away = zge::unit(away);
                    away = scaled(away, ee::Float(3.0f));
                }
                away.y = -7.0f;
                away = away + *vec4(rider->position);
                camera.wantedPosition = away;
            }
            if (!(camera.frames < 301))
                camera.flags |= kFlagGroundMoves;
        }

        // Ground kinds 1, 4, 6, 9 and 12: stands where it was placed and
        // watches the rider, looking a little ahead, in stages. New: after 5
        // frames it watches. Watching, from frame 121 on: a rider within
        // 13 (kind 1) or 20 across the ground gets the kind's field of view
        // (kind 9 leaves after 600 frames instead); a rider further away
        // makes it leave. Leaving: it wants a new place, kind 1 after 60
        // frames, kind 6 after 20 at a wide field of view, the rest at
        // once. Kind 9 rises all the while.
        void groundStands(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            f.scratch = *vec4(f.rider->velocity);
            f.scratch = scaled(f.scratch, ee::Float(1.5f));
            camera.wantedTarget = camera.wantedTarget + f.scratch;
            switch (cameraMode(static_cast<int32_t>(f.player), 1))
            {
            case kStageNew:
                if (!(camera.frames < 5))
                    setCameraMode(f.player, 1, kStageWatching);
                break;
            case kStageWatching:
            {
                const uint32_t frames = camera.frames;
                if (frames < 121)
                    break;
                const ee::Float distanceSquared = groundDistanceSquared(f);
                const ee::Float reach = camera.groundKind == 1 ? ee::Float(13.0f) : ee::Float(20.0f);
                bool leaves = !(distanceSquared < reach * reach);
                if (!leaves)
                {
                    switch (camera.groundKind)
                    {
                    case 1:
                        camera.wantedFieldOfView = 60.0f;
                        break;
                    case 4:
                        camera.wantedFieldOfView = 15.0f;
                        break;
                    case 6:
                    case 12:
                        camera.wantedFieldOfView = 30.0f;
                        break;
                    case 9:
                        leaves = !(frames < 601);
                        break;
                    default:
                        break;
                    }
                }
                if (leaves)
                {
                    setCameraMode(f.player, 1, kStageLeaving);
                    camera.frames = 0;
                }
                break;
            }
            case kStageLeaving:
            {
                const uint8_t kind = camera.groundKind;
                camera.flags &= static_cast<uint8_t>(~kFlagGroundMoves);
                if (kind == 1)
                {
                    if (!(camera.frames < 60))
                        camera.flags |= kFlagGroundMoves;
                }
                else if (kind == 6)
                {
                    if (!(camera.frames < 20))
                        camera.flags |= kFlagGroundMoves;
                    camera.wantedFieldOfView = 60.0f;
                }
                else
                    camera.flags |= kFlagGroundMoves;
                if ((camera.flags & kFlagGroundMoves) && !(camera.flags & kFlagGroundPlaced))
                    placeGroundCamera(f.player, kind);
                break;
            }
            default:
                break;
            }
            if (camera.groundKind == 9)
                camera.wantedPosition.y = camera.wantedPosition.y + ee::Float(-0.05f);
        }

        // Ground kinds 10 and 11: stands where it was placed; from 121
        // frames on, a rider 15 (kind 10) or 30 away across the ground makes
        // it want a new place. Kind 11 zooms in while the pad's zoom is held.
        void groundStandsFar(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            if (!(camera.frames < 121))
            {
                const uint8_t kind = camera.groundKind;
                const ee::Float distanceSquared = groundDistanceSquared(f);
                const ee::Float reachSquared = kind == 10 ? ee::Float(225.0f) : ee::Float(900.0f);
                if (!(distanceSquared < reachSquared))
                    groundCameraMoves(f, kind);
            }
            if (camera.groundKind == 11 && *g_cameraZoomHeld != 0 && camera.frames != 0)
                camera.wantedFieldOfView = 15.0f;
        }

        // The game's mode 1: on the ground near the rider, looking at the
        // drawn rider's chest (pulled toward the drawn body unless it
        // bailed), as the camera's kind says.
        void groundCamera(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            camera.wantedTarget.xyz() = drawnPlaceLifted(f.rider, kChestLift);
            if (rider::physicsState(f.rider) != rider::kPhysicsBail)
                easeToward(camera.wantedTarget, f.rider->drawnPosition, kDrawnPull);
            switch (camera.groundKind)
            {
            case 0:
                groundCircles(f);
                break;
            case 1:
            case 4:
            case 6:
            case 9:
            case 12:
                groundStands(f);
                break;
            case 10:
            case 11:
                groundStandsFar(f);
                break;
            default:
                groundRidesAlong(f);
                break;
            }
        }

        // The game's mode 3: circles the drawn rider, 5 away and 7 up.
        void circleRider(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            camera.wantedTarget.xyz() = drawnPlace(f.rider);
            circle(camera, camera.wantedPosition, ee::Float(5.0f), ee::Float(-7.0f), kCircleStep);
            camera.wantedPosition = camera.wantedPosition + *vec4(f.rider->position);
        }

        // The game's mode 4: at one of the level's camera points, lifted
        // (the next one, at a cut and from a random angle, whenever the low
        // byte of its frame count is 0), looking at a point that circles it.
        void levelPointCamera(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            if ((camera.frames & 0xFFu) == 0)
            {
                const uint8_t point = *g_levelCameraPoint;
                camera.wantedPosition = levelCameraPoint(point);
                camera.cut = 1;
                camera.wantedPosition.y = camera.wantedPosition.y + kLevelPointLift;
                *g_levelCameraPoint = static_cast<uint8_t>((point + 1) % static_cast<int32_t>(kLevelCameraPoints));
                camera.angle = static_cast<int16_t>(zge::randomNumber());
            }
            circle(camera, camera.wantedTarget, ee::Float(5.0f), ee::Float(2.0f), kLevelPointStep);
            camera.wantedTarget = camera.wantedTarget + camera.wantedPosition;
        }

        // The game's modes 6 and 7 (the shown NPC's rider): at the settings'
        // offset in the rider's own space, looking at its chest.
        void besideRider(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            camera.wantedTarget.xyz() = drawnPlaceLifted(f.rider, kChestLift);
            easeToward(camera.wantedTarget, f.rider->drawnPosition, kDrawnPull);
            zge::copyRotationAndTranslation(&f.basis, f.rider->right);
            zge::Vec4 &origin = f.basis.row(3);
            origin = *vec4(f.rider->position);
            origin = origin + *vec4(f.rider->rootOffset);
            placeByOffset(f);
        }

        // Where the camera wants to be and to look, by the game's mode.
        // Mode 2 keeps both; mode 5 (and any past 8) stays and looks at the
        // rider's chest; mode 8 goes far below the world, at a cut.
        void wantedByMode(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            switch (cameraMode(static_cast<int32_t>(f.player), 0))
            {
            case kCameraFollows:
                followRider(f);
                break;
            case kCameraOnGround:
                groundCamera(f);
                break;
            case kCameraMode2:
                break;
            case kCameraMode3:
                circleRider(f);
                break;
            case kCameraLevelPoint:
                levelPointCamera(f);
                break;
            case kCameraMode6:
            case kCameraMode7:
                besideRider(f);
                break;
            case kCameraMode8:
                camera.wantedTarget = *g_cameraMode8Place;
                camera.wantedPosition = *g_cameraMode8Place;
                camera.cut = 1;
                camera.wantedPosition.z = camera.wantedPosition.z + ee::Float(-10.0f);
                break;
            case kCameraMode5:
            default:
                camera.wantedTarget.xyz() = drawnPlaceLifted(f.rider, kChestLift);
                easeToward(camera.wantedTarget, f.rider->drawnPosition, kDrawnPull);
                break;
            }
        }

        // The eye kept the least distance from the drawn rider across the
        // ground: nearer, it goes out to that distance on the side it is on
        // (right on the rider: along the rider's forward row).
        void keepOffRider(Follow &f)
        {
            const ee::Ptr<Rider> rider = f.rider;
            zge::Vec4 &away = f.scratch;
            const ee::Float least = *g_leastRiderDistance;
            const ee::Float leastSquared = least * least;
            away.x = f.eye.x - (rider->position[0] + rider->rootOffset[0]);
            away.y = f.eye.y - (rider->position[1] + rider->rootOffset[1]);
            const ee::Float z = f.eye.z - (rider->position[2] + rider->rootOffset[2]);
            const ee::Float distanceSquared = away.x * away.x + z * z;
            away.z = z;
            if (!(distanceSquared < leastSquared))
                return;
            away.y = 0.0f;
            if (ee::Float(0.0f) < distanceSquared)
                away = zge::unit(away);
            else
            {
                away = *vec4(rider->forward);
                away.y = 0.0f;
            }
            away = scaled(away, least);
            f.eye.x = (away.x + rider->position[0]) + rider->rootOffset[0];
            f.eye.z = (away.z + rider->position[2]) + rider->rootOffset[2];
        }

        // The eye pushed out of the walls: for every face near it that it is
        // in front of and closer to than the camera's radius, straight away
        // from the face's nearest point until it is the radius away.
        void keepOutOfWalls(Follow &f)
        {
            rider::collision::SphereHits *const hits = rider::collision::sphereQuery(&f.eye.x, *g_cameraRadius * kWallReach);
            if (hits == nullptr)
                return;
            zge::Vec4 &toFace = f.scratch;
            while (const rider::collision::SphereFace *const face = rider::collision::nextSphereFace(hits))
            {
                rider::collision::faceNormal(rider::collision::faceRefAt(&face->face), &f.normal.x);
                rider::collision::closestOnFace(face, &toFace.x);
                toFace.x = toFace.x - f.eye.x;
                toFace.y = toFace.y - f.eye.y;
                toFace.z = toFace.z - f.eye.z;
                if (!(zge::dot(toFace, f.normal) <= ee::Float(0.0f)))
                    continue;
                if (!(zge::lengthSquared(&toFace.xyz()) < *g_cameraRadius * *g_cameraRadius))
                    continue;
                const ee::Float distance = zge::vec3Length(&toFace.xyz());
                toFace = scaled(toFace, ee::Float(1.0f) / distance);
                const ee::Float inside = *g_cameraRadius - distance;
                f.eye.z = f.eye.z - toFace.z * inside;
                f.eye.x = f.eye.x - toFace.x * inside;
                f.eye.y = f.eye.y - toFace.y * inside;
            }
        }

        // The view of the drawn rider from the camera's place: blocked for
        // 41 frames since the last cut, a moving rider that has not bailed
        // gets a cut, and a ground camera a new place.
        void watchTheView(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            f.scratch.xyz() = drawnPlace(f.rider);
            if (rider::collision::rayCast(&camera.position.x, &f.scratch.x) == nullptr)
                return;
            camera.blockedFrames = static_cast<uint16_t>(camera.blockedFrames + 1);
            if (camera.blockedFrames < kBlockedLimit)
                return;
            const uint8_t mode = cameraMode(static_cast<int32_t>(f.player), 0);
            if (mode == kCameraFollows)
            {
                if (!(f.rider->speed == 0.0f) && rider::physicsState(f.rider) != rider::kPhysicsBail)
                    camera.cut = 1;
            }
            else if (mode == kCameraOnGround)
                groundCameraMoves(f, camera.groundKind);
        }

        // The camera's eased move. The point looked at and the eased place
        // go their shares of the way; the camera loses its drag and gains
        // speed toward the eased place by how far away that is; the place
        // that gives is kept off the rider and out of the walls (unless the
        // camera's flags 1 and 2 say not to), and the speed becomes the move
        // really made. The field of view goes its share of the way too.
        void easeToWanted(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            easeToward(camera.target, &camera.wantedTarget.x, f.rates.targetEase);
            f.eye = camera.wantedPosition;
            easeToward(camera.easedPosition, &f.eye.x, f.rates.positionEase);
            const ee::Float drag = f.rates.drag;
            camera.velocity.z = camera.velocity.z - camera.velocity.z * drag;
            camera.velocity.x = camera.velocity.x - camera.velocity.x * drag;
            camera.velocity.y = camera.velocity.y - camera.velocity.y * drag;
            zge::Vec4 &toEased = f.scratch;
            toEased.x = camera.easedPosition.x - camera.position.x;
            toEased.y = camera.easedPosition.y - camera.position.y;
            toEased.z = camera.easedPosition.z - camera.position.z;
            const ee::Float gained = zge::vec3Length(&toEased.xyz()) * f.rates.pull;
            if (ee::Float(0.0f) < gained)
            {
                toEased = zge::unit(toEased);
                camera.velocity.x = camera.velocity.x + toEased.x * gained;
                camera.velocity.y = camera.velocity.y + toEased.y * gained;
                camera.velocity.z = camera.velocity.z + toEased.z * gained;
            }
            f.eye.x = camera.position.x + camera.velocity.x;
            f.eye.y = camera.position.y + camera.velocity.y;
            f.eye.z = camera.position.z + camera.velocity.z;

            const uint8_t flags = camera.flags;
            if (!(flags & kFlagNearRider))
                keepOffRider(f);
            if (!(flags & kFlagInWalls))
                keepOutOfWalls(f);
            camera.velocity = f.eye - camera.position;
            camera.position = f.eye;

            if (cameraMode(static_cast<int32_t>(f.player), 0) != kCameraLevelPoint)
                watchTheView(f);

            if (!(camera.fieldOfView == camera.wantedFieldOfView))
            {
                const ee::Float left = camera.wantedFieldOfView - camera.fieldOfView;
                if (kFieldOfViewStep < ee::abs(left))
                    camera.fieldOfView = camera.fieldOfView + left * f.rates.fieldOfViewEase;
                else
                    camera.fieldOfView = camera.wantedFieldOfView;
            }
        }

        // A cut: the camera is where it wants to be, at rest, and the
        // sounds' pitch does not follow the jump.
        void cutToWanted(PlayerCamera &camera)
        {
            camera.target = camera.wantedTarget;
            camera.easedPosition = camera.wantedPosition;
            camera.position = camera.wantedPosition;
            camera.velocity = zge::kZero;
            camera.fieldOfView = camera.wantedFieldOfView;
            camera.cut = 0;
            camera.blockedFrames = 0;
            zge::disableDoppler();
        }

        // The camera's rows from its place and the point it looks at:
        // forward at the point, up (0, -1, 0) (y grows downwards). Looking
        // near straight up or down, the up row of the frame before is kept
        // instead, or (0, 0, -1) where that runs along the view too.
        void rowsFromTarget(PlayerCamera &camera, zge::Vec4 &scratch)
        {
            zge::Vec4 &right = camera.rows.row(0);
            zge::Vec4 &up = camera.rows.row(1);
            zge::Vec4 &forward = camera.rows.row(2);
            forward = camera.target - camera.position;
            forward = zge::unit(forward);
            // (The original keeps the straight up vector in its frame too.)
            scratch.x = 0.0f;
            scratch.y = -1.0f;
            scratch.z = 0.0f;
            if (ee::abs(forward.y) < kNearVertical)
            {
                up.y = -1.0f;
                up.x = 0.0f;
                up.z = 0.0f;
            }
            else if (kNearVertical < ee::abs(zge::dot(forward, up)))
            {
                up.z = -1.0f;
                up.x = 0.0f;
                up.y = 0.0f;
            }
            right = zge::cross(up, forward);
            right = zge::unit(right);
            up = zge::cross(forward, right);
        }

        // One player's camera for this frame.
        void followPlayer(Follow &f)
        {
            PlayerCamera &camera = f.camera;
            const uint32_t player = f.player;

            // Whose rider: the player's, or in modes 6 and 7 the shown
            // NPC's. A level-point camera has none.
            if (cameraMode(static_cast<int32_t>(player), 0) != kCameraLevelPoint)
            {
                const uint8_t mode = cameraMode(static_cast<int32_t>(player), 0);
                uint32_t riderIndex;
                if (mode == kCameraMode6 || mode == kCameraMode7)
                {
                    npcInfo(*g_shownNpc, &f.npc);
                    riderIndex = f.npc.rider;
                }
                else
                    riderIndex = riderIndexOf(player);
                f.rider = rider::riderOf(riderIndex);
                rider::physicsState(f.rider);
            }

            readSettings(f);
            camera.wantedFieldOfView = f.rates.fieldOfView;

            // A ground camera that was placed by a mode change and wants a
            // new place gets the next kind (after kind 9: kind 0).
            if ((camera.flags & (kFlagGroundPlaced | kFlagGroundMoves)) == (kFlagGroundPlaced | kFlagGroundMoves))
            {
                placeGroundCamera(player, camera.groundKind < 10 ? camera.groundKind + 1u : 0u);
                camera.flags |= kFlagGroundPlaced;
            }

            wantedByMode(f);

            if (camera.cut == 0)
                easeToWanted(f);
            else if (camera.cut == 1)
                cutToWanted(camera);

            // The state the camera node gets.
            if (player == *g_cameraPlayer)
            {
                if (f.rowsSet == 0)
                    rowsFromTarget(camera, f.scratch);
                const ee::Ptr<CameraState> state = ee::Ptr<CameraState>(sym::g_cameraStates) + static_cast<int32_t>(player);
                state->fieldOfView = static_cast<int16_t>(ee::toInt(camera.fieldOfView * kUnitsPerDegree));
                state->position = camera.position;
                state->target = camera.target;
                zge::copyRotationAndTranslation(&state->rows, &camera.rows);
                applyCameraState(state);
            }
            camera.frames = camera.frames + 1;
        }
    }

    // followCamera: the camera of the player being shown, worked out and
    // applied (the other players' cameras are left as they are).
    void followCamera()
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> offset;
        ee::Local<zge::Matrix> basis;
        ee::Local<zge::Matrix> turn;
        ee::Local<FollowRates> rates;
        ee::Local<NpcInfo> npc;
        ee::Local<zge::Vec4> normal;
        // (The original keeps these four in its frame.)
        ee::Local<uint32_t> player;
        ee::Local<uint32_t> rowsSet;
        ee::Local<ee::Ptr<zge::Vec4>> offsetKept;
        ee::Local<uint32_t> nextPlayer;

        *player = 0;
        while (*g_multiplayerPlayers != 0 ? *player < *g_multiplayerPlayers : *player == 0)
        {
            if (*player != *g_cameraPlayer)
                *nextPlayer = *player + 1;
            else
            {
                *rowsSet = 0;
                *offsetKept = offset;
                *nextPlayer = *player + 1;
                Follow follow{*player, *playerCamera(*player), ee::Ptr<Rider>(), *scratch, *eye, *offset, *basis, *turn, *rates, *normal, *npc, *rowsSet};
                followPlayer(follow);
            }
            *player = *nextPlayer;
        }
    }
    READABLE_FUNCTION_WAITS(followCamera, Verified);
}
