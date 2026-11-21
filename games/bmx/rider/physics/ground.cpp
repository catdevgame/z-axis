// BMX XXX's ground physics (physics.c). DM2's (games/dm2/rider/physics/
// ground.cpp), read in BMX XXX's machine code.

#include "physics.h"

#include "../collision/collision.h"
#include "../engine_calls.h"
#include "../game_calls.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "sym/bmx/rider/physics/ground.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        // The Turn tuning: on a slope the bike turns downhill, the slower
        // the more, below the top speed x the slow share.
        inline ee::Global<ee::Float, sym::g_topSpeed> g_turnTopSpeed;            // 0.35 a frame
        inline ee::Global<ee::Float, sym::g_wheelieTurnShare> g_turnSlowShare;   // 0.95
        inline ee::Global<ee::Float, sym::g_wheelieTurnDegrees> g_turnSlopeRate; // 42.5
        // The yaw rate's decay a frame (0.35 of it).
        inline ee::Global<ee::Float, sym::g_turnFalloff> g_turnDecay;
        // Degrees to angle units (0x10000 / 360).
        inline ee::Global<ee::Float, sym::g_angleUnitsPerDegree> g_angleUnitsPerDegree;
        // The slope test's angles: up more than 30 degrees from upright and
        // the right row less than 60 degrees from level (as floats).
        inline ee::Global<ee::Float, sym::g_slopeSteeper> g_slopeSteeper; // 5461
        inline ee::Global<ee::Float, sym::g_rightLevel> g_rightLevel;     // 10922

        // A yaw rate this small (in angle units) stops.
        constexpr ee::Float kYawRateRest = 91.0f;
        constexpr uint32_t kContactKindsSloped = 4; // contact kinds 1 .. 3 are faces
    }

    // State 0's turn. With the grind button held, the grind first (a grind
    // or stall taken ends it; BMX XXX's own step, as its wheelie's turn).
    // Then DM2's: on a sloped face (the contact a face, up tilted more than
    // 30 degrees), slow enough, the bike turns downhill (the yaw rate by the
    // right row's slope); while riding (action states 0, 0, then 1 at level
    // 2) the rows and the body are turned by the yaw rate (DM2 by all three
    // angles) and the yaw rate decays.
    void groundTurn(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        if (hasFlag(rider, kFlagGrindAsked))
        {
            const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
            if (state == kPhysicsGrind || state == kPhysicsStall)
                return;
        }
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
                            static_cast<uint16_t>(rider->yawRate) +
                            ee::toInt(rider->right[1] * ((top - speed) * *g_turnSlopeRate) * *g_angleUnitsPerDegree));
                }
            }
        }

        if (actionState(rider, uint32_t(kLevelAction)) != kActionRiding)
            return;
        if (actionState(rider, uint32_t(kLevelSubState)) != 0)
            return;
        if (actionState(rider, uint32_t(kLevelPhase)) != kPhaseHeld)
            return;
        zge::matrixRotateY(turn, turn, rider->yawRate);
        const ee::Ptr<zge::Matrix> rows(rider::matrix(rider->right));
        const ee::Ptr<zge::Matrix> body(rider::matrix(rider->body));
        zge::matrixMultiply(rows, rows, turn);
        zge::matrixMultiply(body, body, turn);
        const int16_t rate = static_cast<int16_t>(
            static_cast<uint16_t>(rider->yawRate) -
            ee::toInt(ee::toFloat(rider->yawRate) * ee::Float(1.0f / 65536.0f) * *g_turnDecay * ee::Float(65536.0f)));
        rider->yawRate = rate;
        if (ee::abs(ee::toFloat(rate)) < kYawRateRest)
            rider->yawRate = 0;
    }
    READABLE_FUNCTION(groundTurn, Verified);

    // The ground move's carry by a moving object: with an object touched
    // (touchedObject, its part), the turn its part made since the last frame
    // (its rotation now against touchRotation) turns the body, the wheels'
    // matrices and velocities about the drawn position's offset from the
    // touch point; `moved` is how far the part's origin and that turn
    // carried the rider, added to the drawn position. Untouched, `moved` is
    // the origin (0, 0, 0, 1). DM2's.
    void rideTouchedObject(Rider *rider, ee::Float *moved)
    {
        if (rider->touchedObject == 0)
        {
            *vec4(moved) = zge::kZero;
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
        const ee::Ptr<zge::Quaternion> touchRotation(quaternion(rider->touchRotation));

        *zero = zge::kZero;
        zge::objectBoneMatrix(object, part, matrix, 0u);
        zge::matrixToQuaternion(rotation, matrix);
        zge::objectBonePoint(object, part, zero, origin, 0u);
        zge::quaternionCopy(last, touchRotation);
        zge::quaternionCopy(touchRotation, rotation);

        // The arm swung by the turn: how far the turn carries the drawn body.
        zge::quaternionInverse(turned, last);
        zge::quaternionMultiply(turned, rotation, turned);
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix, turned);
        *arm = *vec4(rider->drawnPosition) - *vec4(rider->touchPoint);
        *swung = zge::rotated(*matrix, *arm);
        *arm = *swung - *arm;

        // The body and the wheels turned the other way round.
        zge::quaternionInverse(turned, rotation);
        zge::quaternionMultiply(turned, last, turned);
        zge::matrixIdentity(back);
        zge::quaternionToMatrix(back, turned);
        const ee::Ptr<zge::Matrix> body(rider::matrix(rider->body));
        zge::matrixMultiply(body, body, back);
        for (WheelPose &wheel : rider->wheelPoses)
        {
            const ee::Ptr<zge::Matrix> wheelMatrix(rider::matrix(wheel.matrix));
            zge::matrixMultiply(wheelMatrix, wheelMatrix, back);
        }
        for (WheelPose &wheel : rider->wheelPoses)
            *vec4(wheel.velocity) = zge::rotated(*back, *vec4(wheel.velocity));

        *vec4(moved) = *origin - *vec4(rider->touchPoint);
        *vec4(rider->touchPoint) = *origin;
        *vec4(moved) = *vec4(moved) + *arm;
        *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *vec4(moved);
    }
    READABLE_FUNCTION(rideTouchedObject, Verified);

    namespace
    {
        // pushCornersClear's corners: the drawn body's four, this far to the side (0.35) and along it (0.5).
        inline ee::Global<ee::Float, sym::g_cornerSide> g_cornerSide;
        inline ee::Global<ee::Float, sym::g_cornerAlong> g_cornerAlong;
        // The rays touch what they meet as a ride's ray does (touchObject's word).
        constexpr uint32_t kTouchRiding = 1;
        // The rays run this far up the drawn body: their start and the corners are a quarter unit up its y.
        constexpr ee::Float kRayHeight = ee::Float(0.25f);
    }

    // The drawn body kept clear of what is around it: from the body's centre, a quarter unit up, a ray to each of its
    // four corners at that height (the side, then along, each both ways); one that meets something pushes the drawn body and the rider back
    // along the ray by what the corner is short of the hit. 1 when any ray met something.
    int32_t pushCornersClear(Rider *rider)
    {
        ee::Local<zge::Vec4> from;
        ee::Local<zge::Vec4> to;
        ee::Local<zge::Vec4> corner; // then the way back from the hit
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> point;
        int32_t met = 0;
        const zge::Matrix &body = *matrix(rider->body);
        for (uint32_t along = 0; along < 2; ++along)
        {
            for (uint32_t side = 0; side < 2; ++side)
            {
                *corner = zge::kZero;
                corner->y = kRayHeight;
                *from = zge::transformed(body, *corner);
                *from = *from + *vec4(rider->drawnPosition);
                corner->z = *g_cornerAlong;
                corner->x = *g_cornerSide;
                if (along & 1)
                    corner->z = -corner->z;
                if (side & 1)
                    corner->x = -corner->x;
                *to = zge::transformed(body, *corner);
                *to = *to + *vec4(rider->drawnPosition);
                const collision::RayHit *hit = collision::rayCastTouching(rider, &from->x, &to->x, kTouchRiding);
                if (!hit)
                    continue;
                collision::hitFace(hit);
                collision::hitPlane(hit, &plane->x);
                collision::hitPoint(hit, &point->x);
                *corner = *point - *from;
                const ee::Float reach = zge::vec3Length(&corner->xyz());
                *corner = *corner * (ee::Float(1.0f) / reach);
                const ee::Float sideSquared = *g_cornerSide * *g_cornerSide;
                const ee::Float alongSquared = *g_cornerAlong * *g_cornerAlong;
                const ee::Float cornerReach = ee::sqrt(sideSquared + alongSquared);
                *corner = *corner * (reach - cornerReach);
                *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + *corner;
                *vec4(rider->position) = *vec4(rider->position) + *corner;
                met = 1;
            }
        }
        return met;
    }
    READABLE_FUNCTION(pushCornersClear, Verified);

    namespace
    {
        // groundAhead's tests of a hit face (DM2's): the body's right row more than 30 degrees from its normal;
        // the up row within 78 degrees of it (g_probeUpWithin: the same word the wall test reads), or for a
        // trailing wheel the row given within 50 degrees; not steeper than 70 degrees unless a ramp (surface
        // kinds 1 .. 3); and (BMX XXX's own) never a face of surface kind 8.
        constexpr zge::Angle kProbeRightFrom = 0x1556;
        constexpr zge::Angle kProbeSteepest = 0x31C8;
        inline ee::Global<int16_t, sym::g_wallMostAngle> g_probeUpWithin;
        inline ee::Global<int16_t, sym::g_groundRowWithin> g_groundRowWithin;
        constexpr uint32_t kSurfaceKindNoGround = 8;
        // On the ground the third ray reaches further by the slope: the up row's y times the gravity (0.0055)
        // times 42.5 (twice that on a face of surface kind 7, once in the ghost ride).
        inline ee::Global<ee::Float, sym::g_gravity> g_slopeReach;
        inline ee::Global<ee::Float, sym::g_slopeReachScale> g_slopeReachScale;
        constexpr uint32_t kSurfaceKind7 = 7;
        // The rays touch what they meet as the ground's do (rayCastTouching's word).
        constexpr uint32_t kTouchGround = 0;

        ee::Float wheelStat(const Rider *rider, uint32_t wheel) { return wheel == 0 ? rider->frontWheelHeight : rider->backWheelHeight; }

        // A hit face the second and third rays stand on: moving into it, or `row` within 50 degrees of its
        // normal (for the leading wheel only if the face is within 78 degrees of level); not a steep non-ramp
        // face, nor one of kind 8.
        bool standsOn(const Rider *rider, uint32_t wheelIndex, collision::FaceRef face, const zge::Vec4 &normal, const zge::Vec4 &row)
        {
            bool counts = false;
            if (zge::dot(*vec4(rider->velocity), normal) < ee::Float(0.0f))
                counts = true;
            else
            {
                const bool odd = (wheelIndex & 1) != 0;
                const bool ahead = ee::Float(0.0f) <= zge::dot(*vec4(rider->velocity), *vec4(&rider->body[8]));
                const bool leading = ahead ? !odd : odd;
                if ((!leading || zge::angleAcos(-normal.y) < *g_probeUpWithin) && zge::angleAcos(zge::dot(row, normal)) < *g_groundRowWithin)
                    counts = true;
            }
            if (!(zge::angleAcos(-normal.y) < kProbeSteepest) && collision::faceSurfaceKind(face) != 1 && collision::faceSurfaceKind(face) != 2 &&
                collision::faceSurfaceKind(face) != 3)
                counts = false;
            if (collision::faceSurfaceKind(face) == kSurfaceKindNoGround)
                counts = false;
            return counts;
        }

        // The height kept between the rider's limits (stored back).
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

    // groundAhead (rider, wheel, point, height, clearance): a wheel's ground for the ground move: `point` (the
    // wheel's point, moved by its velocity) put on the ground under it at its height, the height (kept between
    // the rider's limits) and the clearance (the height less the spring's length). Three rays, each touching what
    // it meets: 1. down the body's up row from where the point goes, by the wheel's stat and its spring's length
    // and rate: the point at the height above the hit, slid along the face; 2. (kFlagOnOneWheel, this wheel down and
    // the other off) down the body's up row by the spring and the wheel's own up row by its stat leaned by the
    // slope; 3. down the body's up row, further by the slope on the ground. Returns the hit, or none (the point
    // then only moved by the velocity). DM2's, with rayCastTouching and BMX XXX's faces of kind 8.
    collision::RayHit *groundAhead(Rider *rider, uint32_t wheelIndex, ee::Float *point, ee::Float *height, ee::Float *clearance)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> to; // the ray's end, then the hit from the point
        WheelPose &wheel = rider->wheelPoses[wheelIndex];
        const WheelPose &other = rider->wheelPoses[wheelIndex ^ 1];
        const zge::Vec4 &bodyRight = *vec4(&rider->body[0]);
        const ee::Float *bodyUp = &rider->body[4];
        const ee::Float *wheelUp = &wheel.matrix[4];
        const auto readHit = [&](collision::RayHit *found) {
            const collision::FaceRef face = collision::hitFace(found);
            collision::hitPoint(found, &at->x);
            collision::hitPlane(found, &plane->x);
            return face;
        };

        // 1. Straight down from where the wheel goes.
        {
            const ee::Float reach = wheelStat(rider, wheelIndex) + wheel.springLength + wheel.springRate;
            to->x = point[0] + wheel.velocity[0] - bodyUp[0] * reach;
            to->y = point[1] + wheel.velocity[1] - bodyUp[1] * reach;
            to->z = point[2] + wheel.velocity[2] - bodyUp[2] * reach;
        }
        collision::RayHit *hit = collision::rayCastTouching(rider, point, &to->x, kTouchGround);
        if (hit)
        {
            const collision::FaceRef face = readHit(hit);
            if (!(zge::angleAcos(ee::abs(zge::dot(bodyRight, *plane))) < kProbeRightFrom) &&
                zge::angleAcos(zge::dot(*vec4(bodyUp), *plane)) < *g_probeUpWithin &&
                (zge::angleAcos(-plane->y) < kProbeSteepest || collision::faceSurfaceKind(face) == 1 ||
                 collision::faceSurfaceKind(face) == 2 || collision::faceSurfaceKind(face) == 3) &&
                collision::faceSurfaceKind(face) != kSurfaceKindNoGround)
            {
                to->x = at->x - point[0];
                to->y = at->y - point[1];
                to->z = at->z - point[2];
                *height = -zge::dot(*to, *vec4(bodyUp)) - wheelStat(rider, wheelIndex);
                *clearance = *height - wheel.springLength;
                const ee::Float v = clampHeight(rider, height);
                // At the height above the hit, then slid along the face by what is left of the move.
                const ee::Float movedY = point[1] + wheel.velocity[1];
                const ee::Float movedX = point[0] + wheel.velocity[0];
                const ee::Float movedZ = point[2] + wheel.velocity[2];
                const ee::Float x = at->x + bodyUp[0] * (v + wheelStat(rider, wheelIndex));
                point[0] = x;
                const ee::Float leftX = movedX - x;
                const ee::Float y = at->y + bodyUp[1] * (*height + wheelStat(rider, wheelIndex));
                to->x = leftX;
                point[1] = y;
                const ee::Float leftY = movedY - y;
                const ee::Float z = at->z + bodyUp[2] * (*height + wheelStat(rider, wheelIndex));
                to->y = leftY;
                const ee::Float leftZ = movedZ - z;
                to->z = leftZ;
                const ee::Float into = zge::dot(*to, *plane);
                point[0] = x + (leftX - plane->x * into);
                point[1] = y + (leftY - plane->y * into);
                point[2] = z + (leftZ - plane->z * into);
                return hit;
            }
        }

        point[0] = point[0] + wheel.velocity[0];
        point[1] = point[1] + wheel.velocity[1];
        point[2] = point[2] + wheel.velocity[2];

        // 2. Along the wheel's own up row (this wheel down, the other off).
        if (hasFlag(rider, kFlagOnOneWheel) && wheel.ground != kWheelOffGround && other.ground == kWheelOffGround)
        {
            const ee::Float slope = -bodyUp[1] * *g_slopeReach * *g_slopeReachScale;
            const ee::Float reach = wheel.springLength + wheel.springRate;
            const ee::Float lean = wheelStat(rider, wheelIndex) + slope;
            to->z = point[2] - bodyUp[2] * reach - wheelUp[2] * lean;
            to->x = point[0] - bodyUp[0] * reach - wheelUp[0] * lean;
            to->y = point[1] - bodyUp[1] * reach - wheelUp[1] * lean;
            hit = collision::rayCastTouching(rider, point, &to->x, kTouchGround);
            if (hit)
            {
                const collision::FaceRef face = readHit(hit);
                if (!(zge::angleAcos(ee::abs(zge::dot(bodyRight, *plane))) < kProbeRightFrom) &&
                    standsOn(rider, wheelIndex, face, *plane, *vec4(wheelUp)))
                {
                    const ee::Float stat = wheelStat(rider, wheelIndex);
                    to->x = at->x + wheelUp[0] * stat - point[0];
                    to->y = at->y + wheelUp[1] * stat - point[1];
                    to->z = at->z + wheelUp[2] * stat - point[2];
                    const ee::Float h = -zge::dot(*to, *vec4(bodyUp));
                    *height = h;
                    *clearance = h - wheel.springLength;
                    const ee::Float lift = clampHeight(rider, height) - h;
                    point[0] = point[0] + bodyUp[0] * lift;
                    point[1] = point[1] + bodyUp[1] * (*height - h);
                    point[2] = point[2] + bodyUp[2] * (*height - h);
                    return hit;
                }
            }
        }

        // 3. Down the body's up row, further by the slope on the ground.
        ee::Float reach = wheelStat(rider, wheelIndex) + wheel.springLength + wheel.springRate;
        if (wheel.ground != kWheelOffGround)
        {
            ee::Float scale;
            if (physicsState(rider) == kPhysicsGhostRide)
                scale = 1.0f;
            else
            {
                scale = *g_slopeReachScale;
                if (rider->contactKind == 0 && collision::faceSurfaceKind(contactRef(rider)) == kSurfaceKind7)
                    scale = scale + scale;
            }
            reach = reach + -bodyUp[1] * *g_slopeReach * scale;
        }
        to->x = point[0] - bodyUp[0] * reach;
        to->y = point[1] - bodyUp[1] * reach;
        to->z = point[2] - bodyUp[2] * reach;
        hit = collision::rayCastTouching(rider, point, &to->x, kTouchGround);
        if (!hit)
            return nullptr;
        const collision::FaceRef face = readHit(hit);
        if (zge::angleAcos(ee::abs(zge::dot(bodyRight, *plane))) < kProbeRightFrom)
            return nullptr;
        if (!standsOn(rider, wheelIndex, face, *plane, *vec4(bodyUp)))
            return nullptr;
        to->x = at->x - point[0];
        to->y = at->y - point[1];
        to->z = at->z - point[2];
        const ee::Float h = -zge::dot(*to, *vec4(bodyUp));
        *height = h - wheelStat(rider, wheelIndex);
        *clearance = *height - wheel.springLength;
        const ee::Float v = clampHeight(rider, height);
        point[0] = point[0] + bodyUp[0] * (v + wheelStat(rider, wheelIndex) - h);
        point[1] = point[1] + bodyUp[1] * (*height + wheelStat(rider, wheelIndex) - h);
        point[2] = point[2] + bodyUp[2] * (*height + wheelStat(rider, wheelIndex) - h);
        return hit;
    }
    READABLE_FUNCTION(groundAhead, Verified);

    namespace
    {
        // groundProbe's rays: half a unit to either side of the body's right row (the word pushCornersClear
        // reads as its corners' reach), touching what they meet as rays do (rayCastTouching's word 1; 6 the
        // bail's touch).
        inline ee::Global<ee::Float, sym::g_cornerAlong> g_probeHalfWidth;
        constexpr uint32_t kTouchWithRays = 1;
        constexpr uint32_t kTouchBail = 6;
        // A wall that counts is not a ramp's face (surface kind 8 of the world: always counts), nor flatter than
        // 70 degrees, nor of the world's surface kinds 1 .. 3.
        constexpr uint32_t kSurfaceRamp = 8;
        constexpr uint32_t kSurfaceWorldFirst = 1, kSurfaceWorldLast = 3;
        // Faster than 0.1 into it marks the side met (kFlagHitHard, kFlagMirrorSide); faster than 0.1 it sounds
        // (the bike's hit, by the face's sound; 400 a unit of speed, at most 127) and rumbles the pad (the large
        // motor, 765 a unit, at most 255); slower than g_wallBailSpeed (0.58) the rider is turned off the wall,
        // else he bails.
        inline ee::Global<ee::Float, sym::g_probeMarkSpeed> g_probeMarkSpeed;
        inline ee::Global<ee::Float, sym::g_probeSoundSpeed> g_probeSoundSpeed;
        inline ee::Global<ee::Float, sym::g_probeRumblePerSpeed> g_probeRumblePerSpeed;
        inline ee::Global<ee::Float, sym::g_wallBailSpeed> g_probeBailSpeed;
        constexpr uint32_t kSoundBikeHit = 0x23;
        constexpr ee::Float kVolumePerSpeed(400.0f);
        constexpr int32_t kMostVolume = 127;
        constexpr int32_t kMostRumble = 255;
        constexpr uint32_t kRumbleLargeMotor = 1;
        constexpr uint32_t kWallSoundRock = 7, kWallSoundMetal = 2;
        constexpr uint32_t kNoFaceSound = 0xFFFFu;
        // Turned off the wall: the face's own up row (the wheel's up for a face within 5 degrees of level, else
        // the world's); within 30 degrees of head on, a rider going backwards is thrown back (1.75 of his
        // velocity), else pushed off the face (1.05 of the closing speed) and along it a quarter of it, the way
        // he goes (by his velocity, else his forward row, else his up row, the first more than 0.01 along it);
        // glancing, pushed off the face by 1.1 of the closing speed.
        constexpr zge::Angle kFaceLevel = 0x38E;
        constexpr zge::Angle kHeadOn = 0x1555;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        constexpr ee::Float kThrownBack(-1.75f);
        inline ee::Global<ee::Float, sym::g_probePushHeadOn> g_probePushHeadOn;
        inline ee::Global<ee::Float, sym::g_probeAlongLeast> g_probeAlongLeast;
        constexpr ee::Float kAlongShare(0.25f);
        inline ee::Global<ee::Float, sym::g_probePushGlancing> g_probePushGlancing;
        // Rider::bailKind: bounced back off a moving object.
        constexpr uint8_t kBailBounced = 3;
    }

    // groundProbe (rider, wheel, from): a wheel's probe against walls while riding (the ground move's): two rays
    // from `from` to where the wheel will be (its point along the forward row, the other wheel's matrix's when off
    // the ground, plus its velocity), half a unit to either side. The first wall that counts and that the wheel
    // closes on (hitImpact): marked, sounded and rumbled by how hard. Soft (1): bounced off a moving object, the
    // wheel alone takes the bounce (its velocity and matrix); else the rider is pushed off the wall (thrown back,
    // pushed off and along, or pushed off: the force), his rows turned along his velocity on the wheel's up row,
    // and both wheels with them. Hard (2): a bail (in the ghost ride, its tumble), bounced off a moving object,
    // else the distance counted and the object touched. 0 when nothing counts. DM2's (games/dm2/rider/physics/
    // ground.cpp) with BMX XXX's rays, impact, rumble and push off the wall.
    int32_t groundProbe(Rider *rider, uint32_t wheelIndex, const ee::Float *from)
    {
        ee::Local<zge::Vec4> to;
        ee::Local<collision::HitImpact> impact;
        ee::Local<zge::Vec4> plane;
        ee::Local<calls::Rumble> rumble;
        ee::Local<zge::Vec4> level;   // along the face, level on its up row
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> faceUp;  // the face's up row
        WheelPose &wheel = rider->wheelPoses[wheelIndex];
        const WheelPose &other = rider->wheelPoses[wheelIndex ^ 1];
        const zge::Matrix &body = *matrix(rider->body);
        const zge::Vec4 &bodyRight = body.row(0);
        const zge::Vec4 &bodyUp = body.row(1);
        const zge::Vec4 &bodyForward = body.row(2);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        for (uint32_t side = 0; side < 2; ++side)
        {
            ee::Float along = wheelStat(rider, wheelIndex);
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
            collision::RayHit *const hit = collision::rayCastTouching(rider, from, &to->x, kTouchWithRays);
            if (hit == nullptr)
                continue;
            const collision::FaceRef face = collision::hitFace(hit);
            collision::hitPlane(hit, &plane->x);

            // Does the wall count? A flat face of the world is not looked at; a steep one is, unless a floor or one
            // of the world's kinds.
            const bool flatWorld = plane->y == ee::Float(0.0f) &&
                                   (!(collision::hitFlags(hit) & collision::kHitFace) || collision::faceSurfaceKind(face) != kSurfaceWorldFirst);
            if (!flatWorld && !(zge::angleAcos(ee::abs(zge::dot(bodyRight, *plane))) < kProbeRightFrom) &&
                zge::angleAcos(zge::dot(bodyUp, *plane)) < *g_probeUpWithin &&
                !((collision::hitFlags(hit) & collision::kHitFace) && collision::faceSurfaceKind(face) == kSurfaceRamp))
            {
                if (zge::angleAcos(-plane->y) < kProbeSteepest)
                    continue; // a floor
                bool world = false;
                for (uint32_t kind = kSurfaceWorldFirst; kind <= kSurfaceWorldLast && !world; ++kind)
                    world = collision::faceSurfaceKind(face) == kind;
                if (world)
                    continue;
            }

            impact->hit = ee::Ptr<collision::RayHit>(hit);
            *vec4(impact->velocity) = *vec4(wheel.velocity);
            const ee::Float closing = collision::hitImpact(impact);
            if (!(ee::Float(0.0f) < closing))
                continue; // moving away from it

            // Marked, sounded and rumbled by how hard.
            wheel.touched |= 1;
            if (*g_probeMarkSpeed < closing)
            {
                setFlag(rider, kFlagHitHard);
                const bool facesRight = ee::Float(0.0f) <= zge::dot(*plane, *vec4(rider->right));
                if (hasFlag(rider, kFlagMirrored) ? facesRight : !facesRight)
                    setFlag(rider, kFlagMirrorSide);
                else
                    clearFlag(rider, kFlagMirrorSide);
            }
            uint32_t surface = 0;
            if (collision::faceSound(face) != kNoFaceSound)
            {
                const uint32_t sound = collision::faceSound(face);
                surface = sound == 1 || sound == 3 ? kWallSoundRock : sound == 2 ? kWallSoundMetal : 0u;
            }
            if (*g_probeSoundSpeed < closing)
            {
                const int32_t code = surfaceSoundFlags(0u, 0u, 1u, surface);
                const int32_t loud = ee::toInt(closing * kVolumePerSpeed);
                soundEvent(kSoundBikeHit, rider->position, static_cast<uint32_t>(code),
                                  static_cast<uint32_t>(loud < kMostVolume ? loud : kMostVolume), 0u);
                if (!hasFlag(rider, kFlagNoPlayer))
                {
                    const int32_t strength = ee::toInt(closing * *g_probeRumblePerSpeed);
                    rumble->motor = kRumbleLargeMotor;
                    rumble->timing = 0;
                    rumble->strength = static_cast<uint8_t>(strength < kMostRumble ? strength : kMostRumble);
                    calls::rumble(uint32_t(rider->player), rumble);
                }
            }

            if (closing < *g_probeBailSpeed)
            {
                zge::Matrix &wheelRows = *matrix(wheel.matrix);
                if (impact->flags & collision::kImpactBounced)
                {
                    // Bounced off a moving object: the wheel alone, its rows along its new velocity.
                    *vec4(wheel.velocity) = *vec4(impact->riderVelocity);
                    wheelRows.row(2) = *vec4(wheel.velocity);
                    wheelRows.row(2) = zge::unit(wheelRows.row(2));
                    wheelRows.row(0) = zge::cross(wheelRows.row(1), wheelRows.row(2));
                    wheel.speed = zge::vec3Length(&vec4(wheel.velocity)->xyz());
                    return 1;
                }
                // Turned off the wall: the rider's forward row along the face, level on its up row.
                *normal = *plane;
                *faceUp = zge::angleAcos(ee::abs(normal->y)) < kFaceLevel ? wheelRows.row(1) : *g_worldUp;
                *level = zge::cross(*normal, *faceUp);
                *level = zge::unit(*level);
                *faceUp = zge::cross(*level, *normal);
                *to = bodyForward;
                const ee::Float up = zge::dot(*to, *faceUp);
                to->x = to->x - up * faceUp->x;
                to->y = to->y - up * faceUp->y;
                to->z = to->z - up * faceUp->z;
                *to = zge::unit(*to);
                bool backwards = zge::dot(*to, velocity) < ee::Float(0.0f);
                zge::Vec4 &force = *vec4(rider->force);
                if (!(zge::angleAcos(ee::abs(zge::dot(*to, *normal))) < kHeadOn))
                {
                    force = *normal * (closing * *g_probePushGlancing);
                }
                else if (backwards)
                {
                    force = velocity * kThrownBack;
                    backwards = false;
                }
                else
                {
                    force = *normal * (closing * *g_probePushHeadOn);
                    // Along the face the way the rider goes.
                    *to = *level;
                    for (const zge::Vec4 *way : {&velocity, vec4(rider->forward), vec4(rider->up)})
                    {
                        const ee::Float wayAlong = zge::dot(*way, *to);
                        if (*g_probeAlongLeast < ee::abs(wayAlong))
                        {
                            if (wayAlong < ee::Float(0.0f))
                                zge::vec3Negate(&to->xyz(), &to->xyz());
                            break;
                        }
                    }
                    *to = *to * (closing * kAlongShare);
                    force = force + *to;
                }
                velocity = velocity + force;
                rider->speed = zge::vec3Length(&velocity.xyz());
                zge::Vec4 &forward = *vec4(rider->forward);
                zge::Vec4 &upRow = *vec4(rider->up);
                zge::Vec4 &right = *vec4(rider->right);
                forward = velocity;
                if (backwards)
                    zge::vec3Negate(&forward.xyz(), &forward.xyz());
                upRow = wheelRows.row(1);
                right = zge::cross(upRow, forward);
                right = zge::unit(right);
                forward = zge::cross(right, upRow);
                *matrix(rider->body) = *matrix(rider->right);
                for (WheelPose &each : rider->wheelPoses)
                {
                    *matrix(each.matrix) = *matrix(rider->right);
                    *vec4(each.velocity) = velocity;
                    each.speed = rider->speed;
                }
                return 1;
            }

            // Hard: a bail.
            if (impact->flags & collision::kImpactBounced)
            {
                velocity = *vec4(impact->riderVelocity);
                rider->bailKind = kBailBounced;
            }
            else
            {
                rider->bailKind = 0;
                addRideDistance(rider, closing);
                collision::touchObject(rider, hit, kTouchBail);
            }
            if (physicsState(rider) == kPhysicsGhostRide)
                setPhysicsState(rider, uint32_t(kPhysicsGhostRideTumble));
            else
                setPhysicsState(rider, uint32_t(kPhysicsBail));
            return 2;
        }
        return 0;
    }
    READABLE_FUNCTION(groundProbe, Verified);
}
