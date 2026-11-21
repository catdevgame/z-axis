// BMX XXX's ground move (physics state 0's move callback, the move table's
// first): the two wheels on their springs, rolling, sliding and braking on
// the ground under them, the body set between them, the velocity from where
// the wheels go. DM2's (games/dm2/rider/physics/ground_move.cpp), read in BMX
// XXX's machine code: BMX XXX pushes the body's corners clear while the rider
// stands still, works the wheels' points out again when a wheel's probe
// pushes the rider off a wall, has no grind test and no balance on one wheel
// in the air, lays concrete dust with a skid mark, and touches what lies
// between the wheels by its rays (a bail).

#include "physics.h"

#include "../collision/collision.h"

#include "bmx/fx/emitter_objects.h"
#include "bmx/fx/spawn_effects.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/ground_move.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        // The air's gravity a frame (also the slopes' pull here).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;           // 0.0055
        inline ee::Global<ee::Float, sym::g_slideLoss> g_slideLoss;       // 0.02 of a sliding wheel's speed lost turning it to its forward row
        inline ee::Global<ee::Float, sym::g_restSpeed> g_restSpeed;       // 0.0015: slower stops (and the drag's 0.3 more)
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;         // 0.35 (times Rider::speedStat)
        inline ee::Global<ee::Float, sym::g_leanFrom> g_leanFrom;         // 0.0667: the yaw leans the body only faster than this
        inline ee::Global<ee::Float, sym::g_wheelieSlide> g_dragShare;    // 0.75 of the drag on wheel 1, the rest on wheel 0
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upFollow;   // 0.085: the body's up row eased towards the wheels'
        inline ee::Global<ee::Float, sym::g_leanDegrees> g_leanDegrees;   // 30
        inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_leanYawDegrees; // 75
        inline ee::Global<ee::Float, sym::g_skidDustSpread> g_dustSpread; // 0.01
        inline ee::Global<ee::Float, sym::g_spinKeep> g_spinKeep;         // 0.9967 of a wheel's spin kept off the ground
        inline ee::Global<ee::Float, sym::g_rayLift> g_rayLift;           // 0.2: the ray between the wheels runs this far up the body
        inline ee::Global<ee::Float, sym::g_leanUnitsPerDegree> g_angleUnitsPerDegree; // 65536 / 360
        inline ee::Global<ee::Float, sym::g_wheelRestDrag> g_wheelRestDrag; // 0.3
        inline ee::Global<ee::Float, sym::g_spinPerSpeed> g_spinPerSpeed; // 2 / pi
        inline ee::Global<ee::Float, sym::g_riderRestDrag> g_riderRestDrag; // 0.3

        // A wheel's surface, by its sound (WheelPose::sound).
        const Surface &surfaceOf(const WheelPose &pose) { return (*g_surfaces)[pose.sound]; }

        // What a sliding wheel leaves by its sound: a skid mark and concrete
        // dust (0 .. 2, 4), dust (5, 10, 11), nothing (3, 6 .. 9).
        constexpr uint32_t kSkidSounds = 12;
        constexpr uint32_t kSkidMarks = 0x1Fu - 0x08u; // sounds 0, 1, 2, 4
        constexpr uint32_t kSkidDust = 0xC20u;         // sounds 5, 10, 11

        constexpr int16_t kAngle50 = 9102;
        constexpr uint8_t kBailThrown = 5; // Rider::bailKind: thrown by an object between the wheels (DM2's)
        // The rays between the wheels touch what they meet as the bail into it (touchObject's word).
        constexpr uint32_t kTouchBail = 6;
        // Coasting's phase while the rider stands still (riding.cpp's kCoastStill).
        constexpr uint32_t kCoastStill = 1;

        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        // v's x, y and z plus (or less) d's times s. w stays as it was.
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
        // v kept within -1 .. 1.
        ee::Float clampUnit(ee::Float v)
        {
            if (v <= ee::Float(1.0f))
                return ee::Float(-1.0f) <= v ? v : ee::Float(-1.0f);
            return ee::Float(1.0f);
        }
        // A spring's rate damped: the shortening or the lengthening damping by the way it goes.
        ee::Float damped(const Rider *rider, ee::Float rate)
        {
            const ee::Float damping = rate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
            return rate * (ee::Float(1.0f) - damping);
        }
        // A wheel's point in the world: its point on the body through the body's matrix, at the drawn position.
        void placeWheel(const Rider *rider, uint32_t wheel, zge::Vec4 &point)
        {
            point = *vec4(wheelPoint(rider, wheel));
            point = zge::transformed(*matrix(rider->body), point);
            point = point + *vec4(rider->drawnPosition);
        }
        // Where the body's rows put the drawn body's position so that a wheel's point lies at `at`: `at` less the
        // wheel's point turned by the rows (into `out`; `scratch` holds the turned point).
        void bodyPositionFor(zge::Vec4 &out, zge::Vec4 &scratch, const zge::Matrix &rows, const Rider *rider, uint32_t wheel, const zge::Vec4 &at)
        {
            scratch = *vec4(wheelPoint(rider, wheel));
            scratch = zge::transformed(rows, scratch);
            difference(out, at, scratch);
        }
    }

    // State 0's move. While the rider stands still (riding, coasting, its still phase) the body's corners are
    // pushed clear first and the wheels do not probe. Per wheel (0 the front, 1 the back): its point is the
    // body's mount point in the world; the leading one (the front moving forwards, the back moving backwards)
    // probes for walls (groundProbe: pushed off a wall, both points are worked out again; 2, a bail, ends the
    // move); the spring's length (kept between the least and most heights) and rate lift the point; groundAhead
    // finds the ground under it.
    //  - No ground: the wheel is off the ground: gravity pulls its velocity along the body's up row by its
    //    slope, its matrix is the body's, its spring relaxes, it spins down and rides along with a touched
    //    object.
    //  - Ground: the wheel's matrix stands on the face (turned by the yaw rate on the front wheel), its velocity
    //    in the face's plane; the spring carries half the weight; groundImpact (2 ends the move), groundHit;
    //    rolling it keeps its forward speed less the surface's loss, wheel 1 pushed by the pedals, both slowed by
    //    the drag; past the surface's slip it slides (skid marks and concrete dust, or dust).
    // Then a ray between the wheels both ways: what it touches throws the rider (a bail of kind 5). The body's
    // rows from the wheels' points (its up row from the wheels' up rows, leaned into the yaw, eased towards from
    // the body's) and its position halfway between their mounts; each wheel's velocity is where its mount goes
    // next frame (stopped when slow, capped, the spin from it), the rider's velocity their mean (stopped when
    // slow, the wheels down with it) and the force the change; riding fakie when it is against the body's
    // forward row. Both wheels off the ground: the air (state 1). DM2 GameCube 0x8007E3C0's counterpart.
    void groundMove(Rider *rider)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4[2]> points;     // the wheels' points
        ee::Local<zge::Vec4[2]> ahead;      // the rays' ends, then the points next frame
        ee::Local<zge::Vec4> middle;        // the body's position next frame
        ee::Local<zge::Vec4> skidNormal;
        ee::Local<zge::Matrix> rows;        // the body's rows being built
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Vec4> moved;         // rideTouchedObject's carry
        ee::Local<zge::Matrix> lastMatrix;  // a wheel's matrix before
        ee::Local<zge::Vec4> hitPoint;
        ee::Local<zge::Vec4> hitPlane;
        ee::Local<zge::Vec4> across;        // a sliding wheel's pushes: across,
        ee::Local<zge::Vec4> pedalled;      // the pedals',
        ee::Local<zge::Vec4> dragged;       // the drag's
        ee::Local<zge::Vec4> dustAt;        // (a skid mark's direction too)
        ee::Local<zge::Vec4> dustMotion;
        ee::Local<zge::Vec4> skidPlane;
        ee::Local<ee::Float> height;        // groundAhead's: the spring's new length in, the height over the ground out
        ee::Local<ee::Float> clearance;     // and the wheel's sinking along the body's up row
        ee::Local<ee::Ptr<zge::Vec4>> keptPoints; // the points' address, which the original keeps in its frame
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &forward = rows->row(2);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);

        *moved = zge::kZero;
        uint32_t wheelsOff = 0;
        rideTouchedObject(rider, &moved->x);
        const bool standingStill = actionState(rider, uint32_t(kLevelAction)) == kActionRiding &&
                                   actionState(rider, uint32_t(kLevelSubState)) == kRidingCoast &&
                                   actionState(rider, uint32_t(kLevelPhase)) == kCoastStill;
        if (standingStill)
            pushCornersClear(rider);
        // A later call with a shallower stack finds this word in a slot it leaves unset (DM2's: a skinned
        // model's vertices keep it).
        *keptPoints = &points[0];
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &point = points[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            const bool back = (wheel & 1) != 0;
            pose.touched = 0;
            placeWheel(rider, wheel, point);

            if (!standingStill)
            {
                const bool forwards = ee::Float(0.0f) <= zge::dot(wheelVelocity, bodyForward);
                if (back != forwards)
                {
                    const int32_t probe = groundProbe(rider, wheel, &point.x);
                    if (probe == 1)
                    {
                        // Pushed off a wall: the drawn body moved, so both wheels' points again.
                        for (uint32_t each = 0; each < 2; ++each)
                            placeWheel(rider, each, points[each]);
                    }
                    else if (probe == 2)
                        return; // a bail
                }
            }

            // The spring's length and rate: the new length kept between the
            // least and most heights (the rate stopped there).
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
                // Falling: gravity along the body's up row by its slope; the
                // spring relaxes towards its most height; the wheel spins down
                // and rides along with a touched object.
                const ee::Float g = *g_gravity;
                addScaled(wheelVelocity, bodyUp, (g + g) * bodyUp.y);
                *matrix(pose.matrix) = body;
                pose.springRate = damped(rider, pose.springRate + (rider->wheelHeightMost - *height) * rider->springStiffness / rider->springMass);
                const ee::Float spin = ee::toFloat(pose.spin);
                pose.ground = kWheelOffGround;
                pose.spin = ee::toInt(spin * *g_spinKeep);
                if (rider->touchedObject != 0)
                    wheelVelocity = wheelVelocity + *moved;
                ++wheelsOff;
                continue;
            }

            // On the ground: the wheel's matrix on the face (its forward
            // the body's, turned by the yaw rate on the front wheel), its
            // velocity in the face's plane.
            collision::hitFace(hit); // (asked for, not used)
            collision::hitPoint(hit, &hitPoint->x);
            collision::hitPlane(hit, &hitPlane->x);
            // The weight's share: the original weighs the wheel's height by
            // a factor of 0 (a pitch folded away), so each wheel takes half.
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
                return; // bailed
            if (!back || rider->wheelPoses[0].ground == kWheelOffGround)
                groundHit(rider, hit);
            if (zge::dot(wheelVelocity, lastMatrix->row(2)) < ee::Float(0.0f))
                pose.speed = -pose.speed;

            const Surface &surface = surfaceOf(pose);
            if (pose.ground != kWheelSliding)
            {
                // Rolling: the speed along the wheel's forward row, less
                // the surface's loss, pulled by the slope, pushed by the
                // pedals (wheel 1), less the drag (wheel 1 its 0.75).
                const ee::Float rolled = pose.speed * (ee::Float(1.0f) - surface.rollingLoss) + *g_gravity * wheelForward.y;
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
            }
            else
            {
                // Sliding: the velocity pulled by the slope, the speed less
                // the surface's loss; the pushes that turn it back to the
                // wheel's forward row (across it, the pedals, the drag) by
                // the weight, the surface's slip once past its grip.
                const ee::Float pull = *g_gravity * wheelForward.y;
                pose.speed = pose.speed * (ee::Float(1.0f) - surface.rollingLoss);
                addScaled(wheelVelocity, wheelForward, pull);
                const ee::Float length = zge::vec3Length(&wheelVelocity.xyz());
                pose.speed = length;
                *scratch = wheelForward * (length * (ee::Float(1.0f) - *g_slideLoss));
                *across = *scratch - wheelVelocity;
                *across = wheelRight * zge::dot(*across, wheelRight);
                *across = *across * weight;
                *pedalled = zge::kZero;
                if (back)
                    *pedalled = wheelForward * rider->pedalForce;
                const ee::Float along = zge::dot(wheelVelocity, wheelForward);
                const ee::Float share = back ? *g_dragShare : ee::Float(1.0f) - *g_dragShare;
                const ee::Float drag = rider->wheelDrag * share;
                const ee::Float slowed = ee::abs(along) <= drag ? ee::abs(along) : drag;
                *dragged = wheelForward * (ee::Float(0.0f) <= along ? -slowed : slowed);
                scratch->x = across->x + pedalled->x + dragged->x;
                scratch->y = across->y + pedalled->y + dragged->y;
                scratch->z = across->z + pedalled->z + dragged->z;
                const ee::Float push = zge::vec3Length(&scratch->xyz());
                // (The original also works out the grip's limit, for a
                // rolling wheel's test, which never gets here.)
                const ee::Float slipLimit = load * (surface.slip * rider->slip);
                if (push < slipLimit)
                    pose.ground = kWheelRolling;
                *scratch = *scratch * (ee::Float(1.0f) / weight);
                if (pose.ground == kWheelSliding)
                {
                    *scratch = *scratch * (surface.slip * rider->slip);
                    if (ee::Float(1.0f) < push / slipLimit)
                    {
                        // Skidding: a mark and concrete dust, or dust, by the
                        // surface; the skid's length counted.
                        collision::hitPlane(hit, &skidPlane->x);
                        const uint32_t sound = pose.sound;
                        if (sound < kSkidSounds && (kSkidMarks >> sound & 1u) != 0)
                        {
                            skidNormal->x = skidPlane->x; // (x, y, z: w stays as it was)
                            skidNormal->y = skidPlane->y;
                            skidNormal->z = skidPlane->z;
                            difference(*dustAt, *hitPoint, wheelVelocity);
                            spawnSkidMark(hitPoint, dustAt, skidNormal);
                            // (a motion beside it, which the dust does not take)
                            dustMotion->x = wheelVelocity.x * ee::Float(0.5f);
                            dustMotion->y = wheelVelocity.y * ee::Float(0.5f);
                            dustMotion->z = wheelVelocity.z * ee::Float(0.5f);
                            spawnConcreteDust(dustAt);
                        }
                        else if (sound < kSkidSounds && (kSkidDust >> sound & 1u) != 0)
                        {
                            const ee::Float spread = ee::toFloat(int32_t(zge::randomNumber() & 0x1F)) * *g_dustSpread;
                            dustAt->y = 0.0f;
                            dustAt->z = 0.0f;
                            dustAt->x = spread;
                            zge::vec3RotateY(&dustAt->xyz(), &dustAt->xyz(), static_cast<int16_t>(zge::randomNumber()));
                            dustAt->x = dustAt->x + hitPoint->x;
                            dustAt->y = dustAt->y + hitPoint->y;
                            dustAt->z = dustAt->z + hitPoint->z;
                            dustMotion->x = wheelVelocity.x * ee::Float(0.5f);
                            dustMotion->y = wheelVelocity.y * ee::Float(0.5f);
                            dustMotion->z = wheelVelocity.z * ee::Float(0.5f);
                            spawnDust(dustAt);
                        }
                        rider->skidDistance = rider->skidDistance + pose.speed;
                    }
                }
                wheelVelocity = wheelVelocity + *scratch;
                subtractScaled(wheelVelocity, wheelUp, zge::dot(wheelVelocity, wheelUp));
                pose.speed = zge::vec3Length(&wheelVelocity.xyz());
            }
            pose.speed = ee::abs(pose.speed);
        }

        // A ray between the wheels, 0.2 up the body, both ways: what it
        // touches throws the rider off (a bail, kind 5).
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
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailThrown;
            return;
        }

        // The body's forward row from wheel 1's point to wheel 0's; its up
        // row from the wheels' (both down: halfway between their up rows;
        // one: its up row, kFlagOneWheelUpright when within 50 degrees of
        // upright), leaned into the yaw when fast enough, then eased towards
        // from the body's; in the air the body's.
        zge::matrixIdentity(rows);
        difference(forward, points[0], points[1]);
        forward = zge::unit(forward);
        if (wheelsOff < 2)
        {
            if (wheelsOff == 0)
                between(up, matrix(rider->wheelPoses[0].matrix)->row(1), matrix(rider->wheelPoses[1].matrix)->row(1), ee::Float(0.5f));
            else
            {
                // The wheel down.
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

            const ee::Float leanFrom = *g_leanFrom;
            if (rider->yawRate != 0 && leanFrom < rider->speed)
            {
                // Leaned about the forward row by the yaw rate, the more the
                // faster (at most 30 degrees at the top speed).
                zge::matrixIdentity(turn);
                const ee::Float lean = clampUnit(ee::toFloat(rider->yawRate) * ee::Float(360.0f / 65536.0f) * ee::Float(6.5f) / *g_leanYawDegrees *
                                                 ((rider->speed - leanFrom) / (*g_topSpeed - leanFrom)));
                zge::matrixRotateZ(turn, turn, static_cast<int16_t>(-ee::toInt(lean * *g_leanDegrees * *g_angleUnitsPerDegree)));
                right = zge::cross(up, forward);
                zge::matrixMultiply(rows, rows, turn);
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
        *matrix(rider->right) = body;

        // The drawn body halfway between where the wheels' points put it.
        zge::Vec4 &drawn = *vec4(rider->drawnPosition);
        bodyPositionFor(drawn, *scratch, *rows, rider, 0, points[0]);
        bodyPositionFor(*scratch, *scratch, *rows, rider, 1, points[1]);
        between(drawn, drawn, *scratch, ee::Float(0.5f));
        *vec4(rider->position) = drawn;

        // Next frame: the wheels' points moved by their velocities, the body
        // rows through them and its position halfway between their mounts.
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

        // Each wheel's velocity: from its point to where its mount goes
        // (stopped when slower than the rest speed and the drag's share,
        // capped at the top speed); a rolling wheel spins by it. The
        // rider's velocity is their mean; the force keeps the last one.
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &force = *vec4(rider->force);
        force = velocity;
        velocity = zge::kZero;
        const ee::Float restDrag = *g_wheelRestDrag;
        const ee::Float spinPerSpeed = *g_spinPerSpeed;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            zge::Vec4 &next = ahead[wheel];
            const zge::Vec4 &point = points[wheel];
            next = *vec4(wheelPoint(rider, wheel));
            next = zge::transformed(*rows, next);
            next.x = next.x + middle->x;
            next.y = next.y + middle->y;
            next.z = next.z + middle->z;
            difference(wheelVelocity, next, point);
            const ee::Float speed = zge::vec3Length(&wheelVelocity.xyz());
            pose.speed = speed;
            if (speed < *g_restSpeed + rider->wheelDrag * restDrag)
            {
                wheelVelocity = zge::kZero;
                pose.speed = 0.0f;
            }
            else if (const ee::Float most = *g_topSpeed * rider->speedStat; most < speed)
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
            velocity = velocity + wheelVelocity;
        }
        velocity = velocity * ee::Float(0.5f);
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (rider->speed < *g_restSpeed + rider->wheelDrag * *g_riderRestDrag)
        {
            velocity = zge::kZero;
            rider->speed = 0.0f;
            for (WheelPose &pose : rider->wheelPoses)
            {
                if (pose.ground != kWheelOffGround)
                {
                    *vec4(pose.velocity) = zge::kZero;
                    pose.speed = 0.0f;
                }
            }
        }
        difference(force, velocity, force);
        if (zge::dot(velocity, bodyForward) < ee::Float(0.0f))
            setFlag(rider, kFlagFakie);
        else
            clearFlag(rider, kFlagFakie);

        if (wheelsOff == 2)
            setPhysicsState(rider, uint32_t(kPhysicsAir));
    }
    READABLE_FUNCTION_WAITS(groundMove, Verified);
}
