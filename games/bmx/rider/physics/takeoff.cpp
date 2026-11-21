// BMX XXX's take-off (physics state 1's enter): what is under the rider, the
// launch off a ramp's lip or a quarter pipe, off a grind, a stall or a lip
// trick, the alignment the air turns the rows through, the rider's air bodies
// woken, the sounds. DM2's (games/dm2/rider/physics/takeoff.cpp), read in BMX
// XXX's machine code: the ground below by groundContactBelow, a grind and a
// stall lifted along the rail's up row (a grind onto a ramp below only
// rolled), BMX XXX's trick flags, the quarter pipe's share by the face's tag
// itself.

#include "physics.h"

#include "../collision/collision.h"
#include "../game_calls.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/physics/takeoff.cpp.h"

namespace game::bmx::rider::physics
{
    namespace
    {
        using collision::EdgeRef;
        using collision::FaceRef;

        // The ground below (groundContactBelow's kinds): a plain face 0, a ramp's face with an edge 1 .. 3, a
        // kind 4 face (a rail's), an object (kContactObject).
        constexpr int32_t kBelowRampLast = 3;

        // Off the ground: the speed along the ground is capped by the take-off's pitch (more below 45 degrees
        // up): 0.0833 (0.35 x the face's launch tag / 100 on a kind 4 contact) times 1 + the sine of how far
        // the pitch is from 45 degrees, unless 30 degrees or more off.
        inline ee::Global<ee::Float, sym::g_launchSpeed> g_launchSpeed;   // 0.0833
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;         // 0.35
        inline ee::Global<ee::Float, sym::g_launchPercent> g_percent;     // 0.01
        constexpr int32_t kAngle45 = 0x2000;
        constexpr int32_t kAngle30 = 0x1555;
        // Off a stall or a grind: lifted along the rail's up row by wheel 0's height; the push along the forward
        // row (0.0125, not for a trick with kTrickRowsByRight) and on a ramp's face the roll turned over 20
        // frames by the up row's tilt times 0.0375; off a lip (contact kinds 8 and 9) the velocity 0.05 of the
        // forward row and the lip's, up by twice the gravity, the rows turned 60 degrees over the alignment's
        // frames.
        inline ee::Global<ee::Float, sym::g_lipLaunch> g_lipLaunch;     // 0.05
        inline ee::Global<ee::Float, sym::g_stallPush> g_stallPush;     // 0.0125
        inline ee::Global<ee::Float, sym::g_rollOffShare> g_rollShare;  // 0.0375
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;         // 0.0055
        inline ee::Global<int32_t, sym::g_alignFrames> g_alignFrames;   // 32
        inline ee::Global<int16_t, sym::g_alignFrames> g_alignFramesShort; // (its low half as the snap's frames)
        constexpr int16_t kRollFrames = 20;
        constexpr int32_t kAngle60 = 0x2AAA;
        constexpr int32_t kLipTurn = 0x1555; // 30 degrees (the lip trick's turn back)
        // A trick's flags (the grind's and stall's table): with kTrickRowsByRight the stall gives no push and no
        // roll; with kTrickHopsForward no alignment at all; with kTrickUntilted the edge's ring is not sounded.
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;
        constexpr uint32_t kTrickRowsByRight = 0x80u;
        constexpr uint32_t kTrickUntilted = 0x100u;
        constexpr uint32_t kTrickHopsForward = 0x800u;
        constexpr uint8_t kNoTrickHere = 0xFF;
        // Off a ramp's lip: within 55 degrees of level and the face's up within 55 degrees of the edge's normal
        // (kinds 1 and 3), or a quarter pipe (kind 2) within 2 of the edge and 3 of its plane.
        inline ee::Global<int16_t, sym::g_launchWithin> g_launchWithin; // 55 degrees
        inline ee::Global<int16_t, sym::g_lipSteepest> g_spinWithin;    // 50 degrees
        constexpr ee::Float kVertNear = 2.0f;
        constexpr ee::Float kVertPlaneNear = 3.0f;
        // A quarter pipe: the rider set 1.2 from the edge's plane (no higher than 0.5 above the edge), the
        // velocity into the plane taken away, 0.75 of it kept, up by the frames the original speed gives (at
        // least 37) at half gravity, the take-off's share (by the face's launch tag) back towards the plane;
        // the pitch turned to the forward row's mirror over the frames less 3, 0.9 of it.
        inline ee::Global<ee::Float, sym::g_vertOut> g_vertOut;     // 1.2
        inline ee::Global<ee::Float, sym::g_vertKeep> g_vertKeep;   // 0.85
        inline ee::Global<ee::Float, sym::g_vertPitch> g_vertPitch; // 0.9
        inline ee::Global<ee::Float[256], sym::g_vertShares> g_vertShares; // 0.5, 0.54, 0.57 ... by the tag
        constexpr ee::Float kVertKept(0.75f);
        constexpr ee::Float kVertUp(-3.0f);
        constexpr uint32_t kVertFramesLeast = 37;
        constexpr int32_t kVertPitchFramesLess = 3;
        // Off a ramp with the hop flags (kFlagHopUp, kFlagHopDrop: a hop over its edge): the rider held to the
        // edge's plane (within 1 / 26 of it), and lifted to 1.2 past the edge.
        inline ee::Global<ee::Float, sym::g_hopOut> g_hopOut;   // 1 / 26
        inline ee::Global<ee::Float, sym::g_hopPast> g_hopPast; // 1.2
        constexpr ee::Float kHopFallFrames = 26.0f;
        constexpr zge::Angle kHopTurn = 0x2E38;
        constexpr ee::Float kHopClear = 1.5f;
        constexpr ee::Float kHopProbe(0.5f);
        constexpr uint32_t kSpinFramesLeast = 0x1F;
        constexpr int32_t kSpinFramesLess = 0xF;
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp; // (0, -1, 0, 0)
        // The rider's air bodies' size (pushCornersClear's corners are its x and z).
        inline ee::Global<zge::Vec4, sym::g_cornerSide> g_airBodyHalfExtents;
        constexpr uint32_t kAirBodiesTwoSided = 1;

        // The physics states it comes from (the jump tables' 11).
        constexpr uint32_t kFromStates = 11;
        constexpr uint8_t kContactLipFirst = 8; // contact kinds 8 and 9: a lip

        constexpr uint32_t kSoundRampLaunch = 0x1F;
        constexpr uint32_t kSoundEdgeLaunch = 0x20;
        constexpr uint32_t kSoundVolume = 127;
        constexpr int32_t kAngle60Up = 0x2AAB;

        uint32_t trickFlags(uint8_t trick) { return (*g_trickFlags)[trick]; }

        // The rider lifted along the rail's up row by wheel 0's height stat (off a stall or a grind), the drawn
        // body with it.
        void liftOff(Rider *rider, zge::Vec4 &lift)
        {
            const ee::Float h = rider->frontWheelHeight;
            const zge::Vec4 &railUp = matrix(rider->snapRows)->row(1);
            lift.x = railUp.x * h;
            lift.y = railUp.y * h;
            lift.z = railUp.z * h;
            *vec4(rider->position) = *vec4(rider->position) + lift;
            *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + lift;
        }
        // The roll a ramp's face gives (the up row's tilt times the share), over 20 frames from the rows as
        // they are.
        void rollOff(Rider *rider)
        {
            *matrix(rider->alignFrom) = *matrix(rider->right);
            const int32_t tilt = zge::angleAcos(ee::abs(rider->up[1]));
            const int32_t roll = ee::toInt(ee::toFloat(tilt) * ee::Float(1.0f / 65536.0f) * *g_rollShare * ee::Float(65536.0f));
            rider->alignAngles[1] = static_cast<int16_t>(roll);
            if (rider->right[1] < ee::Float(0.0f))
                rider->alignAngles[1] = static_cast<int16_t>(-roll);
            rider->snapFrames = kRollFrames;
        }
        // The rows turned to `target` over the alignment's frames.
        void alignTo(Rider *rider, zge::Matrix &target)
        {
            *matrix(rider->alignFrom) = *matrix(rider->right);
            airRotationTrack(rider->right, &target.row(0).x, *g_alignFrames, rider->alignAngles);
            rider->snapFrames = *g_alignFramesShort;
        }
        // A grind asked for on the way off: taken (the grind or the stall) when the state change gives one.
        bool grindTaken(Rider *rider)
        {
            const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
            return state == kPhysicsGrind || state == kPhysicsStall;
        }
    }

    // State 1's enter, from `from` (the state before); returns `next` (the state change's result), or the state a
    // grind asked for took. The wheels keep the contact; what is under the rider (groundContactBelow) is the
    // ground below; the drawn offset goes into the position, the angles are cleared; then by the state left:
    //  - the ground (0), the nose wheelie and the manual (9, 10): off a ramp's face (kinds 1 .. 3) its edge
    //    decides: a grind asked for, a quarter pipe (kind 2: launched up the edge's plane, the pitch turned
    //    over), a lip's hop (the hop flags), else the lip test and a spin's alignment (kFlagLipLanding); off flat
    //    ground a grind asked for onto a ramp below, the speed along the ground capped by the pitch and the rows
    //    aligned upright;
    //  - a stall (6) or a grind (4): lifted off along the rail, pushed, rolled by a ramp's tilt below (off a lip,
    //    kinds 8 and 9: launched off it);
    //  - a lip trick (8): turned back 30 degrees.
    // Then the rider's air bodies are woken, and the ramp launch and the edge's ring sound last.
    uint32_t airEnter(Rider *rider, uint32_t from, uint32_t next)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4> rayTo;
        ee::Local<zge::Vec4> edgeNormal;  // the edge's plane's normal, towards the rider
        ee::Local<zge::Vec4> nearest;     // the edge's point nearest the rider
        ee::Local<zge::Matrix> target;    // the rows to align to: its up row the world's up
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Vec4> edgePlane;
        ee::Local<int32_t> belowKind;     // the ground below: its kind,
        ee::Local<FaceRef> belowFace;     // its face,
        ee::Local<EdgeRef> belowEdge;     // the face's edge,
        ee::Local<uint32_t> belowObject;  // the object
        zge::Vec4 &targetUp = target->row(1);
        zge::Vec4 &targetForward = target->row(2);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const zge::Vec4 &forward = *vec4(rider->forward);
        const zge::Vec4 &up = *vec4(rider->up);

        const void *below = nullptr; // the ground below's record
        setWheelContact(rider, 0u, rider->contact, uint32_t(rider->contactKind), rider->position);
        setWheelContact(rider, 1u, rider->contact, uint32_t(rider->contactKind), rider->position);
        clearFlag(rider, kFlagWeakHop);
        clearFlag(rider, kFlagLipLanding);
        clearFlag(rider, kFlagOffRailIntoAir);
        rider->edgeHeight = 0.0f;
        if (groundContactBelow(rider, belowKind, belowFace, belowEdge, belowObject) != 0)
        {
            below = belowFace;
            if (*belowKind == kContactObject)
            {
                below = belowObject;
                setContact(rider, belowObject, uint32_t(kContactObject));
            }
        }

        // The drawn offset into the position; the angles cleared.
        *vec4(rider->position) = *vec4(rider->position) + *vec4(rider->rootOffset);
        *vec4(rider->rootOffset) = zge::kZero;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        rider->bodyPitch = 0;
        rider->alignAngles[0] = 0;
        rider->alignAngles[1] = 0;
        rider->alignAngles[2] = 0;
        rider->snapFrames = 0;
        rider->landingTravelAngle = 0;
        zge::matrixIdentity(target);
        targetUp = *g_worldUp;
        bool alignUpright = false;
        clearFlag(rider, kFlagSnapAlign);
        if (zge::dot(velocity, forward) < ee::Float(0.0f) && (from == kPhysicsGround || from == kPhysicsManual || from == kPhysicsNoseWheelie))
            setFlag(rider, kFlagTakeoffBack);
        else
            clearFlag(rider, kFlagTakeoffBack);

        const bool known = from < kFromStates;
        const bool belowRamp = static_cast<uint32_t>(*belowKind) <= uint32_t(kBelowRampLast) && *belowKind != 0;
        if (known)
        {
            switch (from)
            {
            case kPhysicsGrind:
                // Off a grind: rolled by a ramp below, else lifted off and aligned upright.
                if (belowRamp)
                    rollOff(rider);
                else
                {
                    liftOff(rider, *scratch);
                    alignUpright = true;
                }
                break;
            case kPhysicsStall:
                liftOff(rider, *scratch);
                if (static_cast<uint32_t>(rider->contactKind - kContactLipFirst) < 2)
                {
                    // Off a lip: launched along the forward row and the lip's (the snap target's up row), up by
                    // twice the gravity; the rows turned 60 degrees about the upright towards the lean.
                    const ee::Float g = *g_gravity;
                    const ee::Float share = *g_lipLaunch;
                    const zge::Vec4 &lip = matrix(rider->snapRows)->row(1);
                    rider->velocity[0] = rider->forward[0] * share + lip.x * share;
                    rider->velocity[1] = rider->forward[1] * share + lip.y * share + (g + g);
                    rider->velocity[2] = rider->forward[2] * share + lip.z * share;
                    target->row(0) = zge::cross(targetUp, forward);
                    target->row(0) = zge::unit(target->row(0));
                    targetForward = zge::cross(target->row(0), targetUp);
                    zge::matrixIdentity(turn);
                    zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(ee::Float(0.0f) < rider->right[1] ? kAngle60 : -kAngle60));
                    zge::matrixMultiply(target, target, turn);
                    zge::matrixIdentity(turn);
                    zge::matrixRotateX(turn, turn, 0);
                    zge::matrixMultiply(target, target, turn);
                    alignTo(rider, *target);
                    break;
                }
                if ((trickFlags(rider->trick) & kTrickRowsByRight) == 0)
                {
                    const ee::Float push = *g_stallPush;
                    rider->velocity[0] = rider->velocity[0] + rider->forward[0] * push;
                    rider->velocity[1] = rider->velocity[1] + rider->forward[1] * push;
                    rider->velocity[2] = rider->velocity[2] + rider->forward[2] * push;
                }
                {
                    const uint32_t flags = trickFlags(rider->trick);
                    if ((flags & kTrickHopsForward) != 0)
                        break;
                    if (!belowRamp)
                        alignUpright = true;
                    else if ((flags & kTrickRowsByRight) == 0)
                        rollOff(rider);
                }
                break;
            case kPhysicsLipTrick:
            {
                // Off a lip trick: turned back 30 degrees (the other way with kFlagMirrorSide, and rolling
                // backwards).
                *target = *matrix(rider->right);
                zge::matrixIdentity(turn);
                int32_t back = hasFlag(rider, kFlagMirrorSide) ? -kLipTurn : kLipTurn;
                if (zge::dot(velocity, forward) < ee::Float(0.0f))
                    back = -back;
                zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(back));
                zge::matrixMultiply(target, target, turn);
                alignTo(rider, *target);
                break;
            }
            case kPhysicsGround:
            case kPhysicsNoseWheelie:
            case kPhysicsManual:
            {
                const uint8_t kind = rider->contactKind;
                if (kind < 4 && kind != 0)
                {
                    // Off a ramp's face: its edge decides.
                    const EdgeRef edge = collision::faceLinkedEdge(contactRef(rider));
                    if (collision::edgePlane(edge, &edgePlane->x) == 0)
                    {
                        alignUpright = true;
                        setFlag(rider, kFlagHopUp);
                        break;
                    }
                    *edgeNormal = *edgePlane;
                    ee::Float fromPlane = planeDistance(rider->position, &edgePlane->x);
                    if (fromPlane < ee::Float(0.0f))
                    {
                        fromPlane = -fromPlane;
                        zge::vec3Negate(&edgeNormal->xyz(), &edgeNormal->xyz());
                    }
                    closestOnRail(rider->position, edge, 0, &nearest->x);
                    *scratch = *nearest - *vec4(rider->position);
                    rider->edgeHeight = rider->position[1] - nearest->y;
                    bool launch = false;
                    if (rider->velocity[1] < ee::Float(0.0f) && zge::angleAsin(ee::abs(rider->right[1])) < *g_launchWithin)
                    {
                        const uint8_t contactKind = rider->contactKind;
                        if (contactKind == 1 || contactKind == 3)
                            launch = zge::angleAcos(zge::dot(up, *edgeNormal)) < *g_launchWithin;
                        if (!launch && rider->contactKind == 2)
                            launch = rider->edgeHeight < kVertNear && fromPlane < kVertPlaneNear;
                    }
                    if (!launch)
                    {
                        alignUpright = true;
                        setFlag(rider, kFlagHopUp);
                        break;
                    }

                    if (hasFlag(rider, kFlagGrindAsked) && grindTaken(rider))
                        return physicsState(rider);
                    if (rider->contactKind == 2)
                    {
                        // A quarter pipe: up its edge's plane.
                        const ee::Float out = *g_vertOut;
                        const ee::Float toPlane = out - fromPlane;
                        const ee::Float z = rider->position[2] + edgeNormal->z * toPlane;
                        const ee::Float x = rider->position[0] + edgeNormal->x * toPlane;
                        rider->position[1] = ee::min(rider->position[1], nearest->y + ee::Float(0.5f));
                        rider->position[0] = x;
                        rider->position[2] = z;
                        *vec4(rider->drawnPosition) = *vec4(rider->position);
                        targetUp.y = kVertUp;
                        targetUp.x = -edgeNormal->x;
                        targetUp.z = -edgeNormal->z;
                        targetUp = zge::unit(targetUp);
                        const ee::Float kept = -(rider->velocity[1] * *g_vertKeep);
                        const uint32_t rising = static_cast<uint32_t>(ee::toInt((kept + kept) / *g_gravity));
                        rider->velocity[1] = 0.0f;
                        const uint32_t frames = kVertFramesLeast - 1 < rising ? rising : kVertFramesLeast;
                        {
                            // (along the plane only: the rise is set below)
                            const ee::Float into = zge::dot(velocity, *edgeNormal);
                            rider->velocity[0] = rider->velocity[0] - edgeNormal->x * into;
                            rider->velocity[2] = rider->velocity[2] - edgeNormal->z * into;
                        }
                        velocity = velocity * kVertKept;
                        const ee::Float g = *g_gravity;
                        rider->velocity[1] = -(ee::toFloat(int32_t(frames)) * g * ee::Float(0.5f));
                        const uint32_t tag = collision::faceTagByte9(contactRef(rider));
                        const ee::Float back = (out + (*g_vertShares)[tag]) / ee::toFloat(int32_t(frames));
                        const int32_t pitchFrames = int32_t(frames) - kVertPitchFramesLess;
                        rider->velocity[2] = rider->velocity[2] + -edgeNormal->z * back;
                        rider->velocity[0] = rider->velocity[0] + -edgeNormal->x * back;
                        const ee::Float speed = zge::vec3Length(&velocity.xyz());
                        scratch->x = rider->forward[0];
                        scratch->z = rider->forward[2];
                        scratch->y = -rider->forward[1];
                        rider->speed = speed;
                        rider->alignAngles[1] = 0;
                        rider->alignAngles[2] = 0;
                        *matrix(rider->alignFrom) = *matrix(rider->right);
                        const int32_t mirror = zge::angleAcos(zge::dot(*scratch, forward));
                        const int32_t pitch = ee::toInt(ee::toFloat(static_cast<int16_t>(-mirror)) * ee::Float(1.0f / 65536.0f) * *g_vertPitch /
                                                        ee::toFloat(pitchFrames) * ee::Float(65536.0f));
                        rider->alignAngles[0] = static_cast<int16_t>(pitch);
                        if (ee::Float(0.0f) <= zge::dot(velocity, forward))
                            rider->alignAngles[0] = static_cast<int16_t>(-pitch);
                        rider->snapFrames = static_cast<int16_t>(pitchFrames);
                        break;
                    }

                    if (hasFlag(rider, kFlagHopUp) || hasFlag(rider, kFlagHopDrop))
                    {
                        // A hop over the edge: only when there is room (nothing half a unit past the edge and down,
                        // the rider within 1.5 above it next frame).
                        *scratch = *nearest;
                        scratch->x = scratch->x + edgeNormal->x * kHopProbe;
                        scratch->z = scratch->z + edgeNormal->z * kHopProbe;
                        scratch->y = scratch->y + -kHopProbe;
                        *rayTo = *scratch;
                        rayTo->x = rayTo->x - edgeNormal->x;
                        rayTo->z = rayTo->z - edgeNormal->z;
                        if (collision::rayCast(&scratch->x, &rayTo->x) != nullptr ||
                            !(rider->position[1] + rider->velocity[1] - nearest->y < kHopClear))
                        {
                            clearFlag(rider, kFlagHopUp);
                            clearFlag(rider, kFlagHopDrop);
                        }
                    }
                    if (hasFlag(rider, kFlagHopUp) || hasFlag(rider, kFlagHopDrop))
                    {
                        // Held to the edge's plane, falling at most 1 / 26 of a frame's gravity times 26.
                        const ee::Float out = *g_hopOut;
                        if (hasFlag(rider, kFlagHopDrop))
                            velocity = zge::kZero;
                        else
                        {
                            // Turned along the edge: the velocity across it kept, along it 0.4 of a turn off the
                            // edge's normal.
                            *scratch = zge::cross(*edgeNormal, *g_worldUp);
                            *scratch = *scratch * zge::dot(velocity, *scratch);
                            velocity = velocity - *scratch;
                            rider->speed = zge::vec3Length(&velocity.xyz());
                            const ee::Float nx = -edgeNormal->x;
                            rider->velocity[0] = nx * (rider->speed * zge::angleCos(kHopTurn));
                            const ee::Float nz = -edgeNormal->z;
                            rider->velocity[2] = nz * (rider->speed * zge::angleCos(kHopTurn));
                            velocity = velocity + *scratch;
                        }
                        const ee::Float fall = -(*g_gravity * kHopFallFrames);
                        rider->velocity[1] = rider->velocity[1] < fall ? rider->velocity[1] : fall;
                        const ee::Float away = -zge::dot(velocity, *edgeNormal);
                        if (away < out)
                        {
                            const ee::Float more = out - away;
                            rider->velocity[0] = rider->velocity[0] + -edgeNormal->x * more;
                            rider->velocity[2] = rider->velocity[2] + -edgeNormal->z * more;
                        }
                        rider->speed = zge::vec3Length(&velocity.xyz());
                        *scratch = *nearest;
                        const ee::Float past = *g_hopPast;
                        scratch->y = scratch->y + edgeNormal->y * past;
                        scratch->x = scratch->x + edgeNormal->x * past;
                        scratch->z = scratch->z + edgeNormal->z * past;
                        if (scratch->y < rider->position[1])
                        {
                            *vec4(rider->position) = *scratch;
                            *vec4(rider->drawnPosition) = *scratch;
                        }
                        alignUpright = true;
                        break;
                    }

                    // Off a lip: the lip test, and a spin when the up row is near the edge's normal and the air
                    // long enough.
                    *belowEdge = collision::faceLinkedEdge(contactRef(rider));
                    lipTest(rider, *belowEdge, 1);
                    if (rider->contactKind == 3)
                    {
                        rider->velocity[0] = 0.0f;
                        rider->velocity[2] = 0.0f;
                    }
                    if (zge::angleAcos(zge::dot(up, *edgeNormal)) < *g_spinWithin && ee::Float(0.0f) <= zge::dot(velocity, forward))
                    {
                        const ee::Float rise = -rider->velocity[1];
                        const uint32_t airFrames = static_cast<uint32_t>(ee::toInt((rise + rise) / *g_gravity));
                        if (airFrames >= kSpinFramesLeast)
                        {
                            // The rows turned level with the world over the air less 15 frames, as a turn about
                            // the up row.
                            const int32_t frames = int32_t(airFrames) - kSpinFramesLess;
                            *matrix(rider->alignFrom) = *matrix(rider->right);
                            rider->alignAngles[0] = 0;
                            rider->alignAngles[2] = 0;
                            zge::Matrix &level = *turn;
                            level = *matrix(rider->right);
                            level.row(2) = zge::cross(level.row(1), *g_worldUp);
                            level.row(2) = zge::unit(level.row(2));
                            level.row(0) = zge::cross(level.row(1), level.row(2));
                            const ee::Float a = zge::dot(forward, level.row(2));
                            const ee::Float b = -zge::dot(forward, level.row(0));
                            scratch->x = level.row(2).x * a + level.row(0).x * b;
                            scratch->y = level.row(2).y * a + level.row(0).y * b;
                            scratch->z = level.row(2).z * a + level.row(0).z * b;
                            *scratch = zge::unit(*scratch);
                            const int32_t perFrame = zge::angleAcos(zge::dot(*scratch, forward)) / frames;
                            rider->alignAngles[1] = static_cast<int16_t>(-perFrame);
                            if (ee::Float(0.0f) < rider->right[1])
                                rider->alignAngles[1] = static_cast<int16_t>(perFrame);
                            rider->snapFrames = static_cast<int16_t>(frames);
                            setFlag(rider, kFlagSnapAlign);
                        }
                    }
                    setFlag(rider, kFlagLipLanding);
                    break;
                }

                // Off the ground (a plain face, a kind 4 face): onto a ramp below at once when a grind is asked
                // for; else aligned upright.
                if (*belowKind == 0)
                    alignUpright = true;
                else if (belowRamp)
                {
                    if (hasFlag(rider, kFlagGrindAsked))
                    {
                        setContact(rider, below, static_cast<uint32_t>(*belowKind));
                        if (grindTaken(rider))
                            return physicsState(rider);
                    }
                }
                else
                    alignUpright = true;
                const uint8_t now = rider->contactKind;
                ee::Float most;
                if (now == 4)
                    most = *g_topSpeed * (ee::toFloat(int32_t(collision::faceTagByte9(contactRef(rider)))) * *g_percent);
                else
                    most = *g_launchSpeed;
                // The take-off's pitch: the forward row's (in a wheelie the balancing wheel's), turned round when
                // rolling backwards.
                ee::Float slope;
                if (from == kPhysicsNoseWheelie)
                    slope = matrix(rider->wheelPoses[0].matrix)->row(2).y;
                else if (from == kPhysicsManual)
                    slope = matrix(rider->wheelPoses[1].matrix)->row(2).y;
                else
                    slope = rider->forward[1];
                int32_t pitch = zge::angleAsin(-slope);
                if (zge::dot(forward, velocity) < ee::Float(0.0f))
                    pitch = static_cast<int16_t>(-pitch);
                int32_t off = pitch - kAngle45;
                if (off < 0)
                    off = -off;
                off = static_cast<int16_t>(off);
                if (off >= kAngle30)
                    break;
                *scratch = velocity;
                scratch->y = 0.0f;
                const ee::Float limit = zge::angleSin(static_cast<zge::Angle>(off)) * most + most;
                const ee::Float along = zge::vec3Length(&scratch->xyz());
                if (limit < along)
                {
                    const ee::Float share = limit / along;
                    rider->velocity[0] = scratch->x * share;
                    rider->velocity[2] = scratch->z * share;
                }
                break;
            }
            default:
                break;
            }
        }
        // Aligned upright: the rows turned to the world's up about the forward row over the alignment's frames.
        if (alignUpright)
        {
            target->row(0) = zge::cross(targetUp, forward);
            target->row(0) = zge::unit(target->row(0));
            targetForward = zge::cross(target->row(0), targetUp);
            alignTo(rider, *target);
        }
        wakeRiderTumble(rider, ee::Ptr<zge::Vec4>(g_airBodyHalfExtents.get()), kAirBodiesTwoSided);

        // The take-off's sounds by the state left.
        if (!known)
            return next;
        if (from == kPhysicsGround || from == kPhysicsNoseWheelie || from == kPhysicsManual)
        {
            const uint8_t kind = rider->contactKind;
            if (kind >= 4 || kind == 0)
                return next;
            // A ramp's launch: steeper than 60 degrees, the forward row within 45 degrees of level.
            if (zge::angleAcos(ee::abs(rider->up[1])) < kAngle60Up)
                return next;
            if (zge::angleAcos(ee::abs(rider->forward[1])) >= kAngle45)
                return next;
            soundEvent(kSoundRampLaunch, rider->position, static_cast<uint32_t>(surfaceSoundFlags(0u, 0u, 1u, 2u)), kSoundVolume, 0u);
        }
        else if (from == kPhysicsGrind || from == kPhysicsStall)
        {
            if (rider->contactKind != 5)
                return next;
            const uint32_t wall = collision::edgeWallKind(contactRef(rider));
            const uint32_t surface = wall == 1 ? 7u : wall == 2 ? 2u : 0u;
            if (rider->trick != kNoTrickHere && (trickFlags(rider->trick) & kTrickUntilted) != 0)
                return next;
            soundEvent(kSoundEdgeLaunch, rider->position, static_cast<uint32_t>(surfaceSoundFlags(0u, 0u, 2u, surface)), kSoundVolume, 0u);
        }
        return next;
    }
    READABLE_FUNCTION(airEnter, Verified);
}
