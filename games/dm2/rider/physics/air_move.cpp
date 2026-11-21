// DM2's air move (physics states 1 and 8's move callback, the move table's
// second and ninth): the wheels on their springs reaching for the ground (a
// landing, a bounce off a kind 5 face, a crash), the body's probes against
// walls, the lip's landing ray, the shadow's tint by the height.
//
// Counterparts: DM2 GameCube 0x800800F8;
// Xbox 0x059D10; BMX XXX PS2 airMove (low).

#include "../actions/actions.h"
#include "physics.h"
#include "../../fx/spawns.h"

#include "../collision/collision.h"

#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include "sym/dm2/rider/physics/air_move.cpp.h"

namespace game::dm2::rider::physics
{
    namespace
    {
        using collision::FaceRef;

        constexpr uint32_t kNoFaceSound = 0xFFFFu;
        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kHitObjectFlag = 0x2u;

        inline ee::Global<int16_t, sym::g_landWithin> g_landWithin;     // 78 degrees: a face this near the body's up row is ground
        inline ee::Global<int16_t, sym::g_rollWithin> g_rollWithin;     // 30 degrees
        inline ee::Global<ee::Float, sym::g_landSoundFrom> g_landSoundFrom; // 0.1
        inline ee::Global<ee::Float, sym::g_landHardest> g_landHardest;  // 0.5258: faster into the ground crashes
        inline ee::Global<ee::Float, sym::g_landHardShare> g_landHardShare; // 0.3 of it: a hard landing (kFlagHardLanding)
        inline ee::Global<ee::Float, sym::g_landStraight> g_landStraight; // 0.185: a wheel's sideways speed below this lands
        inline ee::Global<ee::Float, sym::g_backWheelCarry> g_backWheelCarry; // 0.8 of the rider's speed onto the back wheel
        inline ee::Global<ee::Float, sym::g_spinPerSpeed> g_spinPerSpeed; // 2 / pi
        inline ee::Global<ee::Float, sym::g_thrownFrom> g_thrownFrom;   // 1e-5: a moving object this fast throws the rider back
        inline ee::Global<ee::Float, sym::g_thrownLift> g_thrownLift;   // -0.05
        inline ee::Global<ee::Float, sym::g_crashThrownFrom> g_crashThrownFrom;
        inline ee::Global<ee::Float, sym::g_spinKeep> g_spinKeep;     // 0.9967
        inline ee::Global<ee::Float, sym::g_wallThrownFrom> g_wallThrownFrom;
        inline ee::Global<ee::Float, sym::g_wallOut> g_wallOut;      // 0.1 off a wall
        inline ee::Global<ee::Float, sym::g_wallThrownLift> g_wallThrownLift;
        inline ee::Global<ee::Float, sym::g_crashThrownLift> g_crashThrownLift;
        inline ee::Global<ee::Float, sym::g_shadowShade> g_shadowShade;  // 8 / 3 a unit of height
        inline ee::Global<ee::Float, sym::g_wallBailSpeed> g_wallBailSpeed; // 0.29
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_upFollow;     // 0.085
        // The body's eight probes: the corners of a box 1 x 0.8 x 0.8 about
        // the drawn body's centre, in its (pitched) rows.
        inline ee::Global<ee::Float, sym::g_railSideReach> g_probeX;       // 0.5
        inline ee::Global<ee::Float, sym::g_probeY> g_probeY;       // 0.4
        inline ee::Global<ee::Float, sym::g_probeZ> g_probeZ;       // 0.4
        inline ee::Global<int32_t, sym::g_alignFrames> g_alignFrames;    // 32
        inline ee::Global<int16_t, sym::g_alignFrames> g_alignFramesShort;
        inline ee::Global<uint32_t, sym::g_backWheelBoneHash> g_lipBoneName;   // the bike bone the lip's ray starts from (a hash; 0 in the ELF)
        inline ee::Global<zge::Vec4, sym::down> g_worldUp; // (0, -1, 0, 0)
        constexpr ee::Float kShadowRay = 150.0f;
        constexpr ee::Float kShadowHeight = 12.0f;
        // The shadows' view test's volume (setSceneShadowViewTest copies its
        // 0x20 bytes; the view test, partInView, reads its centre and radius):
        // the ground point under the rider and a radius of 0.5. Its radius
        // sits in the skid normal's slot, the quadword after groundPoint in
        // the original's frame.
        constexpr ee::Float kShadowViewRadius = 0.5f;
        constexpr int32_t kShadowLeast = 8;

        // The landing's sound by the face's sound.
        constexpr uint8_t kLandingSurface[11] = {0, 7, 2, 7, 5, 4, 0, 0, 0, 0, 4};

        constexpr int32_t kAngle30 = 5461;
        constexpr int32_t kAngle50 = 9102;
        constexpr int32_t kAngle60 = 10922;
        constexpr int32_t kAngle90 = 0x4000;
        constexpr uint32_t kSurfaceBounce = 5;
        constexpr int32_t kSoundLanding = 20;
        constexpr int32_t kSoundBounce = 18;
        constexpr int32_t kSoundVolume = 127;
        constexpr ee::Float kLandingVolumePerSpeed = 400.0f;
        constexpr ee::Float kWallPush = 1.25f;
        constexpr uint8_t kBailLandedSideways = 4;
        constexpr uint8_t kBailThrown = 3;
        constexpr uint8_t kBailCrashed = 2;
        constexpr uint8_t kBailCrashedSteep = 1;
        constexpr uint8_t kBailBody = 5;

        // Rider::flags.
        constexpr uint32_t kFlagAirSpin = 0x4000u;
        constexpr uint32_t kFlagHardLanding = 0x80000u;
        constexpr uint32_t kFlagUpright = 0x2000000u;
        constexpr uint32_t kFlagRolling = 0x40000000u; // the travel along the ground found (Rider::landingTravelAngle its angle)
        constexpr uint32_t kFlagsHop = 0x30000u;

        const ee::Float *wheelPoint(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelPoint : rider->backWheelPoint; }
        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        // The engine's calls the move makes as guest calls: the scales after
        // them carry what they left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void negateByCall(ee::Float *v) { ee::game(sym::vec3Negate, v, v); }
        inline ee::Float lengthByCall(const zge::Vec4 &v) { return ee::game<ee::Float>(sym::vec3Length, &v.x); }
        // A point in a model's bone's space into the world (the fifth word, 0,
        // the space the engine's callers pass).
        inline void bonePointByCall(uint32_t model, ee::Ptr<zge::SceneBone> bone, const zge::Vec4 &point, zge::Vec4 &out)
        {
            ee::game(sym::objectBonePoint, model, bone, &point.x, &out.x, 0u);
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
        // The body's position halfway between where the wheels' mounts put
        // it, through `rows`, from the two points into `out`.
        void bodyBetween(Rider *rider, zge::Vec4 &out, const zge::Vec4 *points, const zge::Matrix &rows, zge::Vec4 &scratch)
        {
            scratch = *vec4(rider->frontWheelPoint);
            engine::transformPoint(&scratch.x, &rows, &scratch.x);
            {
                const ee::Float dy = points[0].y - scratch.y;
                out.x = points[0].x - scratch.x;
                out.y = dy;
                out.z = points[0].z - scratch.z;
            }
            scratch = *vec4(rider->backWheelPoint);
            engine::transformPoint(&scratch.x, &rows, &scratch.x);
            const ee::Float x = points[1].x - scratch.x;
            const ee::Float y = points[1].y - scratch.y;
            const ee::Float z = points[1].z - scratch.z;
            scratch.x = x;
            scratch.y = y;
            scratch.z = z;
            between(out, out, scratch, ee::Float(0.5f));
        }
        // Thrown back by a moving object (faster than `from` towards the
        // rider): the velocity turned round at the object's speed, lifted.
        void thrownBack(Rider *rider, ee::Float approach, ee::Float from, ee::Float lift)
        {
            if (ee::abs(approach) < from)
                return;
            zge::Vec4 &velocity = *vec4(rider->velocity);
            negateByCall(rider->velocity);
            velocity = zge::unit(velocity);
            velocity = zge::scaledCarrying(velocity, approach, zge::scaleLeftover(zge::LeftBy::lastCall));
            rider->bailKind = kBailThrown;
            velocity.y = velocity.y + lift;
        }
    }

    // States 1 and 8's move. The drawn body's rows pitched by bodyPitch;
    // per wheel (0 the front): its velocity the rider's, its point the
    // mount's, its spring kept between the least and most heights; a ray
    // from the point by the velocity and down the up row by the spring
    // and the wheel's stat finds the ground:
    //  - a face within 78 degrees of the up row that the object owning it
    //    accepts, met closing: a kind 5 face bounces the rider (its tag
    //    word along its normal); else the landing test (the front wheel),
    //    the landing's sound; slower than 0.5258 into it the wheel lands on
    //    its spring (rolling along its forward row when its sideways speed
    //    is below 0.185, the back wheel taking 0.8 of the rider's speed;
    //    the contact set), sideways a bail of kind 4; faster a crash
    //    (kind 2); a moving object fast enough throws the rider (kind 3);
    //  - none: the wheel stays off the ground, its spring relaxing.
    // A wheel down: an unfinished trick bails; the body's rows from the
    // wheels (as on the ground); landed upright, rolling along the forward
    // row (or back along it) the body is set between the wheels and the
    // ground takes over (state 0), else a bail of kind 5. Both wheels up:
    // eight probes about the body against walls (steeper than 60 degrees:
    // pushed off, turned along the wall; else bails), the position moved;
    // on a lip (8) its landing ray (a skid mark, or the air); else the
    // shadow under the rider and its tint by the height. GameCube 0x800800F8.
    //
    // Only the move table calls it, as void. Every scale carries what the
    // last call left, as airGravity's does.
    void airMove(Rider *rider)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4[2]> points;   // the wheels' points
        ee::Local<zge::Vec4> centre;      // the probes' centre
        ee::Local<zge::Vec4[2]> next;     // the points next frame
        ee::Local<zge::Vec4> middle;
        ee::Local<zge::Matrix> rows;      // the body's rows being built
        ee::Local<zge::Matrix> drawn;     // the drawn body's rows, pitched
        ee::Local<zge::Matrix> pitchTurn;
        ee::Local<zge::Vec4> normal;      // a hit's normal (the plane's copy)
        ee::Local<zge::Vec4> plane;       // a hit's plane, later the ground's up row
        ee::Local<zge::Vec4> hitPoint;    // a wheel's ground, a probe's direction
        ee::Local<zge::Vec4> probePlane;  // (the lip ray's bone origin too)
        ee::Local<zge::Vec4> probePoint;
        ee::Local<zge::Vec4> groundPlane;
        ee::Local<zge::Vec4> groundPoint;
        ee::Local<zge::Vec4> skidNormal;
        ee::Local<uint8_t[4]> tint;
        ee::Local<ee::Float> approach;      // a moving object's speed towards the rider (hitClosing)
        ee::Local<FaceRef> contactFace;
        ee::Local<uint32_t> contactObject;
        ee::Local<ee::Float> probeApproach;
        // The wheels not yet on the ground: the original keeps the count in
        // its frame, and the 2 it leaves there is read later as stale stack
        // by the renderer, which copies it into vertices' z lanes (rider12).
        ee::Local<uint32_t> wheelsOff;
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &forward = rows->row(2);
        const zge::Vec4 &drawnUp = drawn->row(1);
        const zge::Vec4 &drawnForward = drawn->row(2);
        zge::Vec4 &velocity = *vec4(rider->velocity);

        *contactFace = 0;
        engine::copyMatrix(drawn, rider->right);
        *wheelsOff = 2;
        zge::matrixIdentity(pitchTurn);
        zge::matrixRotateX(pitchTurn, pitchTurn, rider->bodyPitch);
        engine::matrixProduct(drawn, drawn, pitchTurn);

        const collision::RayHit *hit = nullptr;
        for (uint32_t wheel = 0; wheel < 2; ++wheel)
        {
            WheelPose &pose = rider->wheelPoses[wheel];
            zge::Vec4 &point = points[wheel];
            zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
            wheelVelocity = velocity;
            point = *vec4(wheelPoint(rider, wheel));
            engine::transformPoint(&point.x, drawn, &point.x);
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

            // The ray: by the velocity and down the drawn up row.
            const ee::Float reach = h + wheelStat(rider, wheel);
            const ee::Float downY = drawnUp.y * reach;
            const ee::Float downZ = drawnUp.z * reach;
            scratch->x = point.x + wheelVelocity.x - drawnUp.x * reach;
            scratch->y = point.y + wheelVelocity.y - downY;
            scratch->z = point.z + wheelVelocity.z - downZ;
            uint32_t touched = 0;
            hit = collision::rayCast(&point.x, &scratch->x);
            if (hit != nullptr)
            {
                const FaceRef face = collision::hitFace(hit);
                collision::hitPlane(hit, &plane->x);
                *normal = *plane;
                if (!(plane->y == ee::Float(0.0f)) && zge::angleAcos(zge::dot(drawnUp, *normal)) < *g_landWithin)
                    touched = collision::touchObject(rider, hit);
                // (touchObject's 0: off the ground, below.)
                if (touched != 0)
                {
                    *approach = 0.0f;
                    zge::Matrix &wheelRows = *matrix(pose.matrix);
                    zge::Vec4 &wheelRight = wheelRows.row(0);
                    zge::Vec4 &wheelUp = wheelRows.row(1);
                    zge::Vec4 &wheelForward = wheelRows.row(2);
                    wheelUp = *normal;
                    scratch->x = wheelVelocity.x + drawnUp.x * -pose.springRate;
                    scratch->y = wheelVelocity.y + drawnUp.y * -pose.springRate;
                    scratch->z = wheelVelocity.z + drawnUp.z * -pose.springRate;
                    const ee::Float closing = collision::hitClosing(&scratch->x, hit, approach);
                    // Not closing: the touch stands, the wheel neither lands
                    // nor flies.
                    if (ee::Float(0.0f) < closing)
                    {
                        touched = 1;
                        if ((collision::hitFlags(hit) & kHitFaceFlag) != 0 && collision::faceSurfaceKind(face) == kSurfaceBounce)
                        {
                            // A bounce: the velocity into the face taken
                            // away, the face's tag word along its normal.
                            const ee::Float into = zge::dot(velocity, *normal);
                            const ee::Float vy = velocity.y - normal->y * into;
                            const ee::Float vz = velocity.z - normal->z * into;
                            velocity.x = velocity.x - normal->x * into;
                            velocity.y = vy;
                            velocity.z = vz;
                            velocity.x = velocity.x + normal->x * ee::toFloat(collision::faceTagWord(face));
                            velocity.y = velocity.y + normal->y * ee::toFloat(collision::faceTagWord(face));
                            velocity.z = velocity.z + normal->z * ee::toFloat(collision::faceTagWord(face));
                            rider->speed = zge::vec3Length(&velocity.xyz());
                            const int32_t code = surfaceSoundFlags(0u, 0u, 1u, 9u);
                            soundEvent(kSoundBounce, rider->position, code, kSoundVolume);
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
                            soundEvent(kSoundLanding, &hitPoint->x, code, loud < kSoundVolume ? loud : kSoundVolume);
                        }

                        const ee::Float hardest = *g_landHardest;
                        if (!(closing < hardest))
                        {
                            // Too hard: a crash.
                            rider->bailKind = kBailCrashed;
                            thrownBack(rider, *approach, *g_crashThrownFrom, *g_crashThrownLift);
                            setPhysicsState(rider, uint32_t(kPhysicsBail));
                            addRideDistance(rider, closing);
                            collision::touchObject(rider, hit);
                            return;
                        }
                        if (hardest * *g_landHardShare < closing)
                            rider->flags |= kFlagHardLanding;
                        else
                            rider->flags &= ~kFlagHardLanding;

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
                            was + (half * (closing * zge::dot(*normal, drawnUp)) + (most - landed) * rider->springStiffness) / half;
                        const ee::Float damping = springRate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
                        pose.springLength = landed;
                        pose.springRate = springRate * (ee::Float(1.0f) - damping);

                        // The point on the ground at the wheel's height,
                        // then by the velocity along the face.
                        const ee::Float lift = landed + wheelStat(rider, wheel);
                        scratch->x = point.x + wheelVelocity.x;
                        const ee::Float px = hitPoint->x + drawnUp.x * lift;
                        scratch->y = point.y + wheelVelocity.y;
                        scratch->z = point.z + wheelVelocity.z;
                        point.x = px;
                        point.y = hitPoint->y + drawnUp.y * lift;
                        point.z = hitPoint->z + drawnUp.z * lift;
                        {
                            const ee::Float sy = scratch->y;
                            const ee::Float sx = scratch->x - px;
                            const ee::Float sz = scratch->z;
                            scratch->x = sx;
                            scratch->y = sy - point.y;
                            scratch->z = sz - point.z;
                            const ee::Float d = zge::dot(*scratch, *normal);
                            const ee::Float py = point.y;
                            const ee::Float pz = point.z;
                            point.x = point.x + (sx - normal->x * d);
                            point.y = py + (scratch->y - normal->y * d);
                            point.z = pz + (scratch->z - normal->z * d);
                        }

                        // The wheel's matrix on the face along the drawn
                        // forward row.
                        wheelUp = *normal;
                        wheelForward = drawnForward;
                        wheelRight = zge::cross(wheelUp, wheelForward);
                        wheelRight = zge::unit(wheelRight);
                        wheelForward = zge::cross(wheelRight, wheelUp);
                        if (!(ee::abs(zge::dot(wheelRight, wheelVelocity)) < *g_landStraight))
                        {
                            // Landed sideways: a bail.
                            rider->bailKind = kBailLandedSideways;
                            thrownBack(rider, *approach, *g_thrownFrom, *g_thrownLift);
                            setPhysicsState(rider, uint32_t(kPhysicsBail));
                            collision::touchObject(rider, hit);
                            return;
                        }

                        // Rolling on: the speed along the forward row
                        // (the back wheel's mostly the rider's).
                        pose.ground = kWheelRolling;
                        --*wheelsOff;
                        const ee::Float along = zge::dot(wheelForward, wheelVelocity);
                        ee::Float s = ee::abs(along);
                        if (back)
                            s = s + (rider->speed - s) * *g_backWheelCarry;
                        if (along < ee::Float(0.0f))
                            s = -s;
                        {
                            const ee::Float y = wheelForward.y * s;
                            const ee::Float z = wheelForward.z * s;
                            wheelVelocity.x = wheelForward.x * s;
                            wheelVelocity.y = y;
                            wheelVelocity.z = z;
                        }
                        const ee::Float speed = zge::vec3Length(&wheelVelocity.xyz());
                        const ee::Float turns = speed * *g_spinPerSpeed;
                        pose.speed = speed;
                        const int32_t spin = static_cast<int16_t>(ee::toInt(turns * ee::Float(65536.0f)));
                        pose.spin = spin;
                        if (zge::dot(wheelVelocity, wheelForward) < ee::Float(0.0f))
                            pose.spin = -spin;

                        // The contact: a ramp's face (its kind when it
                        // has an edge), or an object.
                        if ((collision::hitFlags(hit) & kHitFaceFlag) != 0 && face != 0)
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
                        else if ((collision::hitFlags(hit) & kHitObjectFlag) != 0)
                        {
                            *contactObject = collision::hitObject(hit);
                            setContact(rider, contactObject, uint32_t(kContactObject));
                        }
                    }
                }
            }

            if (touched == 0)
            {
                // Off the ground: the point moves by the velocity, the
                // spring relaxes, the wheel spins down, its matrix the body's.
                const ee::Float y = point.y;
                const ee::Float pull = (rider->wheelHeightMost - h) * rider->springStiffness;
                const ee::Float z = point.z;
                point.x = point.x + wheelVelocity.x;
                point.y = y + wheelVelocity.y;
                const ee::Float springRate = stretch + pull / rider->springMass;
                point.z = z + wheelVelocity.z;
                const ee::Float damping = springRate < ee::Float(0.0f) ? rider->springDamping[0] : rider->springDamping[1];
                const ee::Float spin = ee::toFloat(pose.spin);
                pose.ground = kWheelOffGround;
                pose.springRate = springRate * (ee::Float(1.0f) - damping);
                pose.spin = ee::toInt(spin * *g_spinKeep);
                engine::copyMatrix(pose.matrix, rider->body);
            }
        }

        if (*wheelsOff < 2)
        {
            // A wheel down.
            if (unfinishedTrickBail(rider) != 0)
                return;
            rows->row(3) = zge::kZero;
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
                        rider->flags |= kFlagUpright;
                    else
                        rider->flags &= ~kFlagUpright;
                }
                else
                    up = matrix(rider->body)->row(1);
            }
            zge::Vec4 &groundUp = *plane;
            groundUp = up;
            between(up, drawnUp, up, *g_upFollow);
            right = zge::cross(up, forward);
            right = zge::unit(right);
            up = zge::cross(forward, right);
            up = zge::unit(up);
            if (zge::angleAcos(zge::dot(up, drawnUp)) < *g_landWithin)
            {
                // The travel along the ground against the forward row.
                *scratch = velocity;
                const ee::Float d = zge::dot(*scratch, groundUp);
                const ee::Float sx = scratch->x - groundUp.x * d;
                const ee::Float sy = scratch->y - groundUp.y * d;
                const uint32_t flags = rider->flags;
                const ee::Float sz = scratch->z - groundUp.z * d;
                rider->landingTravelAngle = 0;
                scratch->x = sx;
                scratch->y = sy;
                scratch->z = sz;
                rider->flags = flags & ~kFlagRolling;
                const ee::Float travel = lengthByCall(*scratch);
                if (*g_landStraight * ee::Float(0.5f) < travel)
                {
                    *scratch = zge::scaledCarrying(*scratch, ee::Float(1.0f) / travel, zge::scaleLeftover(zge::LeftBy::lastCall));
                    const int32_t angle = zge::angleAcos(zge::dot(*scratch, forward));
                    const uint32_t f = rider->flags;
                    rider->landingTravelAngle = static_cast<int16_t>(angle);
                    rider->flags = f | kFlagRolling;
                }
                const int32_t angle = rider->landingTravelAngle;
                const ee::Float off = ee::toFloat(angle - kAngle90);
                // Rolling forwards (within 78 degrees of the forward row),
                // or backwards within 30 degrees: landed.
                if (ee::toFloat(kAngle90 - *g_landWithin) < ee::abs(off) &&
                    (angle < kAngle90 || ee::toFloat(kAngle90 - *g_rollWithin) < ee::abs(off)))
                {
                    engine::copyMatrix(rider->body, rows);
                    engine::copyMatrix(rider->right, rows);
                    zge::Vec4 &drawnPosition = *vec4(rider->drawnPosition);
                    bodyBetween(rider, drawnPosition, &points[0], *rows, *scratch);
                    *vec4(rider->position) = drawnPosition;

                    // Next frame: the points moved by their velocities.
                    for (uint32_t wheel = 0; wheel < 2; ++wheel)
                    {
                        const zge::Vec4 &point = points[wheel];
                        const zge::Vec4 &wheelVelocity = *vec4(rider->wheelPoses[wheel].velocity);
                        zge::Vec4 &to = next[wheel];
                        const ee::Float y = wheelVelocity.y;
                        const ee::Float x = point.x + wheelVelocity.x;
                        const ee::Float z = wheelVelocity.z;
                        to.x = x;
                        to.y = point.y + y;
                        to.z = point.z + z;
                    }
                    difference(forward, next[0], next[1]);
                    forward = zge::unit(forward);
                    right = zge::cross(up, forward);
                    right = zge::unit(right);
                    up = zge::cross(forward, right);
                    bodyBetween(rider, *middle, &next[0], *rows, *scratch);

                    // Each wheel's velocity to where its mount goes (in
                    // its face's plane when down), the rider's their mean.
                    zge::Vec4 &force = *vec4(rider->force);
                    force = velocity;
                    velocity = zge::kZero;
                    for (uint32_t wheel = 0; wheel < 2; ++wheel)
                    {
                        WheelPose &pose = rider->wheelPoses[wheel];
                        zge::Vec4 &wheelVelocity = *vec4(pose.velocity);
                        zge::Vec4 &to = next[wheel];
                        const zge::Vec4 &point = points[wheel];
                        to = *vec4(wheelPoint(rider, wheel));
                        engine::transformPoint(&to.x, rows, &to.x);
                        const uint8_t state = pose.ground;
                        const ee::Float x = to.x + middle->x;
                        const ee::Float y0 = to.y;
                        const ee::Float z0 = to.z;
                        to.x = x;
                        const ee::Float y = y0 + middle->y;
                        to.y = y;
                        const ee::Float z = z0 + middle->z;
                        to.z = z;
                        const ee::Float vx = x - point.x;
                        const ee::Float vy = y - point.y;
                        const ee::Float vz = z - point.z;
                        wheelVelocity.x = vx;
                        wheelVelocity.y = vy;
                        wheelVelocity.z = vz;
                        if (state != kWheelOffGround)
                        {
                            const zge::Vec4 &wheelUp = matrix(pose.matrix)->row(1);
                            const ee::Float d = zge::dot(wheelVelocity, wheelUp);
                            const ee::Float ny = vy - wheelUp.y * d;
                            const ee::Float nz = vz - wheelUp.z * d;
                            wheelVelocity.x = vx - wheelUp.x * d;
                            wheelVelocity.y = ny;
                            wheelVelocity.z = nz;
                        }
                        velocity = velocity + wheelVelocity;
                    }
                    velocity = zge::scaledCarrying(velocity, ee::Float(0.5f), zge::scaleLeftover(zge::LeftBy::lastCall));
                    rider->speed = zge::vec3Length(&velocity.xyz());
                    difference(force, velocity, force);
                    if (zge::dot(velocity, matrix(rider->body)->row(2)) < ee::Float(0.0f))
                        rider->flags |= kFlagFakie;
                    else
                        rider->flags &= ~kFlagFakie;
                    setPhysicsState(rider, uint32_t(kPhysicsGround));
                    return;
                }
            }
            // Down but not upright, or across the forward row: a bail.
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            rider->bailKind = kBailBody;
            collision::touchObject(rider, hit);
            return;
        }

        // Both wheels up: the body's probes, from its centre along the drawn
        // forward row.
        {
            const ee::Float cx = rider->position[0] + rider->rootOffset[0];
            const ee::Float cy = rider->position[1] + rider->rootOffset[1];
            const ee::Float cz = rider->position[2] + rider->rootOffset[2];
            const ee::Float dy = rider->drawnPosition[1] - cy;
            const ee::Float dz = rider->drawnPosition[2] - cz;
            scratch->x = rider->drawnPosition[0] - cx;
            scratch->y = dy;
            scratch->z = dz;
            const ee::Float along = zge::dot(*scratch, drawnForward);
            const ee::Float fy = drawnForward.y * along;
            const ee::Float fz = drawnForward.z * along;
            centre->x = cx + drawnForward.x * along;
            centre->y = cy + fy;
            centre->z = cz + fz;
        }
        const ee::Float thrownFrom = *g_wallThrownFrom;
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            scratch->x = *g_probeX;
            scratch->y = *g_probeY;
            scratch->z = *g_probeZ;
            if ((corner & 1) != 0)
                scratch->z = -scratch->z;
            if ((corner & 2) != 0)
                scratch->x = -scratch->x;
            if ((corner & 4) != 0)
                scratch->y = -scratch->y;
            engine::transformPoint(&scratch->x, drawn, &scratch->x);
            *hitPoint = *scratch; // the probe's direction kept
            if (!(ee::Float(0.0f) <= zge::dot(*scratch, velocity)))
                continue;
            *scratch = *scratch + *centre;
            *scratch = *scratch + velocity;
            const collision::RayHit *const probe = collision::rayCast(&centre->x, &scratch->x);
            if (probe == nullptr)
                continue;
            *probeApproach = 0.0f;
            collision::hitFace(probe); // (asked for, not used)
            collision::hitPlane(probe, &probePlane->x);
            *normal = *probePlane;
            const ee::Float closing = collision::hitClosing(rider->velocity, probe, probeApproach);
            if (!(ee::Float(0.0f) < closing) || collision::touchObject(rider, probe) == 0)
                continue;
            collision::hitPoint(probe, &probePoint->x);
            const ee::Float normalY = normal->y;
            if (!(closing < *g_wallBailSpeed))
            {
                // Hard into it: a crash (kind 1 off a wall, 2 off ground).
                rider->bailKind = zge::angleAcos(-normalY) < kAngle60 ? kBailCrashed : kBailCrashedSteep;
                thrownBack(rider, *probeApproach, thrownFrom, *g_crashThrownLift);
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                addRideDistance(rider, closing);
                collision::touchObject(rider, probe);
                return;
            }
            if (zge::angleAcos(-normalY) < kAngle60 + 1)
            {
                // Not a wall: a bail of kind 5.
                rider->bailKind = kBailBody;
                thrownBack(rider, *probeApproach, thrownFrom, *g_wallThrownLift);
                setPhysicsState(rider, uint32_t(kPhysicsBail));
                collision::touchObject(rider, probe);
                return;
            }

            // A wall: the rider pushed 0.1 off it, pushed back at 1.25 times
            // the speed into it.
            {
                const ee::Float dy = probePoint->y - centre->y;
                scratch->x = probePoint->x - centre->x;
                scratch->y = dy;
                scratch->z = probePoint->z - centre->z;
                const ee::Float out = *g_wallOut - zge::dot(*hitPoint, *normal);
                const ee::Float push = out + zge::dot(*scratch, *normal);
                const ee::Float nx = normal->x, ny = normal->y, nz = normal->z;
                const ee::Float pz = nz * push;
                const ee::Float px = nx * push;
                const ee::Float py = ny * push;
                const ee::Float x = rider->position[0] + px;
                const ee::Float y = rider->position[1] + py;
                rider->position[2] = rider->position[2] + pz;
                rider->position[0] = x;
                rider->position[1] = y;
                const ee::Float dy2 = rider->drawnPosition[1] + py;
                const ee::Float dz2 = rider->drawnPosition[2] + pz;
                rider->drawnPosition[0] = rider->drawnPosition[0] + px;
                rider->drawnPosition[1] = dy2;
                rider->drawnPosition[2] = dz2;
                const ee::Float back = closing * kWallPush;
                rider->force[2] = nz * back;
                rider->force[0] = nx * back;
                rider->force[1] = ny * back;
            }
            velocity = velocity + *vec4(rider->force);
            rider->speed = zge::vec3Length(&velocity.xyz());
            if (zge::angleAcos(-rider->up[1]) >= kAngle30)
                break; // tilted: the move goes on as it is
            // Upright: the rows turned along the wall unless already
            // within 30 degrees of it.
            const ee::Float facing = zge::dot(*vec4(rider->forward), *normal);
            if (zge::angleAcos(ee::abs(facing)) < kAngle30)
                break;
            forward = *vec4(rider->forward);
            {
                const ee::Float s = facing * kWallPush;
                const ee::Float fy = forward.y - normal->y * s;
                const ee::Float fz = forward.z - normal->z * s;
                forward.x = forward.x - normal->x * s;
                forward.y = fy;
                forward.z = fz;
            }
            up = *vec4(rider->up);
            if (rider->snapFrames != 0 && (rider->flags & kFlagAirSpin) == 0)
                up = *g_worldUp;
            right = zge::cross(up, forward);
            right = zge::unit(right);
            forward = zge::cross(right, up);
            airRotationTrack(rider->right, &rows->m[0][0], *g_alignFrames, rider->alignAngles);
            engine::copyMatrix(rider->alignFrom, rider->right);
            const uint32_t flags = rider->flags & ~kFlagAirSpin;
            rider->snapFrames = *g_alignFramesShort;
            rider->flags = flags;
            break;
        }
        *vec4(rider->position) = *vec4(rider->position) + velocity;

        if (physicsState(rider) == kPhysicsLipTrick)
        {
            // On a lip: a ray from the bike's bone, below by wheel 1's
            // height stat, out to the side (0.75 of the right row, the
            // other way with kFlagMirrorSide): a landing there leaves a skid
            // mark; else the air.
            zge::Vec4 &origin = *probePlane;
            origin = zge::kZero;
            bonePointByCall(rider->bikeModel, zge::findModelBone(ee::Ptr<zge::SceneObject>(rider->bikeModel), *g_lipBoneName), origin, *probePoint);
            const ee::Float lift = rider->backWheelHeight;
            const zge::Vec4 &bodyUp = matrix(rider->body)->row(1);
            const ee::Float y = probePoint->y - bodyUp.y * lift;
            const ee::Float z = probePoint->z - bodyUp.z * lift;
            probePoint->x = probePoint->x - bodyUp.x * lift;
            probePoint->y = y;
            probePoint->z = z;
            *scratch = zge::scaledCarrying(*vec4(rider->right), ee::Float(-0.75f), zge::scaleLeftover(zge::LeftBy::lastCall));
            if ((rider->flags & kFlagMirrorSide) != 0)
                zge::vec3Negate(&scratch->xyz(), &scratch->xyz());
            *scratch = *scratch + *probePoint;
            const collision::RayHit *const lipHit = collision::rayCast(&probePoint->x, &scratch->x);
            if (lipHit != nullptr && landingTest(rider, lipHit) != 0)
            {
                collision::hitPoint(lipHit, &groundPoint->x);
                collision::hitPlane(lipHit, &groundPlane->x);
                *scratch = velocity;
                *scratch = *groundPoint - *scratch;
                skidNormal->x = groundPlane->x;
                skidNormal->y = groundPlane->y;
                skidNormal->z = groundPlane->z;
                spawnSkidMark(groundPoint, scratch, &skidNormal->xyz());
                return;
            }
            setPhysicsState(rider, uint32_t(kPhysicsAir));
            return;
        }

        // The shadow: a ray 150 down from the body's centre; its height
        // tints the models (darker nearer, at least 8 of 128).
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
            rider->flags &= ~kFlagsHop;
            return;
        }
        collision::hitPoint(ground, &groundPlane->x);
        rider->groundHeight = groundPlane->y - centre->y; // (the height above the ground)
        *groundPoint = *groundPlane;
        const ee::Ptr<zge::ViewSphere> shadowVolume(groundPoint.address()); // groundPoint, then skidNormal's slot
        shadowVolume->radius = kShadowViewRadius;
        zge::setSceneShadowViewTest(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->messenger)), shadowVolume);
        zge::setSceneShadowViewTest(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->bikeModel)), shadowVolume);
        const int32_t shade = ee::toInt((kShadowHeight - rider->groundHeight) * *g_shadowShade);
        const uint8_t level = static_cast<uint8_t>(shade >= kShadowLeast ? (shade & 0xFF) : kShadowLeast);
        tint[2] = level;
        tint[3] = 0x80;
        tint[0] = level;
        tint[1] = level;
        zge::setSceneRootColour(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->messenger)), tint);
        zge::setSceneRootColour(zge::sceneOf(ee::Ptr<zge::SceneObject>(rider->bikeModel)), tint);
    }
    READABLE_FUNCTION(airMove, Verified);
}
