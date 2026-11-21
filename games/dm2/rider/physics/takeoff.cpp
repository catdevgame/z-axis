// DM2's take-off (physics state 1's enter, g_physicsEnters[1]): what is under the
// rider, the launch off a ramp's lip or a quarter pipe, off a grind, a stall
// or a lip trick, the alignment the air turns the rows through, the sounds.
//
// Counterparts: DM2 GameCube 0x8007A920;
// Xbox 0x056FA0; BMX XXX PS2 airEnter (low).

#include "../actions/actions.h"
#include "physics.h"

#include "../collision/collision.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/physics/takeoff.cpp.h"

namespace game::dm2::rider::physics
{
    void lipTest(Rider *rider, collision::EdgeRef edge, int32_t anyAngle);
    void airRotationTrack(const ee::Float *current, const ee::Float *previous, int32_t frames, int16_t *angles);

    namespace
    {
        using collision::EdgeRef;
        using collision::FaceRef;

        constexpr uint32_t kHitFaceFlag = 0x1u;
        constexpr uint32_t kHitObjectFlag = 0x2u;

        // The ray down from one unit above the rider.
        constexpr ee::Float kGroundRay = 100.0f;
        // Under the rider (the contact the air keeps): none, a plain face
        // (0), a ramp's face with an edge (its kind 1 .. 3), a kind 4 face
        // (a rail's), an object (8).
        constexpr int32_t kBelowNone = -1;
        constexpr int32_t kBelowRampLast = 3;
        constexpr int32_t kBelowRail = 4;
        constexpr int32_t kBelowObject = 8;

        // Off the ground: the speed along the ground is capped by the take-off's
        // pitch (more below 45 degrees up): 0.0833 (0.35 x the face's
        // launch tag / 100 on a kind 4 contact) times 1 + the sine of how
        // far the pitch is from 45 degrees, unless 30 degrees or more off.
        inline ee::Global<ee::Float, sym::g_launchSpeed> g_launchSpeed;   // 0.0833
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;      // 0.35
        inline ee::Global<ee::Float, sym::g_percent> g_percent;       // 0.01
        constexpr int32_t kAngle45 = 0x2000;
        constexpr int32_t kAngle30 = 5461;
        // Off a stall or a grind: the push along the forward row (0.0125,
        // not for a trick with flag 0x80) and on a ramp's face the roll
        // turned over 20 frames by the up row's tilt times 0.0375; off a
        // lip (contact kinds 7 and 8) the velocity 0.05 of the forward row
        // and the lip's, up by twice the gravity, the rows turned 60 degrees
        // over the alignment's frames.
        inline ee::Global<ee::Float, sym::g_lipLaunch> g_lipLaunch;     // 0.05
        inline ee::Global<ee::Float, sym::g_stallPush> g_stallPush;     // 0.0125
        inline ee::Global<ee::Float, sym::g_stallRoll> g_stallRoll;     // 0.0375
        inline ee::Global<ee::Float, sym::g_grindRoll> g_grindRoll;     // 0.0375
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;       // 0.0055
        inline ee::Global<int32_t, sym::g_alignFrames> g_alignFrames;     // 32
        inline ee::Global<int16_t, sym::g_alignFrames> g_alignFramesShort; // (its low half as the snap's frames)
        constexpr int16_t kRollFrames = 20;
        constexpr int32_t kAngle60 = 10922;
        constexpr int32_t kLipTurn = 5461; // 30 degrees (the lip trick's turn back)
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickFlags; // a trick's flags
        constexpr uint16_t kTrickKeepsSpeed = 0x80u;
        constexpr uint16_t kTrickQuiet = 0x100u;
        constexpr uint8_t kNoTrickHere = 0xFF;
        // Off a ramp's lip: within 55 degrees of level and the face's up
        // within 55 degrees of the edge's normal (kinds 1 and 3), or a
        // quarter pipe (kind 2) within 2 of the edge and 3 of its plane.
        inline ee::Global<int16_t, sym::g_launchWithin> g_launchWithin;    // 55 degrees
        inline ee::Global<int16_t, sym::g_lipSteepest> g_spinWithin;      // 50 degrees
        constexpr ee::Float kVertNear = 2.0f;
        constexpr ee::Float kVertPlaneNear = 3.0f;
        // A quarter pipe: the rider set 1.2 from the edge's plane (no higher
        // than 0.5 above the edge), the velocity into the plane taken away,
        // 0.75 of it kept, up by the frames the original speed gives (at
        // least 37) at half gravity, the take-off's share (vertShares by the
        // face's launch tag) back towards the plane; the pitch turned to
        // the forward row's mirror over the frames less 3, 0.9 of it.
        inline ee::Global<ee::Float, sym::g_vertOut> g_vertOut;       // 1.2
        inline ee::Global<ee::Float, sym::g_vertKeep> g_vertKeep;      // 0.85
        inline ee::Global<ee::Float, sym::g_vertPitch> g_vertPitch;     // 0.9
        inline ee::Float vertShare(uint32_t tag) { return ee::Ptr<ee::Float>(sym::vertShares)[tag / 10u]; } // 0.5, 0.75, 1, 1.2 ... by the tag's tens
        constexpr uint32_t kVertFramesLeast = 37;
        // Off a ramp with flags 0x30000 (a hop over its edge): the rider held
        // to the edge's plane (within 1 / 26 of it), and lifted to 1.2 past
        // the edge.
        inline ee::Global<ee::Float, sym::g_hopOut> g_hopOut;        // 1 / 26
        inline ee::Global<ee::Float, sym::g_hopPast> g_hopPast;       // 1.2
        constexpr ee::Float kHopFallFrames = 26.0f;
        constexpr int32_t kHopTurn = 11832;
        constexpr ee::Float kHopClear = 1.5f;
        constexpr int32_t kSpinFramesLeast = 0x1F;
        constexpr int32_t kSpinFramesLess = 0xF;
        inline ee::Global<zge::Vec4, sym::down> g_worldUp; // (0, -1, 0, 0)

        // Rider::flags.
        constexpr uint32_t kFlagGrindAsked = (1u << 20);
        constexpr uint32_t kFlagAirSpin = 0x4000u;    // airRotate turns by the alignment's angles directly
        constexpr uint32_t kFlagOffLip = 0x8000u;
        constexpr uint32_t kFlagOffRamp = 0x10000u;
        constexpr uint32_t kFlagHopRamp = 0x20000u;
        constexpr uint32_t kFlagsHop = 0x30000u;
        constexpr uint32_t kFlagsClearedOnTakeoff = 0x20000000u | 0x8000u;

        // The physics states it comes from (the original's first switch table
        // has a case for each).
        constexpr uint32_t kFromStates = 11;

        constexpr int32_t kSoundRampLaunch = 18;
        constexpr int32_t kSoundEdgeLaunch = 19;
        constexpr int32_t kSoundVolume = 127;

        uint16_t trickFlags(uint8_t trick) { return (*g_trickFlags)[trick]; }

        // rayCast as a guest call: the scale after it carries what it left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline int32_t rayHitsByCall(const ee::Float *from, const ee::Float *to) { return ee::game<int32_t>(sym::rayCast, from, to); }

        // The rider lifted along its up row by wheel 0's height stat (off a
        // stall or a grind), the drawn body with it.
        void liftOff(Rider *rider, zge::Vec4 &lift)
        {
            const ee::Float h = rider->frontWheelHeight;
            const ee::Float y = rider->up[1] * h;
            const ee::Float z = rider->up[2] * h;
            lift.x = rider->up[0] * h;
            lift.y = y;
            lift.z = z;
            *vec4(rider->position) = *vec4(rider->position) + lift;
            *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + lift;
        }
        // The roll a ramp's face gives (the up row's tilt times share),
        // over 20 frames from the rows as they are.
        void rollOff(Rider *rider, ee::Float share)
        {
            engine::copyMatrix(rider->alignFrom, rider->right);
            const int32_t tilt = zge::angleAcos(ee::abs(rider->up[1]));
            const bool left = rider->right[1] < ee::Float(0.0f);
            const int32_t roll = ee::toInt(ee::toFloat(tilt) * ee::Float(1.0f / 65536.0f) * share * ee::Float(65536.0f));
            rider->alignAngles[1] = static_cast<int16_t>(roll);
            if (left)
                rider->alignAngles[1] = static_cast<int16_t>(-roll);
            rider->snapFrames = kRollFrames;
        }
        // The rows turned to `target` over the alignment's frames.
        void alignTo(Rider *rider, const ee::Float *target)
        {
            engine::copyMatrix(rider->alignFrom, rider->right);
            airRotationTrack(rider->right, target, *g_alignFrames, rider->alignAngles);
            rider->snapFrames = *g_alignFramesShort;
        }
    }

    // State 1's enter, from `from` (the state before); returns `next` (the
    // state change's result), or the state a grind asked for took. What is
    // under the rider (a ray 100 down) becomes the wheels' contact; the
    // drawn offset goes into the position, the angles are cleared; then by
    // the state left:
    //  - the ground (0), the nose wheelie and the manual (9, 10): off a ramp's face (kinds
    //    1 .. 3) its edge decides: a grind asked for, a quarter pipe (kind
    //    2: launched up the edge's plane, the pitch turned over), a lip's
    //    hop (flags 0x30000), else the lip test and a spin's alignment
    //    (kFlagOffLip); off flat ground the speed along the ground capped by
    //    the pitch and the rows aligned upright;
    //  - a stall (6) or a grind (4): lifted off, pushed, rolled by a
    //    ramp's tilt (off a lip, kinds 7 and 8: launched off it);
    //  - a lip trick (8): turned back 30 degrees.
    // The ramp launch and the edge's ring sound last. GameCube 0x8007A920.
    //
    // The two scales carry what the last call left, as airGravity's do.
    uint32_t airEnter(Rider *rider, uint32_t from, uint32_t next)
    {
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4> rayTo;
        ee::Local<zge::Vec4> edgeNormal;  // the edge's plane's normal, towards the rider
        ee::Local<zge::Vec4> nearest;     // the edge's point nearest the rider
        ee::Local<zge::Matrix> target;    // the rows to align to: its up row the world's up
        ee::Local<zge::Matrix> turn;
        ee::Local<zge::Vec4> edgePlane;
        ee::Local<zge::Vec4> planeRead;
        ee::Local<zge::Vec4> hitPoint;
        ee::Local<collision::FaceRef> belowFace;  // the contact records: a face,
        ee::Local<uint32_t> belowObject;          // an object
        zge::Vec4 &targetUp = target->row(1);
        zge::Vec4 &targetForward = target->row(2);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const zge::Vec4 &forward = *vec4(rider->forward);
        const zge::Vec4 &up = *vec4(rider->up);

        int32_t below = kBelowNone;
        const void *contact = nullptr; // the record
        *belowFace = 0;
        *belowObject = 0;
        setWheelContact(rider, 0u, rider->contact, uint32_t(rider->contactKind), rider->position);
        setWheelContact(rider, 1u, rider->contact, uint32_t(rider->contactKind), rider->position);
        rider->edgeHeight = 0.0f;

        // What is under the rider: a ray from a unit above, 100 down.
        {
            const ee::Float x = rider->position[0] + rider->up[0];
            const ee::Float z = rider->position[2] + rider->up[2];
            const ee::Float y = rider->position[1] + rider->up[1];
            rayTo->x = x;
            rayTo->z = z;
            rider->flags &= ~kFlagsClearedOnTakeoff;
            scratch->x = x;
            rayTo->y = y + kGroundRay;
            scratch->y = y;
            scratch->z = z;
        }
        if (const collision::RayHit *const hit = collision::rayCast(&scratch->x, &rayTo->x); hit != nullptr)
        {
            collision::hitPoint(hit, &hitPoint->x);
            const FaceRef face = collision::hitFace(hit);
            rider->groundHeight = hitPoint->y - scratch->y; // (the height above it)
            if ((collision::hitFlags(hit) & kHitFaceFlag) != 0 && face != 0)
            {
                const uint32_t kind = collision::faceSurfaceKind(face);
                *belowFace = face;
                contact = belowFace;
                if (kind < 4 && kind != 0)
                {
                    const uint32_t rampKind = collision::faceSurfaceKind(face);
                    below = collision::faceLinkedEdge(face) == 0 ? 0 : static_cast<int32_t>(rampKind);
                }
                else
                    below = kind == 4 ? kBelowRail : 0;
            }
            else if ((collision::hitFlags(hit) & kHitObjectFlag) != 0)
            {
                below = kBelowObject;
                *belowObject = collision::hitObject(hit);
                contact = belowObject;
                setContact(rider, belowObject, uint32_t(kBelowObject));
            }
        }

        // The drawn offset into the position; the angles cleared.
        {
            const ee::Float y = rider->position[1] + rider->rootOffset[1];
            const ee::Float z = rider->position[2] + rider->rootOffset[2];
            rider->position[0] = rider->position[0] + rider->rootOffset[0];
            rider->position[1] = y;
            rider->position[2] = z;
        }
        setOrigin(rider->rootOffset);
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        rider->bodyPitch = 0;
        rider->alignAngles[0] = 0;
        rider->alignAngles[1] = 0;
        rider->alignAngles[2] = 0;
        rider->snapFrames = 0;
        rider->landingTravelAngle = 0;
        targetUp = *g_worldUp;
        bool alignUpright = false;
        rider->flags &= ~kFlagAirSpin;
        if (zge::dot(velocity, forward) < ee::Float(0.0f) && (from == kPhysicsGround || from == kPhysicsManual || from == kPhysicsNoseWheelie))
            rider->flags |= kFlagTakeoffBack;
        else
            rider->flags &= ~kFlagTakeoffBack;

        const bool known = from < kFromStates;
        if (known)
        {
            switch (from)
            {
            case kPhysicsStall:
                liftOff(rider, *scratch);
                if (static_cast<uint32_t>(rider->contactKind - 7) < 2)
                {
                    // Off a lip: launched along the forward row and the
                    // lip's (the snap target's up row), up by twice the
                    // gravity; the rows turned 60 degrees about the upright
                    // towards the lean.
                    const ee::Float g = *g_gravity;
                    const ee::Float share = *g_lipLaunch;
                    const zge::Vec4 &lip = matrix(rider->snapRows)->row(1);
                    const ee::Float y = rider->forward[1] * share + lip.y * share + (g + g);
                    const ee::Float z = rider->forward[2] * share + lip.z * share;
                    rider->velocity[0] = rider->forward[0] * share + lip.x * share;
                    rider->velocity[1] = y;
                    rider->velocity[2] = z;
                    target->row(0) = zge::cross(targetUp, forward);
                    target->row(0) = zge::unit(target->row(0));
                    targetForward = zge::cross(target->row(0), targetUp);
                    zge::matrixIdentity(turn);
                    zge::matrixRotateY(turn, turn, ee::Float(0.0f) < rider->right[1] ? kAngle60 : -kAngle60);
                    engine::matrixProduct(target, target, turn);
                    zge::matrixIdentity(turn);
                    zge::matrixRotateX(turn, turn, 0);
                    engine::matrixProduct(target, target, turn);
                    alignTo(rider, &target->m[0][0]);
                    break;
                }
                else
                {
                    const uint16_t flags = trickFlags(rider->trick);
                    if ((flags & kTrickKeepsSpeed) == 0)
                    {
                        const ee::Float push = *g_stallPush;
                        const ee::Float y = rider->velocity[1] + rider->forward[1] * push;
                        const ee::Float z = rider->velocity[2] + rider->forward[2] * push;
                        rider->velocity[0] = rider->velocity[0] + rider->forward[0] * push;
                        rider->velocity[1] = y;
                        rider->velocity[2] = z;
                    }
                    if (static_cast<uint32_t>(below) < 4 && below != 0 && (trickFlags(rider->trick) & kTrickKeepsSpeed) == 0)
                        rollOff(rider, *g_stallRoll);
                }
                break;
            case kPhysicsGrind:
                liftOff(rider, *scratch);
                if (static_cast<uint32_t>(below) < 4 && below != 0)
                    rollOff(rider, *g_grindRoll);
                break;
            case kPhysicsLipTrick:
            {
                // Off a lip trick: turned back 30 degrees (the other way
                // with kFlagMirrorSide, and rolling backwards).
                engine::copyMatrix(target, rider->right);
                zge::matrixIdentity(turn);
                int32_t back = (rider->flags & kFlagMirrorSide) != 0 ? -kLipTurn : kLipTurn;
                if (zge::dot(velocity, forward) < ee::Float(0.0f))
                    back = -back;
                zge::matrixRotateY(turn, turn, back);
                engine::matrixProduct(target, target, turn);
                alignTo(rider, &target->m[0][0]);
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
                    const EdgeRef edge = collision::faceLinkedEdge(rider->contactRef());
                    collision::edgePlane(edge, &planeRead->x);
                    *edgePlane = *planeRead;
                    *edgeNormal = *edgePlane;
                    ee::Float fromPlane = rider::planeDistance(rider->position, &edgePlane->x);
                    if (fromPlane < ee::Float(0.0f))
                    {
                        fromPlane = -fromPlane;
                        zge::vec3Negate(&edgeNormal->xyz(), &edgeNormal->xyz());
                    }
                    closestOnRail(rider->position, edge, 0u, &nearest->x);
                    *scratch = *nearest - *vec4(rider->position);
                    const bool rising = rider->velocity[1] < ee::Float(0.0f);
                    rider->edgeHeight = rider->position[1] - nearest->y;
                    bool launch = false;
                    if (rising && engine::asinTurn(ee::abs(rider->right[1])) < *g_launchWithin)
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
                        rider->flags |= kFlagOffRamp;
                        break;
                    }

                    if ((rider->flags & kFlagGrindAsked) != 0)
                    {
                        const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
                        if (state == kPhysicsGrind || state == kPhysicsStall)
                            return physicsState(rider);
                    }
                    if (rider->contactKind == 2)
                    {
                        // A quarter pipe: up its edge's plane.
                        const ee::Float out = *g_vertOut;
                        const ee::Float nz = edgeNormal->z;
                        const ee::Float toPlane = out - fromPlane;
                        const ee::Float nx = edgeNormal->x;
                        const ee::Float z = rider->position[2] + nz * toPlane;
                        const ee::Float x = rider->position[0] + nx * toPlane;
                        rider->position[1] = ee::min(rider->position[1], nearest->y + ee::Float(0.5f));
                        rider->position[0] = x;
                        rider->position[2] = z;
                        *vec4(rider->drawnPosition) = *vec4(rider->position);
                        targetUp.y = -3.0f;
                        targetUp.x = -nx;
                        targetUp.z = -nz;
                        targetUp = zge::unit(targetUp);
                        const ee::Float kept = -(rider->velocity[1] * *g_vertKeep);
                        const int32_t frames0 = ee::toInt((kept + kept) / *g_gravity);
                        rider->velocity[1] = 0.0f;
                        const int32_t frames = kVertFramesLeast - 1 >= static_cast<uint32_t>(frames0) ? int32_t(kVertFramesLeast) : frames0;
                        const ee::Float into = zge::dot(velocity, *edgeNormal);
                        const ee::Float vx = rider->velocity[0] - nx * into;
                        rider->velocity[2] = rider->velocity[2] - nz * into;
                        rider->velocity[0] = vx;
                        velocity = zge::scaledCarrying(velocity, ee::Float(0.75f), zge::scaleLeftover(zge::LeftBy::lastCall));
                        const ee::Float g = *g_gravity;
                        rider->velocity[1] = -(ee::toFloat(frames) * g * ee::Float(0.5f)); // (frames is never negative)
                        const uint32_t tag = collision::faceTagByte9(rider->contactRef());
                        const ee::Float back = (out + vertShare(tag)) / ee::toFloat(frames);
                        const int32_t pitchFrames = frames - 3;
                        const ee::Float bx = rider->velocity[0] + -edgeNormal->x * back;
                        rider->velocity[2] = rider->velocity[2] + -edgeNormal->z * back;
                        rider->velocity[0] = bx;
                        const ee::Float speed = zge::vec3Length(&velocity.xyz());
                        const ee::Float mx = rider->forward[0];
                        const ee::Float my = -rider->forward[1];
                        const ee::Float mz = rider->forward[2];
                        rider->alignAngles[1] = 0;
                        rider->alignAngles[2] = 0;
                        rider->speed = speed;
                        scratch->x = mx;
                        scratch->y = my;
                        scratch->z = mz;
                        engine::copyMatrix(rider->alignFrom, rider->right);
                        const int32_t mirror = zge::angleAcos(zge::dot(*scratch, forward));
                        const int32_t pitch = ee::toInt(ee::toFloat(static_cast<int16_t>(-mirror)) * ee::Float(1.0f / 65536.0f) * *g_vertPitch /
                                                        ee::toFloat(pitchFrames) * ee::Float(65536.0f));
                        rider->alignAngles[0] = static_cast<int16_t>(pitch);
                        if (ee::Float(0.0f) <= zge::dot(velocity, forward))
                            rider->alignAngles[0] = static_cast<int16_t>(-pitch);
                        rider->snapFrames = static_cast<int16_t>(pitchFrames);
                        break;
                    }

                    uint32_t flags = rider->flags;
                    if ((flags & kFlagsHop) != 0)
                    {
                        // A hop over the edge: only when there is room.
                        *scratch = *nearest;
                        const ee::Float nx = edgeNormal->x;
                        const ee::Float nz = edgeNormal->z;
                        const ee::Float z = scratch->z + nz * ee::Float(0.5f);
                        const ee::Float y = scratch->y + ee::Float(-0.5f);
                        scratch->x = scratch->x + nx * ee::Float(0.5f);
                        scratch->z = z;
                        scratch->y = y;
                        *rayTo = *scratch;
                        const ee::Float rz = rayTo->z - nz;
                        rayTo->x = rayTo->x - nx;
                        rayTo->z = rz;
                        const int32_t blocked = rayHitsByCall(&scratch->x, &rayTo->x);
                        flags = rider->flags;
                        if (blocked != 0 || !(rider->position[1] + rider->velocity[1] - nearest->y < kHopClear))
                        {
                            rider->flags = flags & ~kFlagsHop;
                            flags = rider->flags;
                        }
                    }
                    if ((flags & kFlagsHop) != 0)
                    {
                        // Held to the edge's plane, falling at most 1 / 26
                        // of a frame's gravity times 26.
                        const ee::Float out = *g_hopOut;
                        ee::Float g;
                        if ((flags & kFlagHopRamp) != 0)
                        {
                            setOrigin(rider->velocity);
                            g = *g_gravity;
                        }
                        else
                        {
                            *scratch = zge::cross(*edgeNormal, *g_worldUp);
                            *scratch = zge::scaledCarrying(*scratch, zge::dot(velocity, *scratch), zge::scaleLeftover(zge::LeftBy::lastCall));
                            velocity = velocity - *scratch;
                            rider->speed = zge::vec3Length(&velocity.xyz());
                            const ee::Float nx = -edgeNormal->x;
                            const ee::Float c0 = engine::cosTurn(kHopTurn);
                            const ee::Float nz = -edgeNormal->z;
                            rider->velocity[0] = nx * (rider->speed * c0);
                            rider->velocity[2] = nz * (rider->speed * engine::cosTurn(kHopTurn));
                            velocity = velocity + *scratch;
                            g = *g_gravity;
                        }
                        const ee::Float fall = -(g * kHopFallFrames);
                        rider->velocity[1] = rider->velocity[1] < fall ? rider->velocity[1] : fall;
                        const ee::Float away = -zge::dot(velocity, *edgeNormal);
                        if (away < out)
                        {
                            const ee::Float more = out - away;
                            const ee::Float z = rider->velocity[2] + -edgeNormal->z * more;
                            rider->velocity[0] = rider->velocity[0] + -edgeNormal->x * more;
                            rider->velocity[2] = z;
                        }
                        rider->speed = zge::vec3Length(&velocity.xyz());
                        *scratch = *nearest;
                        const ee::Float past = *g_hopPast;
                        const ee::Float y = scratch->y + edgeNormal->y * past;
                        const ee::Float x = scratch->x + edgeNormal->x * past;
                        const ee::Float z = scratch->z + edgeNormal->z * past;
                        scratch->y = y;
                        scratch->x = x;
                        scratch->z = z;
                        if (y < rider->position[1])
                        {
                            *vec4(rider->position) = *scratch;
                            *vec4(rider->drawnPosition) = *scratch;
                        }
                        alignUpright = true;
                        break;
                    }

                    // Off a lip: the lip test, and a spin when the up row
                    // is near the edge's normal and the air long enough.
                    lipTest(rider, collision::faceLinkedEdge(rider->contactRef()), 1);
                    if (rider->contactKind == 3)
                    {
                        rider->velocity[0] = 0.0f;
                        rider->velocity[2] = 0.0f;
                    }
                    if (zge::angleAcos(zge::dot(up, *edgeNormal)) < *g_spinWithin &&
                        ee::Float(0.0f) <= zge::dot(velocity, forward))
                    {
                        const ee::Float rise = -rider->velocity[1];
                        const int32_t airFrames = ee::toInt((rise + rise) / *g_gravity);
                        if (static_cast<uint32_t>(airFrames) >= static_cast<uint32_t>(kSpinFramesLeast))
                        {
                            // The rows turned level with the world over the
                            // air less 15 frames, as a turn about the up row.
                            const int32_t frames = airFrames - kSpinFramesLess;
                            engine::copyMatrix(rider->alignFrom, rider->right);
                            rider->alignAngles[0] = 0;
                            rider->alignAngles[2] = 0;
                            zge::Matrix &level = *turn;
                            engine::copyMatrix(turn, rider->right);
                            level.row(2) = zge::cross(level.row(1), *g_worldUp);
                            level.row(2) = zge::unit(level.row(2));
                            level.row(0) = zge::cross(level.row(1), level.row(2));
                            const ee::Float a = zge::dot(forward, level.row(2));
                            const ee::Float b = -zge::dot(forward, level.row(0));
                            const ee::Float y = level.m[2][1] * a + level.m[0][1] * b;
                            const ee::Float z = level.m[2][2] * a + level.m[0][2] * b;
                            scratch->x = level.m[2][0] * a + level.m[0][0] * b;
                            scratch->y = y;
                            scratch->z = z;
                            *scratch = zge::unit(*scratch);
                            const int32_t angle = zge::angleAcos(zge::dot(*scratch, forward));
                            const int32_t perFrame = angle / frames;
                            const bool right = ee::Float(0.0f) < rider->right[1];
                            rider->alignAngles[1] = static_cast<int16_t>(-perFrame);
                            if (right)
                                rider->alignAngles[1] = static_cast<int16_t>(perFrame);
                            rider->snapFrames = static_cast<int16_t>(frames);
                            rider->flags |= kFlagAirSpin;
                        }
                    }
                    rider->flags |= kFlagOffLip;
                    break;
                }

                // Off the ground (a plain face, a kind 4 face): onto a ramp
                // below at once when a grind is asked for.
                if (static_cast<uint32_t>(below) < 4 && below != 0)
                {
                    if ((rider->flags & kFlagGrindAsked) != 0)
                    {
                        setContact(rider, contact, static_cast<uint32_t>(below));
                        const uint8_t state = setPhysicsState(rider, uint32_t(kPhysicsGrind));
                        if (state == kPhysicsGrind || state == kPhysicsStall)
                            return physicsState(rider);
                    }
                }
                else
                    alignUpright = true;
                const uint8_t now = rider->contactKind;
                ee::Float most;
                if (now == 4)
                    most = *g_topSpeed * (ee::toFloat(collision::faceTagByte9(rider->contactRef())) * *g_percent);
                else
                    most = *g_launchSpeed;
                // The take-off's pitch: the forward row's (in a wheelie the
                // balancing wheel's), turned round when rolling backwards.
                ee::Float slope;
                if (from == kPhysicsNoseWheelie)
                    slope = matrix(rider->wheelPoses[0].matrix)->row(2).y;
                else if (from == kPhysicsManual)
                    slope = matrix(rider->wheelPoses[1].matrix)->row(2).y;
                else
                    slope = rider->forward[1];
                int32_t pitch = engine::asinTurn(-slope);
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
                const ee::Float limit = engine::sinTurn(off) * most + most;
                const ee::Float along = zge::vec3Length(&scratch->xyz());
                const ee::Float x = scratch->x;
                if (limit < along)
                {
                    const ee::Float share = limit / along;
                    const ee::Float z = scratch->z * share;
                    rider->velocity[0] = x * share;
                    rider->velocity[2] = z;
                }
                break;
            }
            default:
                break;
            }
        }
        // Aligned upright: the rows turned to the world's up about the
        // forward row over the alignment's frames.
        if (alignUpright)
        {
            target->row(0) = zge::cross(targetUp, forward);
            target->row(0) = zge::unit(target->row(0));
            targetForward = zge::cross(target->row(0), targetUp);
            alignTo(rider, &target->m[0][0]);
        }

        // The take-off's sounds by the state left (the wall rides share the
        // ground's case).
        if (!known)
            return next;
        if (from == kPhysicsGround || from == kPhysicsNoseWheelie || from == kPhysicsManual)
        {
            const uint8_t kind = rider->contactKind;
            if (kind >= 4 || kind == 0)
                return next;
            // A ramp's launch: steeper than 60 degrees, the forward row
            // within 45 degrees of level.
            if (zge::angleAcos(ee::abs(rider->up[1])) < kAngle60 + 1)
                return next;
            if (zge::angleAcos(ee::abs(rider->forward[1])) >= kAngle45)
                return next;
            soundEvent(kSoundRampLaunch, rider->position, surfaceSoundFlags(0u, 0u, 1u, 2u), kSoundVolume);
        }
        else if (from == kPhysicsGrind || from == kPhysicsStall)
        {
            if (rider->contactKind != 5)
                return next;
            const uint32_t wall = collision::edgeWallKind(rider->contactRef());
            const uint32_t surface = wall == 1 ? 7u : wall == 2 ? 2u : 0u;
            if (rider->trick != kNoTrickHere && (trickFlags(rider->trick) & kTrickQuiet) != 0)
                return next;
            soundEvent(kSoundEdgeLaunch, rider->position, surfaceSoundFlags(0u, 0u, 2u, surface), kSoundVolume);
        }
        return next;
    }
    READABLE_FUNCTION(airEnter, Verified);
}
