// DM2's ground move (physics state 0's move callback, the move table's
// first): the two wheels on their springs, rolling, sliding and braking on
// the ground under them, the body set between them, the velocity from where
// the wheels go.
//
// Counterparts: DM2 GameCube 0x8007E3C0;
// Xbox 0x058390; BMX XXX PS2 groundMove (medium).

#include "../collision/collision.h"
#include "physics.h"
#include "../../fx/spawns.h"

#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/physics/ground_move.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        // The air's gravity a frame (also the slopes' pull here).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;      // 0.0055
        inline ee::Global<ee::Float, sym::g_slideLoss> g_slideLoss;    // 0.02 of a sliding wheel's speed lost turning it to its forward row
        inline ee::Global<ee::Float, sym::g_restSpeed> g_restSpeed;    // 0.0015: slower stops (and the drag's 0.3 more)
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;     // 0.35 (times Rider::speedStat)
        inline ee::Global<ee::Float, sym::g_leanFrom> g_leanFrom;     // 0.0667: the yaw leans the body only faster than this
        inline ee::Global<ee::Float, sym::g_balancePull> g_balancePull;  // -0.025
        inline ee::Global<ee::Float, sym::g_wheelieSlide> g_dragShare;    // 0.75 of the drag on wheel 1, the rest on wheel 0
        inline ee::Global<int16_t, sym::g_tiltAllowance> g_tiltAllowance; // 40 degrees
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upFollow;     // 0.085: the body's up row eased towards the wheels'
        inline ee::Global<ee::Float, sym::g_leanDegrees> g_leanDegrees;  // 30
        inline ee::Global<ee::Float, sym::g_wheelieSteerMost> g_leanYawDegrees; // 75
        inline ee::Global<ee::Float, sym::g_dustSpread> g_dustSpread;   // 0.01
        inline ee::Global<ee::Float, sym::g_balanceLeast> g_balanceLeast; // 0.1 (leaning forwards; g_balanceLeastBack backwards)
        inline ee::Global<ee::Float, sym::g_balanceLeastBack> g_balanceLeastBack; // -0.1
        inline ee::Global<ee::Float, sym::g_yawShare> g_yawShare;     // 0.15 of the yaw rate turns an airborne wheel's velocity
        inline ee::Global<ee::Float, sym::g_spinKeep2> g_spinKeep;     // 0.9967 of a wheel's spin kept off the ground
        inline ee::Global<ee::Float, sym::g_rayLift> g_rayLift;      // 0.2: the ray between the wheels runs this far up the body
        inline ee::Global<ee::Float, sym::g_angleUnitsPerDegree> g_angleUnitsPerDegree; // 65536 / 360
        inline ee::Global<ee::Float, sym::g_wheelRestDrag> g_wheelRestDrag; // 0.3
        inline ee::Global<ee::Float, sym::g_spinPerSpeed2> g_spinPerSpeed; // 2 / pi
        inline ee::Global<ee::Float, sym::g_riderRestDrag> g_riderRestDrag; // 0.3
        // The level frame's up (0, -1, 0, 0).
        inline ee::Global<zge::Vec4, sym::down> g_worldUp;

        // A wheel's surface, by its sound (WheelPose::sound).
        const Surface &surfaceOf(const WheelPose &pose) { return (*g_surfaces)[pose.sound]; }

        // What a sliding wheel leaves by its sound: a skid mark (0 .. 2, 4),
        // dust (5, 10, 11), nothing (3, 6 .. 9).
        constexpr uint32_t kSkidSounds = 12;
        constexpr uint32_t kSkidMarks = 0x1Fu - 0x08u; // sounds 0, 1, 2, 4
        constexpr uint32_t kSkidDust = 0xC20u;         // sounds 5, 10, 11

        constexpr int16_t kAngle60 = 10922;
        constexpr int16_t kAngle50 = 9102;
        constexpr int16_t kAirYawMost = 910; // 5 degrees a frame
        constexpr uint8_t kBailThrown = 5;   // Rider::bailKind: thrown by an object between the wheels

        // Rider::flags.
        constexpr uint32_t kFlagGrindAsked = (1u << 20); // the grind button held (riderFrame clears it)
        constexpr uint32_t kFlagUpright = 0x2000000u;   // one wheel up, the other's up row within 50 degrees of upright
        constexpr uint32_t kFlagsOwnGround = kFlagManual | kFlagNoseWheelie;

        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        // The impact and the lengths called as the original calls them
        // (guest calls): the scales after them carry what they left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline int32_t groundImpactByCall(Rider *rider, uint32_t wheel, const collision::RayHit *hit)
        {
            return ee::game<int32_t>(sym::groundImpact, rider, wheel, hit);
        }
        inline ee::Float lengthByCall(const zge::Vec4 &v) { return ee::game<ee::Float>(sym::vec3Length, &v.x); }

        // v's x, y and z plus (or less) d's times s, in the original's
        // order: y and z worked out, then x stored. w stays as it was.
        void addScaled(zge::Vec4 &v, const zge::Vec4 &d, ee::Float s)
        {
            const ee::Float y = v.y + d.y * s;
            const ee::Float z = v.z + d.z * s;
            v.x = v.x + d.x * s;
            v.y = y;
            v.z = z;
        }
        void subtractScaled(zge::Vec4 &v, const zge::Vec4 &d, ee::Float s)
        {
            const ee::Float y = v.y - d.y * s;
            const ee::Float z = v.z - d.z * s;
            v.x = v.x - d.x * s;
            v.y = y;
            v.z = z;
        }
        // a + (b - a) * t into out's x, y and z, x first.
        void between(zge::Vec4 &out, const zge::Vec4 &a, const zge::Vec4 &b, ee::Float t)
        {
            const ee::Float ax = a.x, ay = a.y, az = a.z;
            out.x = ax + (b.x - ax) * t;
            out.y = ay + (b.y - ay) * t;
            out.z = az + (b.z - az) * t;
        }
        // a - b into out's x, y and z (y and z worked out first).
        void difference(zge::Vec4 &out, const zge::Vec4 &a, const zge::Vec4 &b)
        {
            const ee::Float y = a.y - b.y;
            const ee::Float z = a.z - b.z;
            out.x = a.x - b.x;
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

        // A wheel's spring: its rate pulled towards the most height, damped.
        void springTowards(const Rider *rider, WheelPose &pose, ee::Float pull)
        {
            const ee::Float rate = pose.springRate + pull;
            const ee::Float damping = rate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
            pose.springRate = rate * (ee::Float(1.0f) - damping);
        }
    }

    // State 0's move. Per wheel (0 the front, 1 the back): its point is the
    // body's mount point in the world; the leading one (the front moving
    // forwards, the back moving backwards) probes for walls (groundProbe:
    // 2, a bail, ends the move); the spring's length (WheelPose::springLength,
    // kept between the stats' least and most heights) and rate (springRate) lift
    // the point; groundAhead finds the ground under it.
    //  - No ground: the wheel is off the ground (kWheelOffGround): gravity
    //    pulls its velocity (along the body's up row, its slope), or with the
    //    other wheel down (or kFlagsOwnGround) its velocity turns about the
    //    other wheel's frame: the balance (Rider::balance, the tilt left to
    //    60 degrees), the roll and a share of the yaw; its spring relaxes.
    //  - Ground: the wheel's matrix stands on the face, its velocity in the
    //    face's plane; the spring carries half the weight; groundImpact
    //    (2 ends the move), a grind asked for (state 4: 6 ends it),
    //    groundHit; rolling it keeps its forward speed less the surface's
    //    loss, wheel 1 pushed by the pedals, both slowed by the drag; past
    //    the surface's grip it slides (skid marks, dust).
    // Then a ray between the wheels (an object there may throw the rider:
    // a bail of kind 5); the body's rows from the wheels' points (its up
    // row eased towards the wheels' up rows, leaned into the yaw) and its
    // position halfway between their mounts; each wheel's velocity is where
    // its mount goes next frame (capped, stopped when slow, the spin from
    // it), the rider's velocity their mean and the force the change. Both
    // wheels off the ground: the air (state 1). Returns the state's result
    // (2 when the move goes on). GameCube 0x8007E3C0.
    //
    // Every scale carries what the last call left, as airGravity's does.
    int32_t groundMove(Rider *rider)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4[2]> points;     // the wheels' points
        ee::Local<zge::Vec4[2]> ahead;      // the rays' ends, then the points next frame
        ee::Local<zge::Vec4> middle;        // the body's position next frame
        ee::Local<zge::Vec4> skidNormal;
        ee::Local<zge::Matrix> rows;        // the body's rows being built
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Vec4> moved;         // rideTouchedObject's carry
        ee::Local<zge::Matrix> lastMatrix;  // a wheel's matrix before (the between-wheels hit's plane and point later)
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
        ee::Local<ee::Ptr<const zge::Vec4>> keptBodyRight;   // and the drawn body's right and forward rows' addresses,
        ee::Local<ee::Ptr<const zge::Vec4>> keptBodyForward; // which it keeps there too
        // The rest of what the original keeps in its frame, which outlives the
        // return as the words above do: a later call's local it never sets reads
        // them (editor-0: posePart's bone matrices' w lanes, the front wheel
        // point's address, carried into a shadow's posed vertices).
        ee::Local<uint32_t> wheelLoop;                     // the wheel loops' counter
        ee::Local<uint32_t> wheelsOff;                     // the wheels off the ground
        ee::Local<ee::Ptr<zge::Vec4>> keptMoved;           // the carry's address,
        ee::Local<ee::Ptr<zge::Vec4>> keptHitPoint;        // a wheel's hit point's,
        ee::Local<ee::Ptr<zge::Vec4>> keptSummedVelocity;  // the rider's velocity's, as the wheels' are added to it,
        ee::Local<ee::Ptr<zge::Vec4>> keptFrontWheelPoint; // the wheel points',
        ee::Local<ee::Ptr<zge::Vec4>> keptBackWheelPoint;
        ee::Local<ee::Ptr<zge::Vec4>> keptVelocity;        // the rider's velocity's and force's,
        ee::Local<ee::Ptr<zge::Vec4>> keptForce;
        ee::Local<ee::Ptr<zge::Vec4>> keptAhead;           // the rays' ends',
        ee::Local<uint32_t> keptBack;                      // whether the wheel is the back one,
        ee::Local<ee::Ptr<zge::Vec4>> keptWheelForward;    // and a wheel's forward row's address
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &forward = rows->row(2);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);

        *keptMoved = &*moved;
        *moved = zge::kZero;
        // The frame keeps the body's rows' addresses past the return: a
        // later call's matrix whose w lanes nothing writes (bonePoint's,
        // boneMatrix leaves row 3's w) reads the forward row's there, and
        // VU0 carries it into the bodies a dynamics object wakes.
        *keptBodyForward = &body.row(2);
        *keptBodyRight = &body.row(0);
        *wheelsOff = 0;
        rideTouchedObject(rider, &moved->x);
        // A later call with a shallower stack finds this word in a slot it
        // leaves unset: a skinned model's vertices keep it (pq-woodward's
        // 40 heap words at t6000 without it).
        *keptPoints = &points[0];
        for (*wheelLoop = 0; *wheelLoop < 2; ++*wheelLoop)
        {
            const uint32_t wheel = *wheelLoop;
            WheelPose &pose = rider->wheelPoses[wheel];
            WheelPose &other = rider->wheelPoses[wheel ^ 1];
            zge::Vec4 &point = points[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            const bool back = (wheel & 1) != 0;
            *keptBack = back;
            pose.touched = 0;
            point = *vec4(wheelPoint(rider, wheel));
            engine::transformPoint(&point.x, rider->body, &point.x);
            point = point + *vec4(rider->drawnPosition);

            const bool forwards = ee::Float(0.0f) <= zge::dot(wheelVelocity, bodyForward);
            if (back != forwards && groundProbe(rider, wheel, &point.x) == 2)
                return 2;

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
                ee::Float now;
                if ((rider->flags & kFlagsOwnGround) == 0 && other.ground != kWheelOffGround)
                {
                    // Falling: gravity along the body's up row by its slope.
                    const ee::Float g = *g_gravity;
                    addScaled(wheelVelocity, bodyUp, (g + g) * bodyUp.y);
                    engine::copyMatrix(pose.matrix, rider->body);
                    now = *height;
                }
                else
                {
                    // The other wheel's frame: its up row, the body's
                    // forward made level with it.
                    engine::copyMatrix(rows, other.matrix);
                    right = zge::cross(up, bodyForward);
                    right = zge::unit(right);
                    forward = zge::cross(right, up);
                    const int32_t pitch = zge::angleAcos(zge::dot(forward, bodyForward));
                    subtractScaled(wheelVelocity, up, zge::dot(wheelVelocity, up));
                    zge::Matrix &level = *turn;
                    level.row(2) = forward;
                    level.row(0) = zge::cross(*g_worldUp, level.row(2));
                    level.row(0) = zge::unit(level.row(0));
                    level.row(1) = zge::cross(level.row(2), level.row(0));

                    // The tilt left: the level frame's up from upright and
                    // 40 degrees more, at most 60 degrees, less the pitch;
                    // scaled by the other wheel's up row when that wheel
                    // is the low one.
                    int32_t limit;
                    if (zge::angleAcos(-level.row(1).y) + *g_tiltAllowance <= kAngle60)
                        limit = static_cast<int16_t>(*g_tiltAllowance + zge::angleAcos(-level.row(1).y));
                    else
                        limit = kAngle60;
                    const bool climbing = forward.y < ee::Float(0.0f);
                    int32_t left = limit - pitch;
                    if (back == climbing)
                        left = static_cast<int16_t>(ee::toInt(ee::toFloat(limit) * ee::Float(1.0f / 65536.0f) * -up.y * ee::Float(65536.0f))) - pitch;
                    const int16_t tilt = static_cast<int16_t>(left);
                    rider->balance = clampUnit(-(ee::toFloat(tilt) * ee::Float(1.0f / 65536.0f)) * ee::Float(10.0f));

                    // Pulled along the other's up row by the tilt (at least
                    // 0.1 of the wheelbase) and the roll.
                    const ee::Float wheelbase = rider->frontWheelPoint[2] - rider->backWheelPoint[2];
                    const ee::Float sine = engine::sinTurn(tilt);
                    const ee::Float least = *g_balanceLeast;
                    ee::Float pull;
                    if (!(ee::abs(sine) < least))
                        pull = sine * wheelbase;
                    else
                        pull = (ee::Float(0.0f) <= sine ? least : *g_balanceLeastBack) * wheelbase;
                    addScaled(wheelVelocity, up, pull * *g_balancePull);
                    const int16_t roll = back ? static_cast<int16_t>(-rider->roll) : rider->roll;
                    addScaled(wheelVelocity, up, engine::sinTurn(roll) * wheelbase);

                    // A share of the yaw (at most 5 degrees) turns the
                    // velocity in the body's space (the body's position row
                    // is the origin: the transforms only turn it).
                    engine::copyMatrix(turn, rider->body);
                    *scratch = wheelVelocity;
                    zge::matrixTranspose3(turn, turn);
                    engine::transformPoint(&scratch->x, turn, &scratch->x);
                    int32_t yaw = rider->yawRate;
                    if (!back)
                        yaw = -yaw;
                    const int16_t share = static_cast<int16_t>(
                        ee::toInt(ee::toFloat(static_cast<int16_t>(yaw)) * ee::Float(1.0f / 65536.0f) * *g_yawShare * ee::Float(65536.0f)));
                    zge::vec3RotateY(&scratch->xyz(), &scratch->xyz(), share < -kAirYawMost ? -kAirYawMost : share > kAirYawMost ? kAirYawMost : share);
                    zge::matrixTranspose3(turn, turn);
                    engine::transformPoint(&scratch->x, turn, &scratch->x);
                    wheelVelocity = *scratch;
                    now = *height;
                    if (!back)
                        addScaled(wheelVelocity, right, right.y * *g_gravity);
                }

                // The spring relaxes towards its most height; the wheel
                // spins down and rides along with a touched object.
                springTowards(rider, pose, (rider->wheelHeightMost - now) * rider->springStiffness / rider->springMass);
                const ee::Float spin = ee::toFloat(pose.spin);
                pose.ground = kWheelOffGround;
                pose.spin = ee::toInt(spin * *g_spinKeep);
                if (rider->touchedObject != 0)
                    wheelVelocity = wheelVelocity + *moved;
                ++*wheelsOff;
                continue;
            }

            // On the ground: the wheel's matrix on the face (its forward
            // the body's, turned by the yaw rate on the front wheel), its
            // velocity in the face's plane.
            collision::hitFace(hit); // (asked for, not used)
            *keptHitPoint = &*hitPoint;
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
            engine::copyMatrix(lastMatrix, pose.matrix);
            *keptWheelForward = &wheelForward;
            wheelUp = *hitPlane;
            wheelForward = bodyForward;
            wheelRight = zge::cross(wheelUp, wheelForward);
            wheelRight = zge::unit(wheelRight);
            wheelForward = zge::cross(wheelRight, wheelUp);
            if (!back)
            {
                zge::matrixIdentity(turn);
                zge::matrixRotateY(turn, turn, rider->yawRate);
                engine::matrixProduct(pose.matrix, pose.matrix, turn);
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
            const ee::Float damping = springRate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
            pose.springLength = now;
            pose.springRate = springRate * (ee::Float(1.0f) - damping);

            if (groundImpactByCall(rider, wheel, hit) == 2)
                return 2;
            if ((rider->flags & kFlagGrindAsked) != 0)
            {
                const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
                if (state == kPhysicsGrind || state == kPhysicsStall)
                    return 6;
            }
            if (!back || rider->wheelPoses[0].ground == kWheelOffGround)
                groundHit(rider, hit);
            if (zge::dot(wheelVelocity, lastMatrix->row(2)) < ee::Float(0.0f))
                pose.speed = -pose.speed;

            const Surface &surface = surfaceOf(pose);
            ee::Float speed;
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
                speed = pose.speed;
                wheelVelocity = zge::scaledCarrying(wheelForward, speed, zge::scaleLeftover(zge::LeftBy::lastCall));
                pose.ground = kWheelRolling;
            }
            else
            {
                // Sliding: the velocity pulled by the slope, the speed less
                // the surface's loss; the pushes that turn it back to the
                // wheel's forward row (across it, the pedals, the drag) by
                // the weight, the surface's slip once past its grip.
                const ee::Float slope = wheelForward.y;
                const ee::Float pull = *g_gravity * slope;
                const ee::Float x = wheelVelocity.x + wheelForward.x * pull;
                const ee::Float y = wheelVelocity.y + slope * pull;
                const ee::Float z = wheelVelocity.z + wheelForward.z * pull;
                pose.speed = pose.speed * (ee::Float(1.0f) - surface.rollingLoss);
                wheelVelocity.x = x;
                wheelVelocity.y = y;
                wheelVelocity.z = z;
                const ee::Float length = lengthByCall(wheelVelocity);
                const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
                const ee::Float loss = *g_slideLoss;
                pose.speed = length;
                *scratch = zge::scaledCarrying(wheelForward, length * (ee::Float(1.0f) - loss), carried);
                *across = *scratch - wheelVelocity;
                *across = zge::scaledCarrying(wheelRight, zge::dot(*across, wheelRight), carried);
                *across = zge::scaledCarrying(*across, weight, carried);
                *pedalled = zge::kZero;
                if (back)
                    *pedalled = zge::scaledCarrying(wheelForward, rider->pedalForce, carried);
                const ee::Float along = zge::dot(wheelVelocity, wheelForward);
                const ee::Float share = back ? *g_dragShare : ee::Float(1.0f) - *g_dragShare;
                const ee::Float drag = rider->wheelDrag * share;
                const ee::Float slowed = ee::abs(along) <= drag ? ee::abs(along) : drag;
                *dragged = zge::scaledCarrying(wheelForward, ee::Float(0.0f) <= along ? -slowed : slowed, carried);
                {
                    const ee::Float y2 = across->y + pedalled->y + dragged->y;
                    const ee::Float z2 = across->z + pedalled->z + dragged->z;
                    scratch->x = across->x + pedalled->x + dragged->x;
                    scratch->y = y2;
                    scratch->z = z2;
                }
                const ee::Float push = lengthByCall(*scratch);
                const uint64_t carriedPush = zge::scaleLeftover(zge::LeftBy::lastCall);
                // (The original also works out the grip's limit, for a
                // rolling wheel's test, which never gets here.)
                const ee::Float slipLimit = load * (surface.slip * rider->slip);
                if (push < slipLimit)
                    pose.ground = kWheelRolling;
                *scratch = zge::scaledCarrying(*scratch, ee::Float(1.0f) / weight, carriedPush);
                if (pose.ground == kWheelSliding)
                {
                    *scratch = zge::scaledCarrying(*scratch, surface.slip * rider->slip, carriedPush);
                    if (ee::Float(1.0f) < push / slipLimit)
                    {
                        // Skidding: a mark or dust by the surface.
                        collision::hitPlane(hit, &skidPlane->x);
                        const uint32_t sound = pose.sound;
                        if (sound < kSkidSounds && (kSkidMarks >> sound & 1u) != 0)
                        {
                            skidNormal->x = skidPlane->x; // (x, y, z: w stays as it was)
                            skidNormal->y = skidPlane->y;
                            skidNormal->z = skidPlane->z;
                            difference(*dustAt, *hitPoint, wheelVelocity);
                            spawnSkidMark(hitPoint, dustAt, &skidNormal->xyz());
                        }
                        else if (sound < kSkidSounds && (kSkidDust >> sound & 1u) != 0)
                        {
                            const int32_t random = zge::randomNumber();
                            const ee::Float spread = ee::toFloat(random & 0x1F) * *g_dustSpread;
                            dustAt->y = 0.0f;
                            dustAt->z = 0.0f;
                            dustAt->x = spread;
                            zge::vec3RotateY(&dustAt->xyz(), &dustAt->xyz(), static_cast<int16_t>(zge::randomNumber()));
                            const ee::Float y3 = dustAt->y + hitPoint->y;
                            const ee::Float z3 = dustAt->z + hitPoint->z;
                            dustAt->x = dustAt->x + hitPoint->x;
                            dustAt->y = y3;
                            dustAt->z = z3;
                            dustMotion->x = wheelVelocity.x * ee::Float(0.5f);
                            dustMotion->y = wheelVelocity.y * ee::Float(0.5f);
                            dustMotion->z = wheelVelocity.z * ee::Float(0.5f);
                            dust(&dustAt->x, &dustMotion->x);
                        }
                        rider->skidDistance = rider->skidDistance + pose.speed;
                    }
                }
                wheelVelocity = wheelVelocity + *scratch;
                subtractScaled(wheelVelocity, wheelUp, zge::dot(wheelVelocity, wheelUp));
                speed = zge::vec3Length(&wheelVelocity.xyz());
                pose.speed = speed;
            }
            pose.speed = ee::abs(speed);
        }

        // A ray between the wheels, 0.2 up the body, both ways: an object
        // there may throw the rider off (a bail, kind 5).
        const ee::Float lift = *g_rayLift;
        const ee::Float liftX = bodyUp.x * lift, liftY = bodyUp.y * lift, liftZ = bodyUp.z * lift;
        *keptAhead = &ahead[0];
        for (*wheelLoop = 0; *wheelLoop < 2; ++*wheelLoop)
        {
            const uint32_t wheel = *wheelLoop;
            ahead[wheel].x = points[wheel].x + liftX;
            ahead[wheel].y = points[wheel].y + liftY;
            ahead[wheel].z = points[wheel].z + liftZ;
        }
        const collision::RayHit *blocker = collision::rayCast(&ahead[0].x, &ahead[1].x);
        if (blocker == nullptr)
            blocker = collision::rayCast(&ahead[1].x, &ahead[0].x);
        if (blocker != nullptr)
        {
            collision::hitFace(blocker); // (asked for, not used)
            collision::hitPoint(blocker, &lastMatrix->row(1).x);
            collision::hitPlane(blocker, &lastMatrix->row(0).x);
            if (collision::touchObject(rider, blocker) != 0)
            {
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                rider->bailKind = kBailThrown;
                return collision::touchObject(rider, blocker);
            }
        }

        // The body's forward row from wheel 1's point to wheel 0's; its up
        // row from the wheels' (both down: halfway between their up rows;
        // one: its up row, kFlagUpright when within 50 degrees of upright),
        // leaned into the yaw when fast enough, then eased towards from
        // the body's; in the air the body's.
        rows->row(3) = zge::kZero;
        difference(forward, points[0], points[1]);
        forward = zge::unit(forward);
        *keptFrontWheelPoint = vec4(rider->frontWheelPoint);
        *keptBackWheelPoint = vec4(rider->backWheelPoint);
        *keptForce = vec4(rider->force);
        *keptVelocity = vec4(rider->velocity);
        if (*wheelsOff < 2)
        {
            if (*wheelsOff == 0)
                between(up, matrix(rider->wheelPoses[0].matrix)->row(1), matrix(rider->wheelPoses[1].matrix)->row(1), ee::Float(0.5f));
            else
            {
                // The wheel down.
                *wheelLoop = 0;
                while (*wheelLoop < 2 && rider->wheelPoses[*wheelLoop].ground == kWheelOffGround)
                    ++*wheelLoop;
                if (*wheelLoop < 2)
                {
                    const zge::Vec4 &wheelUp = matrix(rider->wheelPoses[*wheelLoop].matrix)->row(1);
                    up = wheelUp;
                    if (zge::angleAcos(-wheelUp.y) < kAngle50)
                        rider->flags |= kFlagUpright;
                    else
                        rider->flags &= ~kFlagUpright;
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
                engine::matrixProduct(rows, rows, turn);
            }
            between(up, bodyUp, up, *g_upFollow);
        }
        else
            up = bodyUp;
        right = zge::cross(up, forward);
        right = zge::unit(right);
        up = zge::cross(forward, right);
        up = zge::unit(up);
        engine::copyMatrix(rider->body, rows);
        engine::copyMatrix(rider->right, rider->body);

        // The drawn body halfway between where the wheels' points put it.
        zge::Vec4 &drawn = *vec4(rider->drawnPosition);
        *scratch = *vec4(rider->frontWheelPoint);
        engine::transformPoint(&scratch->x, rows, &scratch->x);
        {
            const ee::Float dy = points[0].y - scratch->y;
            drawn.x = points[0].x - scratch->x;
            drawn.y = dy;
            drawn.z = points[0].z - scratch->z;
        }
        *scratch = *vec4(rider->backWheelPoint);
        engine::transformPoint(&scratch->x, rows, &scratch->x);
        {
            const ee::Float x = points[1].x - scratch->x;
            const ee::Float y = points[1].y - scratch->y;
            const ee::Float z = points[1].z - scratch->z;
            scratch->x = x;
            scratch->y = y;
            scratch->z = z;
            between(drawn, drawn, *scratch, ee::Float(0.5f));
        }
        *vec4(rider->position) = drawn;

        // Next frame: the wheels' points moved by their velocities, the body
        // rows through them and its position halfway between their mounts.
        for (*wheelLoop = 0; *wheelLoop < 2; ++*wheelLoop)
        {
            const uint32_t wheel = *wheelLoop;
            const zge::Vec4 &point = points[wheel];
            const zge::Vec4 &wheelVelocity = *vec4(rider->wheelPoses[wheel].velocity);
            zge::Vec4 &end = ahead[wheel];
            const ee::Float y = wheelVelocity.y;
            const ee::Float x = point.x + wheelVelocity.x;
            const ee::Float z = wheelVelocity.z;
            end.x = x;
            end.y = point.y + y;
            end.z = point.z + z;
        }
        difference(forward, ahead[0], ahead[1]);
        forward = zge::unit(forward);
        right = zge::cross(up, forward);
        right = zge::unit(right);
        up = zge::cross(forward, right);
        *scratch = *vec4(rider->frontWheelPoint);
        engine::transformPoint(&scratch->x, rows, &scratch->x);
        {
            const ee::Float dy = ahead[0].y - scratch->y;
            middle->x = ahead[0].x - scratch->x;
            middle->y = dy;
            middle->z = ahead[0].z - scratch->z;
        }
        *scratch = *vec4(rider->backWheelPoint);
        engine::transformPoint(&scratch->x, rows, &scratch->x);
        {
            const ee::Float x = ahead[1].x - scratch->x;
            const ee::Float y = ahead[1].y - scratch->y;
            const ee::Float z = ahead[1].z - scratch->z;
            scratch->x = x;
            scratch->y = y;
            scratch->z = z;
            between(*middle, *middle, *scratch, ee::Float(0.5f));
        }

        // Each wheel's velocity: from its point to where its mount goes
        // (stopped when slower than the rest speed and the drag's share,
        // capped at the top speed); a rolling wheel spins by it. The
        // rider's velocity is their mean; the force keeps the last one.
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &force = *vec4(rider->force);
        force = velocity;
        velocity = zge::kZero;
        *keptSummedVelocity = &velocity;
        const ee::Float restDrag = *g_wheelRestDrag;
        const ee::Float spinPerSpeed = *g_spinPerSpeed;
        for (*wheelLoop = 0; *wheelLoop < 2; ++*wheelLoop)
        {
            const uint32_t wheel = *wheelLoop;
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            zge::Vec4 &next = ahead[wheel];
            const zge::Vec4 &point = points[wheel];
            next = *vec4(wheelPoint(rider, wheel));
            engine::transformPoint(&next.x, rows, &next.x);
            const ee::Float x = next.x + middle->x;
            next.x = x;
            const ee::Float y = next.y + middle->y;
            next.y = y;
            const ee::Float z = next.z + middle->z;
            next.z = z;
            wheelVelocity.x = x - point.x;
            wheelVelocity.y = y - point.y;
            wheelVelocity.z = z - point.z;
            const ee::Float speed = lengthByCall(wheelVelocity);
            const ee::Float rest = *g_restSpeed;
            pose.speed = speed;
            const ee::Float top = *g_topSpeed;
            if (speed < rest + rider->wheelDrag * restDrag)
            {
                wheelVelocity = zge::kZero;
                pose.speed = 0.0f;
            }
            else if (const ee::Float most = top * rider->speedStat; most < speed)
            {
                wheelVelocity = zge::unit(wheelVelocity);
                wheelVelocity = zge::scaledCarrying(wheelVelocity, most, zge::scaleLeftover(zge::LeftBy::lastCall));
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
        velocity = zge::scaledCarrying(velocity, ee::Float(0.5f), zge::scaleLeftover(zge::LeftBy::lastCall));
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (rider->speed < *g_restSpeed + rider->wheelDrag * *g_riderRestDrag)
        {
            velocity = zge::kZero;
            rider->speed = 0.0f;
            for (*wheelLoop = 0; *wheelLoop < 2; ++*wheelLoop)
            {
                WheelPose &pose = rider->wheelPoses[*wheelLoop];
                if (pose.ground != kWheelOffGround)
                {
                    *vec4(pose.velocity) = zge::kZero;
                    pose.speed = 0.0f;
                }
            }
        }
        difference(force, velocity, force);
        if (zge::dot(velocity, bodyForward) < ee::Float(0.0f))
            rider->flags |= kFlagFakie;
        else
            rider->flags &= ~kFlagFakie;

        if (*wheelsOff != 2)
            return 2;
        return setPhysicsState(rider, uint32_t(kPhysicsAir));
    }
    READABLE_FUNCTION(groundMove, Verified);
}
