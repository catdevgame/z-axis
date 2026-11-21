// DM2's approach target (physics states 3, 5 and 7's enter; MIXING 14.1's
// grindApproachEnter): where a grind, a stall or a lip trick is to land, and
// the snap that takes the rider there.
//
// The search follows the rider's path ahead: from the drawn position with
// the velocity, gravity added a step, up to a number of steps, the reach
// growing a step. What it looks for depends on the trick asked for (the
// "mode" here, the physics state the target is for): rail edges in reach
// (5: a grind, or a stall on a rail), every fourth step the faces in reach
// that can be stalled on or lipped (7, 6), and the level's wall-tap objects
// (8: their own target and rows). The target found, the rows the rider takes
// there are made (Rider::snapRows, read by approachAlign and the grind), the
// contact set, and the snap's step a frame (Rider::snapStep, approachMove's)
// for the frames the rider takes to reach it in the air.
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x80078CA8; DM2 Xbox 0x051C20.

#include "actions.h"
#include "../../fx/spawns.h"
#include "../collision/collision.h"
#include "../physics/physics.h"

#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "sym/dm2/rider/actions/target.cpp.h"

#include <cstddef>

namespace game::dm2::rider
{
    using collision::EdgeRef;
    using collision::FaceRef;

    void closestOnRail(const ee::Float *point, EdgeRef edge, int32_t clamp, ee::Float *out); // rails.cpp
    uint8_t grindTrick(const Rider *rider);                                                        // tricks.cpp
    uint8_t stallTrick(const Rider *rider);
    uint8_t lipTrick(const Rider *rider);
    namespace collision
    {
        int32_t edgeGrindSide(EdgeRef edge); // round 2's (collision/tags.cpp)
    }

    namespace
    {
        inline ee::Global<char[12], sym::wallTapName> g_wallTapName; // "obj_walltap"
        inline ee::Global<zge::Vec4, sym::down> g_worldUp;           // (0, -1, 0, 0): up in DM2's y-down world (rails.cpp's)
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickKinds; // bit 0x80: a stall trick
        inline ee::Global<int32_t, sym::g_riderDebugDraw> g_debugDraw;
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;        // 0.0055 a frame
        inline ee::Global<ee::Float, sym::g_railReach> g_railReach;      // the rails near the rider
        inline ee::Global<uint32_t, sym::g_grindSteps> g_grindSteps;      // the steps searched for a grind,
        inline ee::Global<uint32_t, sym::g_otherSteps> g_otherSteps;      // and for a stall or a lip
        inline ee::Global<uint32_t, sym::g_leastSnapFrames> g_leastSnapFrames;
        inline ee::Global<ee::Float, sym::g_airReach> g_airReach;       // the reach at the first step, from the air,
        inline ee::Global<ee::Float, sym::g_groundReach> g_groundReach;    // and from the ground
        inline ee::Global<ee::Float, sym::g_reachGrowth> g_reachGrowth;    // the reach's growth a step,
        inline ee::Global<ee::Float, sym::g_lipReachGrowth> g_lipReachGrowth; // and for a lip
        inline ee::Global<ee::Float, sym::g_railMostAcross> g_railMostAcross; // the speed across a rail it can be ground at
        inline ee::Global<ee::Float, sym::g_stallSpeed> g_railLeastAlong; // slower along the rail: a stall on it
        inline ee::Global<ee::Float, sym::g_farthest> g_farthest;       // the best's distance (squared) and height to start from
        inline ee::Global<ee::Float, sym::g_debugRailRadius> g_debugRailRadius;
        inline ee::Global<ee::Float, sym::g_lipMostInto> g_lipMostInto;    // a lip face's normal against the path, at most
        inline ee::Global<ee::Float, sym::g_faceClearSquared> g_faceClearSquared; // the ray's hit this near the face: clear
        inline ee::Global<ee::Float, sym::g_lipLeastPitch> g_lipLeastPitch;  // the lip's forward row steeper than this: turned (angle units)
        inline ee::Global<ee::Float, sym::g_debugArrowLength> g_skidLength; // the lip's skid mark's length along the forward row
        constexpr ee::Float kClearSquared = 0.25f;  // the ray's hit this near the rail: clear
        constexpr ee::Float kLeastLipSide = 1.5f;   // a lip face's shortest side, at least
        constexpr int32_t kStallFaceSteepest = 0x18E3; // 35 degrees (mode 6's faces)
        constexpr int32_t kLipFaceLeast = 0x3556;      // 75 degrees from flat, at least
        constexpr int32_t kRailRowsTurn = 0xAAA;       // 15 degrees: the grind from the ground or a wall leans into the rail
        constexpr int32_t kStallFaceAcross = 0x2000;   // 45 degrees
        constexpr int32_t kLipSideMost = 0x3555;
        constexpr int32_t kLipPitchBase = 0x1555;      // 30 degrees
        constexpr uint8_t kLipTrickFirst = 0x43, kLipTrickOwn = 0x45; // lipTrick's codes: 0x43 .. 0x44 lip tricks, 0x45 the lip trick that asks for the lip state
        constexpr uint8_t kLipSkidTrick = 0x44;        // the lip trick that leaves a skid mark on the lip
        constexpr uint32_t kContactRailEdge = 5;
        constexpr uint32_t kMostFaces = 128;           // the faces' list fills the original's frame up to the colour

        // The search's modes (the physics state the target is for).
        enum Mode : uint32_t
        {
            kModeRail = 5,    // rail edges (a grind; a stall on a rail)
            kModeStall = 6,   // faces to stall on (no trick asks for it)
            kModeLip = 7,     // lip faces
            kModeWallTap = 8, // a wall-tap object
        };

        // A face the sphere found that fits, and its point nearest the path.
        struct FoundFace
        {
            FaceRef face;
            uint8_t _08[8];
            zge::Vec4 point;
        };
        static_assert(sizeof(FoundFace) == 0x20 && offsetof(FoundFace, point) == 0x10);

        // The game side's debug draw's sphere (radius, centre, colour: an
        // empty function in the release).
        inline void debugSphere(ee::Float radius, const ee::Float *centre, const uint8_t *colour)
        {
            ee::game(sym::inlineFxNoopD, radius, centre, colour);
        }
        void negate(zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); }
    }

    // Physics states 3, 5 and 7's enter: the target of the grind, stall or
    // lip trick asked for (`state`), and the snap to it. Without a target the
    // state stays `from`. Returns the state entered (5 when a grind's rail
    // is met across it: a stall on it; 7 for the lip trick's own code).
    //
    // The lip's scale carries what the last call left, as airGravity's does.
    uint32_t grindApproachEnter(Rider *rider, uint32_t from, uint32_t state)
    {
        ee::Local<zge::Vec4> onRail;          // closestOnRail's point
        ee::Local<zge::Vec4> target;          // the best target,
        ee::Local<zge::Vec4> byAngle;         // the rail nearest the forward row's way,
        ee::Local<zge::Vec4> targetVelocity;  // the path's velocity at the best target (then the rider's),
        ee::Local<zge::Vec4> byAngleVelocity;
        ee::Local<zge::Vec4> scratch;
        ee::Local<zge::Vec4> pathPoint;       // the path: a point,
        ee::Local<zge::Vec4> pathVelocity;    // and its velocity
        ee::Local<zge::Vec4> side;
        ee::Local<zge::Vec4> normal;          // the face's normal (or the wall tap's forward row)
        ee::Local<zge::Matrix> rows;          // the rows at the target
        ee::Local<zge::Matrix> turn;
        ee::Local<FoundFace[kMostFaces]> faces;
        ee::Local<uint8_t[4]> colour;         // the debug spheres' colour (r, g, b, a)
        ee::Local<zge::Vec4> railDirection;
        ee::Local<zge::Vec4> wallTapForward;
        ee::Local<zge::Vec4> edgeAlong;       // a rail's direction
        ee::Local<zge::Matrix> objectRows;
        ee::Local<zge::Matrix> wallTapRows;
        ee::Local<uint32_t> wallTapHash;      // the names findObjects looks for (one)
        ee::Local<EdgeRef> bestEdge;          // the contacts setContact copies
        ee::Local<FaceRef> bestFace;
        ee::Local<ee::Ptr<zge::SceneObject>> wallTap;
        ee::Local<uint32_t> keptFrom;         // `from`, which the original keeps in its frame
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
        *keptFrom = from;

        // The trick asked for picks what to look for.
        uint32_t asked = state;
        uint32_t mode, trick;
        if (asked == kPhysicsGrindApproach)
        {
            mode = kModeRail;
            trick = grindTrick(rider);
        }
        else
        {
            trick = lipTrick(rider);
            if (trick < kLipTrickFirst || trick > kLipTrickOwn)
                mode = kModeRail;
            else
            {
                if (trick == kLipTrickOwn)
                    asked = kPhysicsLipApproach;
                mode = kModeLip;
            }
        }
        ee::Float bestHeight = *g_farthest; // the best face's height (y grows downwards)
        ee::Float best = bestHeight;        // the best's squared distance
        *bestEdge = 0;
        EdgeRef byAngleEdge = 0;
        int32_t bestAngle = 0x7FFF;
        *bestFace = 0;
        *wallTap = ee::Ptr<zge::SceneObject>();
        const uint64_t *rails = nullptr;
        ee::Ptr<ee::Ptr<zge::SceneObject>> wallTaps;
        bool wallTapFound = false;
        *pathPoint = drawn;
        *pathVelocity = velocity;
        const ee::Float growth = mode == kModeLip ? *g_lipReachGrowth : *g_reachGrowth;
        ee::Float reach = physicsState(rider) == kPhysicsAir ? *g_airReach : *g_groundReach;
        const uint32_t maxSteps = asked == kPhysicsGrindApproach ? *g_grindSteps : *g_otherSteps;
        if (mode == kModeRail)
            rails = collision::railEdgesNear(rider->drawnPosition, *g_railReach);
        else if (asked != kPhysicsLipApproach)
        {
            *wallTapHash = zge::nameHash(*g_wallTapName);
            wallTaps = zge::findObjects(wallTapHash, 1);
        }

        // The path ahead, a step at a time. (The faces' search counts with
        // the step counter itself, as the original does: after one, the
        // step count is the faces found, plus one.)
        uint32_t step = 0;
        while (maxSteps != 0)
        {
            setColour(0xF0, 0xA0, 0x00);
            const ee::Float reachSquared = reach * reach;
            const ee::Float nextReach = reach + growth;
            if (mode == kModeRail)
            {
                ++step;
                if (rails != nullptr)
                {
                    setColour(0x10, 0x80, 0x20);
                    for (uint32_t i = 0; rails[i] != 0; ++i)
                    {
                        const EdgeRef edge = rails[i];
                        closestOnRail(&pathPoint->x, edge, 1, &onRail->x);
                        *scratch = *onRail - drawn;
                        // Ahead of the rider, in reach of the path's point,
                        // met slowly enough across, with nothing between the
                        // rider and the rail.
                        if (ee::Float(0.0f) <= zge::dot(*scratch, velocity))
                        {
                            *scratch = *onRail - *pathPoint;
                            const ee::Float distance = zge::lengthSquared(&scratch->xyz());
                            if (distance <= reachSquared)
                            {
                                setColour(0x20, 0xF0, 0x40);
                                collision::edgeDirection(edge, &edgeAlong->x);
                                // The path's speed across the rail.
                                const ee::Float along = zge::dot(*pathVelocity, *edgeAlong);
                                {
                                    const ee::Float x = edgeAlong->x * along, y = edgeAlong->y * along, z = edgeAlong->z * along;
                                    scratch->x = pathVelocity->x - x;
                                    scratch->y = pathVelocity->y - y;
                                    scratch->z = pathVelocity->z - z;
                                }
                                if (zge::vec3Length(&scratch->xyz()) < *g_railMostAcross)
                                {
                                    bool clear = true;
                                    if (const collision::RayHit *const hit = collision::rayCast(rider->drawnPosition, &onRail->x); hit != nullptr)
                                    {
                                        collision::hitPoint(hit, &scratch->x);
                                        *scratch = *scratch - *onRail;
                                        clear = zge::lengthSquared(&scratch->xyz()) < kClearSquared;
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
                        }
                        if (*g_debugDraw != 0)
                            debugSphere(*g_debugRailRadius, &onRail->x, colour);
                    }
                }
            }
            else if (mode < kModeRail || mode >= kModeWallTap || (step & 3u) != 0)
                ++step;
            else
            {
                // Every fourth step: the wall taps in reach (ahead, below
                // the tap's up row) ...
                if (wallTaps)
                {
                    for (uint32_t i = 0; wallTaps[i]; ++i)
                    {
                        const ee::Ptr<zge::SceneObject> object = wallTaps[i];
                        *scratch = *zge::objectPosition(object) - *pathPoint;
                        const ee::Float distance = zge::lengthSquared(&scratch->xyz());
                        if (!(distance <= reachSquared))
                            continue;
                        zge::quaternionToMatrix(objectRows, zge::objectOrientation(object));
                        if (!(zge::dot(objectRows->row(1), *scratch) <= ee::Float(0.0f)))
                            continue;
                        scratch->y = ee::Float(0.0f);
                        if (!(ee::Float(0.0f) <= zge::dot(*scratch, *pathVelocity)))
                            continue;
                        *wallTap = object;
                        zge::quaternionToMatrix(wallTapRows, zge::objectOrientation(object));
                        *wallTapForward = wallTapRows->row(2);
                        best = distance;
                        wallTapFound = true;
                        *target = zge::quadwordOf(zge::objectPosition(object));
                        bestHeight = target->y;
                        *targetVelocity = *pathVelocity;
                        setColour(0xF0, 0x20, 0x40);
                        mode = kModeWallTap;
                        break;
                    }
                }
                // ... else the faces in reach: lips (steep, facing the path,
                // big enough) or faces to stall on, the nearest above the
                // best so far with nothing between it and the rider.
                if (!*wallTap)
                {
                    if (collision::SphereHits *const query = collision::sphereQuery(&pathPoint->x, reach); query != nullptr)
                    {
                        uint32_t count = 0;
                        while (const collision::SphereFace *const item = collision::nextSphereFace(query))
                        {
                            const FaceRef face = collision::faceRefAt(&item->face);
                            bool fits = false;
                            if (mode == kModeStall)
                            {
                                collision::faceNormal(face, &normal->x);
                                fits = zge::angleAcos(ee::abs(normal->y)) < kStallFaceSteepest;
                            }
                            else if (mode == kModeLip)
                            {
                                collision::faceNormal(face, &scratch->x);
                                fits = collision::acceptAll() != 0 && !(zge::angleAcos(ee::abs(scratch->y)) < kLipFaceLeast) &&
                                       zge::dot(*scratch, *pathVelocity) < *g_lipMostInto && kLeastLipSide <= collision::faceShortestSide(face);
                            }
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
                            const ee::Float distance = zge::lengthSquared(&scratch->xyz());
                            if (!(distance < best) || !(found.point.y < bestHeight))
                                continue;
                            if (const collision::RayHit *const hit = collision::rayCast(rider->drawnPosition, &found.point.x); hit != nullptr)
                            {
                                collision::hitPoint(hit, &scratch->x);
                                *scratch = *scratch - found.point;
                                if (!(zge::lengthSquared(&scratch->xyz()) < *g_faceClearSquared))
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
                }
                if (*g_debugDraw != 0)
                    debugSphere(reach, &pathPoint->x, colour);
                ++step;
            }
            // The path's next step.
            pathVelocity->y = pathVelocity->y + *g_gravity;
            *pathPoint = *pathPoint + *pathVelocity;
            reach = nextReach;
            if (!(step < maxSteps) || *wallTap)
                break;
        }
        *targetVelocity = velocity;

        // Of the rails, the one nearest the forward row's way when about as
        // near as the nearest.
        EdgeRef edge = *bestEdge;
        if (mode == kModeRail && byAngleEdge != 0 && edge != 0)
        {
            *scratch = *target - drawn;
            const ee::Float nearest = zge::lengthSquared(&scratch->xyz());
            *scratch = *byAngle - drawn;
            const ee::Float byAngleDistance = zge::lengthSquared(&scratch->xyz());
            if (ee::abs(nearest - byAngleDistance) < ee::Float(0.25f))
            {
                *bestEdge = byAngleEdge;
                *target = *byAngle;
                *targetVelocity = *byAngleVelocity;
                edge = byAngleEdge;
            }
        }
        if (edge == 0 && *bestFace == 0 && !wallTapFound)
            return from; // nothing in reach: the state stays

        // A lip's target: off the face by half its normal, up by the back
        // wheel's height along the drawn body's up row.
        if (asked == kPhysicsLipApproach)
        {
            collision::faceNormal(*bestFace, &normal->x);
            const zge::Vec4 &bodyUp = matrix(rider->body)->row(1);
            const ee::Float height = rider->backWheelHeight;
            const ee::Float x = target->x + normal->x * ee::Float(0.5f), y = target->y + normal->y * ee::Float(0.5f),
                            z = target->z + normal->z * ee::Float(0.5f);
            target->x = x + bodyUp.x * height;
            target->y = y + bodyUp.y * height;
            target->z = z + bodyUp.z * height;
        }
        // The frames to it: by the fall when falling faster than moving
        // across, else by the speed across.
        uint32_t frames;
        {
            scratch->x = velocity.x;
            scratch->z = velocity.z;
            scratch->y = ee::Float(0.0f);
            const ee::Float falling = ee::abs(velocity.y);
            const ee::Float across = zge::vec3Length(&scratch->xyz());
            if (across < falling)
            {
                // vy t + g t^2 / 2 = dy: t = (-vy -+ sqrt(vy^2 + 2 g dy)) / g.
                const ee::Float g = *g_gravity;
                const ee::Float dy = target->y - drawn.y;
                const ee::Float vy = velocity.y;
                const ee::Float pull = g * dy;
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
                else
                    frames = maxSteps;
            }
            else if (across == ee::Float(0.0f))
                frames = maxSteps;
            else
            {
                *scratch = *target - drawn;
                scratch->y = ee::Float(0.0f);
                frames = static_cast<uint32_t>(ee::toInt(zge::vec3Length(&scratch->xyz()) / across));
            }
        }
        if (!(frames < maxSteps))
            frames = maxSteps;
        if (frames < *g_leastSnapFrames)
            frames = *g_leastSnapFrames;

        if (mode <= kModeRail || mode > kModeWallTap)
        {
            // A rail: slow along it, a stall on it.
            collision::edgeDirection(edge, &railDirection->x);
            if (!(*g_railLeastAlong <= ee::abs(zge::dot(*railDirection, *targetVelocity))))
            {
                asked = kPhysicsStallApproach;
                trick = stallTrick(rider);
            }
            if (!((*g_trickKinds)[trick] & 0x80))
            {
                // A grind: forward along the rail the way the rider faces,
                // up square to it; from the ground or a wall leaned into the
                // rail's side.
                rowAhead = *railDirection;
                const bool turned = zge::dot(riderForward, rowAhead) < ee::Float(0.0f);
                if (turned)
                    negate(rowAhead);
                rowRight = zge::cross(*g_worldUp, rowAhead);
                rowRight = zge::unit(rowRight);
                rowUp = zge::cross(rowAhead, rowRight);
                rows->row(3) = zge::kZero;
                *scratch = *target - drawn;
                const ee::Float across = zge::dot(rowRight, *scratch);
                if (asked == kPhysicsGrindApproach && (from == kPhysicsGround || from == kPhysicsManual || from == kPhysicsNoseWheelie))
                {
                    zge::matrixIdentity(turn);
                    zge::matrixRotateZ(turn, turn, ee::Float(0.0f) <= across ? -kRailRowsTurn : kRailRowsTurn);
                    physics::engine::matrixProduct(rows, rows, turn);
                }
                // Which side the rider meets it from (the grind side the
                // edge is ground from, else the right row's), against the
                // bike's mirroring. The mirroring goes in as its flag's bit,
                // not as 0 or 1, so a mirrored bike always gets
                // kFlagMirrorSide here (a bug the game keeps).
                const int32_t grindSide = collision::edgeGrindSide(edge);
                const uint32_t onRight = ee::Float(0.0f) <= across ? 1u : 0u;
                const uint32_t mirrored = rider->flags & kFlagMirrored;
                uint32_t mirrorSide;
                if (grindSide != 0)
                {
                    *side = rowRight;
                    if (grindSide >= 0 ? !turned : turned)
                        negate(*side);
                    if (ee::Float(0.0f) <= zge::dot(*side, *scratch))
                        mirrorSide = onRight ^ (mirrored ^ 1u);
                    else
                        mirrorSide = onRight ^ mirrored;
                }
                else
                    mirrorSide = onRight ^ mirrored;
                if (mirrorSide != 0)
                    rider->flags |= kFlagMirrorSide;
                else
                    rider->flags &= ~kFlagMirrorSide;
            }
            else
            {
                // A stall on the rail: right along it the way the rider's
                // right row is, forward square to it.
                rowRight = *railDirection;
                if (zge::dot(riderRight, rowRight) < ee::Float(0.0f))
                    negate(rowRight);
                rowAhead = zge::cross(rowRight, *g_worldUp);
                rowAhead = zge::unit(rowAhead);
                rowUp = zge::cross(rowAhead, rowRight);
                rows->row(3) = zge::kZero;
                rider->flags &= ~kFlagMirrorSide;
            }
            setContact(rider, bestEdge, kContactRailEdge);
            *vec4(rider->snapPoint) = *target;
            physics::engine::copyMatrix(rider->snapRows, rows);
        }
        else
        {
            // A face or a wall tap: its normal (the tap's forward row).
            if (!wallTapFound)
                collision::faceNormal(*bestFace, &normal->x);
            else
                *normal = *wallTapForward;
            rows->row(3) = zge::kZero;
            if (asked != kPhysicsLipApproach)
            {
                // A stall: up the normal, forward square to it and the
                // world's up, the way the rider faces; tilted 45 degrees.
                const bool level = zge::angleAcos(ee::abs(zge::dot(riderForward, *g_worldUp))) < kStallFaceAcross;
                rowAhead = level ? riderRight : riderForward;
                {
                    const ee::Float y = normal->y, z = normal->z;
                    rowUp.x = normal->x;
                    rowUp.y = y;
                    rowUp.z = z;
                    rowAhead.y = ee::Float(0.0f);
                }
                rowAhead = zge::cross(*g_worldUp, rowUp);
                rowAhead = zge::unit(rowAhead);
                const ee::Float aheadY = rowAhead.y; // (0: square to the world's up)
                if (zge::dot(rowAhead, riderForward) < aheadY)
                    negate(rowAhead);
                rowRight = zge::cross(rowUp, rowAhead);
                rowRight = zge::unit(rowRight);
                if (rowRight.y < aheadY)
                    rider->flags |= kFlagMirrorSide;
                else
                    rider->flags &= ~kFlagMirrorSide;
                if (mode != kModeStall)
                {
                    zge::matrixIdentity(turn);
                    zge::matrixRotateZ(turn, turn, rowRight.y < ee::Float(0.0f) ? -kStallFaceAcross : kStallFaceAcross);
                    physics::engine::matrixProduct(rows, rows, turn);
                }
            }
            else
            {
                // A lip: right the face's normal, forward the velocity's way
                // along the face, up square to both (upward).
                rider->flags &= ~kFlagMirrorSide;
                {
                    const ee::Float x = normal->x, y = normal->y, z = normal->z;
                    rowRight.x = x;
                    rowRight.y = y;
                    rowRight.z = z;
                }
                rowAhead = *targetVelocity;
                rowAhead = zge::unit(rowAhead);
                if (zge::dot(rowAhead, riderForward) < ee::Float(0.0f))
                    negate(rowAhead);
                if (zge::angleAcos(ee::abs(zge::dot(rowRight, riderRight))) < kLipSideMost &&
                    !(zge::angleAcos(ee::abs(zge::dot(rowRight, rowAhead))) < kRailRowsTurn))
                {
                    // Along the face: forward less its part along the
                    // normal; pitched back toward 30 degrees when steep; the
                    // velocity kept along forward only.
                    const ee::Float into = zge::dot(rowAhead, rowRight);
                    {
                        const ee::Float x = into * rowRight.x, y = into * rowRight.y, z = into * rowRight.z;
                        rowAhead.x = rowAhead.x - x;
                        rowAhead.y = rowAhead.y - y;
                        rowAhead.z = rowAhead.z - z;
                    }
                    rowAhead = zge::unit(rowAhead);
                    rowUp = zge::cross(rowAhead, rowRight);
                    rowUp = zge::unit(rowUp);
                    if (ee::Float(0.0f) <= rowUp.y)
                    {
                        negate(rowUp);
                        negate(rowRight);
                    }
                    const int32_t pitch = physics::engine::asinTurn(rowAhead.y);
                    if (*g_lipLeastPitch < ee::abs(ee::toFloat(pitch)))
                    {
                        zge::matrixIdentity(turn);
                        const int32_t by = pitch > 0 ? pitch - kLipPitchBase : -pitch - kLipPitchBase;
                        zge::matrixRotateX(turn, turn, static_cast<int16_t>(by));
                        physics::engine::matrixProduct(rows, rows, turn);
                    }
                    const ee::Float kept = zge::dot(rowAhead, *targetVelocity);
                    *targetVelocity = zge::scaledCarrying(rowAhead, kept, zge::scaleLeftover(zge::LeftBy::lastCall));
                    if (zge::dot(*normal, riderRight) < ee::Float(0.0f))
                        rider->flags |= kFlagMirrorSide;
                }
                else
                {
                    // Across the face: forward square to the normal and the
                    // world's up, the way the rider faces; the velocity
                    // stopped.
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
                        rider->flags |= kFlagMirrorSide;
                    }
                    *targetVelocity = zge::kZero;
                }
            }
            if (mode == kModeWallTap)
                setContact(rider, wallTap, uint32_t(kModeWallTap));
            else
                setContact(rider, bestFace, mode);
            *vec4(rider->snapPoint) = *target;
            physics::engine::copyMatrix(rider->snapRows, rows);
            if (mode != kModeWallTap && trick == kLipSkidTrick)
            {
                // The lip's skid mark, along the forward row on the face.
                const ee::Float length = *g_skidLength;
                const ee::Float x = rowAhead.x * length, y = rowAhead.y * length, z = rowAhead.z * length;
                scratch->x = target->x + x;
                scratch->y = target->y + y;
                scratch->z = target->z + z;
                spawnSkidMark(target, scratch, &normal->xyz());
            }
        }
        velocity = *targetVelocity;
        if (asked == from)
            return asked;

        // The snap: the drawn root into the position, then a step a frame
        // that reaches the target in `frames` frames under gravity.
        {
            ee::Float *const p = rider->position;
            const ee::Float x = p[0] + rider->rootOffset[0], y = p[1] + rider->rootOffset[1];
            rider->snapFrames = static_cast<int16_t>(frames);
            const ee::Float z = p[2] + rider->rootOffset[2];
            p[0] = x;
            p[1] = y;
            p[2] = z;
            *vec4(rider->rootOffset) = zge::kZero;
            const ee::Float n = ee::toFloat(static_cast<int32_t>(frames));
            const ee::Float dy = (target->y - y) / n, dx = (target->x - x) / n, dz = (target->z - z) / n;
            const ee::Float fall = *g_gravity * ee::Float(0.5f) * n;
            rider->snapStep[0] = dx;
            rider->snapStep[2] = dz;
            rider->snapStep[1] = dy - fall;
        }
        return asked;
    }
    READABLE_FUNCTION(grindApproachEnter, Verified);
}
