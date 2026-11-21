// BMX XXX's approach target (physics states 3, 5 and 7's enter): where a
// grind, a stall or a lip trick is to land, and the snap that takes the rider
// there. DM2's (games/dm2/rider/actions/target.cpp: grindApproachEnter), read
// in BMX XXX's machine code.
//
// The search follows the rider's path ahead: from the drawn position (lifted
// by the motion's root height) with the velocity, gravity added a step, up to
// a number of steps (the first two searching nothing), the reach growing a
// step. What it looks for depends on the trick asked for (the "mode" here):
// rail edges near the path a few steps on (5: a grind, or a stall on a rail),
// or every fourth step the faces in reach that can be stalled on (7) or
// lipped (8), each facing the way the first one found does. BMX XXX keeps
// DM2's wall-tap tests with no wall tap ever looked for. The target found,
// the rows the rider takes there are made (Rider::snapRows), the contact set;
// onto a rail the approach is over at once (the grind or the stall entered),
// else the snap's step a frame (Rider::snapStep, approachMove's) for the
// frames the rider takes to reach it in the air.

#include "../rider.h"
#include "../collision/collision.h"

#include "bmx/fx/spawn_effects.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/conversions.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/actions/target.cpp.h"

#include <cstddef>

namespace game::bmx::rider
{
    using collision::EdgeRef;
    using collision::FaceRef;

    namespace calls
    {
        // The debug draw's sphere (radius, centre, colour): an empty function in the release, which the linker
        // folded into the animation event hook's (fx/spawns.cpp) with the same empty body.
        // Scaffolding (address-call): docs/SCAFFOLDING.md
        inline void debugSphere(ee::Float radius, const ee::Float *centre, const uint8_t *colour)
        {
            ee::game(sym::animationEventHook, radius, centre, colour);
        }
    }

    namespace
    {
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;                 // (0, -1, 0, 0)
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;      // the trick flags (grind.cpp's)
        constexpr uint32_t kTrickRowsByRight = 0x80u; // a stall trick: the rows along the rail by the right row
        constexpr uint32_t kTrickUntilted = 0x100u;   // a stall on the rail not tilted toward the grind side
        inline ee::Global<int32_t, sym::g_drawBodies> g_drawBodies;             // debug: the search's spheres drawn
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        constexpr uint8_t kLevel12 = 12; // (not named) no lip trick 0x47 there
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;                 // 0.0055 a frame
        inline ee::Global<ee::Float, sym::g_approachRailReach> g_railReach;     // 6.5: the rails near the path
        inline ee::Global<uint32_t, sym::g_approachGrindSteps> g_grindSteps;    // 4: the steps searched for a grind,
        inline ee::Global<uint32_t, sym::g_approachOtherSteps> g_otherSteps;    // 40: and for a stall or a lip
        // 2: the least frames of a snap; also the steps the search starts after, and (with half the steps) the
        // steps on of the rails' centre.
        inline ee::Global<uint32_t, sym::g_leastSnapFrames> g_leastSnapFrames;
        inline ee::Global<ee::Float, sym::g_approachAirReach> g_airReach;       // 0.15: the reach at the first step, from the air,
        inline ee::Global<ee::Float, sym::g_approachGroundReach> g_groundReach; // 0.2: and from the ground
        inline ee::Global<ee::Float, sym::g_approachReachGrowth> g_reachGrowth; // 0.45: the reach's growth a step,
        inline ee::Global<ee::Float, sym::g_approachLipReachGrowth> g_lipReachGrowth; // 0.02: and for a lip
        inline ee::Global<ee::Float, sym::g_railAcrossReach> g_railAcrossReach; // 0.5 of the reach: a rail's point off the path's heading, at most
        inline ee::Global<ee::Float, sym::g_railMostAcross> g_railMostAcross;   // 0.8333: the speed across a rail it can be ground at
        inline ee::Global<ee::Float, sym::g_stallSpeed> g_railLeastAlong;       // 0.0375: slower along the rail, a stall on it
        inline ee::Global<ee::Float, sym::g_approachFarthest> g_farthest;       // 1e8: the best's distance (squared) and height to start from
        inline ee::Global<ee::Float, sym::g_debugRailRadius> g_debugRailRadius; // 0.15
        inline ee::Global<ee::Float, sym::g_sameFaceAlign> g_sameFaceAlign;     // 0.99: the faces taken face within this of the first's
        inline ee::Global<ee::Float, sym::g_lipMostInto> g_lipMostInto;         // 0.0333: a lip face's normal against the path, at most
        inline ee::Global<ee::Float, sym::g_faceClearSquared> g_faceClearSquared; // 0.0025: the ray's hit this near the face: clear
        inline ee::Global<int16_t, sym::g_railTiltByRight> g_railTiltByRight;   // -25 degrees: a stall on a rail tilted toward its grind side
        inline ee::Global<ee::Float, sym::g_lipLeastPitch> g_lipLeastPitch;     // 5461: the lip's forward row steeper than this: turned (angle units)
        inline ee::Global<ee::Float, sym::g_lipSkidLength> g_skidLength;        // 0.3: the lip's skid mark's length along the forward row
        // The physics states' exits (rider, from, to; 0 none) and enters (rider, from, to) -> the state entered.
        using StateExit = ee::FunctionPtr<void(Rider *, uint32_t, uint32_t)>;
        using StateEnter = ee::FunctionPtr<int32_t(Rider *, uint32_t, uint32_t)>;
        inline ee::Global<StateExit[kPhysicsStates], sym::g_physicsExits> g_physicsExits;
        inline ee::Global<StateEnter[kPhysicsStates], sym::g_physicsEnters> g_physicsEnters;
        constexpr ee::Float kClearSquared = 0.25f;    // the ray's hit this near the rail: clear
        constexpr ee::Float kByAngleNear = 2.25f;     // the rail by the forward row's way taken within this of the nearest
        constexpr ee::Float kLeastLipSide = 1.5f;     // a lip face's shortest side, at least
        constexpr ee::Float kPathLift(0.25f);         // the path starts this share of the motion root's height up
        constexpr zge::Angle kStallFaceSteepest = 0x18E3; // 35 degrees (mode 7's faces)
        constexpr zge::Angle kLipFaceLeast = 0x3556;  // 75 degrees from flat, at least
        constexpr zge::Angle kSteepTarget = 0xE38;    // 20 degrees: a target nearer straight below or above than this is reached by the fall
        constexpr zge::Angle kStallFaceAcross = 0x2000; // 45 degrees
        constexpr zge::Angle kLipSideMost = 0x3555;
        constexpr zge::Angle kLipAlongLeast = 0xAAA;  // 15 degrees
        constexpr int32_t kLipPitchBase = 0x1555;     // 30 degrees
        // lipTrick's codes: 0x43 and 0x46 look for lips; 0x47 is the lip trick that asks for the lip state (none
        // on level 12); 0x46 leaves a skid mark on the lip.
        constexpr uint8_t kLipTrickA = 0x43, kLipSkidTrick = 0x46, kLipTrickOwn = 0x47;
        constexpr uint32_t kContactRailEdge = 5;
        constexpr uint32_t kMostFaces = 128;

        // The search's modes (the contact kind the target is for).
        enum Mode : uint32_t
        {
            kModeRail = 5,  // rail edges (a grind; a stall on a rail)
            kModeStall = 7, // faces to stall on
            kModeLip = 8,   // lip faces
            kModeObject = 9, // (DM2's wall taps: never found)
        };

        // A face the sphere found that fits, and its point nearest the path.
        struct FoundFace
        {
            FaceRef face;
            uint8_t _08[8];
            zge::Vec4 point;
        };
        static_assert(sizeof(FoundFace) == 0x20 && offsetof(FoundFace, point) == 0x10);

        void negate(zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); }
    }

    // Physics states 3, 5 and 7's enter: the target of the grind, stall or lip trick asked for (`state`), and the
    // snap to it. Without a target the state stays `from`. Returns the state entered (5 when a grind's rail is met
    // slowly along it: a stall on it; 7 for the lip trick's own code; the grind or the stall itself, 4 or 6, onto
    // a rail).
    uint32_t approachEnter(Rider *rider, uint32_t from, uint32_t state)
    {
        ee::Local<zge::Vec4> onRail;          // closestOnRail's point
        ee::Local<zge::Vec4> target;          // the best target,
        ee::Local<zge::Vec4> byAngle;         // the rail nearest the forward row's way,
        ee::Local<zge::Vec4> targetVelocity;  // the path's velocity at the best target (then the rider's),
        ee::Local<zge::Vec4> byAngleVelocity;
        ee::Local<zge::Vec4> fromPath;        // a point less the path's point
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4> pathPoint;       // the path: a point,
        ee::Local<zge::Vec4> pathVelocity;    // its velocity,
        ee::Local<zge::Vec4> heading;         // and its level heading
        ee::Local<zge::Vec4> edgeAlong;       // a rail's direction
        ee::Local<zge::Vec4> side;
        ee::Local<zge::Vec4> normal;          // the face's normal
        ee::Local<zge::Matrix> rows;          // the rows at the target
        ee::Local<zge::Matrix> turn;
        ee::Local<FoundFace[kMostFaces]> faces;
        ee::Local<uint8_t[4]> colour;         // the debug spheres' colour (r, g, b, a)
        ee::Local<EdgeRef> bestEdge;          // the contacts setContact copies
        ee::Local<FaceRef> bestFace;
        const zge::Vec4 &drawn = *vec4(rider->drawnPosition);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const zge::Vec4 &riderForward = *vec4(rider->forward);
        const zge::Vec4 &riderRight = *vec4(rider->right);
        zge::Vec4 &rowRight = rows->row(0);
        zge::Vec4 &rowUp = rows->row(1);
        zge::Vec4 &rowAhead = rows->row(2);
        auto setColour = [&](uint8_t r, uint8_t g, uint8_t b) {
            colour[0] = r;
            colour[1] = g;
            colour[2] = b;
            colour[3] = 0xFF;
        };
        zge::matrixIdentity(rows);
        zge::matrixIdentity(turn);

        // The trick asked for picks what to look for.
        uint32_t asked = state;
        uint32_t mode = kModeRail;
        uint32_t trick;
        if (asked == kPhysicsGrindApproach)
            trick = grindTrick(rider);
        else
        {
            trick = lipTrick(rider);
            if (trick == kLipTrickOwn)
            {
                asked = kPhysicsLipApproach;
                if (*g_currentLevel == kLevel12)
                    return from;
                mode = kModeLip;
            }
            else if (trick == kLipTrickA || trick == kLipSkidTrick)
                mode = kModeLip;
        }
        ee::Float bestHeight = *g_farthest; // the best face's height (y grows downwards)
        ee::Float best = bestHeight;        // the best's squared distance
        EdgeRef byAngleEdge = 0;
        *bestEdge = 0;
        *bestFace = 0;
        int32_t bestAngle = 0x7FFF;
        const uint64_t *rails = nullptr;
        const ee::Float growth = mode == kModeLip ? *g_lipReachGrowth : *g_reachGrowth;
        ee::Float reach = physicsState(rider) == kPhysicsAir ? *g_airReach : *g_groundReach;
        const uint32_t maxSteps = asked == kPhysicsGrindApproach ? *g_grindSteps : *g_otherSteps;
        if (mode == kModeRail)
        {
            // The rails near where the path is a few steps on.
            *pathPoint = drawn;
            *pathVelocity = velocity;
            const uint32_t on = *g_leastSnapFrames + maxSteps / 2u;
            for (uint32_t step = 0; step < on; ++step)
            {
                *pathPoint = *pathPoint + *pathVelocity;
                pathVelocity->y = pathVelocity->y + *g_gravity;
            }
            rails = collision::railEdgesNear(&pathPoint->x, *g_railReach);
        }

        // The path starts at the drawn position lifted by a quarter of the motion root's height; its heading is
        // the velocity made level.
        *pathPoint = *g_worldUp * (-rider->up[1] * (rider->drawnOffset[1] - rider->motionRoot[1]) * kPathLift);
        *pathPoint = *pathPoint + drawn;
        *pathVelocity = velocity;
        *heading = *pathVelocity;
        heading->y = 0.0f;
        if (const ee::Float level = zge::vec3Length(&heading->xyz()); ee::Float(0.0f) < level)
            *heading = *heading * (ee::Float(1.0f) / level);

        // The path ahead, a step at a time. (The faces' search counts with the step counter itself, as the
        // original does: after one, the step count is the faces found, plus one.)
        uint32_t step = 0;
        while (step < maxSteps)
        {
            setColour(0xF0, 0xA0, 0x00);
            const ee::Float reachSquared = reach * reach;
            const ee::Float nextReach = reach + growth;
            if (step < *g_leastSnapFrames)
                ++step; // (nothing so near)
            else if (mode == kModeRail)
            {
                ++step;
                for (uint32_t i = 0; rails != nullptr && rails[i] != 0; ++i)
                {
                    const EdgeRef edge = rails[i];
                    closestOnRail(&pathPoint->x, edge, 1, &onRail->x);
                    *scratch = *onRail - drawn;
                    // Ahead of the rider, in reach of the path's point and near its heading, met slowly enough
                    // across, with nothing between the rider and the rail.
                    if (!(ee::Float(0.0f) <= zge::dot(*scratch, velocity)))
                        continue;
                    *fromPath = *onRail - *pathPoint;
                    const ee::Float distance = zge::squaredLength(*fromPath);
                    if (!(distance <= reachSquared))
                        continue;
                    setColour(0x10, 0x80, 0x20);
                    *scratch = zge::scaledAfterMeasure(*heading, zge::dot(*heading, *fromPath));
                    *scratch = *fromPath - *scratch;
                    scratch->y = 0.0f;
                    if (zge::vec3Length(&scratch->xyz()) < reach * *g_railAcrossReach)
                    {
                        setColour(0xD0, 0x90, 0x20);
                        collision::edgeDirection(edge, &edgeAlong->x);
                        // The path's speed across the rail.
                        const ee::Float along = zge::dot(*pathVelocity, *edgeAlong);
                        scratch->x = pathVelocity->x - edgeAlong->x * along;
                        scratch->y = pathVelocity->y - edgeAlong->y * along;
                        scratch->z = pathVelocity->z - edgeAlong->z * along;
                        if (zge::vec3Length(&scratch->xyz()) < *g_railMostAcross)
                        {
                            bool clear = true;
                            if (const collision::RayHit *const hit = collision::rayCast(rider->drawnPosition, &onRail->x); hit != nullptr)
                            {
                                collision::hitPoint(hit, &scratch->x);
                                *scratch = *scratch - *onRail;
                                clear = zge::squaredLength(*scratch) < kClearSquared;
                            }
                            if (clear)
                            {
                                if (distance < best)
                                {
                                    best = distance;
                                    *bestEdge = edge;
                                    *target = *onRail;
                                    *targetVelocity = *pathVelocity;
                                    setColour(0x20, 0x20, 0xE0);
                                }
                                collision::edgeDirection(edge, &edgeAlong->x);
                                const int32_t angle = zge::angleAcos(ee::abs(zge::dot(*edgeAlong, riderForward)));
                                if (angle < bestAngle)
                                {
                                    byAngleEdge = edge;
                                    bestAngle = angle;
                                    *byAngle = *onRail;
                                    *byAngleVelocity = *pathVelocity;
                                }
                            }
                        }
                    }
                    if (*g_drawBodies != 0)
                        calls::debugSphere(*g_debugRailRadius, &onRail->x, colour);
                }
            }
            else if (mode < kModeStall || mode >= kModeObject || (step & 3u) != 0)
                ++step;
            else
            {
                // Every fourth step: the faces in reach facing the way the first one does: lips (steep, facing
                // the path, big enough, not themselves a lip's kind) or faces to stall on, the nearest above the
                // best so far with nothing between it and the rider.
                if (collision::SphereHits *const query = collision::sphereQuery(&pathPoint->x, reach); query != nullptr)
                {
                    uint32_t count = 0;
                    zge::Vec4 firstNormal;
                    for (uint32_t seen = 0; const collision::SphereFace *const item = collision::nextSphereFace(query); ++seen)
                    {
                        const FaceRef face = collision::faceRefAt(&item->face);
                        collision::faceNormal(face, &normal->x);
                        if (seen == 0)
                            firstNormal = *normal;
                        if (!(*g_sameFaceAlign < zge::dot(firstNormal, *normal)))
                            break;
                        bool fits = false;
                        if (mode == kModeStall)
                            fits = zge::angleAcos(ee::abs(normal->y)) < kStallFaceSteepest;
                        else if (mode == kModeLip)
                            fits = collision::acceptAll() != 0 && collision::faceSurfaceKind(face) != mode &&
                                   !(zge::angleAcos(ee::abs(normal->y)) < kLipFaceLeast) && zge::dot(*normal, *pathVelocity) < *g_lipMostInto &&
                                   kLeastLipSide <= collision::faceShortestSide(face);
                        if (fits)
                        {
                            FoundFace &found = faces[count];
                            ++count;
                            found.face = face;
                            collision::closestOnFace(item, &found.point.x);
                        }
                    }
                    for (step = 0; step < count; ++step)
                    {
                        const FoundFace &found = faces[step];
                        *scratch = found.point - *pathPoint;
                        const ee::Float distance = zge::squaredLength(*scratch);
                        if (!(distance < best) || !(found.point.y < bestHeight))
                            continue;
                        if (const collision::RayHit *const hit = collision::rayCast(rider->drawnPosition, &found.point.x); hit != nullptr)
                        {
                            collision::hitPoint(hit, &scratch->x);
                            *scratch = *scratch - found.point;
                            if (!(zge::squaredLength(*scratch) < *g_faceClearSquared))
                                continue; // something in the way
                        }
                        best = distance;
                        *bestFace = found.face;
                        *target = found.point;
                        *targetVelocity = *pathVelocity;
                        setColour(0x20, 0x20, 0xE0);
                        bestHeight = target->y;
                    }
                }
                if (*g_drawBodies != 0)
                    calls::debugSphere(reach, &pathPoint->x, colour);
                ++step;
            }
            // The path's next step.
            pathVelocity->y = pathVelocity->y + *g_gravity;
            *pathPoint = *pathPoint + *pathVelocity;
            reach = nextReach;
        }
        *targetVelocity = velocity;

        // Of the rails, the one nearest the forward row's way when about as near as the nearest.
        if (mode == kModeRail && byAngleEdge != 0 && *bestEdge != 0)
        {
            *scratch = *target - drawn;
            const ee::Float nearest = zge::squaredLength(*scratch);
            *scratch = *byAngle - drawn;
            const ee::Float byAngleDistance = zge::squaredLength(*scratch);
            if (ee::abs(nearest - byAngleDistance) < kByAngleNear)
            {
                *bestEdge = byAngleEdge;
                *target = *byAngle;
                *targetVelocity = *byAngleVelocity;
            }
        }
        if (*bestEdge == 0 && *bestFace == 0)
            return from; // nothing in reach: the state stays

        // A lip's target: off the face by half its normal, up by the back wheel's height along the drawn body's
        // up row.
        if (asked == kPhysicsLipApproach)
        {
            collision::faceNormal(*bestFace, &normal->x);
            const zge::Vec4 &bodyUp = matrix(rider->body)->row(1);
            const ee::Float height = rider->backWheelHeight;
            target->x = target->x + normal->x * ee::Float(0.5f) + bodyUp.x * height;
            target->y = target->y + normal->y * ee::Float(0.5f) + bodyUp.y * height;
            target->z = target->z + normal->z * ee::Float(0.5f) + bodyUp.z * height;
        }
        // The frames to it: by the fall when the target is within 20 degrees of straight below or above, else by
        // the speed across.
        uint32_t frames = maxSteps;
        {
            *scratch = *target - drawn;
            const ee::Float dy = scratch->y;
            scratch->y = 0.0f;
            const ee::Float level = zge::vec3Length(&scratch->xyz());
            if (!(dy == ee::Float(0.0f)) && zge::angleAtan(ee::abs(level / dy)) < kSteepTarget)
            {
                // vy t + g t^2 / 2 = dy: t = (-vy -+ sqrt(vy^2 + 2 g dy)) / g.
                const ee::Float g = *g_gravity;
                const ee::Float vy = velocity.y;
                const ee::Float pull = g * (target->y - drawn.y);
                const ee::Float d = vy * vy + (pull + pull);
                if (ee::Float(0.0f) <= d)
                {
                    const ee::Float root = ee::sqrt(d), rising = -vy;
                    if (root <= rising)
                    {
                        frames = static_cast<uint32_t>(ee::toInt((rising - root) / g));
                        if (frames < *g_leastSnapFrames)
                            frames = static_cast<uint32_t>(ee::toInt((rising + root) / g));
                    }
                    else
                        frames = static_cast<uint32_t>(ee::toInt((rising + root) / g));
                }
            }
            else
            {
                *scratch = velocity;
                scratch->y = 0.0f;
                const ee::Float across = zge::vec3Length(&scratch->xyz());
                if (!(across == ee::Float(0.0f)))
                {
                    *scratch = *target - drawn;
                    scratch->y = 0.0f;
                    frames = static_cast<uint32_t>(ee::toInt(zge::vec3Length(&scratch->xyz()) / across));
                }
            }
        }
        if (!(frames < maxSteps))
            frames = maxSteps;
        if (const uint32_t fewest = *g_leastSnapFrames != 0 ? *g_leastSnapFrames : 1u; frames < fewest)
            frames = fewest;

        if (mode < kModeStall || mode > kModeObject)
        {
            // A rail: slow along it, a stall on it.
            collision::edgeDirection(*bestEdge, &edgeAlong->x);
            if (!(*g_railLeastAlong <= ee::abs(zge::dot(*edgeAlong, *targetVelocity))))
            {
                asked = kPhysicsStallApproach;
                trick = stallTrick(rider);
            }
            bool againstVelocity = false; // the grind's way along the rail against the velocity
            const uint32_t flags = (*g_trickFlags)[trick];
            if ((flags & kTrickRowsByRight) == 0)
            {
                // A grind: forward along the rail the way the rider faces, up square to it.
                rowAhead = *edgeAlong;
                const bool turned = zge::dot(riderForward, rowAhead) < ee::Float(0.0f);
                if (turned)
                    negate(rowAhead);
                rowRight = zge::cross(*g_worldUp, rowAhead);
                rowRight = zge::unit(rowRight);
                rowUp = zge::cross(rowAhead, rowRight);
                rows->row(3) = zge::kZero;
                if (asked != kPhysicsStallApproach)
                    againstVelocity = zge::dot(rowAhead, *targetVelocity) < ee::Float(0.0f);
                *scratch = *target - drawn;
                const ee::Float across = zge::dot(rowRight, *scratch);
                // Which side the rider meets it from (the grind side the edge is ground from, else the right
                // row's), against the bike's mirroring. The mirroring goes in as its flag's bit, not as 0 or 1, so
                // a mirrored bike always gets kFlagMirrorSide here (a bug the game keeps, as DM2's).
                const int32_t grindSide = collision::edgeGrindSide(*bestEdge);
                uint32_t mirrorSide;
                if (grindSide != 0)
                {
                    *side = rowRight;
                    if (grindSide >= 0 ? !turned : turned)
                        negate(*side);
                    const uint32_t onRight = ee::Float(0.0f) <= across ? 1u : 0u;
                    uint32_t mirrored = rider->flags[kFlagMirrored.byte] & kFlagMirrored.mask;
                    if (ee::Float(0.0f) <= zge::dot(*side, *scratch))
                        mirrored ^= 1u;
                    mirrorSide = onRight ^ mirrored;
                }
                else
                    mirrorSide = (ee::Float(0.0f) <= across ? 1u : 0u) ^ (rider->flags[kFlagMirrored.byte] & kFlagMirrored.mask);
                if (mirrorSide != 0)
                    setFlag(rider, kFlagMirrorSide);
                else
                    clearFlag(rider, kFlagMirrorSide);
            }
            else
            {
                // A stall on the rail: right along it the way the rider's right row is, forward square to it;
                // tilted toward the rail's grind side (unless kTrickUntilted).
                rowRight = *edgeAlong;
                if (zge::dot(riderRight, rowRight) < ee::Float(0.0f))
                    negate(rowRight);
                rowAhead = zge::cross(rowRight, *g_worldUp);
                rowAhead = zge::unit(rowAhead);
                rowUp = zge::cross(rowAhead, rowRight);
                rows->row(3) = zge::kZero;
                if ((flags & kTrickUntilted) == 0)
                {
                    if (const int32_t grindSide = collision::edgeGrindSide(*bestEdge); grindSide != 0)
                    {
                        int16_t tilt = *g_railTiltByRight;
                        const bool facesRight = !(zge::dot(riderRight, rowRight) < ee::Float(0.0f));
                        if ((grindSide >= 0) == facesRight)
                            tilt = static_cast<int16_t>(-tilt);
                        zge::matrixIdentity(turn);
                        zge::matrixRotateX(turn, turn, tilt);
                        zge::matrixMultiply(rows, rows, turn);
                    }
                }
                clearFlag(rider, kFlagMirrorSide);
            }
            setContact(rider, bestEdge, kContactRailEdge);
            *vec4(rider->snapPoint) = *target;
            *matrix(rider->snapRows) = *rows;
            // The grind's balance turned round when the way along the rail changes it (kFlagBalanceTurnedRound).
            // Bug kept (as railMount's): the 1 is compared with the flag's 0x80, so a grind still against the
            // velocity turns the balance round again.
            if (uint32_t(againstVelocity) != (rider->flags[kFlagBalanceTurnedRound.byte] & kFlagBalanceTurnedRound.mask))
                rider->grindBalance = -rider->grindBalance;
            if (againstVelocity)
                setFlag(rider, kFlagBalanceTurnedRound);
            else
                clearFlag(rider, kFlagBalanceTurnedRound);
        }
        else
        {
            // A face: its normal.
            collision::faceNormal(*bestFace, &normal->x);
            rows->row(3) = zge::kZero;
            if (asked != kPhysicsLipApproach)
            {
                // A stall: up the normal, forward square to it and the world's up, the way the rider faces;
                // tilted 45 degrees for a lip trick's face.
                const bool level = zge::angleAcos(ee::abs(zge::dot(riderForward, *g_worldUp))) < kStallFaceAcross;
                rowAhead = level ? riderRight : riderForward;
                rowUp.x = normal->x;
                rowUp.y = normal->y;
                rowUp.z = normal->z;
                rowAhead.y = ee::Float(0.0f);
                rowAhead = zge::cross(*g_worldUp, rowUp);
                rowAhead = zge::unit(rowAhead);
                if (zge::dot(rowAhead, riderForward) < ee::Float(0.0f))
                    negate(rowAhead);
                rowRight = zge::cross(rowUp, rowAhead);
                rowRight = zge::unit(rowRight);
                if (rowRight.y < ee::Float(0.0f))
                    setFlag(rider, kFlagMirrorSide);
                else
                    clearFlag(rider, kFlagMirrorSide);
                if (mode != kModeStall)
                {
                    zge::matrixIdentity(turn);
                    zge::matrixRotateZ(turn, turn, rowRight.y < ee::Float(0.0f) ? static_cast<zge::Angle>(-kStallFaceAcross) : kStallFaceAcross);
                    zge::matrixMultiply(rows, rows, turn);
                }
            }
            else
            {
                // A lip: right the face's normal, forward the velocity's way along the face, up square to both
                // (upward).
                clearFlag(rider, kFlagMirrorSide);
                rowRight.x = normal->x;
                rowRight.y = normal->y;
                rowRight.z = normal->z;
                rowAhead = *targetVelocity;
                rowAhead = zge::unit(rowAhead);
                if (zge::dot(rowAhead, riderForward) < ee::Float(0.0f))
                    negate(rowAhead);
                if (zge::angleAcos(ee::abs(zge::dot(rowRight, riderRight))) < kLipSideMost &&
                    !(zge::angleAcos(ee::abs(zge::dot(rowRight, rowAhead))) < kLipAlongLeast))
                {
                    // Along the face: forward less its part along the normal; pitched back toward 30 degrees when
                    // steep; the velocity kept along forward only.
                    const ee::Float into = zge::dot(rowAhead, rowRight);
                    rowAhead.x = rowAhead.x - into * rowRight.x;
                    rowAhead.y = rowAhead.y - into * rowRight.y;
                    rowAhead.z = rowAhead.z - into * rowRight.z;
                    rowAhead = zge::unit(rowAhead);
                    rowUp = zge::cross(rowAhead, rowRight);
                    rowUp = zge::unit(rowUp);
                    if (ee::Float(0.0f) <= rowUp.y)
                    {
                        negate(rowUp);
                        negate(rowRight);
                    }
                    const int32_t pitch = zge::angleAsin(rowAhead.y);
                    if (*g_lipLeastPitch < ee::abs(ee::toFloat(pitch)))
                    {
                        zge::matrixIdentity(turn);
                        const int32_t by = pitch > 0 ? pitch - kLipPitchBase : -pitch - kLipPitchBase;
                        zge::matrixRotateX(turn, turn, static_cast<zge::Angle>(by));
                        zge::matrixMultiply(rows, rows, turn);
                    }
                    *targetVelocity = zge::scaledAfterMeasure(rowAhead, zge::dot(rowAhead, *targetVelocity));
                    if (zge::dot(*normal, riderRight) < ee::Float(0.0f))
                        setFlag(rider, kFlagMirrorSide);
                }
                else
                {
                    // Across the face: forward square to the normal and the world's up, the way the rider faces;
                    // the velocity stopped.
                    rowAhead = zge::cross(rowRight, *g_worldUp);
                    rowAhead = zge::unit(rowAhead);
                    rowUp = zge::cross(rowAhead, rowRight);
                    rowUp = zge::unit(rowUp);
                    if (ee::Float(0.0f) <= rowUp.y)
                    {
                        negate(rowUp);
                        negate(rowRight);
                    }
                    if (zge::dot(rowAhead, riderForward) < ee::Float(0.0f))
                    {
                        negate(rowAhead);
                        negate(rowRight);
                        setFlag(rider, kFlagMirrorSide);
                    }
                    *targetVelocity = zge::kZero;
                }
            }
            setContact(rider, bestFace, mode);
            *vec4(rider->snapPoint) = *target;
            *matrix(rider->snapRows) = *rows;
            if (trick == kLipSkidTrick)
            {
                // The lip's skid mark, along the forward row on the face.
                const ee::Float length = *g_skidLength;
                scratch->x = target->x + rowAhead.x * length;
                scratch->y = target->y + rowAhead.y * length;
                scratch->z = target->z + rowAhead.z * length;
                spawnSkidMark(target, scratch, normal);
            }
        }
        velocity = *targetVelocity;
        if (asked == from)
            return asked;

        if (mode == kModeRail && asked != kPhysicsLipApproach)
        {
            // Onto a rail: the approach is over at once, the grind or the stall entered (its exit, then the next
            // state's enter).
            const uint32_t next = asked + 1u;
            if (const StateExit leave = (*g_physicsExits)[asked])
                leave(rider, asked, next);
            rider->physicsState = static_cast<uint8_t>((*g_physicsEnters)[next](rider, asked, next));
            rider->snapFrames = 0;
            return next;
        }

        // The snap: the drawn root into the position, then a step a frame that reaches the target in `frames`
        // frames under gravity.
        {
            ee::Float *const p = rider->position;
            rider->snapFrames = static_cast<int16_t>(frames);
            p[0] = p[0] + rider->rootOffset[0];
            p[1] = p[1] + rider->rootOffset[1];
            p[2] = p[2] + rider->rootOffset[2];
            *vec4(rider->rootOffset) = zge::kZero;
            const ee::Float n = ee::toFloat(static_cast<int32_t>(frames));
            const ee::Float dy = (target->y - p[1]) / n, dx = (target->x - p[0]) / n, dz = (target->z - p[2]) / n;
            const ee::Float fall = *g_gravity * ee::Float(0.5f) * n;
            rider->snapStep[0] = dx;
            rider->snapStep[1] = dy - fall;
            rider->snapStep[2] = dz;
        }
        return asked;
    }
    READABLE_FUNCTION(approachEnter, Verified);
}
