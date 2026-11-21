// DM2's rider on the ground (physics state 0, and the wheelies' after
// callback): the turn, riding a moving object, the wheels' probe against
// walls (groundProbe) and their ground (groundAhead), for the ground move.
//
// Counterparts: DM2 GameCube 0x8007C508
// (groundTurn), 0x8007E1E8 (rideTouchedObject), 0x8007D654 (groundProbe),
// 0x80083A9C (groundAhead); BMX XXX PS2 groundTurn (medium),
// rideTouchedObject, groundProbe (low), groundAhead
// (groundAhead, medium).

#include "physics.h"

#include "ee/dispatch.h"

#include "../collision/collision.h"

#include "ee/readable.h"
#include "ee/vu0.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/physics/ground.cpp.h"

namespace game::dm2::rider::physics
{
    using collision::FaceRef;

    namespace
    {
        // The Turn tuning (from g_topSpeed on): on a slope the bike turns downhill,
        // the slower the more, below kTurnTopSpeed x kTurnSlowShare.
        inline ee::Global<ee::Float, sym::g_topSpeed> g_turnTopSpeed;    // 0.35 a frame
        inline ee::Global<ee::Float, sym::g_wheelieTurnShare> g_turnSlowShare;   // 0.95
        inline ee::Global<ee::Float, sym::g_wheelieTurnDegrees> g_turnSlopeRate;   // 42.5
        // The yaw rate's decay a frame (0.35 of it).
        inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnDecay;
        // Degrees to angle units (0x10000 / 360).
        inline ee::Global<ee::Float, sym::g_angleUnitsPerDegree2> g_angleUnitsPerDegree;
        // The slope test's angles: up more than 30 degrees from upright and
        // the right row less than 60 degrees from level (as floats).
        inline ee::Global<ee::Float, sym::g_slopeSteeper> g_slopeSteeper; // 5461
        inline ee::Global<ee::Float, sym::g_rightLevel> g_rightLevel;   // 10922

        // A yaw rate this small (in angle units) stops.
        constexpr ee::Float kYawRateRest = 91.0f;
        constexpr uint32_t kContactKindsSloped = 4; // contact kinds 1 .. 3 are faces
    }

    // State 0's turn: on a sloped face (the contact a face, up tilted more than
    // 30 degrees), slow enough, the bike turns downhill (the yaw rate by the
    // right row's slope); while riding (action states 0, 0, then 1 at level
    // 2) the rows and the body are turned by the angles and the yaw rate
    // decays. GameCube 0x8007C508.
    void groundTurn(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        zge::matrixIdentity(turn);

        const uint8_t kind = rider->contactKind;
        if (kind < kContactKindsSloped && kind != 0)
        {
            const int32_t tilt = zge::angleAcos(ee::abs(rider->up[1]));
            if (*g_slopeSteeper < ee::abs(ee::toFloat(tilt)))
            {
                const int32_t rightTilt = zge::angleAcos(ee::abs(rider->right[1]));
                const ee::Float top = *g_turnTopSpeed;
                if (ee::abs(ee::toFloat(rightTilt)) < *g_rightLevel)
                {
                    const ee::Float speed = rider->speed;
                    if (speed < top * *g_turnSlowShare)
                        rider->yawRate = static_cast<int16_t>(
                            rider->yawRate +
                            ee::toInt(rider->right[1] * ((top - speed) * *g_turnSlopeRate) * *g_angleUnitsPerDegree));
                }
            }
        }

        if (actionState(rider, 0u) != kActionRiding)
            return;
        if (actionState(rider, 1u) != 0)
            return;
        if (actionState(rider, 2u) != 1)
            return;
        zge::matrixRotateXYZ(turn, turn, eulerAngles(&rider->pitch));
        engine::matrixProduct(rider->right, rider->right, turn);
        engine::matrixProduct(rider->body, rider->body, turn);
        const int16_t rate = static_cast<int16_t>(
            rider->yawRate -
            ee::toInt(ee::toFloat(rider->yawRate) * ee::Float(1.0f / 65536.0f) * *g_turnDecay * ee::Float(65536.0f)));
        rider->yawRate = rate;
        if (ee::abs(ee::toFloat(rate)) < kYawRateRest)
            rider->yawRate = 0;
    }
    READABLE_FUNCTION(groundTurn, Verified);

    // groundMove's carry by a moving object (14.1's groundTilt: the code
    // follows the touched object, it does not tilt to the ground): with an
    // object touched (touchedObject, its part), the turn its part made since
    // the last frame (its rotation now against touchRotation) turns the body,
    // the wheels' matrices and points about the drawn position's offset from
    // the touch point; `moved` is how far the part's origin and that turn
    // carried the rider, added to the drawn position. Untouched, `moved` is
    // the origin (0, 0, 0, 1). GameCube 0x8007E1E8.
    void rideTouchedObject(Rider *rider, ee::Float *moved)
    {
        if (rider->touchedObject == 0)
        {
            setOrigin(moved);
            return;
        }
        ee::Local<zge::Quaternion> last;     // the part's rotation last frame
        ee::Local<zge::Quaternion> turned;   // the turn since
        ee::Local<zge::Vec4> arm;            // the drawn position from the touch point
        ee::Local<zge::Vec4> swung;
        ee::Local<zge::Matrix> back;         // the turn back
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Quaternion> rotation; // the part's rotation now
        ee::Local<zge::Vec4> origin;         // the part's origin in the world
        ee::Local<zge::Vec4> zero;
        const ee::Ptr<zge::SceneObject> object(rider->touchedObject);
        const ee::Ptr<zge::SceneBone> part(rider->touchedPart);

        *zero = zge::kZero;
        zge::objectBoneMatrix(object, part, matrix, 0u);
        zge::matrixToQuaternion(rotation, matrix);
        zge::objectBonePoint(object, part, zero, origin, 0u);
        zge::quaternionCopy(last, quaternion(rider->touchRotation));
        zge::quaternionCopy(quaternion(rider->touchRotation), rotation);

        // The arm swung by the turn: how far the turn carries the drawn body.
        zge::quaternionInverse(turned, last);
        zge::quaternionMultiply(turned, rotation, turned);
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix, turned);
        *arm = *vec4(rider->drawnPosition) - *vec4(rider->touchPoint);
        *swung = zge::rotated(*matrix, *arm);
        *arm = *swung - *arm;

        // The body and the wheels turned the other way round (rows on the
        // left of the product).
        zge::quaternionInverse(turned, rotation);
        zge::quaternionMultiply(turned, last, turned);
        zge::matrixIdentity(back);
        zge::quaternionToMatrix(back, turned);
        engine::matrixProduct(rider->body, rider->body, back);
        engine::matrixProduct(rider->wheelPoses[0].matrix, rider->wheelPoses[0].matrix, back);
        engine::matrixProduct(rider->wheelPoses[1].matrix, rider->wheelPoses[1].matrix, back);
        for (WheelPose &wheel : rider->wheelPoses)
            *vec4(wheel.velocity) = zge::rotated(*back, *vec4(wheel.velocity));

        *vec4(moved) = *origin - *vec4(rider->touchPoint);
        *vec4(rider->touchPoint) = *origin;
        *vec4(moved) = *vec4(moved) + *arm;
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(moved);
    }
    READABLE_FUNCTION(sym::groundTilt, rideTouchedObject, Verified);
    namespace
    {
        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kNoFaceSound = 0xFFFFu;

        // The probe's two rays a wheel, from the wheel's point next frame
        // 0.4 to either side of the body's right row.
        inline ee::Global<ee::Float, sym::g_probeZ> g_probeHalfWidth;
        // What a hit must be to count: the right row more than 30 degrees
        // from the face's normal and the up row within g_probeUpWithin
        // (78 degrees) of it, unless it is an object's face (kind 8): then
        // not flatter than 70 degrees and not a ramp (kinds 1 .. 3). A face
        // standing straight up (normal y 0) counts only as a kind 1 face.
        constexpr int32_t kProbeRightFrom = 0x1556;
        inline ee::Global<int16_t, sym::g_wallMostAngle> g_probeUpWithin;
        constexpr int32_t kProbeSteepest = 0x31C8;
        constexpr uint32_t kSurfaceRampFirst = 1;
        constexpr uint32_t kSurfaceRampLast = 3;
        constexpr uint32_t kSurfaceObjectFace = 8;
        // The hit: faster than 0.1 into it marks the side (kFlagMirrorSide)
        // and sounds (code by the face's sound: 1 and 3 give 7, 2 gives 2,
        // others 0; volume 400 x the speed, at most 127); below 0.29 the
        // rider is deflected (head on, within 15 degrees of the forward
        // row: both wheels bounce back at a quarter of their velocity and
        // the rider turns round), at or above it bails.
        inline ee::Global<ee::Float, sym::g_probeMarkSpeed> g_probeMarkSpeed;
        inline ee::Global<ee::Float, sym::g_probeSoundSpeed> g_probeSoundSpeed;
        inline ee::Global<ee::Float, sym::g_wallBailSpeed> g_probeBailSpeed;
        constexpr ee::Float kProbeVolumePerSpeed = 400.0f;
        constexpr int32_t kProbeVolumeMost = 127;
        constexpr int32_t kSoundWallHit = 22;
        constexpr int32_t kProbeHeadOn = 0xAAA;
        constexpr ee::Float kBounceShare = 0.25f;
        constexpr ee::Float kGlanceAlong = 10.0f;
        constexpr ee::Float kGlancePush = 1.25f;
        // A bail: when the object hit approached at least 1e-5 a frame, the
        // rider is thrown back at its speed and up (0.05; bail kind 3).
        inline ee::Global<ee::Float, sym::g_probeThrownFrom> g_probeThrownFrom;
        inline ee::Global<ee::Float, sym::g_probeThrowLift> g_probeThrowLift;
        constexpr uint8_t kBailHit = 0;
        constexpr uint8_t kBailThrown = 3;

        constexpr uint32_t kFlagHitHard = 0x4000000u;  // a probe's hit faster than g_probeMarkSpeed (inferred)

        // The engine's vec3Negate as the original calls it (a guest call): the
        // scale after it carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void negateByCall(ee::Float *v) { ee::game(sym::vec3Negate, v, v); }
    }

    // A wheel's probe against walls and edges while riding (the ground
    // move's): two rays from `from` to where the wheel will be (its point
    // on the forward row, the other wheel's matrix's when off the ground,
    // plus its velocity), either side of it. The first hit that counts and
    // the object owning it accepts: marked and sounded by how hard; soft,
    // the rider is deflected (head on: bounced back and turned round;
    // glancing: the wheel's velocity pushed off the face, its matrix along
    // it) and 1 returned; hard, the rider bails (2). 0 when nothing counts.
    // GameCube 0x8007D654; BMX XXX PS2 groundProbe.
    //
    // The scales carry what the last call left, as airGravity's do.
    int32_t groundProbe(Rider *rider, uint32_t wheelIndex, const ee::Float *from)
    {
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> hitPlane;
        ee::Local<zge::Vec4> normal;
        ee::Local<ee::Float> objectApproach;
        WheelPose &wheel = rider->wheelPoses[wheelIndex];
        WheelPose &other = rider->wheelPoses[wheelIndex ^ 1];
        zge::Vec4 &wheelVelocity = *vec4(wheel.velocity);
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyRight = body.row(0);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);

        for (uint32_t side = 0; side < 2; ++side)
        {
            ee::Float along = wheelIndex == 0 ? rider->frontWheelHeight : rider->backWheelHeight;
            if ((wheelIndex & 1) != 0)
                along = -along;
            *to = wheel.ground != kWheelOffGround ? bodyForward : matrix(other.matrix)->row(2);
            const ee::Float x = to->x * along + from[0] + wheel.velocity[0];
            const ee::Float y = to->y * along + from[1] + wheel.velocity[1];
            const ee::Float z = to->z * along + from[2] + wheel.velocity[2];
            ee::Float width = *g_probeHalfWidth;
            to->x = x;
            to->y = y;
            to->z = z;
            if ((side & 1) == 0)
                width = -width;
            to->x = x + bodyRight.x * width;
            to->y = y + bodyRight.y * width;
            to->z = z + bodyRight.z * width;

            collision::RayHit *const hit = collision::rayCast(from, &to->x);
            if (hit == nullptr)
                continue;
            const FaceRef face = collision::hitFace(hit);
            collision::hitPlane(hit, &hitPlane->x);
            *normal = *hitPlane;
            *objectApproach = 0.0f;
            const ee::Float closing = collision::hitClosing(wheel.velocity, hit, objectApproach);
            if (!(ee::Float(0.0f) < closing))
                continue;

            // Whether the hit counts (a face standing straight up that is
            // not a kind 1 always does).
            bool counts = true;
            if (!(normal->y == ee::Float(0.0f) &&
                  ((collision::hitFlags(hit) & kHitFaceFlag) == 0 || collision::faceSurfaceKind(face) != kSurfaceRampFirst)))
            {
                if (!(zge::angleAcos(ee::abs(zge::dot(bodyRight, *normal))) < kProbeRightFrom) &&
                    zge::angleAcos(zge::dot(bodyUp, *normal)) < *g_probeUpWithin)
                {
                    if (!((collision::hitFlags(hit) & kHitFaceFlag) != 0 && collision::faceSurfaceKind(face) == kSurfaceObjectFace))
                    {
                        if (zge::angleAcos(-normal->y) < kProbeSteepest)
                            counts = false;
                        else
                        {
                            const uint32_t kind = collision::faceSurfaceKind(face);
                            if (kind == kSurfaceRampFirst)
                                counts = false;
                            else if (collision::faceSurfaceKind(face) == 2)
                                counts = false;
                            else if (collision::faceSurfaceKind(face) == kSurfaceRampLast)
                                counts = false;
                        }
                    }
                }
            }
            if (!counts)
                continue;
            if (collision::touchObject(rider, hit) == 0)
                continue;

            // Marked and sounded by how hard.
            wheel.touched |= 1;
            if (*g_probeMarkSpeed < closing)
            {
                rider->flags |= kFlagHitHard;
                const bool facesRight = ee::Float(0.0f) <= zge::dot(*normal, *vec4(rider->right));
                const bool mirrored = (rider->flags & kFlagMirrored) != 0;
                if (mirrored ? facesRight : !facesRight)
                    rider->flags |= kFlagMirrorSide;
                else
                    rider->flags &= ~kFlagMirrorSide;
            }
            uint32_t surface = 0;
            if (collision::faceSound(face) != kNoFaceSound)
            {
                const uint32_t sound = collision::faceSound(face);
                surface = sound == 1 || sound == 3 ? 7u : sound == 2 ? 2u : 0u;
            }
            if (*g_probeSoundSpeed < closing)
            {
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, surface);
                const int32_t loud = ee::toInt(closing * kProbeVolumePerSpeed);
                soundEvent(kSoundWallHit, rider->position, code, loud < kProbeVolumeMost ? loud : kProbeVolumeMost);
            }

            if (closing < *g_probeBailSpeed)
            {
                const int32_t facing = zge::angleAcos(ee::abs(zge::dot(bodyForward, *normal)));
                if (facing < kProbeHeadOn)
                {
                    // Head on: both wheels bounce back, the rider turns round.
                    WheelPose &back = rider->wheelPoses[0];
                    WheelPose &front = rider->wheelPoses[1];
                    const ee::Float bx = back.velocity[0] * -kBounceShare;
                    const ee::Float by = back.velocity[1] * -kBounceShare;
                    const ee::Float bz = back.velocity[2] * -kBounceShare;
                    const ee::Float bs = back.speed * kBounceShare;
                    const ee::Float fx = front.velocity[0] * -kBounceShare;
                    const ee::Float fy = front.velocity[1] * -kBounceShare;
                    const ee::Float fz = front.velocity[2] * -kBounceShare;
                    const ee::Float fs = front.speed * kBounceShare;
                    back.velocity[0] = bx;
                    back.velocity[1] = by;
                    back.velocity[2] = bz;
                    back.speed = bs;
                    front.speed = fs;
                    front.velocity[0] = fx;
                    front.velocity[1] = fy;
                    front.velocity[2] = fz;
                    rider->flags ^= kFlagFakie;
                    return 1;
                }
                // Glancing: the wheel's velocity pushed off the face; the
                // wheel's matrix along it.
                const ee::Float into = zge::dot(wheelVelocity, *normal);
                to->x = wheel.velocity[0] - normal->x * into;
                to->y = wheel.velocity[1] - normal->y * into;
                to->z = wheel.velocity[2] - normal->z * into;
                const ee::Float slide = engine::cosTurn(facing) * kGlanceAlong;
                // (the original works out the slide along the face and does
                // not use it)
                *to = zge::scaledCarrying(*to, slide, zge::scaleLeftover(zge::LeftBy::lastCall));
                const ee::Float push = closing * kGlancePush;
                const ee::Float pz = wheel.velocity[2] + normal->z * push;
                const ee::Float px = wheel.velocity[0] + normal->x * push;
                const ee::Float py = wheel.velocity[1] + normal->y * push;
                wheel.velocity[2] = pz;
                wheel.velocity[0] = px;
                wheel.velocity[1] = py;
                zge::Matrix &rows = *matrix(wheel.matrix);
                rows.row(2) = wheelVelocity;
                rows.row(2) = zge::unit(rows.row(2));
                rows.row(0) = zge::cross(rows.row(1), rows.row(2));
                wheel.speed = zge::vec3Length(&wheelVelocity.xyz());
                other.speed = zge::vec3Length(&vec4(other.velocity)->xyz());
                return 1;
            }

            // Hard: a bail.
            rider->bailKind = kBailHit;
            if (!(ee::abs(*objectApproach) < *g_probeThrownFrom))
            {
                zge::Vec4 &velocity = *vec4(rider->velocity);
                negateByCall(rider->velocity);
                velocity = zge::unit(velocity);
                velocity = zge::scaledCarrying(velocity, *objectApproach, zge::scaleLeftover(zge::LeftBy::lastCall));
                rider->bailKind = kBailThrown;
                velocity.y = velocity.y + *g_probeThrowLift;
            }
            setPhysicsState(rider, uint32_t(kPhysicsBail));
            addRideDistance(rider, closing);
            collision::touchObject(rider, hit);
            return 2;
        }
        return 0;
    }
    READABLE_FUNCTION(groundProbe, Verified);
    namespace
    {

        // The ground a wheel stands on: the right row more than 30 degrees
        // from its normal; the up row within g_probeUpWithin (78 degrees)
        // of it (the leading wheel), or the row given within
        // g_groundRowWithin (50 degrees); and not steeper than 70 degrees
        // unless a ramp (kinds 1 .. 3).
        inline ee::Global<int16_t, sym::g_groundRowWithin> g_groundRowWithin;
        // On the ground the ray reaches further by the slope: the up row's
        // y times 0.0055 times 42.5 (twice that on a kind 7 face).
        inline ee::Global<ee::Float, sym::g_gravity> g_slopeReach;
        inline ee::Global<ee::Float, sym::g_slopeReachScale> g_slopeReachScale; // 42.5 (also the Turn tuning's slope rate)
        constexpr uint32_t kSurfaceKind7 = 7;
        // Rider::flags: a wheel may find the ground along its own up row
        // (inferred: on a lip or in a wheelie).
        constexpr uint32_t kFlagsWheelOwnGround = kFlagManual | kFlagNoseWheelie;

        // A wheel's stat: its height above the ground (front 0x100, back 0x120).
        ee::Float wheelStat(const Rider *rider, uint32_t wheel)
        {
            return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight;
        }

        // The ray's hit: its face, point and plane (normal, distance).
        FaceRef readHit(const collision::RayHit *hit, ee::Float *point, ee::Float *plane, ee::Float *normal)
        {
            const FaceRef face = collision::hitFace(hit);
            collision::hitPoint(hit, point);
            collision::hitPlane(hit, plane);
            *vec4(normal) = *vec4(plane);
            return face;
        }

        // Steeper than 70 degrees: only a ramp's face (kinds 1 .. 3) stays.
        bool steepAndNotRamp(FaceRef face, const ee::Float *normal)
        {
            if (zge::angleAcos(-normal[1]) < kProbeSteepest)
                return false;
            return collision::faceSurfaceKind(face) != 1 && collision::faceSurfaceKind(face) != 2 &&
                   collision::faceSurfaceKind(face) != 3;
        }

        // Whether a hit below a wheel is ground to stand on (the second and
        // third rays): moving into it, or `row` within 50 degrees of its
        // normal (for the leading wheel only if the face is within 78
        // degrees of level), and not a steep non-ramp face.
        bool standsOn(const Rider *rider, uint32_t wheelIndex, FaceRef face, const ee::Float *normal, const ee::Float *row)
        {
            bool counts = false;
            if (zge::dot(*vec4(rider->velocity), *vec4(normal)) < 0.0f)
                counts = true;
            else
            {
                const bool odd = (wheelIndex & 1) != 0;
                const bool ahead = ee::Float(0.0f) <= zge::dot(*vec4(rider->velocity), *vec4(rider->body + 8));
                const bool leading = ahead ? !odd : odd;
                if (!leading || zge::angleAcos(-normal[1]) < *g_probeUpWithin)
                {
                    if (zge::angleAcos(zge::dot(*vec4(row), *vec4(normal))) < *g_groundRowWithin)
                        counts = true;
                }
            }
            if (steepAndNotRamp(face, normal))
                counts = false;
            return counts;
        }

        // The height clamped to the stats' limits (stored back).
        ee::Float clampHeight(const Rider *rider, ee::Float *height)
        {
            ee::Float v = *height;
            if (rider->wheelHeightMost < v)
            {
                v = rider->wheelHeightMost;
                *height = v;
            }
            else if (v < rider->wheelHeightLeast)
            {
                *height = rider->wheelHeightLeast;
                v = rider->wheelHeightLeast;
            }
            return v;
        }
    }

    // A wheel's ground for the ground move: `point` (the wheel's point,
    // moved by its velocity) put on the ground under it at its height, and
    // the height (*height, kept between the stats' limits) and the
    // clearance (*clearance, the height less the spring's length). Three
    // rays, the first that finds ground the object owning it accepts:
    //   1. down the body's up row from the point moved, by the wheel's
    //      stat and its spring's length and rate: the point put at the
    //      height above the hit, then slid along the face;
    //   2. (kFlagsWheelOwnGround, this wheel on the ground and the other
    //      off) down the body's up row by the spring's length and rate and
    //      the wheel's own up row by its stat leaned by the slope;
    //   3. down the body's up row by the stat and the spring's length and
    //      rate, further by the slope on the ground.
    // Returns the hit, or none (the point then only moved by the velocity).
    // GameCube 0x80083A9C; BMX XXX PS2 groundAhead.
    collision::RayHit *groundAhead(Rider *rider, uint32_t wheelIndex, ee::Float *point, ee::Float *height, ee::Float *clearance)
    {
        ee::Local<ee::Float[4]> plane;
        ee::Local<ee::Float[4]> at;
        ee::Local<ee::Float[4]> normal;
        ee::Local<ee::Float[4]> to; // the ray's end, then the hit from the point
        WheelPose &wheel = rider->wheelPoses[wheelIndex];
        const WheelPose &other = rider->wheelPoses[wheelIndex ^ 1];
        const ee::Float *bodyRight = rider->body;
        const ee::Float *bodyUp = rider->body + 4;
        const ee::Float *wheelUp = wheel.matrix + 4;

        // 1. Straight down from where the wheel goes.
        {
            const ee::Float reach = wheelStat(rider, wheelIndex) + wheel.springLength + wheel.springRate;
            const ee::Float x = point[0] + wheel.velocity[0] - bodyUp[0] * reach;
            const ee::Float y = point[1] + wheel.velocity[1] - bodyUp[1] * reach;
            const ee::Float z = point[2] + wheel.velocity[2] - bodyUp[2] * reach;
            to[0] = x;
            to[1] = y;
            to[2] = z;
        }
        collision::RayHit *hit = collision::rayCastAhead(point, to);
        if (hit != nullptr)
        {
            const FaceRef face = readHit(hit, at, plane, normal);
            if (!(zge::angleAcos(ee::abs(zge::dot(*vec4(bodyRight), *vec4(normal)))) < kProbeRightFrom) &&
                zge::angleAcos(zge::dot(*vec4(bodyUp), *vec4(normal))) < *g_probeUpWithin && !steepAndNotRamp(face, normal) &&
                collision::touchObject(rider, hit) != 0)
            {
                to[0] = at[0] - point[0];
                to[1] = at[1] - point[1];
                to[2] = at[2] - point[2];
                *height = -zge::dot(*vec4(to), *vec4(bodyUp)) - wheelStat(rider, wheelIndex);
                *clearance = *height - wheel.springLength;
                const ee::Float v = clampHeight(rider, height);
                // At the height above the hit, then slid along the face by
                // what is left of the move.
                const ee::Float movedX = point[0] + wheel.velocity[0];
                const ee::Float movedY = point[1] + wheel.velocity[1];
                const ee::Float movedZ = point[2] + wheel.velocity[2];
                const ee::Float x = at[0] + bodyUp[0] * (v + wheelStat(rider, wheelIndex));
                point[0] = x;
                const ee::Float leftX = movedX - x;
                to[0] = leftX;
                const ee::Float y = at[1] + bodyUp[1] * (*height + wheelStat(rider, wheelIndex));
                point[1] = y;
                const ee::Float leftY = movedY - y;
                const ee::Float z = at[2] + bodyUp[2] * (*height + wheelStat(rider, wheelIndex));
                const ee::Float leftZ = movedZ - z;
                to[1] = leftY;
                to[2] = leftZ;
                const ee::Float into = zge::dot(*vec4(to), *vec4(normal));
                point[0] = x + (leftX - normal[0] * into);
                point[1] = y + (leftY - normal[1] * into);
                point[2] = z + (leftZ - normal[2] * into);
                return hit;
            }
        }

        point[0] = point[0] + wheel.velocity[0];
        point[1] = point[1] + wheel.velocity[1];
        point[2] = point[2] + wheel.velocity[2];

        // 2. Along the wheel's own up row (this wheel down, the other off).
        if ((rider->flags & kFlagsWheelOwnGround) != 0 && wheel.ground != kWheelOffGround && other.ground == kWheelOffGround)
        {
            const ee::Float slope = -bodyUp[1] * *g_slopeReach * *g_slopeReachScale;
            const ee::Float reach = wheel.springLength + wheel.springRate;
            const ee::Float lean = wheelStat(rider, wheelIndex) + slope;
            const ee::Float z = point[2] - bodyUp[2] * reach - wheelUp[2] * lean;
            const ee::Float x = point[0] - bodyUp[0] * reach - wheelUp[0] * lean;
            const ee::Float y = point[1] - bodyUp[1] * reach - wheelUp[1] * lean;
            to[2] = z;
            to[0] = x;
            to[1] = y;
            hit = collision::rayCastAhead(point, to);
            if (hit != nullptr)
            {
                const FaceRef face = readHit(hit, at, plane, normal);
                if (!(zge::angleAcos(ee::abs(zge::dot(*vec4(bodyRight), *vec4(normal)))) < kProbeRightFrom) &&
                    standsOn(rider, wheelIndex, face, normal, wheelUp) && collision::touchObject(rider, hit) != 0)
                {
                    const ee::Float stat = wheelStat(rider, wheelIndex);
                    to[0] = at[0] + wheelUp[0] * stat - point[0];
                    to[1] = at[1] + wheelUp[1] * stat - point[1];
                    to[2] = at[2] + wheelUp[2] * stat - point[2];
                    const ee::Float h = -zge::dot(*vec4(to), *vec4(bodyUp));
                    *height = h;
                    *clearance = h - wheel.springLength;
                    const ee::Float lift = clampHeight(rider, height) - h;
                    const ee::Float y = point[1];
                    const ee::Float z = point[2];
                    point[0] = point[0] + bodyUp[0] * lift;
                    point[1] = y + bodyUp[1] * (*height - h);
                    point[2] = z + bodyUp[2] * (*height - h);
                    return hit;
                }
            }
        }

        // 3. Down the body's up row, further by the slope on the ground.
        ee::Float reach = wheelStat(rider, wheelIndex) + wheel.springLength + wheel.springRate;
        if (wheel.ground != kWheelOffGround)
        {
            ee::Float scale = *g_slopeReachScale;
            if (rider->contactKind == 0 &&
                collision::faceSurfaceKind(rider->contactRef()) == kSurfaceKind7)
                scale = scale + scale;
            reach = reach + -bodyUp[1] * *g_slopeReach * scale;
        }
        {
            const ee::Float x = point[0] - bodyUp[0] * reach;
            const ee::Float y = point[1] - bodyUp[1] * reach;
            const ee::Float z = point[2] - bodyUp[2] * reach;
            to[0] = x;
            to[1] = y;
            to[2] = z;
        }
        hit = collision::rayCastAhead(point, to);
        if (hit == nullptr)
            return nullptr;
        const FaceRef face = readHit(hit, at, plane, normal);
        if (zge::angleAcos(ee::abs(zge::dot(*vec4(bodyRight), *vec4(normal)))) < kProbeRightFrom)
            return nullptr;
        if (!standsOn(rider, wheelIndex, face, normal, bodyUp) || collision::touchObject(rider, hit) == 0)
            return nullptr;
        to[0] = at[0] - point[0];
        to[1] = at[1] - point[1];
        to[2] = at[2] - point[2];
        const ee::Float h = -zge::dot(*vec4(to), *vec4(bodyUp));
        *height = h - wheelStat(rider, wheelIndex);
        *clearance = *height - wheel.springLength;
        const ee::Float v = clampHeight(rider, height);
        const ee::Float x = point[0];
        const ee::Float y = point[1];
        const ee::Float z = point[2];
        point[0] = x + bodyUp[0] * (v + wheelStat(rider, wheelIndex) - h);
        point[1] = y + bodyUp[1] * (*height + wheelStat(rider, wheelIndex) - h);
        point[2] = z + bodyUp[2] * (*height + wheelStat(rider, wheelIndex) - h);
        return hit;
    }
    READABLE_FUNCTION(groundAhead, Verified);
}
