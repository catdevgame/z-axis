// BMX XXX's ghost ride move (physics states 16 and 17's move): the rider
// tumbles off and the bike rolls on by itself, DM2's ground move cut down to
// the wheels and the bike (no grind, no sliding, no balance on one wheel, no
// rider velocity); then, in the tumble, both tumble. BMX XXX's own.

#include "physics.h"

#include "../collision/collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/riderless_bike.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;          // 0.0055
        inline ee::Global<ee::Float, sym::g_restSpeed> g_restSpeed;      // 0.0015: slower stops (and the drag's 0.3 more)
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;        // 0.35 (times Rider::speedStat)
        inline ee::Global<ee::Float, sym::g_wheelieSlide> g_dragShare;   // 0.75 of the drag on wheel 1, the rest on wheel 0
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upFollow;  // 0.085: the body's up row eased towards the wheels'
        inline ee::Global<ee::Float, sym::g_bikeSpinKeep> g_spinKeep;    // 0.9967 of a wheel's spin kept off the ground
        inline ee::Global<ee::Float, sym::g_bikeRayLift> g_rayLift;      // 0.2: the ray between the wheels runs this far up the body
        inline ee::Global<ee::Float, sym::g_bikeWheelRestDrag> g_wheelRestDrag; // 0.3
        inline ee::Global<ee::Float, sym::g_bikeSpinPerSpeed> g_spinPerSpeed;   // 2 / pi
        constexpr zge::Angle kAngle50 = 9102;
        // The rays between the wheels touch what they meet as the bail into it (touchObject's word).
        constexpr uint32_t kTouchBail = 6;

        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        void addScaled(zge::Vec4 &v, const zge::Vec4 &d, ee::Float s)
        {
            v.x = v.x + d.x * s;
            v.y = v.y + d.y * s;
            v.z = v.z + d.z * s;
        }
        void subtractScaled(zge::Vec4 &v, const zge::Vec4 &d, ee::Float s)
        {
            v.x = v.x - d.x * s;
            v.y = v.y - d.y * s;
            v.z = v.z - d.z * s;
        }
        // a + (b - a) * t into out's x, y and z.
        void between(zge::Vec4 &out, const zge::Vec4 &a, const zge::Vec4 &b, ee::Float t)
        {
            const ee::Float ax = a.x, ay = a.y, az = a.z;
            out.x = ax + (b.x - ax) * t;
            out.y = ay + (b.y - ay) * t;
            out.z = az + (b.z - az) * t;
        }
        // a - b into out's x, y and z.
        void difference(zge::Vec4 &out, const zge::Vec4 &a, const zge::Vec4 &b)
        {
            const ee::Float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
            out.x = x;
            out.y = y;
            out.z = z;
        }
        ee::Float damped(const Rider *rider, ee::Float rate)
        {
            const ee::Float damping = rate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
            return rate * (ee::Float(1.0f) - damping);
        }
        // The body's position for a wheel's point at `at` through `rows` (into `out`; `scratch` holds the turned
        // point).
        void bodyPositionFor(zge::Vec4 &out, zge::Vec4 &scratch, const zge::Matrix &rows, const Rider *rider, uint32_t wheel, const zge::Vec4 &at)
        {
            scratch = *vec4(wheelPoint(rider, wheel));
            scratch = zge::transformed(rows, scratch);
            difference(out, at, scratch);
        }
    }

    // riderlessBikeMove (rider): the ghost ride's bike rolling on by itself a frame. Per wheel (0 the front): its
    // point is the body's mount point in the world; the leading wheel probes for walls (groundProbe: anything but
    // 0, the tumble); the spring's length and rate lift the point; groundAhead finds the ground under it:
    //  - none: the wheel off the ground, gravity along the body's up row by its slope, its matrix the body's, its
    //    spring relaxing, its spin down;
    //  - ground: the wheel's matrix on the face (the front turned by the yaw rate), the spring with half the weight
    //    on it, groundImpact (2 ends the move), and the wheel rolling along its forward row: less the surface's
    //    loss, pulled by the slope, wheel 1 pushed by the pedals, both slowed by the drag.
    // Something between the wheels (both ways, touched as a bail) tumbles the bike (state 17). The body's rows from
    // the wheels' points (its up row from the wheels' up rows, kFlagOneWheelUpright with one down, eased towards
    // from the body's; in the air the body's) and its position halfway between their mounts; each wheel's velocity
    // is where its mount goes next frame (stopped when slow, capped, the spin from it). Both wheels off the
    // ground: the tumble.
    void riderlessBikeMove(Rider *rider)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4[2]> points;     // the wheels' points
        ee::Local<zge::Vec4[2]> ahead;      // the rays' ends, then the points next frame
        ee::Local<zge::Vec4> middle;        // the body's position next frame
        ee::Local<zge::Matrix> rows;        // the body's rows being built
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Matrix> lastMatrix;  // a wheel's matrix before
        ee::Local<zge::Vec4> hitPoint;
        ee::Local<zge::Vec4> hitPlane;
        ee::Local<ee::Float> height;        // groundAhead's: the spring's new length in, the height over the ground out
        ee::Local<ee::Float> clearance;     // and the wheel's sinking along the body's up row
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &forward = rows->row(2);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);
        zge::Vec4 &drawn = *vec4(rider->drawnPosition);

        uint32_t wheelsOff = 0;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &point = points[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            const bool back = (wheel & 1) != 0;
            pose.touched = 0;
            point = *vec4(wheelPoint(rider, wheel));
            point = zge::transformed(body, point);
            point = point + drawn;
            const bool forwards = ee::Float(0.0f) <= zge::dot(wheelVelocity, bodyForward);
            if (back != forwards && groundProbe(rider, wheel, &point.x) != 0)
            {
                setPhysicsState(rider, uint32_t(kPhysicsGhostRideTumble)); // into a wall
                return;
            }

            // The spring's length and rate: the new length kept between the least and most heights (the rate
            // stopped there).
            const ee::Float length = pose.springLength;
            const ee::Float rate = pose.springRate;
            ee::Float h = length + rate;
            *height = h;
            ee::Float stretch;
            if (h < rider->wheelHeightLeast)
            {
                *height = rider->wheelHeightLeast;
                stretch = rider->wheelHeightLeast - length;
                h = rider->wheelHeightLeast;
                pose.springRate = 0.0f;
            }
            else
            {
                const ee::Float most = rider->wheelHeightMost;
                stretch = rate;
                if (most < h)
                {
                    *height = most;
                    stretch = most - length;
                    h = most;
                    pose.springRate = 0.0f;
                }
            }
            pose.springLength = h;
            if (pose.ground != kWheelOffGround)
                addScaled(point, bodyUp, stretch);

            *clearance = 0.0f;
            const collision::RayHit *const hit = groundAhead(rider, wheel, &point.x, height, clearance);
            if (hit == nullptr)
            {
                // Falling: gravity along the body's up row by its slope; the spring relaxes towards its most
                // height; the wheel spins down.
                const ee::Float g = *g_gravity;
                addScaled(wheelVelocity, bodyUp, (g + g) * bodyUp.y);
                *matrix(pose.matrix) = body;
                pose.springRate = damped(rider, pose.springRate + (rider->wheelHeightMost - *height) * rider->springStiffness / rider->springMass);
                const ee::Float spin = ee::toFloat(pose.spin);
                pose.ground = kWheelOffGround;
                pose.spin = ee::toInt(spin * *g_spinKeep);
                ++wheelsOff;
                continue;
            }

            // On the ground: the wheel's matrix on the face (its forward the body's, turned by the yaw rate on the
            // front wheel), its velocity in the face's plane.
            collision::hitFace(hit); // (asked for, not used)
            collision::hitPoint(hit, &hitPoint->x);
            collision::hitPlane(hit, &hitPlane->x);
            // The weight's share: the wheel's height weighed by a factor of 0 (a pitch folded away), so each wheel
            // takes half.
            const ee::Float zero(0.0f);
            const ee::Float lean = back ? zero : -zero;
            const ee::Float weight =
                (ee::max(ee::min(lean * (wheelStat(rider, wheel) + rider->massHeight + pose.springLength) /
                                     (rider->frontWheelPoint[2] - rider->backWheelPoint[2]),
                                 ee::Float(0.5f)),
                         ee::Float(-0.5f)) +
                 ee::Float(0.5f)) *
                rider->mass;
            zge::Matrix &wheelRows = *matrix(pose.matrix);
            zge::Vec4 &wheelRight = wheelRows.row(0);
            zge::Vec4 &wheelUp = wheelRows.row(1);
            zge::Vec4 &wheelForward = wheelRows.row(2);
            *lastMatrix = wheelRows;
            wheelUp = *hitPlane;
            wheelForward = bodyForward;
            wheelRight = zge::cross(wheelUp, wheelForward);
            wheelRight = zge::unit(wheelRight);
            wheelForward = zge::cross(wheelRight, wheelUp);
            if (!back)
            {
                zge::matrixIdentity(turn);
                zge::matrixRotateY(turn, turn, rider->yawRate);
                zge::matrixMultiply(&wheelRows, &wheelRows, turn);
            }
            *scratch = wheelVelocity;
            subtractScaled(*scratch, wheelUp, zge::dot(*scratch, wheelUp));
            const ee::Float sinking = zge::dot(*scratch, bodyUp);

            // The spring with the wheel's weight on it.
            const ee::Float now = *height;
            const ee::Float load = weight * (-bodyUp.y * *g_gravity);
            *clearance = sinking;
            const ee::Float springRate =
                pose.springRate + (load + (rider->wheelHeightMost - now) * rider->springStiffness) / (rider->mass * ee::Float(0.5f));
            pose.springLength = now;
            pose.springRate = damped(rider, springRate);

            if (groundImpact(rider, wheel, hit) == 2)
                return;
            if (zge::dot(wheelVelocity, lastMatrix->row(2)) < ee::Float(0.0f))
                pose.speed = -pose.speed;

            // Rolling: the speed along the wheel's forward row, less the surface's loss, pulled by the slope,
            // pushed by the pedals (wheel 1), less the drag (wheel 1 its 0.75).
            const ee::Float rolled = pose.speed * (ee::Float(1.0f) - (*g_surfaces)[pose.sound].rollingLoss) + *g_gravity * wheelForward.y;
            pose.speed = rolled;
            ee::Float share;
            if (!back)
                share = ee::Float(1.0f) - *g_dragShare;
            else
            {
                share = *g_dragShare;
                pose.speed = rolled + rider->pedalForce / weight;
            }
            const ee::Float s = pose.speed;
            const ee::Float drag = rider->wheelDrag * share / weight;
            const ee::Float slowed = ee::abs(s) <= drag ? ee::abs(s) : drag;
            pose.speed = ee::Float(0.0f) <= s ? s - slowed : s + slowed;
            wheelVelocity = wheelForward * pose.speed;
            pose.ground = kWheelRolling;
            pose.speed = ee::abs(pose.speed);
        }

        // A ray between the wheels, 0.2 up the body, both ways: what it touches tumbles the bike.
        const ee::Float lift = *g_rayLift;
        const ee::Float liftX = bodyUp.x * lift, liftY = bodyUp.y * lift, liftZ = bodyUp.z * lift;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            ahead[wheel].x = points[wheel].x + liftX;
            ahead[wheel].y = points[wheel].y + liftY;
            ahead[wheel].z = points[wheel].z + liftZ;
        }
        if (collision::rayCastTouching(rider, &ahead[0].x, &ahead[1].x, kTouchBail) != nullptr ||
            collision::rayCastTouching(rider, &ahead[1].x, &ahead[0].x, kTouchBail) != nullptr)
        {
            setPhysicsState(rider, uint32_t(kPhysicsGhostRideTumble));
            return;
        }

        // The body's forward row from wheel 1's point to wheel 0's; its up row from the wheels' (both down: halfway
        // between their up rows; one: its up row, kFlagOneWheelUpright when within 50 degrees of upright), eased
        // towards from the body's; in the air the body's.
        zge::matrixIdentity(rows);
        difference(forward, points[0], points[1]);
        forward = zge::unit(forward);
        if (wheelsOff < 2)
        {
            if (wheelsOff == 0)
                between(up, matrix(rider->wheelPoses[0].matrix)->row(1), matrix(rider->wheelPoses[1].matrix)->row(1), ee::Float(0.5f));
            else
            {
                uint32_t wheel = 0;
                while (wheel < 2 && rider->wheelPoses[wheel].ground == kWheelOffGround)
                    ++wheel;
                if (wheel < 2)
                {
                    const zge::Vec4 &wheelUp = matrix(rider->wheelPoses[wheel].matrix)->row(1);
                    up = wheelUp;
                    if (zge::angleAcos(-wheelUp.y) < kAngle50)
                        setFlag(rider, kFlagOneWheelUpright);
                    else
                        clearFlag(rider, kFlagOneWheelUpright);
                }
            }
            between(up, bodyUp, up, *g_upFollow);
        }
        else
            up = bodyUp;
        right = zge::cross(up, forward);
        right = zge::unit(right);
        up = zge::cross(forward, right);
        up = zge::unit(up);
        *matrix(rider->body) = *rows;

        // The drawn body halfway between where the wheels' points put it.
        bodyPositionFor(drawn, *scratch, *rows, rider, 0, points[0]);
        bodyPositionFor(*scratch, *scratch, *rows, rider, 1, points[1]);
        between(drawn, drawn, *scratch, ee::Float(0.5f));

        // Next frame: the wheels' points moved by their velocities, the body rows through them and its position
        // halfway between their mounts.
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            const zge::Vec4 &wheelVelocity = *vec4(rider->wheelPoses[wheel].velocity);
            ahead[wheel].x = points[wheel].x + wheelVelocity.x;
            ahead[wheel].y = points[wheel].y + wheelVelocity.y;
            ahead[wheel].z = points[wheel].z + wheelVelocity.z;
        }
        difference(forward, ahead[0], ahead[1]);
        forward = zge::unit(forward);
        right = zge::cross(up, forward);
        right = zge::unit(right);
        up = zge::cross(forward, right);
        bodyPositionFor(*middle, *scratch, *rows, rider, 0, ahead[0]);
        bodyPositionFor(*scratch, *scratch, *rows, rider, 1, ahead[1]);
        between(*middle, *middle, *scratch, ee::Float(0.5f));

        // Each wheel's velocity: from its point to where its mount goes (stopped when slower than the rest speed
        // and the drag's share, capped at the top speed); a rolling wheel spins by it.
        const ee::Float restDrag = *g_wheelRestDrag;
        const ee::Float spinPerSpeed = *g_spinPerSpeed;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            zge::Vec4 &next = ahead[wheel];
            next = *vec4(wheelPoint(rider, wheel));
            next = zge::transformed(*rows, next);
            next.x = next.x + middle->x;
            next.y = next.y + middle->y;
            next.z = next.z + middle->z;
            difference(wheelVelocity, next, points[wheel]);
            const ee::Float speed = zge::vec3Length(&wheelVelocity.xyz());
            pose.speed = speed;
            if (speed < *g_restSpeed + rider->wheelDrag * restDrag)
            {
                wheelVelocity = zge::kZero;
                pose.speed = 0.0f;
            }
            else if (*g_topSpeed * rider->speedStat < speed)
            {
                wheelVelocity = zge::unit(wheelVelocity);
                wheelVelocity = wheelVelocity * (*g_topSpeed * rider->speedStat);
            }
            if (pose.ground == kWheelRolling)
            {
                const int32_t spin = static_cast<int16_t>(ee::toInt(pose.speed * spinPerSpeed * ee::Float(65536.0f)));
                pose.spin = spin;
                if (zge::dot(matrix(pose.matrix)->row(2), wheelVelocity) < ee::Float(0.0f))
                    pose.spin = -spin;
            }
        }
        if (wheelsOff == 2)
            setPhysicsState(rider, uint32_t(kPhysicsGhostRideTumble)); // off the ground
    }
    READABLE_FUNCTION(riderlessBikeMove, Verified);

    // ghostRideMove (rider): physics states 16 and 17's move: the rider tumbles; the bike rolls on by itself (any
    // state but 17), or in the ghost ride's tumble tumbles too.
    void ghostRideMove(Rider *rider)
    {
        if (physicsState(rider) == kPhysicsGhostRideTumble)
        {
            riderTumbleMove(rider);
            return game::tail(bikeTumbleMove, rider);
        }
        riderTumbleMove(rider);
        game::tail(riderlessBikeMove, rider);
    }
    READABLE_FUNCTION(ghostRideMove, Verified);
}
