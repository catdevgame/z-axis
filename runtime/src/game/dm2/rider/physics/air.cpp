// DM2's rider in the air (physics states 1 air and 8 lip, and the snaps 3,
// 5 and 7): the gravity, the turn of the rows by the air's angles and an
// alignment, the rotation a frame, and the exit to the ground.
//
// Counterparts: DM2 GameCube 0x8007D450
// (airGravity), 0x8007C768 (airRotate), 0x80085E5C (airRotationTrack),
// 0x80078B14 (leaveAirExit); BMX XXX PS2 airGravity.

#include "physics.h"

#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/physics/air.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // The air's gravity, units a frame a frame (y grows downwards).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        // On a lip the gravity is less by this times the rider's wall ride stat.
        inline ee::Global<ee::Float, sym::g_lipGravityScale> g_lipGravityScale;
        // The air's drag: the velocity kept a frame (0.99995).
        inline ee::Global<ee::Float, sym::g_airDrag> g_airDrag;
    }

    // States 1 and 8's forces: gravity into the velocity (less on a lip by
    // the rider's stat), the speed, then the drag. GameCube 0x8007D450 (the
    // drag after the speed there too); BMX XXX PS2 airGravity.
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
        // The scale carries what this function's caller left beside the drag
        // (the state and the length leave it alone), where DM2's later vector
        // code finds it (faceCentre's case: a skinned model's vertices).
        velocity = zge::scaledCarrying(velocity, *g_airDrag, zge::scaleLeftover(zge::LeftBy::caller));
    }
    READABLE_FUNCTION(airGravity, Verified);

    // The exit of states 1, 3, 5 and 7. To the ground: the drawn rider's
    // offset is the motion's root (z and y turned round) through the body,
    // and the physics position the drawn one; to a bail no offset. Either
    // way the front wheel's contact is the rider's, at the position. Returns
    // the state left. GameCube 0x80078B14.
    uint32_t leaveAirExit(Rider *rider, uint32_t from, uint32_t to)
    {
        if (to == kPhysicsGround)
        {
            *vec4(rider->rootOffset) = *vec4(rider->motionRoot);
            rider->rootOffset[2] = -rider->rootOffset[2];
            rider->rootOffset[1] = -rider->rootOffset[1];
            engine::transformPoint(rider->rootOffset, rider->body, rider->rootOffset);
            *vec4(rider->position) = *vec4(rider->drawnPosition);
        }
        else if (to == kPhysicsBail)
        {
            setOrigin(rider->rootOffset);
        }
        setWheelContact(rider, 1u, rider->contact, uint32_t(rider->contactKind), rider->position);
        return from;
    }
    READABLE_FUNCTION(leaveAirExit, Verified);

    // The turn from `previous` to `current` (two matrices) as angles, each
    // divided by `frames`: the rotation a frame the air's spin and flip count.
    // GameCube 0x80085E5C.
    void airRotationTrack(const ee::Float *current, const ee::Float *previous, int32_t frames, int16_t *angles)
    {
        ee::Local<zge::Matrix> turn;
        engine::copyMatrix(turn, current);
        zge::matrixTranspose3(turn, turn);
        engine::matrixProduct(turn, turn, previous);
        zge::matrixToEuler(eulerAngles(angles), turn);
        // (frames 0: the original's divide traps; never in the game)
        for (int i = 0; i < 3; ++i)
            angles[i] = static_cast<int16_t>(angles[i] / frames);
    }
    READABLE_FUNCTION(airRotationTrack, Verified);

    // States 1 and 8's turn: the rows turned by the air's angles (pitch,
    // yaw rate, roll); while a snap's frames last, turned toward the target
    // (alignFrom, by alignAngles a frame); with kFlagTrickTurn a second turn
    // (spinFrom, spinAngles); the rows made orthonormal again and the body's
    // rotation the rows. GameCube 0x8007C768.
    void airRotate(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> inverse;
        zge::Matrix *const rows = matrix(rider->right); // right, up, forward, rowsOrigin
        ee::Float *right = rider->right;
        ee::Float *up = rider->up;
        ee::Float *forward = rider->forward;

        zge::matrixIdentity(turn);
        zge::matrixRotateXYZ(turn, turn, eulerAngles(&rider->pitch));
        engine::matrixProduct(rows, rows, turn);

        // Each row turned into the target's space, by the angles, and back.
        auto turnThrough = [&](const ee::Float *about, const int16_t *angles, bool forwardFirst) {
            engine::copyMatrix(inverse, about);
            zge::matrixTranspose3(inverse, inverse);
            ee::Float *order[3] = {right, up, forward};
            if (forwardFirst)
                std::swap(order[0], order[2]);
            for (ee::Float *row : order)
                engine::transformPoint(row, inverse, row);
            zge::matrixRotateXYZ(rows, rows, eulerAngles(angles));
            zge::matrixTranspose3(inverse, inverse);
            for (ee::Float *row : order)
                engine::transformPoint(row, inverse, row);
        };

        if (rider->snapFrames != 0)
        {
            if ((rider->flags & kFlagAlignAbout) == 0)
            {
                turnThrough(rider->alignFrom, rider->alignAngles, false);
            }
            else
            {
                zge::matrixIdentity(turn);
                zge::matrixRotateXYZ(turn, turn, eulerAngles(rider->alignAngles));
                engine::matrixProduct(rows, rows, turn);
            }
            rider->snapFrames = static_cast<int16_t>(rider->snapFrames - 1);
        }
        if (rider->flags & kFlagTrickTurn)
            turnThrough(rider->spinFrom, rider->spinAngles, true);

        zge::Vec4 &rightRow = *vec4(right);
        zge::Vec4 &upRow = *vec4(up);
        zge::Vec4 &forwardRow = *vec4(forward);
        upRow = zge::unit(upRow);
        rightRow = zge::cross(upRow, forwardRow);
        rightRow = zge::unit(rightRow);
        forwardRow = zge::cross(rightRow, upRow);
        engine::copyMatrix(rider->body, rows);
    }
    READABLE_FUNCTION(airRotate, Verified);
}
