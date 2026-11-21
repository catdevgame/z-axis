// DM2's nose wheelie and manual (physics states 9 and 10, tricks 0x47 and
// 0x46; MIXING 14.1's "wall ride" rows are these): the probe ahead of the
// wheel the rider balances on, and the move on that wheel.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x8007DDA8 (wheelieProbe),
// 0x80082914 (wheelieMove); DM2 Xbox 0x053F40, 0x05BF30.

#include "../collision/collision.h"
#include "actions.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/actions/wheelie.cpp.h"

namespace game::dm2::rider
{
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface);

    namespace
    {
        constexpr int32_t kWallHitSound = 22;
        constexpr int32_t kMostVolume = 127;
        constexpr int32_t kTurn30Degrees = 5462;        // the plane steeper than this from the wheel's right row
        constexpr int32_t kFloorAngle = 12744;          // a plane within 70 degrees of flat: no wall
        constexpr uint32_t kSurfaceKindWorld = 1;       // the face kinds the wheelie passes over (1 .. 3)
        constexpr uint32_t kSurfaceKindRamp = 8;
        constexpr uint8_t kBailKindWallHit = 3;          // Rider::bailKind
        inline ee::Global<ee::Float, sym::g_wallBounceLeast> g_wallBounceLeast; // the speed out under this: no bounce
        inline ee::Global<ee::Float, sym::g_wallSoundLeast> g_wallSoundLeast;
        inline ee::Global<ee::Float, sym::g_wallBounceLift> g_wallBounceLift;
        inline ee::Global<ee::Float, sym::g_wallBailSpeed> g_wallBailSpeed;   // into the wall slower than this: down to the ground
        inline ee::Global<int16_t, sym::g_wallMostAngle> g_wallMostAngle;
        constexpr float kRaySide = 0.25f;                // the two rays a quarter unit to each side

        // The engine's calls the probe makes as guest calls: the scales after
        // them carry what the last one left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void negateByCall(ee::Float *v) { ee::game(sym::vec3Negate, v, v); }
        inline collision::RayHit *rayHitByCall(const ee::Float *from, const ee::Float *to)
        {
            return ee::game<collision::RayHit *>(sym::rayCast, from, to);
        }
        // The speed into the hit's face; speedOut gets the speed out of it.
        inline ee::Float closingByCall(const ee::Float *velocity, const collision::RayHit *hit, ee::Float *speedOut)
        {
            return ee::game<ee::Float>(sym::hitDistance, velocity, hit, speedOut);
        }
        inline uint32_t surfaceKindByCall(collision::FaceRef face) { return ee::game<uint32_t>(sym::faceSurfaceKind, face); }
        // Non-zero when the hit counts (the touched object agreeing).
        inline int32_t touchCountsByCall(Rider *rider, const collision::RayHit *hit)
        {
            return ee::game<int32_t>(sym::touchObject, rider, hit);
        }
    }

    // States 9 and 10's probe: two rays from the drawn body along the
    // balancing wheel's forward row (the way the rider moves), a quarter unit to each
    // side. A hit on a face the rider rides into (not a floor, not the
    // world's surface kinds 1 .. 3 at a wall's angle, the touched object
    // agreeing) makes the hit's sound; slow, the rider drops to the ground
    // (1); else he bails (2), bounced off it when moving out fast enough.
    // Returns 0 when neither ray hits so.
    //
    // Every scale carries what the last call left, as airGravity's does.
    int32_t wheelieProbe(Rider *rider)
    {
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> side;
        ee::Local<zge::Matrix> wheelRows;           // the balancing wheel's matrix: right, up, forward, position
        ee::Local<zge::Vec4> facePlane;             // the hit face's plane (normal, distance)
        ee::Local<zge::Vec4> plane;                 // and the copy the tests read
        ee::Local<ee::Float> speedOut;              // the speed out of the face (hitDistance's second result)
        ee::Local<ee::Ptr<ee::Float>> from;         // the drawn position: the rays start there
        ee::Local<ee::Ptr<ee::Float>> wheelRight;   // the wheel's right row
        ee::Local<ee::Ptr<ee::Float>> riderRight;   // the rider's right row
        // (the original keeps these three addresses in its frame)
        zge::Vec4 &velocity = *vec4(rider->velocity);
        *from = rider->drawnPosition;
        *riderRight = rider->right;
        const uint8_t physics = physicsState(rider);
        *wheelRight = &wheelRows->row(0).x;
        const ee::Float bounceLeast = *g_wallBounceLeast;
        // The balancing wheel (wheel 0, the front, in the nose wheelie; 1, the back, in the manual): its matrix.
        const uint32_t wheel = physics == kPhysicsManual ? 1u : 0u;
        physics::engine::copyMatrix(wheelRows, rider->wheelPoses[wheel].matrix);
        const zge::Vec4 &wheelUp = wheelRows->row(1);
        const zge::Vec4 &wheelForward = wheelRows->row(2);
        for (uint32_t ray = 0; ray < 2; ++ray)
        {
            *to = wheelForward;
            if (zge::dot(velocity, wheelForward) < ee::Float(0.0f))
                negateByCall(&to->x);
            const ee::Float across = (ray & 1u) ? ee::Float(-kRaySide) : ee::Float(kRaySide);
            *side = zge::scaledCarrying(*vec4(*riderRight), across, zge::scaleLeftover(zge::LeftBy::lastCall));
            *to = *to + *side;
            *to = *to + *vec4(*from);
            collision::RayHit *const hit = rayHitByCall(*from, &to->x);
            if (hit == nullptr)
                continue;
            const collision::FaceRef face = collision::hitFace(hit);
            collision::hitPlane(hit, &facePlane->x);
            *plane = *facePlane;
            *speedOut = ee::Float(0.0f);
            const ee::Float closing = closingByCall(&velocity.x, hit, speedOut);
            if (!(ee::Float(0.0f) < closing))
                continue; // moving away from it

            // Is it a wall the rider rides into? A flat face of the world
            // is not; a steep one at the wall's angle is, unless it is a
            // floor or one of the world's surface kinds (a ramp's always is).
            const bool flatWorld = plane->y == ee::Float(0.0f) &&
                                   (!(collision::hitFlags(hit) & 1) || collision::faceSurfaceKind(face) != kSurfaceKindWorld);
            if (!flatWorld && zge::angleAcos(ee::abs(zge::dot(*vec4(*wheelRight), *plane))) >= kTurn30Degrees &&
                zge::angleAcos(zge::dot(wheelUp, *plane)) < *g_wallMostAngle)
            {
                ee::Float y = plane->y;
                bool ramp = false;
                if (collision::hitFlags(hit) & 1)
                {
                    ramp = collision::faceSurfaceKind(face) == kSurfaceKindRamp;
                    y = plane->y;
                }
                if (!ramp)
                {
                    if (zge::angleAcos(-y) < kFloorAngle)
                        continue; // a floor
                    bool world = false;
                    for (uint32_t kind = kSurfaceKindWorld; kind <= 3 && !world; ++kind)
                        world = surfaceKindByCall(face) == kind;
                    if (world)
                        continue;
                }
            }
            if (touchCountsByCall(rider, hit) == 0)
                continue;

            // The hit's sound, by the face's.
            uint32_t surface = 0;
            if (collision::faceSound(face) != 0xFFFF)
            {
                const uint32_t sound = collision::faceSound(face);
                surface = sound == 1 || sound == 3 ? 7u : sound == 2 ? 2u : 0u;
            }
            if (*g_wallSoundLeast < closing)
            {
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, surface);
                const int32_t loud = ee::toInt(closing * ee::Float(400.0f));
                soundEvent(kWallHitSound, rider->position, code, loud < kMostVolume ? loud : kMostVolume);
            }
            if (closing < *g_wallBailSpeed)
            {
                setPhysicsState(rider, uint32_t(kPhysicsGround));
                return 1;
            }
            // A bail, bounced back off the wall (lifted) when moving out of
            // it fast enough.
            rider->bailKind = 0;
            if (!(ee::abs(*speedOut) < bounceLeast))
            {
                negateByCall(&velocity.x);
                velocity = zge::unit(velocity);
                velocity = zge::scaledCarrying(velocity, *speedOut, zge::scaleLeftover(zge::LeftBy::lastCall));
                const ee::Float vy = velocity.y;
                const ee::Float lift = *g_wallBounceLift;
                rider->bailKind = kBailKindWallHit;
                velocity.y = vy + lift;
            }
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            addRideDistance(rider, closing);
            collision::touchObject(rider, hit);
            return 2;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(wheelieProbe, Verified);

    namespace
    {
        constexpr uint32_t kFlagGrindAsked = (1u << 20); // the grind button held (states.cpp's; riderFrame clears it)
        inline ee::Global<ee::Float, sym::g_gravity> g_gravityStep;   // 0.0055 a frame
        inline ee::Global<ee::Float, sym::g_wheelieSlide> g_wheelieSlide;     // the push's share against the drag
        inline ee::Global<ee::Float, sym::g_restSpeed> g_restSpeed;     // slower than this (plus the drag's share): stopped
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;      // 0.35 a frame (ground.cpp's turn top speed)
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upShare;       // the drawn up row's turn to the wheel's a frame (0.085)
        // The balance's lean of the drawn body (turns): in the manual (the
        // symbol's "front" is the old reading of the wheel's index), and in
        // the nose wheelie.
        inline ee::Global<ee::Float, sym::g_frontLeanAngle> g_manualLean;
        inline ee::Global<ee::Float, sym::g_backLeanAngle> g_noseWheelieLean;
        inline ee::Global<ee::Float, sym::g_rightShare> g_rightShare;    // the drawn right row's turn a frame
        inline ee::Global<ee::Float, sym::g_stopDragShare> g_stopDragShare; // the drag's share of the stopped speed: the wheel's,
        inline ee::Global<ee::Float, sym::g_stopDragShareBody> g_stopDragShareBody; // and the rider's
        inline ee::Global<ee::Float, sym::g_spinRate> g_spinRate;      // the wheel's spin from its speed (turns a unit)
        constexpr ee::Float kTurn = 65536.0f;          // angle units a turn
        constexpr int32_t kManualLeanBase = 0xE38;     // 20 degrees (0x10000 a turn)
        constexpr int32_t kNoseWheelieLeanBase = 0x18E3; // 35 degrees

        // A wheel's point in the body's space and its height (the stats).
        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelHeight(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        // The ground move's own, called as the original calls them (guest
        // calls): the scale after groundHit carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline int32_t groundImpactByCall(Rider *rider, uint32_t wheel, const collision::RayHit *hit)
        {
            return ee::game<int32_t>(sym::groundImpact, rider, wheel, hit);
        }
        inline void groundHitByCall(Rider *rider, const collision::RayHit *hit) { ee::game(sym::groundHit, rider, hit); }
        inline ee::Float lengthByCall(const ee::Float *v) { return ee::game<ee::Float>(sym::vec3Length, v); }
    }

    // States 9 and 10's move: the balancing wheel (the front in 9, the
    // back in 10) rides the ground as in the ground move. The wheel's point
    // (in the world) is moved by its spring; groundAhead finds the face
    // under it (none: into the air). The wheel's rows stand on the face's
    // plane, along the drawn body's forward row, turned by the yaw rate;
    // the spring pulls toward its rest (the gravity across the face pulling
    // the wheel's share of the mass), the impact and the hits are the
    // ground's (a bail ends it; the grind button held goes to the grind).
    // The wheel's speed: the drag of the surface, gravity along the slope,
    // the pedals' push in the manual and the drag of both; its velocity
    // along its forward row. The drawn body turns toward the wheel's rows,
    // leaned by the balance, and is placed by the wheel's point; the rider
    // moves with the wheel's velocity (stopped when slow, held under the
    // top speed), riding fakie when it is against the drawn forward row.
    //
    // Only the move table calls it, as void. Every scale carries what the
    // last call left, as airGravity's does.
    void wheelieMove(Rider *rider)
    {
        ee::Local<zge::Vec4> across;     // the wheel's velocity across the face; then its point turned by the new rows
        ee::Local<zge::Vec4[2]> points;  // the wheel's point in the world (the original keeps a slot for each wheel)
        ee::Local<zge::Matrix> rows;     // the drawn body's new rows
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> before;   // the wheel's matrix before the move
        ee::Local<zge::Vec4> groundPoint;
        ee::Local<zge::Vec4> groundPlane;
        ee::Local<ee::Float> height;     // groundAhead's: the spring's new length in, the height over the ground out
        ee::Local<ee::Float> clearance;  // and the height less the spring's length
        const uint32_t wheel = physicsState(rider) == kPhysicsManual ? 1u : 0u;
        const bool manual = (wheel & 1u) != 0; // on the back wheel, which the pedals drive
        WheelPose &pose = rider->wheelPoses[wheel];
        zge::Matrix &wheelRows = *matrix(pose.matrix);
        zge::Vec4 &wheelRight = wheelRows.row(0);
        zge::Vec4 &wheelUp = wheelRows.row(1);
        zge::Vec4 &wheelForward = wheelRows.row(2);
        zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
        zge::Vec4 &drawn = *vec4(rider->drawnPosition);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyRight = body.row(0);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);

        // The wheel's point in the world.
        zge::Vec4 &point = points[wheel];
        pose.touched = 0;
        point = *vec4(wheelPoint(rider, wheel));
        physics::engine::transformPoint(&point.x, rider->body, &point.x);
        point = point + drawn;
        if (wheelieProbe(rider) == 2)
            return; // a bail (the impact)

        // The spring: its new length held within the stats' limits (its
        // rate stopped there); the wheel's point moves along the drawn up
        // row by the change.
        const ee::Float length = pose.springLength, rate = pose.springRate;
        const ee::Float least = rider->wheelHeightLeast;
        ee::Float to = length + rate;
        *height = to;
        ee::Float moved = rate;
        if (to < least)
        {
            *height = least;
            moved = least - length;
            to = least;
            pose.springRate = ee::Float(0.0f);
        }
        else if (const ee::Float most = rider->wheelHeightMost; most < to)
        {
            *height = most;
            moved = most - length;
            to = most;
            pose.springRate = ee::Float(0.0f);
        }
        pose.springLength = to;
        const ee::Float zero = ee::Float(0.0f); // (the original keeps a zero for the lean's term and the damping's test)
        {
            const ee::Float ux = bodyUp.x * moved, uy = bodyUp.y * moved, uz = bodyUp.z * moved;
            point.x = point.x + ux;
            point.y = point.y + uy;
            point.z = point.z + uz;
        }
        *clearance = zero;
        collision::RayHit *const hit = physics::groundAhead(rider, wheel, &point.x, height, clearance);
        if (hit == nullptr)
        {
            setPhysicsState(rider, uint32_t(kPhysicsAir)); // no ground under the wheel: into the air
            return;
        }

        collision::hitFace(hit); // (asked for, not used)
        collision::hitPoint(hit, &groundPoint->x);
        collision::hitPlane(hit, &groundPlane->x);
        // The wheel's share of the mass (the lean's term is zero: half,
        // either way).
        ee::Float share;
        {
            const ee::Float lean = manual ? zero : -zero;
            const ee::Float lift = wheelHeight(rider, wheel) + rider->massHeight;
            const ee::Float wheelbase = rider->frontWheelPoint[2] - rider->backWheelPoint[2];
            share = lean * (lift + pose.springLength) / wheelbase;
            share = ee::max(ee::min(share, ee::Float(0.5f)), ee::Float(-0.5f)) + ee::Float(0.5f);
            share = share * rider->mass;
        }
        physics::engine::copyMatrix(before, pose.matrix);
        // The wheel's rows: up the face's normal, forward the drawn body's,
        // square, then turned by the yaw rate.
        wheelUp = *groundPlane;
        wheelForward = bodyForward;
        wheelRight = zge::cross(wheelUp, wheelForward);
        wheelRight = zge::unit(wheelRight);
        wheelForward = zge::cross(wheelRight, wheelUp);
        zge::matrixIdentity(turn);
        zge::matrixRotateY(turn, turn, int32_t(rider->yawRate));
        physics::engine::matrixProduct(pose.matrix, pose.matrix, turn);
        // The wheel's velocity across the face, its part along the drawn up
        // row (for groundAhead's second result).
        *across = wheelVelocity;
        {
            const ee::Float into = zge::dot(*across, wheelUp);
            const ee::Float x = wheelUp.x * into, y = wheelUp.y * into, z = wheelUp.z * into;
            across->x = across->x - x;
            across->y = across->y - y;
            across->z = across->z - z;
        }
        const ee::Float alongUp = zge::dot(*across, bodyUp);
        // The spring: gravity across the face on the wheel's share, the pull
        // toward the most height, over half the mass; damped.
        {
            const ee::Float pull = -bodyUp.y * *g_gravityStep;
            const ee::Float got = *height;
            const ee::Float spring = (rider->wheelHeightMost - got) * rider->springStiffness;
            *clearance = alongUp;
            ee::Float springRate = pose.springRate + (share * pull + spring) / (rider->mass * ee::Float(0.5f));
            const ee::Float damping = springRate < zero ? rider->springDamping[0] : rider->springDamping[1];
            springRate = springRate * (ee::Float(1.0f) - damping);
            pose.springLength = got;
            pose.springRate = springRate;
        }
        if (groundImpactByCall(rider, wheel, hit) == 2)
            return; // bailed
        if (rider->flags & kFlagGrindAsked)
        {
            const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
            if (state == kPhysicsGrind || state == kPhysicsStall)
                return;
        }
        groundHitByCall(rider, hit);

        // The wheel's speed: turned back with the wheel, less the surface's
        // drag, plus gravity along the forward row; the pedals' push in the
        // manual. Then the drag of both, never past zero.
        ee::Float &speed = pose.speed;
        if (zge::dot(wheelVelocity, before->row(2)) < ee::Float(0.0f))
            speed = -speed;
        {
            const ee::Float drag = (*physics::g_surfaces)[pose.sound].rollingLoss;
            const ee::Float slope = *g_gravityStep * wheelForward.y;
            speed = speed * (ee::Float(1.0f) - drag) + slope;
        }
        ee::Float slide;
        if (manual)
        {
            const ee::Float push = rider->pedalForce;
            slide = *g_wheelieSlide;
            speed = speed + push / share;
        }
        else
            slide = ee::Float(1.0f) - *g_wheelieSlide;
        {
            const ee::Float v = speed;
            const ee::Float most = rider->wheelDrag * slide / share;
            const ee::Float drag = ee::abs(v) <= most ? ee::abs(v) : most;
            speed = ee::Float(0.0f) <= v ? v - drag : v + drag;
        }
        const ee::Float wheelSpeed = speed;
        wheelVelocity = zge::scaledCarrying(wheelForward, wheelSpeed, zge::scaleLeftover(zge::LeftBy::lastCall));
        pose.ground = kWheelRolling;
        speed = ee::abs(wheelSpeed);

        // The drawn body: toward the wheel's rows, leaned by the balance
        // (about its right row), at the origin while it turns.
        physics::engine::copyMatrix(rows, pose.matrix);
        rows->row(3) = zge::kZero;
        const ee::Float balance = rider->balance;
        int32_t lean;
        if (manual)
            lean = -(ee::toInt(balance * *g_manualLean * kTurn) + kManualLeanBase);
        else
            lean = ee::toInt(-balance * *g_noseWheelieLean * kTurn) + kNoseWheelieLeanBase;
        zge::matrixIdentity(turn);
        zge::matrixRotateX(turn, turn, int32_t(static_cast<int16_t>(lean)));
        physics::engine::matrixProduct(rows, rows, turn);
        auto blend = [](const zge::Vec4 &from, zge::Vec4 &row, ee::Float by) {
            const ee::Float x = from.x, y = from.y, z = from.z;
            const ee::Float dx = (row.x - x) * by, dy = (row.y - y) * by, dz = (row.z - z) * by;
            row.x = x + dx;
            row.y = y + dy;
            row.z = z + dz;
            row = zge::unit(row);
        };
        blend(bodyRight, rows->row(0), *g_rightShare);
        blend(bodyUp, rows->row(1), *g_upShare);
        rows->row(2) = zge::cross(rows->row(0), rows->row(1));
        rows->row(2) = zge::unit(rows->row(2));
        rows->row(1) = zge::cross(rows->row(2), rows->row(0));
        physics::engine::copyMatrix(rider->body, rows);
        physics::engine::copyMatrix(rider->right, rider->body);
        // Placed so the wheel's point is where it rides.
        *across = *vec4(wheelPoint(rider, wheel));
        physics::engine::transformPoint(&across->x, rows, &across->x);
        {
            const ee::Float x = point.x - across->x, y = point.y - across->y, z = point.z - across->z;
            drawn.x = x;
            drawn.y = y;
            drawn.z = z;
        }
        *vec4(rider->position) = drawn;

        // The wheel's speed: none when slow, held under the top speed.
        const ee::Float now = lengthByCall(pose.velocity);
        speed = now;
        const ee::Float drag = rider->wheelDrag;
        const ee::Float top = *g_topSpeed;
        if (now < *g_restSpeed + drag * *g_stopDragShare)
        {
            wheelVelocity = zge::kZero;
            pose.speed = ee::Float(0.0f);
        }
        else if (const ee::Float most = top * rider->speedStat; most < now)
        {
            wheelVelocity = zge::unit(wheelVelocity);
            wheelVelocity = zge::scaledCarrying(wheelVelocity, most, zge::scaleLeftover(zge::LeftBy::lastCall));
        }
        const ee::Float wheelSpeedNow = speed;
        // The wheel's spin from its speed, backwards when it rolls back.
        const int32_t spin = static_cast<int16_t>(ee::toInt(wheelSpeedNow * *g_spinRate * kTurn));
        pose.spin = spin;
        if (zge::dot(wheelForward, wheelVelocity) < ee::Float(0.0f))
            pose.spin = -spin;
        // The other wheel goes with it.
        WheelPose &other = rider->wheelPoses[wheel ^ 1u];
        *vec4(other.velocity) = wheelVelocity;
        other.speed = wheelSpeedNow;

        // The rider: the wheel's velocity (the force the change), stopped
        // when slow; fakie when moving against the drawn forward row.
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &force = *vec4(rider->force);
        force = velocity;
        velocity = wheelVelocity;
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (rider->speed < *g_restSpeed + rider->wheelDrag * *g_stopDragShareBody)
        {
            velocity = zge::kZero;
            rider->speed = ee::Float(0.0f);
        }
        force = velocity - force;
        if (zge::dot(velocity, bodyForward) < ee::Float(0.0f))
            rider->flags |= kFlagFakie;
        else
            rider->flags &= ~kFlagFakie;
    }
    READABLE_FUNCTION_WAITS(wheelieMove, Verified);
}
