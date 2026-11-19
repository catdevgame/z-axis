// BMX XXX's action cameras (action_camera.h): the follow mode, the camera
// behind the rider (./src/com/action/camera.c). BMX XXX's own code, DM2's
// line (DM2 has it as one part of its follow camera's frame).
//
// Each frame the mode says where the camera wants to be and what it wants
// to look at; the frame's update (update.cpp) moves the view there. What
// the mode does is its second level (the rider code sets it):
//   0      behind a riding rider, as the player's camera type says
//   1, 2   swinging round the rider to face along the way it goes (1) or
//          to its nearer side (2); camera types 1 and 3 stay behind
//   3      as a bail starts: from where the camera is, level
//   4      above the rider, looking down at the point it is drawn at
//   5      the first-person view: the weapon's, or the rider's eyes
//   6      the view is someone else's to place
// The names of the levels' values are by what this code does with them.

#include "action_camera.h"

#include "bmx/rider/physics/physics.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "bmx/weapon/weapon.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/camera/follow.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::Rider;
    using rider::vec4;
    using rider::physics::upperHalf;

    namespace
    {
        constexpr uint32_t kCameraTypes = 5;
        constexpr uint32_t kLayoutGroups = 4;
        // The settings of the follow mode: by how the screen is shared and
        // the player's camera type; while its second level is 3, by how the
        // screen is shared; and while it is 5.
        inline ee::Global<CameraSettings[kLayoutGroups][kCameraTypes], sym::g_followCameraSettings> g_followCameraSettings;
        inline ee::Global<CameraSettings[kLayoutGroups], sym::g_followSpecialCameraSettings> g_followSpecialCameraSettings;
        inline ee::Global<CameraSettings, sym::g_followFifthCameraSettings> g_followFifthCameraSettings;
        // How the screen is shared, for each of the first ten viewport
        // layouts: 0 one view, 1 two views one above the other, 2 two side
        // by side, 3 four.
        inline ee::Global<uint8_t[10], sym::g_layoutGroups> g_layoutGroups;
        // By camera type (a sixth value follows the five types): how many
        // frames of the rider's velocity the point looked at leads by, and
        // how far above the rider the camera stands while the rider is off
        // the ground.
        inline ee::Global<ee::Float[kCameraTypes + 1], sym::g_followLeads> g_followLeads;
        inline ee::Global<ee::Float[kCameraTypes + 1], sym::g_followAirHeights> g_followAirHeights;

        // The follow mode's second level.
        enum FollowKind : uint32_t
        {
            kFollowRiding = 0,
            kFollowSwingBehind = 1,
            kFollowSwingToSide = 2,
            kFollowBail = 3,
            kFollowAbove = 4,
            kFollowFirstPerson = 5,
            kFollowPlacedElsewhere = 6,
            kFollowKinds = 7,
        };
        // The camera types with their own code here: 1 looks further ahead,
        // 1 and 3 do not swing, 4 is the rider's eyes.
        constexpr uint8_t kTypeFarAhead = 1;
        constexpr uint8_t kTypeNoSwing = 3;
        constexpr uint8_t kTypeRiderEyes = 4;

        // The rider's states this reads that rider.h does not name (DM2's
        // names): the physics state in the air, the riding action's coast.
        constexpr uint8_t kPhysicsAir = 1;
        constexpr uint8_t kRidingCoast = 0;

        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);
        constexpr ee::Float kTurnsAnAngle = ee::Float(1.0f / 65536.0f);
        constexpr ee::Float kAnglesADegree = std::bit_cast<float>(0x43360B61u); // 65536 / 360
        constexpr ee::Float kWatchedLift = ee::Float(0.2f);     // the point looked at above the drawn rider, along its up
        constexpr ee::Float kDrawnPull = ee::Float(0.33f);      // the share of the way the point goes to the drawn body
        constexpr ee::Float kSeatLift = ee::Float(0.8f);        // the point above a coasting rider's position, along its up
        constexpr ee::Float kFallAllowance = ee::Float(0.15f);  // an airborne rider's fall speed the lead leaves out
        constexpr ee::Float kStandingLead = ee::Float(0.01f);   // a standing rider leads the far view along its forward row
        constexpr ee::Float kNoDirection = ee::Float(0.00001f); // a direction shorter than this on x and on z is none
        constexpr ee::Float kEyesAhead = ee::Float(5.0f);       // the rider's eyes look this far along their forward row
        constexpr ee::Float kCoastHeight = ee::Float(0.2f);     // the camera's height behind a coasting rider
        constexpr ee::Float kContactHeight = ee::Float(1.5f);   // and off the ground with a contact of kind 1 or 3
        constexpr ee::Float kSwingDrop = ee::Float(3.5f);       // the swing's eye sinks by this many of the offset's heights
        constexpr ee::Float kSwingShare = ee::Float(0.085f);    // the part of the turn left that a frame of the swing takes
        constexpr uint32_t kSwingFrames = 30;                   // the frames the swing eases over
        constexpr ee::Float kBailLookAtLift = ee::Float(-0.25f);
        constexpr ee::Float kAbovePull = ee::Float(0.35f);      // the view from above looks this far from the drawn body to the rider
        constexpr ee::Float kAboveSlope = std::bit_cast<float>(0x3F13B646u); // 0.577: its height for each unit between them
        constexpr ee::Float kLeastAbove = ee::Float(3.0f);      // and its least height
        constexpr ee::Float kEyesLift = ee::Float(0.5f);        // the rider's eyes from its position: up,
        constexpr ee::Float kEyesBack = ee::Float(-0.1f);       // and along its forward row

        // The frame's working values, which the kinds share.
        struct Follow
        {
            ActionCamera *camera;
            Rider *rider;
            Vec4 &scratch;
            Vec4 &offset;        // the settings' offset (a view changes its height)
            Matrix &turn;
            Matrix &basis;       // the space the offset is in: rows right, up, forward and its origin
            Matrix &swing;       // the swing's space, facing from the point looked at to the eye
            EulerAngles &angles;
            CameraView &view;
        };

        const Matrix &riderRows(const Rider *rider) { return *reinterpret_cast<const Matrix *>(rider->right); }
        const Vec3 *vec3(const ee::Float *lanes) { return reinterpret_cast<const Vec3 *>(lanes); }

        // The rider's shooter record (weapon.c's).
        const Shooter *riderShooter(const Rider *rider)
        {
            return ee::Ptr<Shooter>(rider->shooter);
        }

        // Riding along with no motion running over the coast: the rider sits still on the bike.
        bool coasting(const Rider *rider)
        {
            return rider::actionState(rider, rider::kLevelAction) == rider::kActionRiding &&
                   rider::actionState(rider, rider::kLevelSubState) == kRidingCoast &&
                   rider::actionState(rider, rider::kLevelPhase) == rider::kPhaseHeld;
        }

        // The settings row a camera that stands behind the rider has.
        const CameraSettings &behindSettings(const ActionCamera *camera)
        {
            return (*g_followCameraSettings)[(*g_layoutGroups)[static_cast<uint8_t>(camera->viewportLayout)]][camera->type];
        }

        // A level space that faces along `forward`: up straight (y grows
        // downwards), right across. `forward` is left as it was.
        void levelRight(Vec4 &right, Vec4 &up, const Vec4 &forward)
        {
            up.x = ee::Float(0.0f);
            up.y = ee::Float(-1.0f);
            up.z = ee::Float(0.0f);
            right = cross(up, forward);
            right = unit(right);
        }

        // Behind a riding rider. The point looked at is the drawn rider (or,
        // coasting, the seat), pulled toward the drawn body off the ground or
        // moving, and led by the velocity as the camera type says. The camera
        // stands at the settings' offset in a space at the drawn rider that
        // faces along the velocity, level.
        void followRiding(Follow &f)
        {
            ActionCamera *camera = f.camera;
            const Rider *rider = f.rider;
            Vec4 &lookAt = camera->wantedLookAt;
            if (coasting(rider))
            {
                lookAt.x = rider->position[0] + rider->up[0] * kSeatLift;
                lookAt.y = rider->position[1] + rider->up[1] * kSeatLift;
                lookAt.z = rider->position[2] + rider->up[2] * kSeatLift;
            }
            else
            {
                lookAt.x = rider->position[0] + rider->rootOffset[0];
                lookAt.y = rider->position[1] + rider->rootOffset[1];
                lookAt.z = rider->position[2] + rider->rootOffset[2];
                if (rider::physicsState(rider) != rider::kPhysicsGround || !(rider->speed == ee::Float(0.0f)))
                {
                    lookAt.x = lookAt.x + (rider->drawnPosition[0] - (rider->position[0] + rider->rootOffset[0])) * kDrawnPull;
                    lookAt.y = lookAt.y + (rider->drawnPosition[1] - (rider->position[1] + rider->rootOffset[1])) * kDrawnPull;
                    lookAt.z = lookAt.z + (rider->drawnPosition[2] - (rider->position[2] + rider->rootOffset[2])) * kDrawnPull;
                }
            }

            if (camera->type == kTypeFarAhead)
            {
                // Twice the lead, level; a standing rider leads along its forward row.
                uint64_t carried = scaleLeftover(LeftBy::lastCall);
                if (rider->speed == ee::Float(0.0f))
                    f.scratch = scaledCarrying(*vec4(rider->forward), kStandingLead, carried);
                else
                {
                    f.scratch = *vec4(rider->velocity);
                    carried = upperHalf(f.scratch);
                }
                const ee::Float lead = (*g_followLeads)[camera->type];
                f.scratch.y = ee::Float(0.0f);
                f.scratch = scaledCarrying(f.scratch, lead + lead, carried);
                lookAt = lookAt + f.scratch;
            }
            else if (camera->type == kTypeRiderEyes)
            {
                // The view's axes are the rider's, turned by the bars' lean;
                // it looks ahead along them.
                f.basis = riderRows(rider);
                matrixIdentity(&f.turn);
                matrixRotateY(&f.turn, &f.turn, static_cast<Angle>(ee::toInt(rider->barLean[1] * kAnglesATurn)));
                matrixMultiply(&f.basis, &f.basis, &f.turn);
                matrixIdentity(&f.turn);
                matrixRotateX(&f.turn, &f.turn, static_cast<Angle>(ee::toInt(rider->barLean[0] * kAnglesATurn)));
                matrixMultiply(&f.basis, &f.basis, &f.turn);
                const Vec4 &ahead = f.basis.row(2);
                lookAt.x = lookAt.x + (rider->up[0] * kSeatLift + ahead.x * kEyesAhead);
                lookAt.y = lookAt.y + (rider->up[1] * kSeatLift + ahead.y * kEyesAhead);
                lookAt.z = lookAt.z + (rider->up[2] * kSeatLift + ahead.z * kEyesAhead);
                camera->axes = f.basis;
                camera->flags |= kCameraModePlacedAxes;
            }
            else
            {
                // The lead is level but for an airborne rider's fall past the allowance.
                f.scratch = *vec4(rider->velocity);
                if (rider::physicsState(rider) == kPhysicsAir && kFallAllowance < rider->velocity[1])
                    f.scratch.y = f.scratch.y - kFallAllowance;
                else
                    f.scratch.y = ee::Float(0.0f);
                f.scratch = scaledCarrying(f.scratch, (*g_followLeads)[camera->type], scaleLeftover(LeftBy::lastCall));
                lookAt = lookAt + f.scratch;
            }

            // The space the offset is in. With no way to face (no velocity
            // and a forward row that points straight up or down) it faces
            // against the rider's up row.
            f.basis = riderRows(rider);
            Vec4 &right = f.basis.row(0);
            Vec4 &up = f.basis.row(1);
            Vec4 &forward = f.basis.row(2);
            Vec4 &origin = f.basis.row(3);
            if (!(rider->speed == ee::Float(0.0f)))
                forward = *vec4(rider->velocity);
            forward.y = ee::Float(0.0f);
            if (ee::abs(forward.x) < kNoDirection && ee::abs(forward.z) < kNoDirection)
            {
                forward = *vec4(rider->forward);
                forward.y = ee::Float(0.0f);
                if (ee::abs(forward.x) < kNoDirection && ee::abs(forward.z) < kNoDirection)
                    vec3Negate(&forward.xyz(), vec3(rider->up));
            }
            forward = unit(forward);
            levelRight(right, up, forward);
            forward = cross(right, up);
            if (coasting(rider))
            {
                origin.x = rider->position[0] + rider->up[0] * kSeatLift;
                origin.y = rider->position[1] + rider->up[1] * kSeatLift;
                origin.z = rider->position[2] + rider->up[2] * kSeatLift;
            }
            else
            {
                origin = *vec4(rider->position);
                origin = origin + *vec4(rider->rootOffset);
            }

            if (camera->type != kTypeFarAhead && camera->type != kTypeRiderEyes)
            {
                // The views behind the rider change their height: low behind
                // a coasting rider, high off the ground (less high with a
                // contact of kind 1 or 3 in the air and on the approaches).
                if (coasting(rider))
                    f.offset.y = kCoastHeight;
                const uint8_t state = rider::physicsState(rider);
                if (state == kPhysicsAir || (state >= rider::kPhysicsGrindApproach && state <= rider::kPhysicsStall))
                {
                    f.offset.y = (*g_followAirHeights)[camera->type];
                    const uint8_t now = rider::physicsState(rider);
                    if ((now == kPhysicsAir || now == rider::kPhysicsGrindApproach || now == rider::kPhysicsStallApproach) &&
                        (rider->contactKind == 1 || rider->contactKind == 3))
                        f.offset.y = kContactHeight;
                }
            }
            origin.w = ee::Float(1.0f);
            camera->wantedEye = transformed(f.basis, f.offset);
        }

        // Swinging round the rider. The eye wants to be the settings'
        // distance from the drawn rider: behind the way the rider goes (or
        // faces, standing), or on the side of the rider the camera is on
        // now; and a little above. It gets there round the rider, not
        // through it: the turn from where the eye is now to there is worked
        // out as three angles, and the eye asked for is 0.085 of that turn
        // on from where it is, the same distance out.
        void followSwing(Follow &f, uint32_t kind)
        {
            ActionCamera *camera = f.camera;
            const Rider *rider = f.rider;
            camera->wantedLookAt = *vec4(rider->position) + *vec4(rider->rootOffset);
            f.scratch = *vec4(rider->forward);
            if (kind == kFollowSwingToSide)
            {
                f.scratch = camera->wantedLookAt - camera->eye;
                if (ee::Float(0.0f) <= dot(f.scratch, *vec4(rider->right)))
                    f.scratch = *vec4(rider->right);
                else
                    vec3Negate(&f.scratch.xyz(), vec3(rider->right));
            }
            else if (!(rider->speed == ee::Float(0.0f)))
            {
                f.scratch = *vec4(rider->velocity);
            }
            f.scratch.w = ee::Float(0.0f);
            f.scratch.y = ee::Float(0.0f);
            f.scratch = unit(f.scratch);
            f.scratch = scaledAfterMeasure(f.scratch, -behindSettings(camera).distance);
            f.scratch.y = f.scratch.y - f.offset.y * kSwingDrop;
            f.scratch = unit(f.scratch);

            // The space that faces from the rider to where the eye wants to be.
            Vec4 &right = f.basis.row(0);
            Vec4 &up = f.basis.row(1);
            Vec4 &toWanted = f.basis.row(2);
            toWanted = f.scratch;
            f.scratch = scaledCarrying(f.scratch, -behindSettings(camera).distance, upperHalf(f.scratch));
            camera->wantedEye = f.scratch + camera->wantedLookAt;

            // And the one that faces from the rider to where the eye is.
            Vec4 &eyeRight = f.swing.row(0);
            Vec4 &eyeUp = f.swing.row(1);
            Vec4 &toEye = f.swing.row(2);
            toEye = camera->eye - camera->wantedLookAt;
            toEye = unit(toEye);
            toEye.w = ee::Float(0.0f);
            f.basis.row(3) = kZero;
            levelRight(right, up, toWanted);
            up = cross(toWanted, right);
            f.swing.row(3) = kZero;
            levelRight(eyeRight, eyeUp, toEye);
            eyeUp = cross(toEye, eyeRight);

            // The turn from the eye's space to the wanted one, a part of it
            // taken, and the eye's space turned by that.
            matrixIdentity(&f.turn);
            f.turn = f.swing;
            matrixTranspose3(&f.turn, &f.turn);
            matrixMultiply(&f.turn, &f.turn, &f.basis);
            matrixToEuler(&f.angles, &f.turn);
            const ee::Float x = ee::toFloat(static_cast<int16_t>(f.angles.x));
            const ee::Float y = ee::toFloat(static_cast<int16_t>(f.angles.y));
            const ee::Float z = ee::toFloat(static_cast<int16_t>(f.angles.z));
            f.angles.x = static_cast<Angle>(ee::toInt(x * kTurnsAnAngle * kSwingShare * kAnglesATurn));
            f.angles.y = static_cast<Angle>(ee::toInt(y * kTurnsAnAngle * kSwingShare * kAnglesATurn));
            f.angles.z = static_cast<Angle>(ee::toInt(z * kTurnsAnAngle * kSwingShare * kAnglesATurn));
            matrixIdentity(&f.turn);
            matrixRotateXYZ(&f.turn, &f.turn, &f.angles);
            matrixMultiply(&f.swing, &f.swing, &f.turn);
            f.scratch = scaledCarrying(toEye, behindSettings(camera).distance, scaleLeftover(LeftBy::lastCall));
            camera->wantedEye = f.scratch + camera->wantedLookAt;
            camera->easing = camera->modeFrames[1] < kSwingFrames ? CameraEasing::Swing : CameraEasing::Snap;
        }

        // As a bail starts: the camera looks at the rider's position, a
        // little up, from the settings' offset in a level space at the
        // rider that faces from the camera to the rider.
        // Kept from the original: that direction is used only when it runs
        // straight along x (no z at all); any other gets the rider's
        // forward row, as a camera right above the rider does.
        void followBail(Follow &f)
        {
            ActionCamera *camera = f.camera;
            const Rider *rider = f.rider;
            camera->wantedLookAt = *vec4(rider->position);
            camera->wantedLookAt.y = camera->wantedLookAt.y + kBailLookAtLift;
            f.basis = riderRows(rider);
            Vec4 &forward = f.basis.row(2);
            forward = *vec4(rider->position) - camera->wantedEye;
            forward.y = ee::Float(0.0f);
            if (forward.x == ee::Float(0.0f) || !(forward.z == ee::Float(0.0f)))
            {
                forward = *vec4(rider->forward);
                forward.y = ee::Float(0.0f);
            }
            forward = unit(forward);
            levelRight(f.basis.row(0), f.basis.row(1), forward);
            f.basis.row(3) = *vec4(rider->position);
            camera->wantedEye = transformed(f.basis, f.offset);
        }

        // From above: the camera looks at a point between the drawn body
        // and the rider's position from straight over that position, higher
        // the further the two are apart across the ground, and never under
        // three units up.
        void followAbove(Follow &f)
        {
            ActionCamera *camera = f.camera;
            const Rider *rider = f.rider;
            const Vec4 &drawn = *vec4(rider->drawnPosition);
            f.scratch = *vec4(rider->position) - drawn;
            f.scratch = scaledCarrying(f.scratch, kAbovePull, scaleLeftover(LeftBy::lastCall));
            f.scratch = f.scratch + drawn;
            camera->wantedLookAt = f.scratch;
            camera->wantedEye = *vec4(rider->position);
            f.scratch = drawn - *vec4(rider->position);
            f.scratch.y = ee::Float(0.0f);
            const ee::Float height = vec3Length(&f.scratch.xyz()) * kAboveSlope;
            if (kLeastAbove <= height)
                camera->wantedEye.y = camera->wantedEye.y - height;
            else
                camera->wantedEye.y = camera->wantedEye.y - kLeastAbove;
        }

        // The first-person view. A rider whose camera the rider code has
        // let go of (kFlagNoPlayer) is seen through its own eyes; any other
        // gets the view of its weapon: from the shooter's place, lifted
        // along the rider's up row by the settings' offset's x, along the
        // shooter's aim, with the shooter's field of view. That view is
        // placed here and now.
        void followFirstPerson(Follow &f)
        {
            ActionCamera *camera = f.camera;
            const Rider *rider = f.rider;
            if (rider::hasFlag(rider, rider::kFlagNoPlayer))
            {
                riderViewCamera(camera, rider);
            }
            else
            {
                const Shooter *shooter = riderShooter(rider);
                f.view.fov = static_cast<Angle>(ee::toInt(shooter->fov * kAnglesADegree));
                const ee::Float lift = f.offset.x;
                f.view.eye.x = shooter->position.x + rider->up[0] * lift;
                f.view.eye.y = shooter->position.y + rider->up[1] * lift;
                f.view.eye.z = shooter->position.z + rider->up[2] * lift;
                f.view.lookAt = shooter->position + shooter->aim.row(2);
                f.view.axes = shooter->aim;
                setCameraView(camera, &f.view);
                camera->flags |= kCameraModePlacedView;
            }
            camera->flags |= kCameraModePlacedAxes;
        }
    }

    // riderViewCamera: the view through the rider's eyes: from half a unit
    // up the rider's up row and a tenth back along its forward row, at the
    // drawn rider; looking a unit ahead along the rider's axes turned by
    // the bars' lean, which are the view's axes too (the caller says the
    // mode placed them).
    void riderViewCamera(ActionCamera *camera, const rider::Rider *rider)
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> back;
        ee::Local<Matrix> basis;
        ee::Local<Matrix> turn;
        *basis = riderRows(rider);
        matrixIdentity(turn);
        matrixRotateY(turn, turn, static_cast<Angle>(ee::toInt(rider->barLean[1] * kAnglesATurn)));
        matrixMultiply(basis, basis, turn);
        matrixIdentity(turn);
        matrixRotateX(turn, turn, static_cast<Angle>(ee::toInt(rider->barLean[0] * kAnglesATurn)));
        matrixMultiply(basis, basis, turn);
        const uint64_t carried = scaleLeftover(LeftBy::lastCall);
        *eye = scaledCarrying(*vec4(rider->up), kEyesLift, carried);
        *back = scaledCarrying(*vec4(rider->forward), kEyesBack, carried);
        *eye = *eye + *back;
        *eye = *eye + *vec4(rider->position);
        *eye = *eye + *vec4(rider->rootOffset);
        camera->wantedLookAt = scaledCarrying(basis->row(2), ee::Float(1.0f), carried);
        camera->wantedLookAt = camera->wantedLookAt + *eye;
        camera->wantedEye = *eye;
        camera->axes = *basis;
    }
    READABLE_FUNCTION(riderViewCamera, Verified);

    // followCameraMode: the follow mode's frame (the kinds are at the top
    // of the file). Before the kind's own work the point looked at is put
    // a little above the drawn rider and a third of the way to the drawn
    // body: the kinds that set no point of their own keep that.
    uint32_t followCameraMode(ActionCamera *camera)
    {
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> offset;
        ee::Local<Matrix> turn;
        ee::Local<Matrix> basis;
        ee::Local<Matrix> swing;
        ee::Local<EulerAngles> angles;
        ee::Local<CameraView> view;
        Rider *rider = rider::riderOf(static_cast<uint8_t>(camera->rider));
        Follow f{camera, rider, *scratch, *offset, *turn, *basis, *swing, *angles, *view};

        // The settings' offset. (From above has none and reads none.)
        switch (cameraMode(camera, 1))
        {
        case kFollowBail:
            f.offset = (*g_followSpecialCameraSettings)[(*g_layoutGroups)[static_cast<uint8_t>(camera->viewportLayout)]].offset;
            break;
        case kFollowAbove:
            break;
        case kFollowFirstPerson:
            f.offset = g_followFifthCameraSettings->offset;
            break;
        default:
            f.offset = behindSettings(camera).offset;
            break;
        }

        Vec4 &lookAt = camera->wantedLookAt;
        lookAt.x = rider->position[0] + rider->rootOffset[0] + rider->up[0] * kWatchedLift;
        lookAt.y = rider->position[1] + rider->rootOffset[1] + rider->up[1] * kWatchedLift;
        lookAt.z = rider->position[2] + rider->rootOffset[2] + rider->up[2] * kWatchedLift;
        lookAt.x = lookAt.x + (rider->drawnPosition[0] - lookAt.x) * kDrawnPull;
        lookAt.y = lookAt.y + (rider->drawnPosition[1] - lookAt.y) * kDrawnPull;
        lookAt.z = lookAt.z + (rider->drawnPosition[2] - lookAt.z) * kDrawnPull;

        uint32_t kind = cameraMode(camera, 1);
        if ((kind == kFollowSwingBehind || kind == kFollowSwingToSide) && (camera->type == kTypeFarAhead || camera->type == kTypeNoSwing))
            kind = kFollowRiding;
        switch (kind)
        {
        case kFollowRiding:
            followRiding(f);
            break;
        case kFollowSwingBehind:
        case kFollowSwingToSide:
            followSwing(f, kind);
            break;
        case kFollowBail:
            followBail(f);
            break;
        case kFollowAbove:
            followAbove(f);
            break;
        case kFollowFirstPerson:
            followFirstPerson(f);
            break;
        case kFollowPlacedElsewhere:
            camera->flags |= kCameraModePlacedAxes | kCameraModePlacedView;
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION(followCameraMode, Verified);
}
