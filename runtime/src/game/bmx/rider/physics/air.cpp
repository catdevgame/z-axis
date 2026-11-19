// BMX XXX's air physics (physics.c): the air's forces. DM2's
// (game/dm2/rider/physics/air.cpp), and BMX XXX's rider model's bodies
// carried along.

#include "../rider.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/air.cpp.h"

namespace game::bmx::rider
{
    namespace
    {
        // The air's gravity, units a frame a frame (0.0055; y grows downwards).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        // On a lip the gravity is less by this (0.33) times the rider's wall ride stat.
        inline ee::Global<ee::Float, sym::g_lipGravityScale> g_lipGravityScale;
        // The air's drag: the velocity kept a frame (0.99995).
        inline ee::Global<ee::Float, sym::g_airDrag> g_airDrag;
        // The rider model's bodies take the velocity a second (60 frames).
        constexpr ee::Float kFramesASecond = 60.0f;
    }

    // States 1 and 8's forces: gravity into the velocity (less on a lip by
    // the rider's stat), the speed, then the drag. Then each body of the
    // rider model's dynamics has no force and the rider's velocity, a
    // second's (BMX XXX's own step).
    void airGravity(Rider *rider)
    {
        rider->force[0] = 0.0f;
        rider->force[1] = *g_gravity;
        if (physicsState(rider) == kPhysicsLipTrick)
            rider->force[1] = rider->force[1] * (ee::Float(1.0f) - *g_lipGravityScale * rider->wallRideStat);
        rider->force[2] = 0.0f;
        rider->velocity[1] = rider->velocity[1] + rider->force[1];
        zge::Vec4 &velocity = *vec4(rider->velocity);
        rider->speed = zge::vec3Length(&velocity.xyz());
        // Each scale carries what this function's caller left beside its
        // factor (the state and the length leave it alone), as DM2's does.
        velocity = zge::scaledCarrying(velocity, *g_airDrag, zge::scaleLeftover(zge::LeftBy::caller));
        for (ee::Ptr<zge::RigidBody> body = rider->riderAnimDynamics->bodies; body; body = body->next)
        {
            body->force = zge::kZero;
            body->velocity = zge::scaledCarrying(velocity, kFramesASecond, zge::scaleLeftover(zge::LeftBy::caller));
        }
    }
    READABLE_FUNCTION(airGravity, Verified);

    // The exit of the air states and the approaches (1, 3, 5, 7). To the
    // ground: the root offset the motion's root (y and z the other way) in
    // the drawn body's rows, the position the drawn body's. To a bail: no
    // root offset. Then the back wheel's contact the rider's, at the
    // position. Returns the state left. DM2's.
    uint32_t leaveAirExit(Rider *rider, uint32_t from, uint32_t to)
    {
        if (to == kPhysicsGround)
        {
            *vec4(rider->rootOffset) = *vec4(rider->motionRoot);
            rider->rootOffset[2] = -rider->rootOffset[2];
            rider->rootOffset[1] = -rider->rootOffset[1];
            *vec4(rider->rootOffset) = zge::transformed(*matrix(rider->body), *vec4(rider->rootOffset));
            *vec4(rider->position) = *vec4(rider->drawnPosition);
        }
        else if (to == kPhysicsBail)
        {
            *vec4(rider->rootOffset) = zge::kZero;
        }
        setWheelContact(rider, 1u, rider->contact, uint32_t(rider->contactKind), rider->position);
        return from;
    }
    READABLE_FUNCTION(leaveAirExit, Verified);

    namespace
    {
        // airRotationTrack's turn read as a yaw, a pitch and a roll while the
        // turn tips the up row less than 60 degrees; past it, the engine's
        // Euler angles.
        constexpr zge::Angle kTrackUprightMost = 0x2AAA;
        // Under this the forward row's length on the ground is no direction
        // (the yaw is read off the right row).
        constexpr ee::Float kTrackLeastLength = 0.001f;
        constexpr ee::Float kAngleUnits = 65536.0f; // a turn
    }

    // The angles a frame that turn the rows `current` into `previous` over
    // `frames` frames (an alignment's: airRotate turns the rows by them):
    // the turn between the two; nearly upright, read as the yaw about y
    // (by the forward row on the ground, or the right row's), then the
    // pitch about x and the roll about z, each taken off before the next;
    // tipped further, the engine's Euler angles. Each angle is then shared
    // out over the frames. DM2's reads the Euler angles always.
    void airRotationTrack(const ee::Float *current, ee::Float *previous, int32_t frames, int16_t *angles)
    {
        ee::Local<zge::Matrix> turn;
        const ee::Float share = ee::Float(1.0f) / ee::toFloat(frames);
        *turn = *matrix(current);
        zge::matrixTranspose3(turn, turn);
        zge::matrixMultiply(turn, turn, matrix(previous));
        const auto &m = turn->m;
        if (zge::angleAcos(m[1][1]) < kTrackUprightMost)
        {
            const ee::Float level = ee::sqrt(m[2][0] * m[2][0] + m[2][2] * m[2][2]);
            angles[1] = kTrackLeastLength < level ? zge::angleAtan2(m[2][0], m[2][2]) : zge::angleAtan2(m[0][0], m[0][2]);
            zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(-angles[1]));
            const ee::Float levelAfter = ee::sqrt(m[2][0] * m[2][0] + m[2][2] * m[2][2]);
            angles[0] = zge::angleAtan2(-m[2][1], levelAfter);
            zge::matrixRotateX(turn, turn, static_cast<zge::Angle>(-angles[0]));
            angles[2] = zge::angleAtan2(m[0][1], m[0][0]);
        }
        else
        {
            zge::matrixToEuler(eulerAngles(angles), turn);
        }
        for (int i = 0; i < 3; ++i)
            angles[i] = static_cast<int16_t>(ee::toInt(ee::toFloat(int32_t(angles[i])) * (ee::Float(1.0f) / kAngleUnits) * share * kAngleUnits));
    }
    READABLE_FUNCTION(airRotationTrack, Verified);

    namespace
    {
        // airRotate's realignment to the world's up (kFlagCoastStarted) runs over 20 frames.
        constexpr int16_t kUprightAlignFrames = 20;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsName;

        // Each of the rows (right, up, forward; or forward first) through a matrix as points.
        void transformRows(zge::Matrix &rows, const zge::Matrix &through, bool forwardFirst)
        {
            const int order[3] = {forwardFirst ? 2 : 0, 1, forwardFirst ? 0 : 2};
            for (int row : order)
                rows.row(row) = zge::transformed(through, rows.row(row));
        }
    }

    // airRotate (rider): states 1 and 8's turn: the rows turned by the air's angles (pitch, yaw rate, roll); with
    // kFlagCoastStarted an alignment to the world's up over 20 frames starts. Then, on a copy of the rows: while a snap's
    // frames last, turned toward the target (through alignFrom by alignAngles, or with kFlagSnapAlign by them
    // about x, y and then z); with kFlagTrickTurn a second turn (spinFrom, spinAngles); made orthonormal again.
    // The rows and the drawn body take the copy, and the air's motions' hips body its turn (up and forward the
    // other way). DM2's, which turns the rows themselves and has no bodies.
    void airRotate(Rider *rider)
    {
        ee::Local<zge::Matrix> work;    // the rows being turned (right, up, forward, origin)
        ee::Local<zge::Matrix> turn;    // the air's angles' turn, and the snap's about x, y, then z
        ee::Local<zge::Matrix> through; // a target's inverse
        zge::Matrix *const rows = matrix(rider->right);

        zge::matrixIdentity(turn);
        zge::matrixRotateXYZ(turn, turn, eulerAngles(&rider->pitch));
        zge::matrixMultiply(rows, rows, turn);

        if (hasFlag(rider, kFlagCoastStarted))
        {
            // The rows the alignment heads for: upright, facing as now.
            work->row(1) = *g_worldUp;
            work->row(0) = zge::cross(work->row(1), *vec4(rider->forward));
            work->row(0) = zge::unit(work->row(0));
            work->row(2) = zge::cross(work->row(0), work->row(1));
            *matrix(rider->alignFrom) = *rows;
            airRotationTrack(rider->right, &work->m[0][0], kUprightAlignFrames, rider->alignAngles);
            rider->snapFrames = kUprightAlignFrames;
        }
        *work = *rows;

        if (rider->snapFrames != 0)
        {
            if (!hasFlag(rider, kFlagSnapAlign))
            {
                *through = *matrix(rider->alignFrom);
                zge::matrixTranspose3(through, through);
                transformRows(*work, *through, false);
                zge::matrixRotateXYZ(work, work, eulerAngles(rider->alignAngles));
                zge::matrixTranspose3(through, through);
                transformRows(*work, *through, false);
            }
            else
            {
                zge::matrixIdentity(turn);
                zge::matrixRotateX(turn, turn, rider->alignAngles[0]);
                zge::matrixRotateY(turn, turn, rider->alignAngles[1]);
                zge::matrixMultiply(work, work, turn);
                zge::matrixIdentity(turn);
                zge::matrixRotateZ(turn, turn, rider->alignAngles[2]);
                zge::matrixMultiply(work, work, turn);
            }
            rider->snapFrames = static_cast<int16_t>(rider->snapFrames - 1);
        }
        if (hasFlag(rider, kFlagTrickTurn))
        {
            *through = *matrix(rider->spinFrom);
            zge::matrixTranspose3(through, through);
            transformRows(*work, *through, true);
            zge::matrixRotateXYZ(work, work, eulerAngles(rider->spinAngles));
            zge::matrixTranspose3(through, through);
            transformRows(*work, *through, true);
        }

        zge::Vec4 &right = work->row(0);
        zge::Vec4 &up = work->row(1);
        zge::Vec4 &forward = work->row(2);
        up = zge::unit(up);
        right = zge::cross(up, forward);
        right = zge::unit(right);
        forward = zge::cross(right, up);
        *rows = *work;
        *matrix(rider->body) = *work;

        // The hips body of the air's motions turned with the rows (the first so named; with none, the place the
        // original writes at a null body).
        ee::Ptr<zge::RigidBody> hips = rider->riderAnimDynamics->bodies;
        while (hips && hips->nameHash != *g_hipsName)
            hips = hips->next;
        zge::vec3Negate(&up.xyz(), &up.xyz());
        zge::vec3Negate(&forward.xyz(), &forward.xyz());
        zge::matrixToQuaternion(&hips->orientation, &*work);
    }
    READABLE_FUNCTION(airRotate, Verified);
}
