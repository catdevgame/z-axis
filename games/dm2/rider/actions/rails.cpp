// DM2's rails (grind edges): the closest point on one, the same edge seen
// from either end, the closest edge near a point, the rider put onto an edge
// and the slope's pull along it (physics state 4; MIXING 14.1's grind rows).
//
// An edge is collision's EdgeRef (a face's side, 64 bits); its ends come
// from edgePoint and its unit direction from edgeDirection (the symbol
// table's edgePoints and edgeOfHit), both round 2's (collision/).
//
// Counterparts (MIXING 14.1): DM2 GameCube 0x80084F2C (closestOnRail),
// 0x80078864 (sameRailEdge), 0x800789AC (closestRailEdge), 0x80084D24
// (grindOnto), 0x8007D4EC (grindSlope); BMX XXX PS2 closestOnRail, sameRailEdge,
// closestRailEdge, grindOnto, grindSlope.

#include "actions.h"
#include "../collision/collision.h"
#include "../physics/physics.h"

#include "ee/abi.h"
#include <cstring>
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "zge/collision/closest.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/dm2/rider/actions/rails.cpp.h"

namespace game::dm2::rider
{
    using collision::EdgeRef;

    namespace
    {
        // A rider's contact kind (+0xFF1) when it is a rail edge.
        constexpr uint8_t kContactRailEdge = 5;
        // The slope's pull (approachGravity's gravity, 0.0055 a frame).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        // grindOnto: the rows turn to the rail when the rail is further than
        // this from the forward row (a cosine), and the speed along the rail
        // makes up this share of the speed lost to the turn.
        inline ee::Global<ee::Float, sym::g_railTurnCos> g_railTurnCos;
        inline ee::Global<ee::Float, sym::g_railSpeedKept> g_railSpeedKept;
        // The rider's contact the rail edge, as the original calls setContact
        // (through the game: grindMove reads what the call leaves); its
        // result is grindOnto's.
        inline int32_t setRailContact(Rider *rider, const EdgeRef *edge)
        {
            return ee::game<int32_t>(sym::setContact, rider, edge, uint32_t(kContactRailEdge));
        }
        inline ee::Global<ee::Float[4], sym::down> g_worldUp; // (0, -1, 0, 0): up in DM2's y-down world
    }

    // The closest point of an edge to a point: on the edge's line, or held
    // to its ends (`clamp`). Leaves VU0 as the original does (callers read
    // it). No caller reads a result.
    void closestOnRail(const ee::Float *point, EdgeRef edge, int32_t clamp, ee::Float *out)
    {
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> end;
        ee::Local<zge::Vec4> fromStart;
        collision::edgeDirection(edge, &direction->x);
        collision::edgePoint(edge, 0, &start->x);
        collision::edgePoint(edge, 1, &end->x);
        if (clamp != 0)
        {
            zge::closestPointOnSegment(start, end, direction, vec4(point), vec4(out));
            return;
        }
        // start + direction x ((point - start) . direction). The scale
        // carries what this function's caller left (the edge's lookups leave
        // it alone).
        *fromStart = *vec4(point) - *start;
        const ee::Float along = zge::dot(*fromStart, *direction);
        *vec4(out) = zge::scaledCarrying(*direction, along, zge::scaleLeftover(zge::LeftBy::caller));
        *vec4(out) = *vec4(out) + *start;
    }

    // Whether two edges are the same (their ends equal, either way round).
    // MIXING 14.1 named it nextRailEdge; it finds no edge.
    int32_t sameRailEdge(EdgeRef a, EdgeRef b)
    {
        ee::Local<zge::Vec4> startsApart;
        ee::Local<zge::Vec4> endsApart;
        ee::Local<zge::Vec4> endStartApart;
        ee::Local<zge::Vec4> startEndApart;
        ee::Local<ee::Float[4]> a0;
        ee::Local<ee::Float[4]> a1;
        ee::Local<ee::Float[4]> b0;
        ee::Local<ee::Float[4]> b1;
        collision::edgePoint(a, 0, a0);
        collision::edgePoint(a, 1, a1);
        collision::edgePoint(b, 0, b0);
        collision::edgePoint(b, 1, b1);
        *startsApart = *vec4(a0) - *vec4(b0);
        *endsApart = *vec4(a1) - *vec4(b1);
        *endStartApart = *vec4(a1) - *vec4(b0);
        *startEndApart = *vec4(a0) - *vec4(b1);
        if (zge::dot(*startsApart, *startsApart) == ee::Float(0.0f) && zge::dot(*endsApart, *endsApart) == ee::Float(0.0f))
            return 1;
        if (zge::dot(*endStartApart, *endStartApart) == ee::Float(0.0f))
            return zge::dot(*startEndApart, *startEndApart) == ee::Float(0.0f) ? 1 : 0;
        return 0;
    }

    // The rail edge nearest a point within `radius` (its closest point into
    // `closest`), not the one the rider grinds now (state 4 on a rail edge
    // contact). Returns the edge, or 0. MIXING 14.1's followRail.
    EdgeRef closestRailEdge(Rider *rider, const ee::Float *point, ee::Float *closest, ee::Float radius)
    {
        const EdgeRef *next = collision::railEdgesNear(point, radius); // ended by an edge of node 0
        EdgeRef nearest = 0;
        if (next == nullptr)
            return nearest;
        ee::Local<ee::Float[4]> onEdge;
        ee::Local<zge::Vec4> apart;
        ee::Float nearestSquared = radius * radius;
        for (EdgeRef edge = *next++; static_cast<uint32_t>(edge) != 0; edge = *next++)
        {
            const bool grinding = physicsState(rider) == kPhysicsGrind;
            if (grinding && rider->contactKind == kContactRailEdge && sameRailEdge(edge, rider->contactRef()) != 0)
                continue;
            closestOnRail(point, edge, 1, onEdge);
            *apart = *vec4(onEdge) - *vec4(point);
            const ee::Float squared = collision::engine::lengthSquared(&apart->x);
            if (squared < nearestSquared)
            {
                nearest = edge;
                nearestSquared = squared;
                *vec4(closest) = *vec4(onEdge);
            }
        }
        return nearest;
    }

    // The rider put onto an edge: the position (and the drawn one) at its
    // closest point; the rail's direction the way the rider faces; in a
    // grind turned further than g_railTurnCos, the grind's rows from the
    // rail; the velocity along the rail (slower than the speed: made up by
    // a share of the rest); the contact the edge. MIXING 14.1's grindEnd.
    int32_t grindOnto(Rider *rider, EdgeRef edge)
    {
        ee::Local<zge::Vec4> onEdge;   // the rail's point nearest the rider, then the velocity the rail takes off
        ee::Local<zge::Vec4> kept;     // the velocity along the rail
        ee::Local<ee::Float[4]> direction;
        ee::Local<zge::Matrix> rows;   // the grind's rows: right, up, forward (along the rail), position
        ee::Local<EdgeRef> railEdge;
        auto negate = [](zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); };
        *railEdge = edge;
        closestOnRail(rider->position, edge, 1, &onEdge->x);
        *vec4(rider->position) = *onEdge;
        *vec4(rider->drawnPosition) = *onEdge;
        collision::edgeDirection(*railEdge, direction);
        zge::Vec4 &along = rows->row(2);
        along = *vec4(direction);
        if (zge::dot(*vec4(rider->forward), along) < ee::Float(0.0f))
            negate(along);
        if (physicsState(rider) == kPhysicsGrind && zge::dot(along, *vec4(rider->forward)) < *g_railTurnCos)
        {
            // right = down x along, up = along x right (the grind's rows)
            rows->row(0) = zge::cross(*vec4(*g_worldUp), along);
            rows->row(0) = zge::unit(rows->row(0));
            rows->row(1) = zge::cross(along, rows->row(0));
            rows->row(3) = zge::kZero;
            physics::engine::copyMatrix(rider->snapRows, rows);
        }
        // The speed along the rail: at least the share g_railSpeedKept of
        // what the turn took off it.
        ee::Float speedAlong = zge::dot(*vec4(rider->velocity), along);
        const ee::Float speed = rider->speed;
        if (ee::abs(speedAlong) < speed)
        {
            const bool backwards = speedAlong < ee::Float(0.0f);
            const ee::Float magnitude = ee::abs(speedAlong);
            speedAlong = magnitude + (speed - magnitude) * *g_railSpeedKept;
            if (backwards)
                speedAlong = -speedAlong;
        }
        // The velocity along the rail, and what it loses (the original
        // leaves both in its frame).
        kept->x = along.x * speedAlong;
        kept->y = along.y * speedAlong;
        const ee::Float keptZ = along.z * speedAlong;
        const ee::Float vz = rider->velocity[2];
        const ee::Float lostX = rider->velocity[0] - kept->x;
        const ee::Float lostY = rider->velocity[1] - kept->y;
        onEdge->x = lostX;
        onEdge->y = lostY;
        onEdge->z = vz - keptZ;
        kept->z = keptZ;
        *vec4(rider->velocity) = *kept;
        std::memcpy(rider->contact, &*railEdge, sizeof(EdgeRef));
        return setRailContact(rider, railEdge);
    }

    // State 4's forces: the slope pulls along the rail (gravity times the
    // rail's fall, its direction the way the rider faces) into the force
    // and the velocity (w the rail direction's, as the copy has it); the
    // rail's surface drags the velocity, less with more balance; the speed.
    void grindSlope(Rider *rider)
    {
        ee::Local<zge::Vec4> pull;
        ee::Local<zge::Vec4> direction;
        const EdgeRef edge = rider->contactRef();
        collision::edgeDirection(edge, &direction->x);
        *pull = *direction;
        if (zge::dot(*vec4(rider->forward), *direction) < ee::Float(0.0f))
            zge::vec3Negate(&pull->xyz(), &pull->xyz());
        const ee::Float fall = pull->y;
        const ee::Float share = *g_gravity * fall;
        pull->x = pull->x * share;
        pull->z = pull->z * share;
        pull->y = fall * share;
        *vec4(rider->force) = *pull;
        zge::Vec4 &velocity = *vec4(rider->velocity);
        velocity = velocity + *pull;
        const physics::Surface &surface = (*physics::g_surfaces)[collision::edgeWallKind(edge)];
        const ee::Float kept = ee::Float(1.0f) - surface.grindDrag * ((ee::Float(1.0f) - rider->balanceStat) * ee::Float(10.0f) + ee::Float(1.0f));
        // The scale carries what this function's caller left (the rail's
        // calls leave it alone), as airGravity's does.
        velocity = zge::scaledCarrying(velocity, kept, zge::scaleLeftover(zge::LeftBy::caller));
        rider->speed = zge::vec3Length(&velocity.xyz());
    }

    uint8_t grindTrick(const Rider *rider); // tricks.cpp
    uint8_t stallTrick(const Rider *rider);
    int32_t surfaceSoundFlags(uint32_t flag20000, uint32_t flag10000, uint32_t kind, uint32_t surface);
    namespace collision
    {
        EdgeRef faceLinkedEdge(FaceRef ref);   // round 2's (collision/tags.cpp)
        uint32_t edgeGrindable(EdgeRef edge);
        void edgePlane(EdgeRef edge, ee::Float *plane);
    }

    namespace
    {
        // grindEnter's.
        constexpr int32_t kGrindSound = 20;
        constexpr int32_t kGrindVolume = 127;
        constexpr uint8_t kContactFaceKinds = 4;        // the ground's contact kinds 1 .. 3 (a face) the search starts from
        constexpr uint8_t kContactKind2 = 2;            // (inferred) a face kind whose rows are not tilted
        constexpr int32_t kRailTilt = 2730;             // 15 degrees about the rail's forward
        inline ee::Global<ee::Float, sym::g_stallSpeed> g_stallSpeed; // along the rail slower than this: a stall (0.0375)
        inline ee::Global<uint16_t[256], sym::g_trickKindsR> g_trickKindsR;
        constexpr uint16_t kTrickRowsByRight = 0x80;    // the trick's rows from the rail and the right row
        constexpr ee::Float kNearSquared = 0.25f;     // the drawn body this near the rail (squared) or above it
    }

    // Physics state 4's enter: from the ground, a nose wheelie or a manual
    // (states 0, 9, 10) onto the rail edge linked to the face under the wheels, if it can
    // be ground and the drawn body is above it or within half a unit; the
    // target its closest point, the grind trick (slow along it: a stall,
    // state 6) and the rows from the rail and its plane (tilted 15 degrees
    // into it, or by the right row for some tricks), mirrored by the
    // plane's side, the contact the edge. From the approach the target and
    // contact are set already. Then the angles still; a grind goes onto the
    // rail with its speed, a stall stops at the target; fakie by the
    // velocity; the sound by the rail's surface. Returns the state (or the
    // state it came from when there is no rail).
    uint32_t grindEnter(Rider *rider, uint32_t from, uint32_t state)
    {
        ee::Local<zge::Vec4> apart;            // the drawn body from the rail
        ee::Local<zge::Vec4> plane;            // the rail's plane, the side the rider is on
        ee::Local<ee::Float[4]> onRail;        // the rail's point nearest the rider
        ee::Local<zge::Matrix> rows;           // right, up, forward, position
        ee::Local<zge::Matrix> tilt;
        ee::Local<ee::Float[4]> railPlane;
        ee::Local<ee::Float[4]> direction;
        ee::Local<ee::Float[4]> edgePlane;
        ee::Local<EdgeRef> edge;
        ee::Local<ee::Ptr<ee::Float>> drawnAt; // (the original keeps the drawn position's address in its frame)
        auto negate = [](zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); };
        uint32_t result = state;
        *drawnAt = ee::Ptr<ee::Float>(rider->drawnPosition);
        if (from == kPhysicsGround || from == kPhysicsNoseWheelie || from == kPhysicsManual)
        {
            const uint8_t kind = rider->contactKind;
            if (kind >= kContactFaceKinds || kind == 0)
                return from;
            *edge = collision::faceLinkedEdge(rider->contactRef());
            if (collision::edgeGrindable(*edge) == 0)
                return from;
            closestOnRail(rider->position, *edge, 0, onRail);
            *apart = *vec4(rider->drawnPosition) - *vec4(onRail);
            const ee::Float near = collision::engine::lengthSquared(&apart->x);
            if (!(apart->y < ee::Float(0.0f)) && !(near < kNearSquared))
                return from;
            *vec4(rider->snapPoint) = *vec4(onRail);
            collision::edgePlane(*edge, edgePlane);
            *vec4(railPlane) = *vec4(edgePlane);
            *plane = *vec4(railPlane);
            if (rider::planeDistance(rider->position, railPlane) < ee::Float(0.0f))
                negate(*plane);
            collision::edgeDirection(*edge, direction);
            uint8_t trick;
            if (*g_stallSpeed <= ee::abs(zge::dot(*vec4(direction), *vec4(rider->velocity))))
                trick = grindTrick(rider);
            else
            {
                result = kPhysicsStall;
                trick = stallTrick(rider);
            }
            const EdgeRef railEdge = *edge;
            zge::Vec4 &right = rows->row(0);
            zge::Vec4 &up = rows->row(1);
            zge::Vec4 &ahead = rows->row(2);
            if (!((*g_trickKindsR)[trick] & kTrickRowsByRight))
            {
                collision::edgeDirection(railEdge, direction);
                ahead = *vec4(direction);
                right = *plane;
                up = zge::cross(ahead, right);
                if (ee::Float(0.0f) < up.y)
                {
                    negate(ahead);
                    negate(up);
                }
                if (zge::dot(*vec4(rider->forward), ahead) < ee::Float(0.0f))
                {
                    negate(ahead);
                    negate(right);
                }
                if (rider->contactKind != kContactKind2)
                {
                    zge::matrixIdentity(tilt);
                    const int32_t angle = zge::dot(*plane, right) < ee::Float(0.0f) ? kRailTilt : -kRailTilt;
                    zge::matrixRotateZ(tilt, tilt, angle);
                    physics::engine::matrixProduct(rows, rows, tilt);
                }
            }
            else
            {
                collision::edgeDirection(railEdge, direction);
                right = *vec4(direction);
                if (zge::dot(*vec4(rider->right), right) < ee::Float(0.0f))
                    negate(right);
                ahead = zge::cross(right, *vec4(*g_worldUp));
                ahead = zge::unit(ahead);
                up = zge::cross(ahead, right);
            }
            rows->row(3) = zge::kZero;
            physics::engine::copyMatrix(rider->right, rows);
            if (zge::dot(right, *plane) < ee::Float(0.0f))
                rider->flags |= kFlagMirrorSide;
            else
                rider->flags &= ~kFlagMirrorSide;
            setContact(rider, edge, uint32_t(kContactRailEdge));
            physics::engine::copyMatrix(rider->snapRows, rows);
        }
        const EdgeRef onEdge = rider->contactRef();
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        rider->balance = 0.0f;
        *edge = onEdge;
        if (result == kPhysicsGrind)
            grindOnto(rider, onEdge);
        else
        {
            *vec4(rider->velocity) = zge::kZero;
            *vec4(rider->force) = zge::kZero;
            rider->speed = 0.0f;
            *vec4(rider->position) = *vec4(rider->snapPoint);
            *vec4(rider->drawnPosition) = *vec4(rider->snapPoint);
        }
        if (zge::dot(*vec4(rider->velocity), *vec4(rider->forward)) < ee::Float(0.0f))
            rider->flags |= kFlagFakie;
        else
            rider->flags &= ~kFlagFakie;
        const int32_t surface = collision::edgeWallKind(rider->contactRef());
        const uint32_t sound = surface == 1 ? 7u : surface == 2 ? 2u : 0u;
        collision::touchObject(rider, nullptr);
        rider->groundHeight = 0.0f;
        const int32_t code = surfaceSoundFlags(0u, 0u, 2u, sound);
        soundEvent(kGrindSound, rider->position, code, kGrindVolume);
        return result;
    }
    READABLE_FUNCTION_WAITS(grindEnter, Verified);

    namespace
    {
        // grindBalance's tuning.
        inline ee::Global<ee::Float, sym::g_lipLeanLeast> g_lipLeanLeast;   // the lip's lean at least this either way
        inline ee::Global<ee::Float, sym::g_lipLeanLeastBack> g_lipLeanLeastBack;
        inline ee::Global<ee::Float, sym::g_railPitchFalloff> g_railPitchFalloff;
        inline ee::Global<ee::Float, sym::g_fallOffAside> g_fallOffAside;   // off a rail: along its right
        inline ee::Global<ee::Float, sym::g_fallOffAhead> g_fallOffAhead;   // (by the right row) along the rail, ahead
        inline ee::Global<ee::Float, sym::g_fallOffBehind> g_fallOffBehind;
        inline ee::Global<ee::Float, sym::g_stallPitchFalloff> g_stallPitchFalloff;
        inline ee::Global<ee::Float, sym::g_lipPitchFalloff> g_lipPitchFalloff;
        inline ee::Global<ee::Float, sym::g_railTip> g_railTip;        // the rail's tip a frame
        inline ee::Global<ee::Float, sym::g_railSteady> g_railSteady;     // at this speed or more, no tip from slowness
        inline ee::Global<ee::Float, sym::g_railSlowTip> g_railSlowTip;
        inline ee::Global<ee::Float, sym::g_lipTip> g_lipTip;
        inline ee::Global<int16_t, sym::g_railFallAngle> g_railFallAngle;    // the up row this far from the rail's: off
        inline ee::Global<ee::Float, sym::g_railTimeTip> g_railTimeTip;    // (the wheelie's too)
        inline ee::Global<uint32_t, sym::g_railTimeFull> g_railTimeFull;
        constexpr uint8_t kContactStall = 6;
        constexpr int32_t kLipFallAngle = 2730;                     // a lip: the forward row steeper than 15 degrees off
        const ee::Float kTurnUnit(1.0f / 65536.0f);

        // The engine's calls the original makes here (grindBalance and
        // grindMove), as guest calls: the scales after them carry what the
        // last one left.
        // Scaffolding (guest-call-kept): docs/SCAFFOLDING.md
        inline void edgeEndByCall(EdgeRef edge, ee::Float *point) { ee::game(sym::edgePoints, edge, 1, point); }
        inline void negateByCall(ee::Float *v) { ee::game(sym::vec3Negate, v, v); }
        inline int32_t rayHitsByCall(const ee::Float *from, const ee::Float *to) { return ee::game<int32_t>(sym::rayCast, from, to); }

        int16_t pitchFalloff(int16_t pitch, ee::Float share)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(pitch) -
                                        ee::toInt(ee::toFloat(pitch) * kTurnUnit * share * ee::Float(65536.0f)));
        }
    }

    // States 4 and 6's turn (and the lip's, 8): the balance. On a stall the
    // tilt (the pitch) undone; on a lip the tilt grows by the lean (right
    // row's y) against the skill, off it into the air when the forward row
    // is steep; on a rail the tilt grows by the lean, slowness and the time
    // on the rail against the skill, the rows turned by it about the rail,
    // the balance the up row's angle from the target's, past the most off
    // the rail into the air (pushed aside, or ahead / behind).
    //
    // Only the turn table calls it, as void. The scales carry what the last
    // call left, as airGravity's do.
    void grindBalance(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;  // the tilt's turn; off the rail, the target's rows
        ee::Local<zge::Vec4> level;   // the up row less its part along the rail; off the rail, the push
        zge::matrixIdentity(turn);
        const uint8_t kind = rider->contactKind;
        if (kind == kContactStall)
        {
            zge::matrixRotateY(turn, turn, int32_t(static_cast<int16_t>(-rider->pitch)));
            rider->pitch = pitchFalloff(rider->pitch, *g_stallPitchFalloff);
            physics::engine::matrixProduct(rider->right, rider->right, turn);
            physics::engine::matrixProduct(rider->body, rider->body, turn);
            return;
        }
        if (kind < 7)
        {
            if (kind != kContactRailEdge)
                return;
        }
        else
        {
            if (kind >= 9)
                return;
            // A lip.
            ee::Float lean = rider->right[1];
            const ee::Float least = *g_lipLeanLeast;
            if (ee::abs(lean) < least)
                lean = ee::Float(0.0f) <= lean ? least : *g_lipLeanLeastBack;
            const int32_t pitch = static_cast<uint16_t>(rider->pitch) -
                                  ee::toInt(lean * (ee::Float(1.0f) - rider->balanceStat + ee::Float(1.0f)) *
                                            *g_lipTip * ee::Float(65536.0f));
            rider->pitch = static_cast<int16_t>(pitch);
            zge::matrixRotateZ(turn, turn, int32_t(static_cast<int16_t>(pitch)));
            physics::engine::matrixProduct(rider->right, rider->right, turn);
            physics::engine::matrixProduct(rider->body, rider->body, turn);
            const ee::Float down = -rider->up[1];
            rider->pitch = pitchFalloff(rider->pitch, *g_lipPitchFalloff);
            if (physics::engine::asinTurn(down) < kLipFallAngle)
                setPhysicsState(rider, uint32_t(kPhysicsAir));
            return;
        }

        // A rail.
        const uint8_t trick = rider->trick;
        const bool byRight = (*g_trickKindsR)[trick] & kTrickRowsByRight;
        ee::Float lean = byRight ? -rider->forward[1] : rider->right[1];
        if (ee::abs(lean) < ee::Float(0.25f))
            lean = ee::Float(0.0f) <= lean ? ee::Float(0.25f) : ee::Float(-0.25f);
        const ee::Float steady = *g_railSteady;
        const uint32_t full = *g_railTimeFull;
        const uint32_t ridden = static_cast<uint32_t>(rider->balanceFrames);
        const int32_t pitch =
            static_cast<uint16_t>(rider->pitch) -
            ee::toInt(lean * (ee::Float(1.0f) - rider->balanceStat + ee::Float(1.0f)) *
                      ((steady - ee::min(steady, rider->speed)) * *g_railSlowTip / steady) *
                      (ee::toFloat(int32_t(ridden < full ? ridden : full)) * ((*g_railTimeTip + ee::Float(1.0f)) / ee::toFloat(int32_t(full)))) *
                      *g_railTip * ee::Float(65536.0f));
        rider->pitch = static_cast<int16_t>(pitch);
        if ((*g_trickKindsR)[trick] & kTrickRowsByRight)
            zge::matrixRotateX(turn, turn, int32_t(static_cast<int16_t>(pitch)));
        else
            zge::matrixRotateZ(turn, turn, int32_t(static_cast<int16_t>(pitch)));
        const zge::Vec4 &up = *vec4(rider->up);
        physics::engine::matrixProduct(rider->right, rider->right, turn);
        physics::engine::matrixProduct(rider->body, rider->body, turn);
        rider->pitch = pitchFalloff(rider->pitch, *g_railPitchFalloff);
        // The up row less its part along the rail (the target's forward, or
        // right for the right row's tricks): its angle from the target's up.
        *level = up;
        const zge::Matrix &target = *matrix(rider->snapRows);
        const zge::Vec4 &along = (*g_trickKindsR)[rider->trick] & kTrickRowsByRight ? target.row(0) : target.row(2);
        const ee::Float part = zge::dot(*level, along);
        {
            const ee::Float ax = along.x, ay = along.y, az = along.z;
            const ee::Float ly = level->y - ay * part;
            const ee::Float lz = level->z - az * part;
            level->x = level->x - ax * part;
            level->y = ly;
            level->z = lz;
        }
        *level = zge::unit(*level);
        const ee::Float off = ee::toFloat(zge::angleAcos(zge::dot(*level, target.row(1))));
        const ee::Float balance = off * kTurnUnit / (ee::toFloat(int32_t(*g_railFallAngle)) * kTurnUnit);
        rider->balance = balance;
        const zge::Vec4 &side = (*g_trickKindsR)[rider->trick] & kTrickRowsByRight ? target.row(2) : target.row(0);
        if (zge::dot(*level, side) < ee::Float(0.0f))
            rider->balance = -balance;
        if (!(ee::toFloat(int32_t(*g_railFallAngle)) < ee::abs(off)))
            return;

        // Off the rail: pushed out from it along its side (or, by the right
        // row, ahead or behind), into the air. The target's rows flattened:
        // the row across the rail level, the push the row square to it and
        // the other, pointing down.
        physics::engine::copyMatrix(turn, rider->snapRows);
        zge::Vec4 &across = turn->row(0);
        zge::Vec4 &push = turn->row(1);
        zge::Vec4 &ahead = turn->row(2);
        const ee::Float pushScale = *g_gravity * ee::Float(16.0f);
        if (!((*g_trickKindsR)[rider->trick] & kTrickRowsByRight))
        {
            across.y = ee::Float(0.0f);
            across = zge::unit(across);
            push = zge::cross(ahead, across);
            ee::Float y = across.y;
            if (zge::dot(across, up) < y)
            {
                negateByCall(&across.x);
                y = across.y;
            }
            *level = zge::scaledCarrying(push, pushScale, zge::scaleLeftover(zge::LeftBy::lastCall));
            const ee::Float aside = *g_fallOffAside;
            const ee::Float py = level->y + y * aside;
            const ee::Float pz = level->z + across.z * aside;
            level->x = level->x + across.x * aside;
            level->y = py;
            level->z = pz;
        }
        else
        {
            ahead.y = ee::Float(0.0f);
            ahead = zge::unit(ahead);
            push = zge::cross(ahead, across);
            ee::Float y = ahead.y;
            const ee::Float flat = y;
            if (zge::dot(ahead, up) < y)
            {
                negateByCall(&ahead.x);
                y = ahead.y;
            }
            *level = zge::scaledCarrying(push, pushScale, zge::scaleLeftover(zge::LeftBy::lastCall));
            const bool behind = zge::dot(*vec4(rider->forward), push) < flat;
            ee::Float by = *g_fallOffAhead;
            const ee::Float x = ahead.x;
            if (behind)
                by = *g_fallOffBehind;
            const ee::Float py = level->y + y * by;
            const ee::Float pz = level->z + ahead.z * by;
            level->x = level->x + x * by;
            level->y = py;
            level->z = pz;
        }
        zge::Vec4 &velocity = *vec4(rider->velocity);
        velocity = velocity + *level;
        setPhysicsState(rider, uint32_t(kPhysicsAir));
        rider->flags |= kFlagFellOff;
    }
    READABLE_FUNCTION(grindBalance, Verified);

    namespace
    {
        // grindMove's tuning.
        inline ee::Global<ee::Float, sym::g_grindSpinKept> g_grindSpinKept;  // the back wheel's spin kept a frame
        inline ee::Global<ee::Float, sym::g_offRailLift> g_offRailLift;    // off the rail: the velocity this far along up
        inline ee::Global<ee::Float, sym::g_railSideReach> g_railSideReach;  // the rays' reach to each side (along right)
        inline ee::Global<ee::Float, sym::g_nextRailReach> g_nextRailReach;  // past an end: the next rail within this
        inline ee::Global<ee::Float, sym::g_railAlignShare> g_railAlignShare; // the forward row turned this share to the rail (0.085)
        constexpr int32_t kNextRailMostTurn = 13653;                // 75 degrees (0x10000 a turn)
    }

    // State 4's move: the back wheel's spin runs down (the front stops);
    // between the rail's ends the forward row turns toward it, past an end
    // the rider goes onto the next rail ahead (turned less than 75 degrees)
    // if there is one; then two rays from above the rider, ahead along the
    // rail and to each side, must find nothing: the rider moves on by his
    // velocity. Else (off the rail, or blocked) the velocity lifts along up
    // and the rider is in the air. GameCube 0x80082248.
    //
    // Every scale carries what the last call left, as airGravity's does.
    void grindMove(Rider *rider)
    {
        ee::Local<zge::Vec4> step;
        ee::Local<zge::Vec4> toStart;
        ee::Local<zge::Vec4> toEnd;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> nextDirection;
        ee::Local<zge::Vec4> rayFrom;
        ee::Local<zge::Vec4> side;
        ee::Local<int32_t> onRail;          // (the original keeps it in its frame)
        ee::Local<ee::Ptr<ee::Float>> drawn; // the drawn position (kept in the frame, as the original does)
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &forward = *vec4(rider->forward);
        zge::Vec4 &position = *vec4(rider->position);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const ee::Float spinKept = *g_grindSpinKept;
        *drawn = rider->drawnPosition;
        const EdgeRef edge = rider->contactRef();
        rider->wheelPoses[0].spin = ee::toInt(ee::toFloat(rider->wheelPoses[0].spin) * spinKept);
        rider->wheelPoses[1].spin = 0;

        collision::edgeDirection(edge, &direction->x);
        *onRail = 1;
        collision::edgePoint(edge, 0, &toStart->x);
        edgeEndByCall(edge, &toEnd->x);
        *toStart = *toStart - position;
        *toEnd = *toEnd - position;
        const bool betweenEnds = zge::dot(*toStart, *toEnd) <= ee::Float(0.0f);
        const ee::Float nextReach = *g_nextRailReach;
        if (betweenEnds)
        {
            // The forward row a share toward the rail's direction (the way
            // the rider faces), the rows square again.
            *step = *direction;
            if (zge::dot(forward, *step) < ee::Float(0.0f))
                negateByCall(&step->x);
            const ee::Float share = *g_railAlignShare;
            const ee::Float fx = forward.x, fy = forward.y, fz = forward.z;
            const ee::Float y = fy + (step->y - fy) * share;
            const ee::Float z = fz + (step->z - fz) * share;
            forward.x = fx + (step->x - fx) * share;
            forward.y = y;
            forward.z = z;
            right = zge::cross(up, forward);
            right = zge::unit(right);
            up = zge::cross(forward, right);
        }
        else
        {
            // Past an end: onto the next rail, if one is near and turned
            // less than 75 degrees from this one.
            const EdgeRef next = closestRailEdge(rider, rider->position, &step->x, nextReach);
            *onRail = 0;
            if (static_cast<uint32_t>(next) != 0)
            {
                collision::edgeDirection(next, &nextDirection->x);
                const ee::Float cosine = zge::dot(*nextDirection, *direction);
                if (zge::angleAcos(ee::abs(cosine)) < kNextRailMostTurn)
                {
                    grindOnto(rider, next);
                    *onRail = 1;
                }
            }
        }
        if (*onRail != 0)
        {
            // The rays: from a unit above the rider to 0.75 of the rail's
            // direction (the way he moves) to the side, plus the velocity.
            *rayFrom = zge::scaledCarrying(up, ee::Float(1.0f), zge::scaleLeftover(zge::LeftBy::lastCall));
            *rayFrom = *rayFrom + position;
            for (uint32_t ray = 0; ray < 2; ++ray)
            {
                *step = *direction;
                if (zge::dot(velocity, *step) < ee::Float(0.0f))
                    negateByCall(&step->x);
                const uint64_t carried = zge::scaleLeftover(zge::LeftBy::lastCall);
                *step = zge::scaledCarrying(*step, ee::Float(0.75f), carried);
                ee::Float reach = *g_railSideReach;
                if ((ray & 1u) == 0)
                    reach = -reach;
                *side = zge::scaledCarrying(right, reach, carried);
                *step = *step + *side;
                *step = *step + velocity;
                *step = *step + *rayFrom;
                if (rayHitsByCall(&rayFrom->x, &step->x) != 0)
                {
                    *onRail = 0;
                    break;
                }
            }
            if (*onRail != 0)
            {
                position = position + velocity;
                *vec4(*drawn) = *vec4(*drawn) + velocity;
                return;
            }
        }
        // Off the rail: lifted along up, into the air.
        *step = up;
        *step = zge::scaledCarrying(*step, *g_offRailLift, zge::scaleLeftover(zge::LeftBy::lastCall));
        velocity = velocity + *step;
        setPhysicsState(rider, uint32_t(kPhysicsAir));
    }

    READABLE_FUNCTION(grindMove, Verified);
    READABLE_FUNCTION_VU0(closestOnRail, Verified);
    READABLE_FUNCTION(sym::nextRailEdge, sameRailEdge, Verified);
    READABLE_FUNCTION(sym::followRail, closestRailEdge, Verified);
    READABLE_FUNCTION(sym::grindEnd, grindOnto, Verified);
    READABLE_FUNCTION(grindSlope, Verified);
}
