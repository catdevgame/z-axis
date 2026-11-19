// BMX XXX's bike pose after the physics move (physics.c): the drawn rider
// and bike placed from the physics, on the ground and in the air, and the
// ground states' after callback. DM2's (game/dm2/rider/physics/pose.cpp),
// whose ground callback poses the bike first; BMX XXX's makes only the
// rolling sound.
//
// The drawn rows are the physics rows turned about the right row by the
// body's pitch; the drawn position is the physics position plus the
// motion's root offset, less the motion's root turned by the drawn rows.

#include "engine_calls.h"
#include "game_calls.h"
#include "rider.h"
#include "collision/collision.h"

#include "bmx/fx/spawns.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/closest.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/pose.cpp.h"

#include <cstddef>

namespace game::bmx::rider
{
    namespace calls
    {
        // The rolling sound: the voice played by a sound code at a position
        // and a rate (the sound module's updateRollSound, game/bmx/sound/
        // roll_sound.h; a function pointer here for the ground's tail call).
        // Scaffolding (address-call): docs/SCAFFOLDING.md
        inline constexpr ee::FunctionPtr<void(uint32_t slot, uint32_t sound, ee::Address position, int32_t speed)>
            updateRollSound{sym::updateRollSound};
    }
    namespace
    {
        // The rolling sound's surface by the wheel's surface sound (0 .. 15;
        // the jump table's cases, DM2's values).
        constexpr uint8_t kRollingSurface[16] = {0, 7, 2, 5, 5, 4, 6, 6, 4, 5, 8, 1, 1, 5, 5, 0};
        constexpr uint32_t kGroundSoundKind = 1;
        // The riding sub-states with their own sound: the brake's (from its
        // full drag, 0.05), pedalling's, and fakie's silence. (DM2's file
        // calls the first two the manual's and the crouch's; BMX XXX's
        // motion names and its sub-state table say which they are.)
        inline ee::Global<ee::Float, sym::g_brakeDrag> g_brakeDrag;
    }

    // The after callback of the ground states: the rolling sound by the
    // surface under the first wheel on the ground, at the back wheel's rate:
    // braking (riding sub-state 2, its drag share at 0.05) the brake's
    // sound at the front wheel's rate, pedalling (sub-state 1) its own,
    // fakie (sub-state 4) silent.
    void groundAfter(Rider *rider)
    {
        uint32_t sound = 0;
        uint8_t surface = 0;
        if (rider->wheelPoses[0].ground != kWheelOffGround)
            sound = rider->wheelPoses[0].sound;
        else if (rider->wheelPoses[1].ground != kWheelOffGround)
            sound = rider->wheelPoses[1].sound;
        if (sound < 16)
            surface = kRollingSurface[sound];

        uint32_t brake = 0;
        uint32_t pedal = 0;
        int32_t rate = rider->wheelPoses[1].spin;
        if (actionState(rider, uint32_t(kLevelAction)) == kActionRiding &&
            actionState(rider, uint32_t(kLevelSubState)) == kRidingBrake && *g_brakeDrag <= rider->wheelDrag)
        {
            brake = 1;
            rate = rider->wheelPoses[0].spin;
        }
        else if (actionState(rider, uint32_t(kLevelAction)) == kActionRiding &&
                 actionState(rider, uint32_t(kLevelSubState)) == kRidingPedal)
        {
            pedal = 1;
        }
        else if (actionState(rider, uint32_t(kLevelAction)) == kActionRiding)
        {
            if (actionState(rider, uint32_t(kLevelSubState)) == kRidingFakie)
                rate = 0;
        }
        const int32_t code = surfaceSoundFlags(brake, pedal, kGroundSoundKind, uint32_t(surface));
        // The original's last act: a jump to the sound, its frame gone.
        calls::updateRollSound.tail(rider->rollingVoice, uint32_t(code), rider->drawnPosition, rate);
    }
    READABLE_FUNCTION(groundAfter, Verified);
}

namespace game::bmx::rider
{
    namespace
    {
        // v's y and z the other way (the models' y and z run against the
        // physics').
        void flipYZ(ee::Float *v)
        {
            const ee::Float z = -v[2];
            v[1] = -v[1];
            v[2] = z;
        }
        // The rows' up and forward the other way.
        void flipUpForward(zge::Matrix &rows)
        {
            zge::vec3Negate(&rows.row(1).xyz(), &rows.row(1).xyz());
            zge::vec3Negate(&rows.row(2).xyz(), &rows.row(2).xyz());
        }
    }

    // The rider's and the bike's models placed in the air: the body the
    // physics rows; both models at the position and the root offset, less
    // the motion's root in the drawn rows; the bike's root (drawnPosition)
    // from drawnOffset; both turned by the drawn rows with up and forward
    // the other way. DM2's, with the engine's copies and transforms inline.
    void poseBikeAir(Rider *rider)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> rows;
        *matrix(rider->body) = *matrix(rider->right);
        *rows = *matrix(rider->right);
        zge::matrixIdentity(turn);
        zge::matrixRotateX(turn, turn, rider->bodyPitch);
        zge::matrixMultiply(rows, rows, turn);

        *at = *vec4(rider->motionRoot);
        flipYZ(&at->x);
        *at = zge::transformed(*rows, *at);
        zge::vec3Negate(&at->xyz(), &at->xyz());
        *at = *at + *vec4(rider->rootOffset);
        *at = *at + *vec4(rider->position);
        zge::setObjectPosition(rider->messenger, at);
        zge::setObjectPosition(rider->bikeModel, at);

        *vec4(rider->drawnPosition) = *vec4(rider->drawnOffset);
        flipYZ(rider->drawnPosition);
        *vec4(rider->drawnPosition) = zge::transformed(*rows, *vec4(rider->drawnPosition));
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *at;
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(rider->bikeModel, rows);
        zge::setObjectRotationMatrix(rider->messenger, rows);
    }
    READABLE_FUNCTION(poseBikeAir, Verified);

    namespace
    {
        // The bars' bone's name hash ("Handlebars"; riderSetup).
        inline ee::Global<uint32_t, sym::g_handlebarsBoneHash> g_handlebarsBoneHash;
        // A trick's flags (one word a trick): these bits turn the bars
        // themselves (DM2's test).
        inline ee::Global<uint32_t[256], sym::g_trickFlags2> g_trickFlags;
        constexpr uint32_t kTrickTurnsBars = 0x6Cu;
        // The bars turn against the yaw rate, 1.35 times it. In a level-1
        // state 1 (the riding's pedalling) only from 0.35 of this speed.
        inline ee::Global<ee::Float, sym::g_barsTurnByYawRate> g_barsTurnByYawRate; // -1.35
        inline ee::Global<ee::Float, sym::g_leanFrom> g_leanFrom;                   // 0.0667
        inline ee::Global<ee::Float, sym::g_pedalBarsTurnFrom> g_pedalBarsTurnFrom; // 0.35
        // In the manual (state 10) the bike's root is lifted by 0.1 less
        // 0.15 of the forward row's y.
        inline ee::Global<ee::Float, sym::g_manualLiftByForward> g_manualLiftByForward;
        inline ee::Global<ee::Float, sym::g_manualLift> g_manualLift;
        constexpr ee::Float kTurnsToAngle = 65536.0f;
    }

    // The bike and rider placed on the ground: the physics rows and
    // position taken from the drawn body; both models at the drawn position
    // less the bike's root (drawnOffset, y and z the other way) in the
    // body's rows; the root offset the motion's root from the bike's in
    // those rows; both turned by the rows with up and forward the other
    // way; the bones posed. Then the bars turn against the yaw rate and the
    // hands follow their grips, unless the trick on top turns the bars
    // itself (kFlagBarsHeld, set here and kept until the physics state
    // changes). DM2's, but DM2 turns the bars only below a speed, the
    // slower the more, up to a quarter turn; BMX XXX at any speed (in a
    // level-1 state 1 only from a speed up), with no limit.
    void poseBikeGround(Rider *rider)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Quaternion> barsTurn;
        *matrix(rider->right) = *matrix(rider->body);
        *rows = *matrix(rider->body);
        *vec4(rider->position) = *vec4(rider->drawnPosition);
        *at = *vec4(rider->drawnOffset);
        if (physicsState(rider) == kPhysicsManual)
            at->y = at->y + (*g_manualLift - rider->forward[1] * *g_manualLiftByForward);
        flipYZ(&at->x);
        *at = zge::transformed(*rows, *at);
        *at = *vec4(rider->drawnPosition) - *at;
        zge::setObjectPosition(rider->bikeModel, at);
        zge::setObjectPosition(rider->messenger, at);

        *vec4(rider->rootOffset) = *vec4(rider->motionRoot) - *vec4(rider->drawnOffset);
        flipYZ(rider->rootOffset);
        *vec4(rider->rootOffset) = zge::transformed(*rows, *vec4(rider->rootOffset));
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(rider->bikeModel, rows);
        zge::setObjectRotationMatrix(rider->messenger, rows);
        poseRiderBones(rider);

        if (rider->trickMotion != kNoTrick && hasFlag(rider, kFlagTopTrick) &&
            ((*g_trickFlags)[rider->trickMotion] & kTrickTurnsBars) != 0)
            setFlag(rider, kFlagBarsHeld);
        if (hasFlag(rider, kFlagBarsHeld))
            return;
        int16_t bars = 0;
        if (rider->yawRate != 0 && ee::Float(0.0f) < rider->speed)
        {
            if (actionState(rider, uint32_t(kLevelSubState)) != 1 || *g_leanFrom * *g_pedalBarsTurnFrom <= rider->speed)
                bars = static_cast<int16_t>(ee::toInt(ee::toFloat(rider->yawRate) * ee::Float(1.0f / 65536.0f) *
                                                      *g_barsTurnByYawRate * kTurnsToAngle));
        }
        if (bars == 0)
            return;
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, bars);
        zge::matrixRotateX(turn, turn, rider->barsTilt);
        zge::matrixToQuaternion(barsTurn, turn);
        zge::setBoneRotation(rider->bikeBarsBone, barsTurn);
        zge::invalidateAnimator(rider->bikeModel);
        zge::updatePose(rider->bikeModel);
        for (int hand = 0; hand < 2; ++hand)
        {
            const ee::Ptr<zge::SceneBone> barsBone = zge::findModelBone(rider->bikeModel, *g_handlebarsBoneHash);
            zge::objectBonePoint(rider->bikeModel, barsBone, ee::Ptr<const zge::Vec4>(vec4(rider->grips[hand])), at, 0u);
            zge::aimBoneChain(&rider->limbTargets[hand], at);
        }
        zge::invalidateAnimator(rider->messenger);
    }
    READABLE_FUNCTION(poseBikeGround, Verified);

    namespace
    {
        // The rider model's root bone's name hash ("Hips"; riderSetup).
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsBoneHash;
        // The rolling sound's code in the air: kind 1, surface 3 (no flags).
        constexpr uint32_t kAirSoundKind = 1;
        constexpr uint32_t kAirSoundSurface = 3;
    }

    // The air states' after callback: the rider's model turned by the drawn
    // rows and its Hips bone given the motion's root (rotation and place);
    // the root offset the motion's root in the physics rows in the grind
    // and stall approaches (else none); the models placed, the bones posed,
    // and the rolling sound as in the air. DM2's, which leaves the Hips
    // bone alone.
    void airAfter(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> rows;
        *rows = *matrix(rider->right);
        zge::matrixIdentity(turn);
        zge::matrixRotateX(turn, turn, rider->bodyPitch);
        zge::matrixMultiply(rows, rows, turn);
        zge::setObjectRotationMatrix(rider->messenger, rows);
        const ee::Ptr<zge::AnimationChannel> hips = zge::findAnimatorBone(rider->messenger, *g_hipsBoneHash);
        zge::setBoneRotation(hips, ee::Ptr<zge::Quaternion>(quaternion(rider->motionRootRotation)));
        zge::setBoneTranslation(hips, ee::Ptr<zge::Vec4>(vec4(rider->motionRoot)));
        zge::markPoseStale(rider->messenger);

        const uint8_t state = physicsState(rider);
        if (state == kPhysicsGrindApproach || state == kPhysicsStallApproach)
        {
            *vec4(rider->rootOffset) = *vec4(rider->motionRoot);
            flipYZ(rider->rootOffset);
            *vec4(rider->rootOffset) = zge::transformed(*matrix(rider->right), *vec4(rider->rootOffset));
        }
        else
        {
            *vec4(rider->rootOffset) = zge::kZero;
        }
        poseBikeAir(rider);
        poseRiderBones(rider);
        const int32_t sound = surfaceSoundFlags(0u, 0u, kAirSoundKind, kAirSoundSurface);
        calls::updateRollSound(rider->rollingVoice, uint32_t(sound), rider->drawnPosition, rider->wheelPoses[1].spin);
    }
    READABLE_FUNCTION(airAfter, Verified);

    // Physics state 0's after callback: the bike posed on the ground, then
    // the rolling sound (the original's last act a jump to it). DM2's
    // groundAfter is both in one.
    void groundPoseAfter(Rider *rider)
    {
        poseBikeGround(rider);
        game::tail(groundAfter, rider);
    }
    READABLE_FUNCTION(groundPoseAfter, Verified);

    // The after callback of physics states 14 and 15 (BMX XXX's own): the
    // models placed as in the air, then the ground's rolling sound.
    void airPoseGroundSoundAfter(Rider *rider)
    {
        poseBikeAir(rider);
        game::tail(groundAfter, rider);
    }
    READABLE_FUNCTION(airPoseGroundSoundAfter, Draft);

    namespace
    {
        inline ee::Global<uint32_t, sym::g_hipsDeckBoneHash> g_bikeRootName; // the bike model's root bone ("Hipsdeck")
        constexpr uint32_t kWorldSpace = 0; // objectBoneMatrix's
    }

    // The after callback of physics states 9 and 10 (the nose wheelie and the manual; BMX XXX's own): the bike
    // balanced on one wheel (the front's in 9, the back's in 10). The physics rows the drawn body's; the position
    // the balancing wheel's ground point (its point on the body, its spring's length down) in the body's rows. The
    // models are placed so that the bike's root bone keeps that wheel's point where it is: at the position, less
    // the wheel point in the root bone's rows, the root offset (the motion's root from the bike's root, and the wheel
    // point taken off) in the body's rows, and the motion's root (y and z the other way) in them; both turned by
    // the body's rows with up and forward the other way. Then the rider's bones and the ground's rolling sound.
    void wheelieAfter(Rider *rider)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> pivot;      // the wheel's point turned with the bike's root bone
        ee::Local<zge::Matrix> rows;     // the drawn body
        ee::Local<zge::Matrix> rootBone; // the bike's root bone in the world, no translation
        *matrix(rider->right) = *matrix(rider->body);
        *rows = *matrix(rider->body);
        const uint32_t wheel = physicsState(rider) == kPhysicsManual ? 1u : 0u;
        const ee::Float *wheelPoint = wheel ? rider->backWheelPoint : rider->frontWheelPoint;
        const WheelPose &pose = rider->wheelPoses[wheel];

        *at = *vec4(wheelPoint);
        at->y = at->y + pose.springLength;
        *at = zge::transformed(*rows, *at);
        *vec4(rider->position) = *vec4(rider->drawnPosition) + *at;

        const ee::Ptr<zge::SceneBone> root = zge::findModelBone(rider->bikeModel, *g_bikeRootName);
        zge::objectBoneMatrix(rider->bikeModel, root, rootBone, kWorldSpace);
        flipUpForward(*rootBone);
        *pivot = *vec4(wheelPoint);
        pivot->y = pivot->y + pose.springLength;
        rootBone->row(3) = zge::kZero;
        *pivot = zge::transformed(*rootBone, *pivot);
        zge::vec3Negate(&pivot->xyz(), &pivot->xyz());
        *pivot = *at + *pivot;

        zge::Vec4 &rootOffset = *vec4(rider->rootOffset);
        rootOffset = *vec4(rider->motionRoot) - *vec4(rider->drawnOffset);
        flipYZ(rider->rootOffset);
        zge::vec3Negate(&at->xyz(), &vec4(wheelPoint)->xyz());
        rootOffset = rootOffset + *at;
        rootOffset = zge::transformed(*rows, rootOffset);
        rootOffset = rootOffset + *pivot;

        *at = *vec4(rider->motionRoot);
        flipYZ(&at->x);
        *at = zge::transformed(*rows, *at);
        zge::vec3Negate(&at->xyz(), &at->xyz());
        *at = *vec4(rider->position) + *at;
        *at = rootOffset + *at;
        zge::setObjectPosition(rider->bikeModel, at);
        zge::setObjectPosition(rider->messenger, at);
        flipUpForward(*rows);
        rows->row(3) = zge::kZero;
        zge::setObjectRotationMatrix(rider->bikeModel, rows);
        zge::setObjectRotationMatrix(rider->messenger, rows);
        poseRiderBones(rider);
        groundAfter(rider);
    }
    READABLE_FUNCTION(wheelieAfter, Verified);

    namespace
    {
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_tumbleHipsName;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // A tumble's rows lean 0.095 of the way a frame toward upright and toward the way the rider goes on the
        // ground (when faster than 0.0083 a frame there: a squared 6.9e-5).
        inline ee::Global<ee::Float, sym::g_tumbleUpright> g_tumbleUpright;
        inline ee::Global<ee::Float, sym::g_tumbleLeastLevelSpeed> g_tumbleLeastLevelSpeed;
    }

    // poseTumble (rider, landed): the rider model placed by the air's motions' hips body (the tumble's): the
    // rows from the body's turn, leaned toward upright and toward the way he goes, made orthonormal; the hips
    // body and the model turned by them; the hips bone by the motion's root rotation. Landed, the root offset is
    // the motion's root (y and z the other way) in the rows, and the hips bone is put at the motion's root.
    void poseTumble(Rider *rider, int32_t landed)
    {
        ee::Local<zge::Vec4> step;
        ee::Local<zge::Matrix> rows;
        ee::Ptr<zge::RigidBody> hips = rider->riderAnimDynamics->bodies;
        while (hips && hips->nameHash != *g_tumbleHipsName)
            hips = hips->next;
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        zge::quaternionToMatrix(matrix(rider->right), &hips->orientation);
        zge::vec3Negate(&up.xyz(), &up.xyz());
        zge::vec3Negate(&forward.xyz(), &forward.xyz());
        const ee::Float lean = *g_tumbleUpright;
        *step = *g_worldUp - up;
        *step = *step * lean;
        rows->row(1) = up + *step;
        rows->row(2) = forward;
        *step = *vec4(rider->velocity);
        step->y = 0.0f;
        if (*g_tumbleLeastLevelSpeed < zge::dot(*step, *step))
            rows->row(2) = zge::unit(*step);
        *step = rows->row(2) - forward;
        *step = *step * lean;
        rows->row(2) = forward + *step;
        rows->row(0) = zge::cross(rows->row(1), rows->row(2));
        rows->row(0) = zge::unit(rows->row(0));
        rows->row(2) = zge::cross(rows->row(0), rows->row(1));
        rows->row(3) = zge::kZero;
        *matrix(rider->right) = *rows;
        flipUpForward(*rows);
        zge::matrixToQuaternion(&hips->orientation, &*rows);
        zge::setObjectOrientation(rider->messenger, ee::Ptr<zge::Quaternion>(&hips->orientation));
        const ee::Ptr<zge::AnimationChannel> bone = zge::findAnimatorBone(rider->messenger, *g_tumbleHipsName);
        zge::setBoneRotation(bone, ee::Ptr<zge::Quaternion>(quaternion(rider->motionRootRotation)));
        zge::markPoseStale(rider->messenger);
        if (!landed)
            return;
        zge::Vec4 &rootOffset = *vec4(rider->rootOffset);
        rootOffset = *vec4(rider->motionRoot);
        flipYZ(rider->rootOffset);
        rootOffset = zge::transformed(*matrix(rider->right), rootOffset);
        // The hips bone's place is worked out (the position plus the root offset) and not used: the bone is given
        // the motion's root itself (bug kept).
        *step = *vec4(rider->position) + rootOffset;
        zge::setBoneTranslation(bone, ee::Ptr<zge::Vec4>(vec4(rider->motionRoot)));
    }
    READABLE_FUNCTION(poseTumble, Verified);

    // The after callback of physics states 12 and 13 (the tumble; BMX XXX's own): the tumble's pose, landed in 13.
    void tumbleAfter(Rider *rider)
    {
        game::tail(poseTumble, rider, int32_t(physicsState(rider) == kPhysicsTumbleLanded));
    }
    READABLE_FUNCTION(tumbleAfter, Verified);

    // ghostRideAfter (rider): physics states 16 and 17's after callback: the rider model placed by the tumble; the
    // riderless bike's model placed by the drawn body (its root's offset, y and z the other way, through the body's
    // rows, from the drawn position; the rows' up and forward the other way) and its rolling sound (groundAfter);
    // in the ghost ride's tumble the bike tumbles instead.
    void ghostRideAfter(Rider *rider)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Matrix> rows;
        if (physicsState(rider) == kPhysicsGhostRideTumble)
        {
            poseTumble(rider, 0);
            bikeTumbleMove(rider);
            return;
        }
        poseTumble(rider, 0);
        *rows = *matrix(rider->body);
        *at = *vec4(rider->drawnOffset);
        flipYZ(&at->x);
        *at = zge::transformed(*rows, *at);
        *at = *vec4(rider->drawnPosition) - *at;
        zge::setObjectPosition(rider->bikeModel, at);
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(rider->bikeModel, rows);
        groundAfter(rider);
    }
    READABLE_FUNCTION(ghostRideAfter, Verified);

    namespace
    {
        // The pull-up's rows go upright this share of the way a frame (0.085).
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_pullUpUprightShare;
    }

    // pullUpAfter (rider): physics state 11's after callback (the bail's pull-up off a held edge): the rows a share
    // of the way upright (forward as it is, made square again); the root offset the motion's root (y and z the
    // other way) in the rows; the rider model placed at the position by it (less the motion's root) and turned by
    // the rows (up and forward the other way); the air's rolling sound.
    void pullUpAfter(Rider *rider)
    {
        ee::Local<zge::Vec4> at; // the up row's step upright, then the model's place
        ee::Local<zge::Matrix> rows;
        *at = *g_worldUp - *vec4(rider->up);
        *at = *at * *g_pullUpUprightShare;
        rows->row(1) = *vec4(rider->up) + *at;
        rows->row(2) = *vec4(rider->forward);
        rows->row(0) = zge::cross(rows->row(1), rows->row(2));
        rows->row(0) = zge::unit(rows->row(0));
        rows->row(2) = zge::cross(rows->row(0), rows->row(1));
        rows->row(3) = zge::kZero;
        *matrix(rider->right) = *rows;
        *vec4(rider->rootOffset) = *vec4(rider->motionRoot);
        flipYZ(rider->rootOffset);
        *vec4(rider->rootOffset) = zge::transformed(*rows, *vec4(rider->rootOffset));
        *at = *vec4(rider->motionRoot);
        flipYZ(&at->x);
        *at = zge::transformed(*rows, *at);
        zge::vec3Negate(&at->xyz(), &at->xyz());
        *at = *at + *vec4(rider->rootOffset);
        *at = *at + *vec4(rider->position);
        zge::setObjectPosition(rider->messenger, at);
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(rider->messenger, rows);
        const int32_t sound = surfaceSoundFlags(0u, 0u, kAirSoundKind, kAirSoundSurface);
        calls::updateRollSound(rider->rollingVoice, uint32_t(sound), rider->drawnPosition, rider->wheelPoses[1].spin);
    }
    READABLE_FUNCTION(pullUpAfter, Draft);
}

namespace game::bmx::rider
{
    namespace
    {
        // The grinds' tricks (0x3C .. 0x46); the last holds the front wheel
        // too (DM2's 0x44, a grind on both pegs).
        constexpr uint8_t kFirstGrindTrick = 0x3C;
        constexpr uint8_t kGrindTricks = 11;
        constexpr uint8_t kGrindBothWheels = 0x46;
        // The contact kinds a grind can have: 5 an edge (a rail), the
        // object's and kind 6 nothing to stay on, the rest faces.
        constexpr uint8_t kContactEdge = 5;
        // Scaffolding (unnamed-field): docs/SCAFFOLDING.md
        constexpr uint8_t kContactKind6 = 6; // BMX XXX's own kind, not named: it holds no face
        // The grind's rolling sound's surface by the rail's wall kind: 1
        // gives 7, 2 gives 2 (and no sparks), others 0 (DM2's).
        constexpr uint32_t kRailSoundKind = 2;
        constexpr uint32_t kRailNoSparks = 2;
        constexpr uint8_t kRumbleFull = 0xFF;

        // Above this speed (0.03) a grind rumbles the pad.
        inline ee::Global<ee::Float, sym::g_grindRumbleSpeed> g_grindRumbleSpeed;
        // Sparks: from up to 0.15 off the rider in a random direction, at
        // 0.75 .. 1.25 of the velocity, a little up (1/24), 4 / 0.35 of the
        // speed of them.
        inline ee::Global<ee::Float, sym::g_sparkSpread> g_sparkSpread;       // 0.01 a step of 0 .. 15
        inline ee::Global<ee::Float, sym::g_sparkSpeedStep> g_sparkSpeedStep; // 1/30 a step of 0 .. 15
        inline ee::Global<ee::Float, sym::g_sparkLift> g_sparkLift;
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed; // 0.35
        constexpr ee::Float kSparkSpeedLeast = 0.75f;
        constexpr ee::Float kSparksPerSpeed = 4.0f;
        // The grind sound's rate: 2 / pi of the speed, as a 16.16 value.
        inline ee::Global<ee::Float, sym::g_grindSoundRate> g_grindSoundRate;
    }

    // The after callback of the grind (4) and the stall (6): the wheels
    // stop (the front one too on both pegs); on a moving object's rail or
    // face the rider is kept on it (the nearest point of the edge or the
    // face); the rider's model turned by the rows, the root offset the
    // motion's root in them, the models placed (poseBikeAir) and the bones
    // posed. A grind rumbles above a speed and throws sparks (not off a
    // kind 2 rail); both make the rolling sound by the rail. DM2's.
    void grindAfter(Rider *rider)
    {
        ee::Local<zge::Vec4> a;
        ee::Local<zge::Vec4> b;
        ee::Local<zge::Vec4> c;
        ee::Local<zge::Vec4> nearest;
        ee::Local<zge::Vec4> direction;
        ee::Local<calls::Rumble> request;
        ee::Local<zge::Vec4> sparkAt;
        ee::Local<zge::Vec4> sparkMotion;

        rider->wheelPoses[1].spin = 0;
        if (rider->trick == kGrindBothWheels)
            rider->wheelPoses[0].spin = 0;

        // A rail or face of a moving object: on it.
        if (static_cast<uint8_t>(rider->trick - kFirstGrindTrick) < kGrindTricks && rider->contactKind < kContactKinds &&
            rider->contactKind != kContactObject && rider->contactKind != kContactKind6)
        {
            const uint64_t ref = contactRef(rider);
            const ee::Ptr<zge::Node> owner(static_cast<uint32_t>(collision::nodeOf(collision::widen(uint32_t(collision::refNode(ref))))));
            if (zge::nodeSceneObject(owner, ee::Ptr<ee::Ptr<zge::SceneBone>>()))
            {
                if (rider->contactKind == kContactEdge)
                {
                    collision::edgeDirection(ref, &direction->x);
                    collision::edgePoint(ref, 0u, &a->x);
                    collision::edgePoint(ref, 1u, &b->x);
                    zge::closestPointOnSegment(a, b, direction, vec4(rider->position), nearest);
                }
                else
                {
                    collision::faceVertex(ref, 0u, &a->x);
                    collision::faceVertex(ref, 1u, &b->x);
                    collision::faceVertex(ref, 2u, &c->x);
                    zge::closestPointOnTriangle(vec4(rider->position), a, b, c, nearest, nullptr);
                }
                *vec4(rider->position) = *nearest;
            }
        }

        zge::setObjectRotationMatrix(rider->messenger, ee::Ptr<zge::Matrix>(matrix(rider->right)));
        *vec4(rider->rootOffset) = *vec4(rider->motionRoot);
        flipYZ(rider->rootOffset);
        *vec4(rider->rootOffset) = zge::transformed(*matrix(rider->right), *vec4(rider->rootOffset));
        poseBikeAir(rider);
        poseRiderBones(rider);

        uint32_t surface = 0;
        if (physicsState(rider) == kPhysicsGrind)
        {
            if (*g_grindRumbleSpeed < rider->speed && !hasFlag(rider, kFlagNoPlayer))
            {
                request->strength = kRumbleFull;
                request->motor = 0;
                request->timing = 0;
                calls::rumble(uint32_t(rider->player), request);
            }
            const uint32_t wall = collision::edgeWallKind(contactRef(rider));
            surface = wall == 1 ? 7u : wall == 2 ? 2u : 0u;
            if (surface != kRailNoSparks)
            {
                *sparkAt = zge::kZero;
                *sparkMotion = zge::kZero;
                const int32_t spread = int32_t(zge::randomNumber() & 0xF);
                sparkAt->y = 0.0f;
                sparkAt->z = 0.0f;
                sparkAt->x = ee::toFloat(spread) * *g_sparkSpread;
                zge::Vec3 &at = sparkAt->xyz();
                zge::vec3RotateY(&at, &at, static_cast<int16_t>(zge::randomNumber()));
                zge::vec3RotateX(&at, &at, static_cast<int16_t>(zge::randomNumber()));
                *sparkAt = *sparkAt + *vec4(rider->position);
                const ee::Float share = ee::toFloat(int32_t(zge::randomNumber() & 0xF)) * *g_sparkSpeedStep + kSparkSpeedLeast;
                // The scale carries what the random number's call left
                // beside the share, as airGravity's does.
                *sparkMotion = zge::scaledCarrying(*vec4(rider->velocity), share, zge::scaleLeftover(zge::LeftBy::lastCall));
                const ee::Float perSpeed = kSparksPerSpeed / *g_topSpeed;
                sparkMotion->y = sparkMotion->y - *g_sparkLift;
                bmx::sparks(&sparkAt->x, &sparkMotion->x, ee::toInt(rider->speed * perSpeed));
            }
        }
        const int16_t rate = static_cast<int16_t>(ee::toInt(rider->speed * *g_grindSoundRate * ee::Float(65536.0f)));
        const int32_t code = surfaceSoundFlags(0u, 0u, kRailSoundKind, surface);
        calls::updateRollSound(rider->rollingVoice, uint32_t(code), rider->drawnPosition, int32_t(rate));
    }
    READABLE_FUNCTION(grindAfter, Verified);
}
