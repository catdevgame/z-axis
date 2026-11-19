// BMX XXX's air move (physics states 1 and 8's move callback, the move table's
// second and ninth): the wheels on their springs reaching for the ground (a
// landing graded for its motion, a bounce off a kind 5 face, a crash), the
// rider's air bodies against walls, the lip's landing ray, the shadow's tint
// by the height. DM2's (game/dm2/rider/physics/air_move.cpp), read in BMX
// XXX's machine code: the wheels' rays touch what they meet, the move stops
// when that touch changed the move, landings rumble and pick a landing
// motion by how hard and how far round they come down, and with both wheels
// up the rider's air bodies (riderTumbleMove) stand for DM2's eight probes.

#include "physics.h"

#include "../collision/collision.h"
#include "../game_calls.h"

#include "bmx/fx/spawn_effects.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/dynamics.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/physics/air_move.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        using collision::FaceRef;

        // The move table (physics frame's): a wheel's touch may change the physics state, and the move stops when
        // the new state's move is not this one.
        using StateCallback = ee::FunctionPtr<void(Rider *rider)>;
        inline ee::Global<StateCallback[kPhysicsStates], sym::g_moveCallbacks> g_moveCallbacks;

        inline ee::Global<int16_t, sym::g_landWithin> g_landWithin;         // 78 degrees: a face this near the drawn up row is ground
        inline ee::Global<int16_t, sym::g_rollWithin> g_rollWithin;         // 30 degrees
        inline ee::Global<ee::Float, sym::g_landSoundFrom> g_landSoundFrom; // 0.1
        inline ee::Global<ee::Float, sym::g_landRumblePerSpeed> g_landRumblePerSpeed; // 765
        inline ee::Global<ee::Float, sym::g_landHardest> g_landHardest;     // 0.8333: faster into the ground crashes
        inline ee::Global<ee::Float, sym::g_landStraight> g_landStraight;   // 0.185: a wheel's sideways speed below this lands
        inline ee::Global<ee::Float, sym::g_backWheelCarry> g_backWheelCarry; // 0.8 of the rider's speed onto the back wheel
        inline ee::Global<ee::Float, sym::g_airSpinPerSpeed> g_spinPerSpeed; // 2 / pi
        inline ee::Global<ee::Float, sym::g_airSpinKeep> g_spinKeep;        // 0.9967
        inline ee::Global<ee::Float, sym::g_shadowShade> g_shadowShade;     // 16 / 3 a unit of height
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upFollow;     // 0.085
        // The landing's motion (Rider::landingKind) by how hard the wheel came down: past these speeds into the
        // ground the landings 4, 3, 2 and 1 (0.5333, 0.4, 0.3167, 0.125), else 0.
        inline ee::Global<ee::Float, sym::g_landKind4From> g_landKind4From;
        inline ee::Global<ee::Float, sym::g_landKind3From> g_landKind3From;
        inline ee::Global<ee::Float, sym::g_landKind2From> g_landKind2From;
        constexpr ee::Float kLandKind1From(0.125f);
        // The rider model's "Hips" body (its air bodies follow the rider) and the bike's bone the lip's ray starts
        // from.
        inline ee::Global<uint32_t, sym::g_hipsBoneHash> g_hipsName;
        inline ee::Global<uint32_t, sym::g_backWheelBoneHash> g_lipBoneName;
        constexpr ee::Float kLipReach(-0.375f); // the lip's ray: this much of the right row (the other way mirrored)
        // The volume the models' shadows are tested against the view with (setSceneShadowViewTest's 0x20 bytes, a
        // zge::ViewSphere): a sphere about the ground under the rider.
        constexpr ee::Float kShadowViewRadius(0.5f);
        constexpr ee::Float kShadowRay = 150.0f;
        constexpr ee::Float kShadowHeight = 12.0f;
        constexpr int32_t kShadowLeast = 16, kShadowMost = 255;

        // The landing's sound by the face's sound.
        constexpr uint8_t kLandingSurface[11] = {0, 7, 2, 7, 5, 4, 0, 0, 0, 0, 4};

        constexpr zge::Angle kAngle32 = 0x16C2;
        constexpr zge::Angle kAngle45 = 0x2001;
        constexpr zge::Angle kAngle50 = 9102;
        constexpr zge::Angle kAngle60 = 0x2AAB;
        constexpr zge::Angle kAngle70 = 0x31C8;
        constexpr int32_t kAngle90 = 0x4000;
        constexpr uint32_t kNoFaceSound = 0xFFFFu;
        constexpr uint32_t kSurfaceBounce = 5;
        constexpr uint32_t kSurfaceRamp = 8;
        constexpr uint32_t kSurfaceWorldFirst = 1, kSurfaceWorldLast = 3;
        constexpr uint32_t kSoundBounce = 0x1F;
        constexpr uint32_t kSoundLanding = 0x21;
        constexpr int32_t kSoundVolume = 127;
        constexpr ee::Float kLandingVolumePerSpeed = 400.0f;
        constexpr int32_t kMostRumble = 255;
        constexpr uint32_t kRumbleLargeMotor = 1;
        // touchObject's words: the wheels' rays, the lip's, and the bail into what was met.
        constexpr uint32_t kTouchLanding = 3;
        constexpr uint32_t kTouchLip = 4;
        constexpr uint32_t kTouchBail = 6;
        constexpr uint8_t kLandingKindMost = 4;
        // Rider::bailKind (DM2's).
        constexpr uint8_t kBailCrashed = 2;
        constexpr uint8_t kBailBounced = 3; // off a moving object
        constexpr uint8_t kBailLandedSideways = 4;
        constexpr uint8_t kBailBody = 5;

        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

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
        // v's x, y and z less d's times s.
        void subtractScaled(zge::Vec4 &v, const zge::Vec4 &d, ee::Float s)
        {
            v.x = v.x - d.x * s;
            v.y = v.y - d.y * s;
            v.z = v.z - d.z * s;
        }
        // The body's position halfway between where the wheels' mounts put it, through `rows`, from the two points
        // into `out`.
        void bodyBetween(const Rider *rider, zge::Vec4 &out, const zge::Vec4 *points, const zge::Matrix &rows, zge::Vec4 &scratch)
        {
            scratch = *vec4(rider->frontWheelPoint);
            scratch = zge::transformed(rows, scratch);
            difference(out, points[0], scratch);
            scratch = *vec4(rider->backWheelPoint);
            scratch = zge::transformed(rows, scratch);
            difference(scratch, points[1], scratch);
            between(out, out, scratch, ee::Float(0.5f));
        }
        // The landing motion by how hard the wheel came down.
        uint8_t landingKindBySpeed(ee::Float closing)
        {
            if (*g_landKind4From < closing)
                return 4;
            if (*g_landKind3From < closing)
                return 3;
            if (*g_landKind2From < closing)
                return 2;
            if (kLandKind1From < closing)
                return 1;
            return 0;
        }
        // Landed rolling forwards, the landing motion raised by how far round the travel comes down from the
        // forward row: from 32 degrees one more (at least the second), from 45 two (the third), from 60 three
        // (the fourth), at most the fourth.
        void raiseLandingKind(Rider *rider, int32_t travelAngle)
        {
            int32_t more = 0, least = 0;
            if (travelAngle >= kAngle60)
                more = 3, least = 4;
            else if (travelAngle >= kAngle45)
                more = 2, least = 3;
            else if (travelAngle >= kAngle32)
                more = 1, least = 2;
            int32_t kind = rider->landingKind + more;
            if (!(kind < kLandingKindMost + 1))
                kind = kLandingKindMost;
            if (kind < least)
                kind = least;
            rider->landingKind = static_cast<uint8_t>(kind);
        }
    }

    // States 1 and 8's move. The drawn body's rows pitched by bodyPitch; per wheel (0 the front): its velocity the
    // rider's, its point the mount's, its spring kept between the least and most heights; a ray from the point by
    // the velocity and down the up row by the spring and the wheel's stat touches what it meets (a touch that
    // changed the move ends this one) and finds the ground:
    //  - a face within 78 degrees of the drawn up row (steeper than 70 degrees from level only of the world's
    //    kinds 1 .. 3; never a ramp's, kind 8), met closing: a kind 5 face bounces the rider (its tag word along
    //    its normal); else the landing test (the front wheel), the landing's sound and rumble; slower than 0.8333
    //    into it the wheel lands on its spring (the landing motion by the speed; rolling along its forward row
    //    when its sideways speed is below 0.185, the back wheel taking 0.8 of the rider's speed; the contact set),
    //    sideways a bail of kind 4; faster a crash (kind 2); bounced off a moving object, a bail of kind 3;
    //  - none: the wheel stays off the ground, its spring relaxing.
    // A wheel down: an unfinished trick bails; the body's rows from the wheels (as on the ground); landed upright,
    // rolling along the forward row (the landing motion raised by the travel's angle) or back along it, the body
    // is set between the wheels and the ground takes over (state 0), else a bail of kind 5. Both wheels up: the
    // rider's air bodies step (riderTumbleMove); unless they met something hard (kFlagHitHard) the rider flies on
    // by his velocity and the hips' body goes with him; on a lip (8) its landing ray (a skid mark, or the air);
    // else the shadow under the rider and the models' tint by the height.
    void airMove(Rider *rider)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4[2]> points;   // the wheels' points
        ee::Local<zge::Vec4> centre;      // the shadow ray's start
        ee::Local<zge::Vec4[2]> next;     // the points next frame
        ee::Local<zge::Vec4> middle;
        ee::Local<zge::Matrix> rows;      // the body's rows being built
        ee::Local<zge::Matrix> drawn;     // the drawn body's rows, pitched
        ee::Local<zge::Matrix> pitchTurn;
        ee::Local<zge::Vec4> hitPoint;    // a wheel's ground, the lip's, the shadow's
        ee::Local<zge::Vec4> plane;       // a hit's plane
        ee::Local<collision::HitImpact> impact;
        ee::Local<calls::Rumble> rumble;
        ee::Local<uint8_t[4]> tint;       // (in the rumble's place)
        ee::Local<zge::Vec4> origin;      // the lip bone's origin
        ee::Local<zge::Vec4> bonePoint;   // the lip ray's start
        ee::Local<zge::Vec4> lipPlane;
        ee::Local<zge::Vec4> skidNormal;
        ee::Local<zge::ViewSphere> shadowView; // (in the lip's plane's place)
        ee::Local<FaceRef> contactFace;
        ee::Local<uint32_t> contactObject;
        // The wheels not yet on the ground: the original keeps the count in its frame (DM2's renderer reads the 2
        // it leaves there as stale stack).
        ee::Local<uint32_t> wheelsOff;
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &forward = rows->row(2);
        const zge::Vec4 &drawnUp = drawn->row(1);
        const zge::Vec4 &drawnForward = drawn->row(2);
        zge::Vec4 &velocity = *vec4(rider->velocity);

        *contactFace = 0;
        *wheelsOff = 2;
        *drawn = *matrix(rider->right);
        zge::matrixIdentity(pitchTurn);
        zge::matrixRotateX(pitchTurn, pitchTurn, rider->bodyPitch);
        zge::matrixMultiply(drawn, drawn, pitchTurn);

        collision::RayHit *hit = nullptr;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &point = points[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            wheelVelocity = velocity;
            point = *vec4(wheelPoint(rider, wheel));
            point = zge::transformed(*drawn, point);
            point = point + *vec4(rider->drawnPosition);

            // The spring kept between the least and most heights.
            const ee::Float length = pose.springLength;
            const ee::Float rate = pose.springRate;
            ee::Float h = length + rate;
            ee::Float stretch = rate;
            if (h < rider->wheelHeightLeast)
            {
                stretch = rider->wheelHeightLeast - length;
                h = rider->wheelHeightLeast;
                pose.springRate = 0.0f;
            }
            else if (rider->wheelHeightMost < h)
            {
                stretch = rider->wheelHeightMost - length;
                h = rider->wheelHeightMost;
                pose.springRate = 0.0f;
            }
            pose.springLength = h;

            // The ray: by the velocity and down the drawn up row; it touches what it meets.
            const ee::Float reach = h + wheelStat(rider, wheel);
            scratch->x = point.x + wheelVelocity.x - drawnUp.x * reach;
            scratch->y = point.y + wheelVelocity.y - drawnUp.y * reach;
            scratch->z = point.z + wheelVelocity.z - drawnUp.z * reach;
            bool touched = false;
            hit = collision::rayCastTouching(rider, &point.x, &scratch->x, kTouchLanding);
            if (!((*g_moveCallbacks)[physicsState(rider)] == StateCallback(sym::airMove)))
                return; // the touch changed the move
            if (hit != nullptr)
            {
                const FaceRef face = collision::hitFace(hit);
                collision::hitPlane(hit, &plane->x);
                // Ground: not steeper than 70 degrees unless of the world's kinds, not a ramp's face, within 78
                // degrees of the drawn up row.
                bool ground = true;
                if (!(zge::angleAcos(-plane->y) < kAngle70))
                {
                    bool world = false;
                    for (uint32_t kind = kSurfaceWorldFirst; kind <= kSurfaceWorldLast && !world; ++kind)
                        world = collision::faceSurfaceKind(face) == kind;
                    ground = world;
                }
                if (ground && (collision::hitFlags(hit) & collision::kHitFace) && collision::faceSurfaceKind(face) == kSurfaceRamp)
                    ground = false;
                if (ground && !(zge::angleAcos(zge::dot(drawnUp, *plane)) < *g_landWithin))
                    ground = false;
                if (ground)
                {
                    touched = true;
                    zge::Matrix &wheelRows = *matrix(pose.matrix);
                    zge::Vec4 &wheelRight = wheelRows.row(0);
                    zge::Vec4 &wheelUp = wheelRows.row(1);
                    zge::Vec4 &wheelForward = wheelRows.row(2);
                    wheelUp = *plane;
                    scratch->x = wheelVelocity.x + drawnUp.x * -pose.springRate;
                    scratch->y = wheelVelocity.y + drawnUp.y * -pose.springRate;
                    scratch->z = wheelVelocity.z + drawnUp.z * -pose.springRate;
                    impact->hit = ee::Ptr<collision::RayHit>(hit);
                    *vec4(impact->velocity) = *scratch;
                    const ee::Float closing = collision::hitImpact(impact);
                    // Not closing: the touch stands, the wheel neither lands nor flies.
                    if (ee::Float(0.0f) < closing)
                    {
                        if ((collision::hitFlags(hit) & collision::kHitFace) && collision::faceSurfaceKind(face) == kSurfaceBounce)
                        {
                            // A bounce: the velocity into the face taken away, the face's tag word along its normal.
                            subtractScaled(velocity, *plane, zge::dot(velocity, *plane));
                            velocity.x = velocity.x + plane->x * ee::toFloat(int32_t(collision::faceTagWord(face)));
                            velocity.y = velocity.y + plane->y * ee::toFloat(int32_t(collision::faceTagWord(face)));
                            velocity.z = velocity.z + plane->z * ee::toFloat(int32_t(collision::faceTagWord(face)));
                            rider->speed = zge::vec3Length(&velocity.xyz());
                            const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 9u);
                            soundEvent(kSoundBounce, rider->position, static_cast<uint32_t>(code), uint32_t(kSoundVolume), 0u);
                            return;
                        }
                        const bool back = (wheel & 1) != 0;
                        collision::hitPoint(hit, &hitPoint->x);
                        if (!back)
                            landingTest(rider, hit);
                        uint32_t sound = 0;
                        if (collision::faceSound(face) != kNoFaceSound)
                            sound = collision::faceSound(face);
                        const uint32_t surface = sound < 11 ? kLandingSurface[sound] : 0u;
                        if (*g_landSoundFrom < closing)
                        {
                            const int32_t code = surfaceSoundFlags(0u, 0u, 1u, surface);
                            const int32_t loud = ee::toInt(closing * kLandingVolumePerSpeed);
                            soundEvent(kSoundLanding, &hitPoint->x, static_cast<uint32_t>(code),
                                              static_cast<uint32_t>(loud < kSoundVolume ? loud : kSoundVolume), 0u);
                        }
                        if (!hasFlag(rider, kFlagNoPlayer))
                        {
                            const int32_t strength = ee::toInt(closing * *g_landRumblePerSpeed);
                            rumble->motor = kRumbleLargeMotor;
                            rumble->timing = 0;
                            rumble->strength = static_cast<uint8_t>(strength < kMostRumble ? strength : kMostRumble);
                            calls::rumble(uint32_t(rider->player), rumble);
                        }

                        if (!(closing < *g_landHardest))
                        {
                            // Too hard: a crash (off a moving object that bounced the rider, his velocity after it).
                            if (impact->flags & collision::kImpactBounced)
                            {
                                velocity = *vec4(impact->riderVelocity);
                                rider->bailKind = kBailBounced;
                            }
                            else
                            {
                                rider->bailKind = kBailCrashed;
                                addRideDistance(rider, closing);
                                collision::touchObject(rider, hit, kTouchBail);
                            }
                            setPhysicsState(rider, uint32_t(kPhysicsBail));
                            return;
                        }
                        rider->landingKind = landingKindBySpeed(closing);

                        // The spring takes the landing.
                        difference(*scratch, *hitPoint, point);
                        const ee::Float most = rider->wheelHeightMost;
                        ee::Float landed = -zge::dot(*scratch, drawnUp) - wheelStat(rider, wheel);
                        const ee::Float was = pose.springRate;
                        if (most < landed)
                            landed = most;
                        else if (landed < rider->wheelHeightLeast)
                            landed = rider->wheelHeightLeast;
                        const ee::Float half = rider->mass * ee::Float(0.5f);
                        const ee::Float springRate =
                            was + (half * (closing * zge::dot(*plane, drawnUp)) + (most - landed) * rider->springStiffness) / half;
                        const ee::Float damping = springRate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
                        pose.springLength = landed;
                        pose.springRate = springRate * (ee::Float(1.0f) - damping);

                        // The point on the ground at the wheel's height, then by the velocity along the face.
                        const ee::Float lift = landed + wheelStat(rider, wheel);
                        scratch->x = point.x + wheelVelocity.x;
                        scratch->y = point.y + wheelVelocity.y;
                        scratch->z = point.z + wheelVelocity.z;
                        point.x = hitPoint->x + drawnUp.x * lift;
                        point.y = hitPoint->y + drawnUp.y * lift;
                        point.z = hitPoint->z + drawnUp.z * lift;
                        difference(*scratch, *scratch, point);
                        {
                            const ee::Float d = zge::dot(*scratch, *plane);
                            point.x = point.x + (scratch->x - plane->x * d);
                            point.y = point.y + (scratch->y - plane->y * d);
                            point.z = point.z + (scratch->z - plane->z * d);
                        }

                        // The wheel's matrix on the face along the drawn forward row.
                        wheelUp = *plane;
                        wheelForward = drawnForward;
                        wheelRight = zge::cross(wheelUp, wheelForward);
                        wheelRight = zge::unit(wheelRight);
                        wheelForward = zge::cross(wheelRight, wheelUp);
                        if (!(ee::abs(zge::dot(wheelRight, wheelVelocity)) < *g_landStraight))
                        {
                            // Landed sideways: a bail (off a moving object that bounced the rider, as the crash).
                            if (impact->flags & collision::kImpactBounced)
                            {
                                velocity = *vec4(impact->riderVelocity);
                                rider->bailKind = kBailBounced;
                            }
                            else
                            {
                                rider->bailKind = kBailLandedSideways;
                                collision::touchObject(rider, hit, kTouchBail);
                            }
                            setPhysicsState(rider, uint32_t(kPhysicsBail));
                            return;
                        }

                        // Rolling on: the speed along the forward row (the back wheel's mostly the rider's).
                        pose.ground = kWheelRolling;
                        --*wheelsOff;
                        const ee::Float along = zge::dot(wheelForward, wheelVelocity);
                        const bool backwards = along < ee::Float(0.0f);
                        ee::Float s = ee::abs(along);
                        if (back)
                            s = s + (rider->speed - s) * *g_backWheelCarry;
                        if (backwards)
                            s = -s;
                        wheelVelocity.x = wheelForward.x * s;
                        wheelVelocity.y = wheelForward.y * s;
                        wheelVelocity.z = wheelForward.z * s;
                        const ee::Float speed = zge::vec3Length(&wheelVelocity.xyz());
                        const ee::Float turns = speed * *g_spinPerSpeed;
                        pose.speed = speed;
                        const int32_t spin = static_cast<int16_t>(ee::toInt(turns * ee::Float(65536.0f)));
                        pose.spin = spin;
                        if (zge::dot(wheelVelocity, wheelForward) < ee::Float(0.0f))
                            pose.spin = -spin;

                        // The contact: a ramp's face (its kind when it has an edge), or an object.
                        if ((collision::hitFlags(hit) & collision::kHitFace) && face != 0)
                        {
                            const uint32_t kind = collision::faceSurfaceKind(face);
                            if (kind < 4 && kind != 0)
                            {
                                *contactFace = face;
                                const uint32_t rampKind = collision::faceSurfaceKind(face);
                                const bool edge = collision::faceLinkedEdge(face) != 0;
                                setContact(rider, contactFace, edge ? rampKind : 0u);
                            }
                        }
                        else if (collision::hitFlags(hit) & collision::kHitObject)
                        {
                            *contactObject = collision::hitObject(hit);
                            setContact(rider, contactObject, uint32_t(kContactObject));
                        }
                    }
                }
            }

            if (!touched)
            {
                // Off the ground: the point moves by the velocity, the spring relaxes, the wheel spins down, its
                // matrix the body's.
                point.x = point.x + wheelVelocity.x;
                point.y = point.y + wheelVelocity.y;
                point.z = point.z + wheelVelocity.z;
                const ee::Float springRate = stretch + (rider->wheelHeightMost - h) * rider->springStiffness / rider->springMass;
                const ee::Float damping = springRate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
                const ee::Float spin = ee::toFloat(pose.spin);
                pose.ground = kWheelOffGround;
                pose.springRate = springRate * (ee::Float(1.0f) - damping);
                pose.spin = ee::toInt(spin * *g_spinKeep);
                *matrix(pose.matrix) = *matrix(rider->body);
            }
        }

        if (*wheelsOff < 2)
        {
            // A wheel down.
            if (unfinishedTrickBail(rider) != 0)
                return;
            zge::matrixIdentity(rows);
            difference(forward, points[0], points[1]);
            forward = zge::unit(forward);
            if (*wheelsOff == 0)
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
                else
                    up = matrix(rider->body)->row(1);
            }
            const zge::Vec4 groundUp = up;
            between(up, drawnUp, up, *g_upFollow);
            right = zge::cross(up, forward);
            right = zge::unit(right);
            up = zge::cross(forward, right);
            up = zge::unit(up);
            if (zge::angleAcos(zge::dot(up, drawnUp)) < *g_landWithin)
            {
                // The travel along the ground against the forward row.
                *scratch = velocity;
                subtractScaled(*scratch, groundUp, zge::dot(*scratch, groundUp));
                rider->landingTravelAngle = 0;
                clearFlag(rider, kFlagTravelFound);
                const ee::Float travel = zge::vec3Length(&scratch->xyz());
                if (*g_landStraight * ee::Float(0.5f) < travel)
                {
                    *scratch = *scratch * (ee::Float(1.0f) / travel);
                    rider->landingTravelAngle = zge::angleAcos(zge::dot(*scratch, forward));
                    setFlag(rider, kFlagTravelFound);
                }
                // Rolling forwards (within 78 degrees of the forward row), or backwards within 30 degrees: landed.
                const int32_t angle = rider->landingTravelAngle;
                const int32_t off = angle - kAngle90 < 0 ? -(angle - kAngle90) : angle - kAngle90;
                bool landed = kAngle90 - *g_landWithin < off;
                if (landed)
                {
                    if (angle < kAngle90)
                        raiseLandingKind(rider, angle);
                    else
                        landed = kAngle90 - *g_rollWithin < off;
                }
                if (landed)
                {
                    *matrix(rider->body) = *rows;
                    *matrix(rider->right) = *rows;
                    zge::Vec4 &drawnPosition = *vec4(rider->drawnPosition);
                    bodyBetween(rider, drawnPosition, &points[0], *rows, *scratch);
                    *vec4(rider->position) = drawnPosition;

                    // Next frame: the points moved by their velocities.
                    for (uint32_t wheel = 0; wheel < 2; ++wheel)
                    {
                        const zge::Vec4 &wheelVelocity = *vec4(rider->wheelPoses[wheel].velocity);
                        next[wheel].x = points[wheel].x + wheelVelocity.x;
                        next[wheel].y = points[wheel].y + wheelVelocity.y;
                        next[wheel].z = points[wheel].z + wheelVelocity.z;
                    }
                    difference(forward, next[0], next[1]);
                    forward = zge::unit(forward);
                    right = zge::cross(up, forward);
                    right = zge::unit(right);
                    up = zge::cross(forward, right);
                    bodyBetween(rider, *middle, &next[0], *rows, *scratch);

                    // Each wheel's velocity to where its mount goes (in its face's plane when down), the rider's
                    // their mean.
                    zge::Vec4 &force = *vec4(rider->force);
                    force = velocity;
                    velocity = zge::kZero;
                    for (uint32_t wheel = 0; wheel < 2; ++wheel)
                    {
                        WheelPose &pose = rider->wheelPoses[wheel];
                        zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
                        zge::Vec4 &to = next[wheel];
                        to = *vec4(wheelPoint(rider, wheel));
                        to = zge::transformed(*rows, to);
                        to.x = to.x + middle->x;
                        to.y = to.y + middle->y;
                        to.z = to.z + middle->z;
                        difference(wheelVelocity, to, points[wheel]);
                        if (pose.ground != kWheelOffGround)
                        {
                            const zge::Vec4 &wheelUp = matrix(pose.matrix)->row(1);
                            subtractScaled(wheelVelocity, wheelUp, zge::dot(wheelVelocity, wheelUp));
                        }
                        velocity = velocity + wheelVelocity;
                    }
                    velocity = velocity * ee::Float(0.5f);
                    rider->speed = zge::vec3Length(&velocity.xyz());
                    difference(force, velocity, force);
                    if (zge::dot(velocity, matrix(rider->body)->row(2)) < ee::Float(0.0f))
                        setFlag(rider, kFlagFakie);
                    else
                        clearFlag(rider, kFlagFakie);
                    setPhysicsState(rider, uint32_t(kPhysicsGround));
                    return;
                }
            }
            // Down but not upright, or across the forward row: a bail.
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailBody;
            collision::touchObject(rider, hit, kTouchBail);
            return;
        }

        // Both wheels up: the rider's air bodies step; unless they met something hard the rider flies on by his
        // velocity and the hips' body goes with him.
        zge::Vec4 &position = *vec4(rider->position);
        const zge::Vec4 flown = position + velocity;
        const zge::Vec4 flying = velocity;
        riderTumbleMove(rider);
        if (!hasFlag(rider, kFlagHitHard))
        {
            position = flown;
            velocity = flying;
            if (const ee::Ptr<zge::RigidBody> hips = bodyNamed(rider->riderAnimDynamics, *g_hipsName))
            {
                hips->position = position;
                hips->velocity = velocity;
            }
        }

        if (physicsState(rider) == kPhysicsLipTrick)
        {
            // On a lip: a ray from the bike's bone, below by wheel 1's height stat, out to the side (0.375 of the
            // right row, the other way with kFlagMirrorSide): a landing there leaves a skid mark; else the air.
            *origin = zge::kZero;
            const ee::Ptr<zge::SceneObject> bike(rider->bikeModel);
            zge::objectBonePoint(bike, zge::findModelBone(bike, *g_lipBoneName), origin, bonePoint, 0u);
            subtractScaled(*bonePoint, matrix(rider->body)->row(1), rider->backWheelHeight);
            *scratch = *vec4(rider->right) * kLipReach;
            if (hasFlag(rider, kFlagMirrorSide))
                zge::vec3Negate(&scratch->xyz(), &scratch->xyz());
            *scratch = *scratch + *bonePoint;
            collision::RayHit *const lipHit = collision::rayCastTouching(rider, &bonePoint->x, &scratch->x, kTouchLip);
            if (lipHit != nullptr && landingTest(rider, lipHit) != 0)
            {
                collision::hitPoint(lipHit, &hitPoint->x);
                collision::hitPlane(lipHit, &lipPlane->x);
                *scratch = velocity;
                *scratch = *hitPoint - *scratch;
                skidNormal->x = lipPlane->x;
                skidNormal->y = lipPlane->y;
                skidNormal->z = lipPlane->z;
                spawnSkidMark(hitPoint, scratch, skidNormal);
                return;
            }
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            return;
        }

        // The shadow: a ray 150 down from the body's centre; its height tints the models (darker nearer, 16 .. 255
        // of 128).
        {
            const ee::Float x = rider->position[0] + rider->rootOffset[0];
            const ee::Float y = rider->position[1] + rider->rootOffset[1];
            const ee::Float z = rider->position[2] + rider->rootOffset[2];
            scratch->x = x;
            centre->y = y;
            scratch->y = kShadowRay;
            scratch->z = z;
            centre->x = x;
            centre->z = z;
        }
        const collision::RayHit *const ground = collision::rayCast(&centre->x, &scratch->x);
        if (ground == nullptr || landingTest(rider, ground) == 0)
        {
            rider->flags[3] &= static_cast<uint8_t>(~kHopFlags);
            return;
        }
        collision::hitPoint(ground, &hitPoint->x);
        rider->groundHeight = hitPoint->y - centre->y;
        shadowView->centre = *hitPoint;
        shadowView->radius = kShadowViewRadius;
        zge::setSceneShadowViewTest(zge::sceneOf(rider->messenger), shadowView);
        zge::setSceneShadowViewTest(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->bikeModel)), shadowView);
        const int32_t shade = ee::toInt((kShadowHeight - rider->groundHeight) * *g_shadowShade);
        const uint8_t level = static_cast<uint8_t>(shade >= kShadowMost + 1 ? kShadowMost : shade < kShadowLeast ? kShadowLeast : (shade & 0xFF));
        tint[2] = level;
        tint[3] = 0xFF;
        tint[0] = level;
        tint[1] = level;
        zge::setSceneRootColour(zge::sceneOf(rider->messenger), tint);
        zge::setSceneRootColour(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->bikeModel)), tint);
    }
    READABLE_FUNCTION(airMove, Verified);
}
