// DM2's bike pose (physics.c): the drawn rider and bike placed from the
// physics after the move, on the ground, in the air and on rails; the
// states' after callbacks (g_afterCallbacks).
//
// The drawn rows are the physics rows turned about the right row by the
// body's pitch (bodyPitch); the drawn position is the physics position plus
// the motion's root offset, less the motion's root turned by the drawn rows.
//
// Counterparts: DM2 GameCube 0x80076EC0
// (poseBikeAir), 0x80083534 (airAfter), 0x800769AC (poseBikeGround),
// 0x80083384 (groundAfter), 0x800836A8 (grindAfter); BMX XXX PS2 poseBikeAir,
// groundAfter, grindAfter (medium).

#include "physics.h"

#include "../collision/collision.h"

#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/collision/closest.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/physics/pose.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // The rolling sound: the voice played by a sound code at a position
        // and a rate (the game side's).
        inline constexpr ee::FunctionPtr<void(uint32_t voice, int32_t code, const ee::Float *position, int32_t rate)>
            updateRollSound{sym::updateRollSound};
        // The rolling sound's code in the air: kind 1, surface 3 (no flags).
        constexpr uint32_t kAirSoundKind = 1;
        constexpr uint32_t kAirSoundSurface = 3;
        // Set: the drawn rider keeps the motion's root offset in every air
        // state (else only in the grind and stall approaches; 0 in DM2's data,
        // inferred a debug switch).
        inline ee::Global<int32_t, sym::g_rootOffsetInAir> g_rootOffsetInAir;

        // The drawn rows: the physics rows turned by the body's pitch.
        void drawnRows(Rider *rider, ee::Ptr<zge::Matrix> rows, ee::Ptr<zge::Matrix> turn)
        {
            engine::copyMatrix(rows, rider->right);
            zge::matrixIdentity(turn);
            zge::matrixRotateX(turn, turn, int32_t(rider->bodyPitch));
            engine::matrixProduct(rows, rows, turn);
        }

        // v = the motion's root (y and z the other way) turned by the rows.
        void rootIn(ee::Float *v, ee::Address rows, const ee::Float *root)
        {
            *vec4(v) = *vec4(root);
            const ee::Float y = -v[1];
            v[2] = -v[2];
            v[1] = y;
            engine::transformPoint(v, rows, v);
        }
        // The rows' up and forward the other way: the models' y and z.
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
    // the other way (the models' y and z). GameCube 0x80076EC0.
    void poseBikeAir(Rider *rider)
    {
        ee::Local<ee::Float[4]> at;
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> rows;
        engine::copyMatrix(rider->body, rider->right);
        drawnRows(rider, rows, turn);

        rootIn(at, rows, rider->motionRoot);
        zge::vec3Negate(&vec4(at)->xyz(), &vec4(at)->xyz());
        *vec4(at) = *vec4(at) + *vec4(rider->rootOffset);
        *vec4(at) = *vec4(at) + *vec4(rider->position);
        engine::setObjectPosition(rider->messenger, at);
        engine::setObjectPosition(rider->bikeModel, at);

        rootIn(rider->drawnPosition, rows, rider->drawnOffset);
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(at);
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->bikeModel), rows);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->messenger), rows);
    }
    READABLE_FUNCTION(poseBikeAir, Verified);

    // The after callback of states 1, 3, 5, 7 and 8: the rider's model turned
    // by the drawn rows; the root offset the motion's root in the physics
    // rows in the grind and stall approaches (else none); the models placed
    // (poseBikeAir), the bones posed, and the rolling sound as in the air.
    // GameCube 0x80083534.
    void airAfter(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> rows;
        drawnRows(rider, rows, turn);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->messenger), rows);
        const uint8_t state = physicsState(rider);
        if (state == kPhysicsGrindApproach || state == kPhysicsStallApproach || *g_rootOffsetInAir != 0)
            rootIn(rider->rootOffset, rider->right, rider->motionRoot);
        else
            setOrigin(rider->rootOffset);
        poseBikeAir(rider);
        poseRiderBones(rider);
        const int32_t sound = surfaceSoundFlags(0u, 0u, kAirSoundKind, kAirSoundSurface);
        updateRollSound(rider->rollingVoice, sound, rider->drawnPosition, rider->wheelPoses[1].spin);
    }
    READABLE_FUNCTION(airAfter, Verified);

    namespace
    {
        // An object's parts' matrices from its bones (DM2's engine; AI has
        // none).
        inline void modelPose(uint32_t object) { ee::game(sym::modelPose, object); }
        // The bars' part's name hash (set at start-up).
        inline ee::Global<uint32_t, sym::g_handlebarsBoneHash> g_barsPartName;
        // A trick's flags (one word a trick): these bits turn the bars
        // themselves (spins, whips; inferred).
        inline ee::Global<uint32_t[256], sym::g_trickFlags2> g_trickFlags;
        constexpr uint32_t kTrickTurnsBars = 0x6Cu;
        // The bars turn with the yaw rate below this speed (0.35), the
        // slower the more, up to a quarter turn either way.
        inline ee::Global<ee::Float, sym::g_leanFrom> g_barsTurnTopSpeed;
        constexpr int32_t kBarsTurnMost = 0x4000;
        // In the manual (state 10) the bike's root is lifted by 0.1 less
        // 0.15 of the forward row's y.
        inline ee::Global<ee::Float, sym::g_manualLiftByForward> g_manualLiftByForward;
        inline ee::Global<ee::Float, sym::g_manualLift> g_manualLift;
    }

    // The bike and rider placed on the ground (states 0, 9, 10): the physics
    // rows and position taken from the drawn body; both models at the drawn
    // position less the bike's root (drawnOffset, y and z the other way) in
    // the body's rows; the root offset the motion's root from the bike's in
    // those rows; both turned by the rows with up and forward the other way;
    // the bones posed. While turning slowly (and no trick turning the bars)
    // the bars turn with the yaw rate and the hands follow their grips.
    // GameCube 0x800769AC.
    void poseBikeGround(Rider *rider)
    {
        ee::Local<ee::Float[4]> at;
        ee::Local<zge::Matrix> rows; // right, up, forward, position
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Quaternion> barsTurn;
        engine::copyMatrix(rider->right, rider->body);
        engine::copyMatrix(rows, rider->body);
        *vec4(rider->position) = *vec4(rider->drawnPosition);
        *vec4(at) = *vec4(rider->drawnOffset);
        if (physicsState(rider) == kPhysicsManual)
            at[1] = at[1] + (*g_manualLift - rider->forward[1] * *g_manualLiftByForward);
        const ee::Float z = -at[2];
        at[1] = -at[1];
        at[2] = z;
        engine::transformPoint(at, rows, at);
        *vec4(at) = *vec4(rider->drawnPosition) - *vec4(at);
        engine::setObjectPosition(rider->bikeModel, at);
        engine::setObjectPosition(rider->messenger, at);

        *vec4(rider->rootOffset) = *vec4(rider->motionRoot) - *vec4(rider->drawnOffset);
        const ee::Float rootZ = -rider->rootOffset[2];
        rider->rootOffset[1] = -rider->rootOffset[1];
        rider->rootOffset[2] = rootZ;
        engine::transformPoint(rider->rootOffset, rows, rider->rootOffset);
        flipUpForward(*rows);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->bikeModel), rows);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->messenger), rows);
        poseRiderBones(rider);

        if (rider->trickMotion != kNoTrick && (rider->flags & kFlagTopTrick) != 0 &&
            ((*g_trickFlags)[rider->trickMotion] & kTrickTurnsBars) != 0)
            return;
        int16_t bars = 0;
        const ee::Float top = *g_barsTurnTopSpeed;
        if (rider->yawRate != 0 && ee::Float(0.0f) < rider->speed && rider->speed < top)
        {
            const int16_t angle = static_cast<int16_t>(ee::toInt(
                -(ee::toFloat(rider->yawRate) * ee::Float(1.0f / 65536.0f) * (top - rider->speed) / top) *
                ee::Float(65536.0f)));
            if (angle < -kBarsTurnMost)
                bars = -kBarsTurnMost;
            else if (angle < kBarsTurnMost)
                bars = angle;
            else
                bars = kBarsTurnMost;
        }
        if (bars == 0)
            return;
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, int32_t(bars));
        zge::matrixRotateX(turn, turn, int32_t(rider->barsTilt));
        zge::matrixToQuaternion(barsTurn, turn);
        zge::setBoneRotation(rider->bikeBarsBone, barsTurn);
        modelPose(rider->bikeModel);
        const ee::Ptr<zge::SceneObject> bike(rider->bikeModel);
        for (int hand = 0; hand < 2; ++hand)
        {
            const ee::Ptr<zge::SceneBone> part = zge::findModelBone(bike, *g_barsPartName);
            zge::objectBonePoint(bike, part, vec4(rider->grips[hand]), vec4(at), 0u);
            zge::aimBoneChain(&rider->limbTargets[hand], vec4(at));
        }
    }
    READABLE_FUNCTION(poseBikeGround, Verified);

    namespace
    {
        // The rolling sound's surface by the wheel's surface sound (0 .. 15;
        // groundAfter's jump table).
        constexpr uint8_t kRollingSurface[16] = {0, 7, 2, 5, 5, 4, 6, 6, 4, 5, 8, 1, 1, 5, 5, 0};
        constexpr uint32_t kGroundSoundKind = 1;
        // The manual's own rolling sound from this far into it.
        inline ee::Global<ee::Float, sym::g_manualBlend> g_manualSoundFrom; // 0.05
    }

    // The after callback of states 0, 9 and 10: the bike posed on the
    // ground, then the rolling sound by the surface under the first wheel on
    // the ground, at the front wheel's rate: in a manual (action states 0,
    // 2) the manual's sound at the back wheel's rate, a crouch (0, 1) its
    // own, a level-1 state 4 silent. GameCube 0x80083384; BMX XXX PS2
    // groundAfter.
    void groundAfter(Rider *rider)
    {
        // (Its callees run below the original's frame, 0x80 bytes: the dead
        // words they leave are where DM2 has them. At 0x70, airMove's first
        // wheel point's w, the lane airEnter's lift never writes, sat 16
        // bytes off and reached the record's position w after a stall's
        // take-off.)
        poseBikeGround(rider);
        uint32_t sound = 0;
        uint8_t surface = 0;
        if (rider->wheelPoses[0].ground != kWheelOffGround)
            sound = rider->wheelPoses[0].sound;
        else if (rider->wheelPoses[1].ground != kWheelOffGround)
            sound = rider->wheelPoses[1].sound;
        if (sound < 16)
            surface = kRollingSurface[sound];

        uint32_t manual = 0;
        uint32_t crouch = 0;
        int32_t rate = rider->wheelPoses[1].spin;
        if (actionState(rider, 0u) == kActionRiding && actionState(rider, 1u) == 2 &&
            *g_manualSoundFrom <= rider->wheelDrag)
        {
            manual = 1;
            rate = rider->wheelPoses[0].spin;
        }
        else if (actionState(rider, 0u) == kActionRiding && actionState(rider, 1u) == 1)
        {
            crouch = 1;
        }
        else if (actionState(rider, 0u) == kActionRiding)
        {
            if (actionState(rider, 1u) == 4)
                rate = 0;
        }
        const int32_t code = surfaceSoundFlags(manual, crouch, kGroundSoundKind, uint32_t(surface));
        // The original's last act: a jump to the sound, its frame gone.
        updateRollSound.tail(rider->rollingVoice, code, rider->drawnPosition, rate);
    }
    READABLE_FUNCTION(groundAfter, Verified);

    namespace
    {
        // DM2's random number (the same routine as zge::randomNumber, called
        // through the game: the carried register below is what it leaves).
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline int32_t randomNumberByCall() { return ee::game<int32_t>(sym::randomNumber); }

        // The grinds' tricks (0x3C .. 0x44); 0x44 holds the back wheel too
        // (inferred: a grind on both pegs).
        constexpr uint8_t kFirstGrindTrick = 0x3C;
        constexpr uint8_t kGrindTricks = 9;
        constexpr uint8_t kGrindBothWheels = 0x44;
        // The contact kinds a grind can have: 5 an edge (a rail), 8 an
        // object, the rest faces.
        constexpr uint8_t kContactEdge = 5;
        // The grind's rolling sound's surface by the rail's wall kind: 1
        // gives 7, 2 gives 2 (and no sparks), others 0 (inferred materials).
        constexpr uint32_t kRailSoundKind = 2;
        constexpr uint32_t kRailNoSparks = 2;

        // Above this speed (0.03) a grind rumbles the pad.
        inline ee::Global<ee::Float, sym::g_grindRumbleSpeed> g_grindRumbleSpeed;
        // Sparks: from up to 0.15 off the rider in a random direction, at
        // 0.75 .. 1.25 of the velocity, a little up (1/24), 4 / 0.35 of the
        // speed of them.
        inline ee::Global<ee::Float, sym::g_sparkSpread> g_sparkSpread;   // 0.01 a step of 0 .. 15
        inline ee::Global<ee::Float, sym::g_sparkSpeedStep> g_sparkSpeedStep; // 1/30 a step of 0 .. 15
        inline ee::Global<ee::Float, sym::g_sparkLift> g_sparkLift;
        inline ee::Global<ee::Float, sym::g_topSpeed> g_turnTopSpeed;   // 0.35 (the Turn tuning's)
        constexpr ee::Float kSparkSpeedLeast = 0.75f;
        constexpr ee::Float kSparksPerSpeed = 4.0f;
        // The grind sound's rate: 2 / pi of the speed, as a 16.16 value.
        inline ee::Global<ee::Float, sym::g_grindSoundRate> g_grindSoundRate;
    }

    // The after callback of the grind (4) and the stall (6): the wheels
    // stop (the back one too on both pegs); on a moving object's rail or
    // face the rider is kept on it (the nearest point of the edge's line or
    // the face); the rider's model turned by the rows, the root offset the
    // motion's root in them, the models placed (poseBikeAir) and the bones
    // posed. A grind rumbles above a speed and throws sparks (not off a
    // kind 2 rail); both make the rolling sound by the rail. GameCube
    // 0x800836A8; BMX XXX PS2 grindAfter.
    void grindAfter(Rider *rider)
    {
        ee::Local<ee::Float[4]> a;
        ee::Local<ee::Float[4]> b;
        ee::Local<ee::Float[4]> c;
        ee::Local<ee::Float[4]> nearest;
        ee::Local<ee::Float[4]> direction;
        ee::Local<ee::Float[4]> sparkAt;
        ee::Local<ee::Float[4]> sparkMotion;

        rider->wheelPoses[1].spin = 0;
        if (rider->trick == kGrindBothWheels)
            rider->wheelPoses[0].spin = 0;

        // A rail or face of a moving object: on it.
        if (static_cast<uint8_t>(rider->trick - kFirstGrindTrick) < kGrindTricks && rider->contactKind < kContactKinds &&
            rider->contactKind != kContactObject)
        {
            const collision::FaceRef ref = rider->contactRef();
            if (collision::engine::nodeObject(collision::nodeOf(collision::widen(collision::refNode(ref))), 0u) != 0)
            {
                if (rider->contactKind == kContactEdge)
                {
                    collision::edgeDirection(ref, direction);
                    collision::edgePoint(ref, 0u, a);
                    collision::edgePoint(ref, 1u, b);
                    zge::closestPointOnSegment(vec4(a), vec4(b), vec4(direction), vec4(rider->position), vec4(nearest));
                }
                else
                {
                    collision::faceVertex(ref, 0u, a);
                    collision::faceVertex(ref, 1u, b);
                    collision::faceVertex(ref, 2u, c);
                    zge::closestPointOnTriangle(vec4(rider->position), vec4(a), vec4(b), vec4(c), vec4(nearest), nullptr); // (no squared distance)
                }
                *vec4(rider->position) = *vec4(nearest);
            }
        }

        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->messenger), matrix(rider->right));
        rootIn(rider->rootOffset, rider->right, rider->motionRoot);
        poseBikeAir(rider);
        poseRiderBones(rider);

        uint32_t surface = 0;
        if (physicsState(rider) == kPhysicsGrind)
        {
            if (*g_grindRumbleSpeed < rider->speed && (rider->flags & kFlagNoPlayer) == 0)
                rumble(uint32_t(rider->player), 0u, 1u);
            const uint32_t wall = collision::edgeWallKind(rider->contactRef());
            surface = wall == 2 ? 2u : wall == 1 ? 7u : 0u;
            if (surface != kRailNoSparks)
            {
                const int32_t spread = zge::randomNumber() & 0xF;
                sparkAt[0] = ee::toFloat(spread) * *g_sparkSpread;
                sparkAt[1] = 0.0f;
                sparkAt[2] = 0.0f;
                zge::Vec3 &at = vec4(sparkAt)->xyz();
                zge::vec3RotateY(&at, &at, int32_t(static_cast<int16_t>(zge::randomNumber())));
                zge::vec3RotateX(&at, &at, int32_t(static_cast<int16_t>(zge::randomNumber())));
                *vec4(sparkAt) = *vec4(sparkAt) + *vec4(rider->position);
                const ee::Float share = ee::toFloat(randomNumberByCall() & 0xF) * *g_sparkSpeedStep + kSparkSpeedLeast;
                // The scale carries what the last call left beside the share,
                // as airGravity's does.
                *vec4(sparkMotion) = zge::scaledCarrying(*vec4(rider->velocity), share, zge::scaleLeftover(zge::LeftBy::lastCall));
                const ee::Float perSpeed = kSparksPerSpeed / *g_turnTopSpeed;
                sparkMotion[1] = sparkMotion[1] - *g_sparkLift;
                sparks(sparkAt, sparkMotion, ee::toInt(rider->speed * perSpeed));
            }
        }
        const int16_t rate = static_cast<int16_t>(ee::toInt(rider->speed * *g_grindSoundRate * ee::Float(65536.0f)));
        const int32_t code = surfaceSoundFlags(0u, 0u, kRailSoundKind, surface);
        updateRollSound(rider->rollingVoice, code, rider->drawnPosition, int32_t(rate));
    }
    READABLE_FUNCTION(grindAfter, Verified);
}
