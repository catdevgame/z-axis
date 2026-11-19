// BMX XXX's grind (physics.c): the rail's forces. DM2's (game/dm2/rider/
// actions/rails.cpp), read in BMX XXX's machine code.

#include "../collision/collision.h"
#include "../game_calls.h"
#include "../physics/physics.h"
#include "../rider.h"

#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/closest.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sym/bmx/rider/actions/rails.cpp.h"

#include <cstring>

namespace game::bmx::rider
{
    using collision::EdgeRef;

    namespace
    {
        // A rider's contact kind when it is a rail edge.
        constexpr uint8_t kContactRailEdge = 5;
        // The air's gravity, units a frame a frame (airGravity's), and the
        // share of it a rail's slope pulls with (0.75).
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        inline ee::Global<ee::Float, sym::g_railGravityShare> g_railGravityShare;
        // A grind is no faster than this (0.35) times the rider's speed stat.
        inline ee::Global<ee::Float, sym::g_topSpeed> g_topSpeed;
        // Without balance a rail drags ten times more on top of its own.
        constexpr ee::Float kDragWithoutBalance = 10.0f;
    }

    // The closest point of an edge to a point: on the edge's line, or held
    // to its ends (`clamp`). DM2's.
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
        // start + direction x ((point - start) . direction)
        *fromStart = *vec4(point) - *start;
        const ee::Float along = zge::dot(*fromStart, *direction);
        *vec4(out) = zge::scaledAfterMeasure(*direction, along);
        *vec4(out) = *vec4(out) + *start;
    }
    READABLE_FUNCTION(closestOnRail, Verified);

    // Whether two edges are the same (their ends equal, either way round).
    // DM2's.
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
    READABLE_FUNCTION(sameRailEdge, Verified);

    // The rail edge nearest a point within `radius` (its closest point into
    // `closest`), not the one the rider grinds now (state 4 on a rail edge
    // contact). Returns the edge, or 0. DM2's.
    EdgeRef closestRailEdge(Rider *rider, const ee::Float *point, ee::Float *closest, ee::Float radius)
    {
        const EdgeRef *next = collision::railEdgesNear(point, radius); // ended by an edge of 0
        EdgeRef nearest = 0;
        if (next == nullptr)
            return nearest;
        ee::Local<ee::Float[4]> onEdge;
        ee::Local<zge::Vec4> apart;
        ee::Float nearestSquared = radius * radius;
        for (EdgeRef edge = *next++; edge != 0; edge = *next++)
        {
            const bool grinding = physicsState(rider) == kPhysicsGrind;
            if (grinding && rider->contactKind == kContactRailEdge && sameRailEdge(edge, contactRef(rider)) != 0)
                continue;
            closestOnRail(point, edge, 1, onEdge);
            *apart = *vec4(onEdge) - *vec4(point);
            const ee::Float squared = zge::squaredLength(*apart);
            if (squared < nearestSquared)
            {
                nearest = edge;
                nearestSquared = squared;
                *vec4(closest) = *vec4(onEdge);
            }
        }
        return nearest;
    }
    READABLE_FUNCTION(closestRailEdge, Verified);

    namespace
    {
        // The trick flags (a word a trick): this bit, the trick's rows are
        // made from the rail and the right row (DM2's), and its lean is a
        // turn about the right row.
        inline ee::Global<uint32_t[256], sym::g_trickKindsR> g_trickFlags;
        constexpr uint32_t kTrickRowsByRight = 0x80u;
        // The lean at a balance of 1: 35 degrees about the rail, 7.5 about
        // the right row for those tricks.
        inline ee::Global<int16_t, sym::g_railLeanMost> g_railLeanMost;
        inline ee::Global<int16_t, sym::g_railLeanMostByRight> g_railLeanMostByRight;
        // The rows go this share of the way to the leaning rail's a frame (0.135).
        inline ee::Global<ee::Float, sym::g_railLeanShare> g_railLeanShare;
        // The stall's move keeps this much of the front wheel's spin a frame (0.9967).
        inline ee::Global<ee::Float, sym::g_stallSpinKept> g_stallSpinKept;
        const ee::Float kTurnUnit(1.0f / 65536.0f);
    }

    // The rider's rows eased toward the rail's (the snap target's rows)
    // leaning by the grind balance: the target turned about the rail (or,
    // for the tricks whose rows go by the right row, about the right row)
    // by the balance times the most lean; the forward and up rows a share
    // of the way to it; the rows made square again and given to the drawn
    // body. BMX XXX's own (DM2 turns the rows by the tilt in grindBalance).
    void leanRowsToRail(Rider *rider, uint32_t trick)
    {
        ee::Local<zge::Matrix> target;
        ee::Local<zge::Matrix> lean;
        zge::matrixIdentity(lean);
        if (!((*g_trickFlags)[trick] & kTrickRowsByRight))
        {
            const int16_t angle = static_cast<int16_t>(
                ee::toInt(rider->grindBalance * (ee::toFloat(int32_t(*g_railLeanMost)) * kTurnUnit) * ee::Float(65536.0f)));
            zge::matrixRotateZ(lean, lean, angle);
        }
        else
        {
            const int16_t angle = static_cast<int16_t>(
                ee::toInt(rider->grindBalance * (ee::toFloat(int32_t(*g_railLeanMostByRight)) * kTurnUnit) * ee::Float(65536.0f)));
            zge::matrixRotateX(lean, lean, angle);
        }
        zge::matrixMultiply(target, ee::Ptr<zge::Matrix>(matrix(rider->snapRows)), lean);

        const ee::Float share = *g_railLeanShare;
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        const zge::Vec4 &targetUp = target->row(1);
        const zge::Vec4 &targetForward = target->row(2);
        const ee::Float fx = forward.x + (targetForward.x - forward.x) * share;
        const ee::Float fy = forward.y + (targetForward.y - forward.y) * share;
        const ee::Float fz = forward.z + (targetForward.z - forward.z) * share;
        const ee::Float ux = up.x + (targetUp.x - up.x) * share;
        const ee::Float uy = up.y + (targetUp.y - up.y) * share;
        const ee::Float uz = up.z + (targetUp.z - up.z) * share;
        forward.x = fx;
        forward.y = fy;
        forward.z = fz;
        up.x = ux;
        up.y = uy;
        up.z = uz;
        forward = zge::unit(forward);
        right = zge::cross(up, forward);
        right = zge::unit(right);
        up = zge::cross(forward, right);
        *matrix(rider->body) = *matrix(rider->right);
    }
    READABLE_FUNCTION(leanRowsToRail, Verified);

    namespace
    {
        // Up in the y-down world: (0, -1, 0, 0).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // The speed along the rail makes up this share of the speed the
        // turn onto it lost (1: all of it; DM2's 0.75).
        inline ee::Global<ee::Float, sym::g_railSpeedKept> g_railSpeedKept;
        // A boosted start of a grind: the velocity along the rail times
        // this (1.1), and the speed then at least this (0.175).
        inline ee::Global<ee::Float, sym::g_railBoost> g_railBoost;
        inline ee::Global<ee::Float, sym::g_railBoostSpeedLeast> g_railBoostSpeedLeast;
    }

    // The rider put onto an edge: the position (and the drawn one) at its
    // closest point; the snap target's rows from the rail (forward along
    // it the way the rider faces, right level, up square to both) and the
    // rider's rows leaned to them for `trick`; the velocity along the rail
    // (slower than the speed: made up by a share of the rest), boosted
    // when `boost` says; the contact the edge. DM2's, with BMX XXX's boost
    // and its rows made every time.
    void grindOnto(Rider *rider, EdgeRef edge, int32_t boost, uint32_t trick)
    {
        ee::Local<zge::Vec4> onEdge;   // the rail's point nearest the rider
        ee::Local<zge::Vec4> kept;     // the velocity along the rail (its fourth lane is never set: what the stack held goes to the velocity's with it)
        ee::Local<ee::Float[4]> direction;
        ee::Local<zge::Matrix> rows;   // the rail's rows: right, up, forward (along the rail), position
        ee::Local<EdgeRef> railEdge;
        *railEdge = edge;
        closestOnRail(rider->position, edge, 1, &onEdge->x);
        *vec4(rider->position) = *onEdge;
        *vec4(rider->drawnPosition) = *onEdge;
        collision::edgeDirection(*railEdge, direction);
        zge::Vec4 &along = rows->row(2);
        along = *vec4(direction);
        if (zge::dot(*vec4(rider->forward), along) < ee::Float(0.0f))
            zge::vec3Negate(&along.xyz(), &along.xyz());
        // right = up x along, up = along x right
        rows->row(0) = zge::cross(*g_worldUp, along);
        rows->row(0) = zge::unit(rows->row(0));
        rows->row(1) = zge::cross(along, rows->row(0));
        rows->row(3) = zge::kZero;
        *matrix(rider->snapRows) = *rows;
        leanRowsToRail(rider, trick);

        // The speed along the rail: at least the share g_railSpeedKept of
        // what the turn took off it.
        const zge::Vec4 &rail = matrix(rider->snapRows)->row(2);
        ee::Float speedAlong = zge::dot(*vec4(rider->velocity), rail);
        const ee::Float speed = rider->speed;
        if (ee::abs(speedAlong) < speed)
        {
            const bool backwards = speedAlong < ee::Float(0.0f);
            const ee::Float magnitude = ee::abs(speedAlong);
            speedAlong = magnitude + (speed - magnitude) * *g_railSpeedKept;
            if (backwards)
                speedAlong = -speedAlong;
        }
        kept->x = rail.x * speedAlong;
        kept->y = rail.y * speedAlong;
        kept->z = rail.z * speedAlong;
        // (Each scale's factor goes to VU0 by the register a quadword was
        // last copied through: the rider's right row, then the kept
        // velocity; their upper lanes go with it.)
        if (boost != 0)
            *kept = zge::scaledCarrying(*kept, *g_railBoost, physics::upperHalf(*vec4(rider->right)));
        zge::Vec4 &velocity = *vec4(rider->velocity);
        velocity = *kept;
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (boost != 0 && rider->speed < *g_railBoostSpeedLeast)
        {
            velocity = zge::scaledCarrying(velocity, *g_railBoostSpeedLeast / rider->speed, physics::upperHalf(*kept));
            rider->speed = *g_railBoostSpeedLeast;
        }
        std::memcpy(rider->contact, &*railEdge, sizeof(EdgeRef));
        setContact(rider, &*railEdge, kContactRailEdge);
    }
    READABLE_FUNCTION(grindOnto, Verified);

    // The stall's move (physics state 6): the front wheel's spin runs down,
    // the back wheel stops; on a rail edge the rows lean to it by the
    // balance (the original's last act a jump there).
    void stallMove(Rider *rider)
    {
        const ee::Float kept = *g_stallSpinKept;
        rider->wheelPoses[0].spin = ee::toInt(ee::toFloat(rider->wheelPoses[0].spin) * kept);
        rider->wheelPoses[1].spin = 0;
        if (rider->contactKind == kContactRailEdge)
            game::tail(leanRowsToRail, rider, uint32_t(rider->trick));
    }
    READABLE_FUNCTION(stallMove, Verified);

    // The grind's forces: the slope pulls along the rail (a share of
    // gravity times the rail's fall, its direction the way the rider faces)
    // into the force and the velocity; the rail's surface drags the
    // velocity, less with more balance; the speed, held to the rider's top
    // speed (BMX XXX's own step: DM2 has no limit here, and pulls with all
    // of gravity).
    void grindSlope(Rider *rider)
    {
        ee::Local<zge::Vec4> pull;
        ee::Local<zge::Vec4> direction;
        const collision::EdgeRef edge = contactRef(rider);
        collision::edgeDirection(edge, &direction->x);
        *pull = *direction;
        if (zge::dot(*vec4(rider->forward), *direction) < ee::Float(0.0f))
            zge::vec3Negate(&pull->xyz(), &pull->xyz());
        const ee::Float share = *g_gravity * *g_railGravityShare * pull->y;
        pull->z = pull->z * share;
        pull->x = pull->x * share;
        pull->y = pull->y * share;
        *vec4(rider->force) = *pull;
        zge::Vec4 &velocity = *vec4(rider->velocity);
        velocity = velocity + *pull;
        const physics::Surface &surface = (*physics::g_surfaces)[collision::edgeWallKind(edge)];
        const ee::Float kept = ee::Float(1.0f) - surface.grindDrag * ((ee::Float(1.0f) - rider->balanceStat) * kDragWithoutBalance + ee::Float(1.0f));
        // (The scale's factor goes to VU0 by the register the pull was
        // copied to the force through: its upper lanes go with it.)
        velocity = zge::scaledCarrying(velocity, kept, physics::upperHalf(*pull));
        const ee::Float top = *g_topSpeed * rider->speedStat;
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (top < rider->speed)
        {
            velocity = zge::unit(velocity);
            velocity = zge::scaledAfterMeasure(velocity, top);
            rider->speed = top;
        }
    }
    READABLE_FUNCTION(grindSlope, Verified);

    namespace
    {
        // railMount's: the drawn rider over the rail (above it, or within 0.5 of it).
        constexpr ee::Float kNearSquared(0.25f);
        // Slower than this along the rail, a stall (0.0375).
        inline ee::Global<ee::Float, sym::g_stallSpeed> g_stallSpeed;
        // The trick flags: this bit, a trick with rows by the right row is not tilted by the rail's grind side
        // (the toothpick, the icepick, the tail tap).
        constexpr uint32_t kTrickUntilted = 0x100u;
        // A trick with rows by the right row is tilted 25 degrees about the right row toward the rail's grind side.
        inline ee::Global<zge::Angle, sym::g_railTiltByRight> g_railTiltByRight;
    }

    // railMount (rider, edge, state): onto a rail edge from the ground or the air (grindEnter's mount: DM2's
    // grindEnter's first half). The edge must be grindable, the drawn rider above the rail's nearest point or
    // within 0.5 of it, and the edge have a plane. The snap point the nearest point; a grind (4) in `state`, or a
    // stall (6) when slower than 0.0375 along the rail, with its trick. The rows: by the rail (ahead along it, up
    // upwards, both turned to the rider's forward; a grind against its ahead row turns the balance round,
    // kFlagBalanceTurnedRound), or for a trick with rows by the right row, the right row along the rail (the
    // rider's way) tilted toward the rail's grind side. The mirrored side by the right row against the rail's
    // plane, the contact the edge, the rows the snap rows and the rider's. 1 when the rider goes onto it, else 0.
    int32_t railMount(Rider *rider, EdgeRef edge, uint32_t *state)
    {
        ee::Local<zge::Matrix> rows; // right, up, ahead, (0, 0, 0, 1)
        ee::Local<zge::Matrix> tilt;
        ee::Local<zge::Vec4> apart;
        ee::Local<ee::Float[4]> onRail;
        ee::Local<ee::Float[4]> direction;
        ee::Local<zge::Vec4> plane; // the rail's plane, the side the rider is on
        ee::Local<ee::Float[4]> railPlane;
        ee::Local<EdgeRef> rail;
        auto negate = [](zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); };
        *rail = edge;
        if (collision::edgeGrindable(*rail) == 0)
            return 0;
        closestOnRail(rider->position, *rail, 0, onRail);
        *apart = *vec4(rider->drawnPosition) - *vec4(onRail);
        const ee::Float near = zge::squaredLength(*apart);
        if (!(apart->y < ee::Float(0.0f)) && !(near < kNearSquared))
            return 0;
        if (collision::edgePlane(*rail, railPlane) == 0)
            return 0;
        *vec4(rider->snapPoint) = *vec4(onRail);
        *plane = *vec4(railPlane);
        if (planeDistance(rider->position, railPlane) < ee::Float(0.0f))
            negate(*plane);
        collision::edgeDirection(*rail, direction);
        uint8_t trick;
        if (*g_stallSpeed <= ee::abs(zge::dot(*vec4(direction), *vec4(rider->velocity))))
        {
            *state = kPhysicsGrind;
            trick = grindTrick(rider);
        }
        else
        {
            *state = kPhysicsStall;
            trick = stallTrick(rider);
        }
        const uint32_t kinds = (*g_trickFlags)[trick];
        zge::Vec4 &right = rows->row(0);
        zge::Vec4 &up = rows->row(1);
        zge::Vec4 &ahead = rows->row(2);
        bool backwards = false;
        if (!(kinds & kTrickRowsByRight))
        {
            collision::edgeDirection(*rail, direction);
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
            if (*state != kPhysicsStall)
                backwards = zge::dot(ahead, *vec4(rider->velocity)) < ee::Float(0.0f);
        }
        else
        {
            collision::edgeDirection(*rail, direction);
            right = *vec4(direction);
            if (zge::dot(*vec4(rider->right), right) < ee::Float(0.0f))
                negate(right);
            ahead = zge::cross(right, *g_worldUp);
            ahead = zge::unit(ahead);
            up = zge::cross(ahead, right);
            const int32_t side = (kinds & kTrickUntilted) ? 0 : collision::edgeGrindSide(*rail);
            if (side != 0)
            {
                bool away = side > 0;
                zge::Angle angle = *g_railTiltByRight;
                if (zge::dot(*vec4(rider->right), right) < ee::Float(0.0f))
                    away = !away;
                if (away)
                    angle = static_cast<zge::Angle>(-angle);
                zge::matrixIdentity(tilt);
                zge::matrixRotateX(tilt, tilt, angle);
                zge::matrixMultiply(rows, rows, tilt);
            }
        }
        rows->row(3) = zge::kZero;
        // The original means to turn the balance round when the grind's way changes, but compares the 1 with the
        // flag's bit (0x80): it turns round whenever either is set (bug kept).
        if (backwards || hasFlag(rider, kFlagBalanceTurnedRound))
            rider->grindBalance = -rider->grindBalance;
        if (backwards)
            setFlag(rider, kFlagBalanceTurnedRound);
        else
            clearFlag(rider, kFlagBalanceTurnedRound);
        if (zge::dot(right, *plane) < ee::Float(0.0f))
            setFlag(rider, kFlagMirrorSide);
        else
            clearFlag(rider, kFlagMirrorSide);
        setContact(rider, &*rail, kContactRailEdge);
        *matrix(rider->snapRows) = *rows;
        *matrix(rider->right) = *rows;
        return 1;
    }
    READABLE_FUNCTION(railMount, Verified);

    namespace
    {
        // grindEnter's: the face kinds of the ground's contacts (1 .. 3) a rail can be found from.
        constexpr uint8_t kContactFaceKinds = 4;
        // The grind's sound (the rider sound table's 33) by the rail's surface: a wall kind 1 sounds as 7, 2 as
        // 2, the rest as 0; kind 2 of the rider's sounds, at full volume.
        constexpr uint32_t kSoundGrindStart = 33;
        constexpr uint32_t kGrindSoundKind = 2;
        constexpr uint32_t kSoundVolume = 127;
        // How a rail touches its object (touchObject's word).
        constexpr uint32_t kTouchGrind = 5;
    }

    // grindEnter (rider, from, state): physics state 4's enter. From the ground, the air or a wheelie (0, 1, 9,
    // 10): onto the rail edge linked to the face the rider is on, when there is one and the rider can go onto it
    // (railMount, which can make it a stall); off the ground both wheels' contacts become the rider's. Then the
    // angles still; a grind goes onto the rail with a boost, else the rider stops at the snap point; fakie by the
    // velocity; the rail's object touched; the grind's sound by the rail's surface. Returns the state (or the state
    // it came from when there is no rail). DM2's, with the mount apart.
    uint32_t grindEnter(Rider *rider, uint32_t from, uint32_t state)
    {
        ee::Local<uint32_t> to;
        *to = state;
        if (from == kPhysicsGround || from == kPhysicsAir || from == kPhysicsNoseWheelie || from == kPhysicsManual)
        {
            const uint8_t kind = rider->contactKind;
            if (kind >= kContactFaceKinds || kind == 0)
                return from;
            const EdgeRef edge = collision::faceLinkedEdge(contactRef(rider));
            if (!railMount(rider, edge, &*to))
                return from;
            if (from != kPhysicsAir)
            {
                setWheelContact(rider, 0u, rider->contact, uint32_t(rider->contactKind), rider->position);
                setWheelContact(rider, 1u, rider->contact, uint32_t(rider->contactKind), rider->position);
            }
        }
        rider->snapFrames = 0;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        const EdgeRef onEdge = contactRef(rider);
        if (*to == kPhysicsGrind)
        {
            grindOnto(rider, onEdge, 1, uint32_t(rider->trick));
        }
        else
        {
            *vec4(rider->velocity) = zge::kZero;
            *vec4(rider->force) = zge::kZero;
            rider->speed = 0.0f;
            *vec4(rider->position) = *vec4(rider->snapPoint);
            *vec4(rider->drawnPosition) = *vec4(rider->snapPoint);
        }
        if (zge::dot(*vec4(rider->velocity), *vec4(rider->forward)) < ee::Float(0.0f))
            setFlag(rider, kFlagFakie);
        else
            clearFlag(rider, kFlagFakie);
        const uint32_t wall = collision::edgeWallKind(contactRef(rider));
        const uint32_t surface = wall == 1 ? 7u : wall == 2 ? 2u : 0u;
        collision::touchObject(rider, nullptr, kTouchGrind);
        rider->groundHeight = 0.0f;
        const int32_t code = surfaceSoundFlags(0u, 0u, kGrindSoundKind, surface);
        soundEvent(kSoundGrindStart, rider->position, ee::Address(code), kSoundVolume, 0u);
        return *to;
    }
    READABLE_FUNCTION(grindEnter, Verified);

    namespace
    {
        // grindMove's: the back wheel's spin kept a frame (0.9967; the front's stops); past an end the next rail
        // within 0.35, turned less than 75 degrees from this one.
        inline ee::Global<ee::Float, sym::g_grindSpinKept> g_grindSpinKept;
        inline ee::Global<ee::Float, sym::g_nextRailReach> g_nextRailReach;
        inline ee::Global<zge::Angle, sym::g_nextRailMostTurn> g_nextRailMostTurn;
        // The rays: from a unit above the rider to 0.75 along the rail (the way he moves) and 0.35 to each side
        // (the word pushCornersClear reads as its corners' side), plus the velocity.
        constexpr ee::Float kRayHeight(1.0f);
        constexpr ee::Float kRayAhead(0.75f);
        inline ee::Global<ee::Float, sym::g_cornerSide> g_railSideReach;
        // Blocked, the rider is pushed off the rail's side by a sixtieth of a unit a frame; off the rail, lifted
        // 0.075 up.
        inline ee::Global<ee::Float, sym::g_railBlockedPush> g_railBlockedPush;
        inline ee::Global<ee::Float, sym::g_offRailLift> g_offRailLift;
    }

    // grindMove (rider): state 4's move. The back wheel's spin runs down (the front stops); between the rail's
    // ends the rows lean to the rail (leanRowsToRail), past an end the rider goes onto the next rail if there is
    // one; then two rays from above the rider, ahead along the rail and to each side, must find nothing: the
    // rider moves on by his velocity. Blocked, he is pushed off the rail's side (by its grind side, the rail rows'
    // right row; else that row turned up) and falls off (kFlagFellOff); off the rail, the velocity lifts and the
    // rider is in the air (kFlagOffRailIntoAir). DM2's, with BMX XXX's lean and the push off a blocked rail.
    void grindMove(Rider *rider)
    {
        ee::Local<zge::Vec4> step;
        ee::Local<zge::Vec4> toStart;
        ee::Local<zge::Vec4> toEnd;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> nextDirection;
        ee::Local<zge::Vec4> rayFrom;
        ee::Local<zge::Vec4> side;
        auto negate = [](zge::Vec4 &v) { zge::vec3Negate(&v.xyz(), &v.xyz()); };
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &position = *vec4(rider->position);
        zge::Vec4 &velocity = *vec4(rider->velocity);
        const ee::Float spinKept = *g_grindSpinKept;
        const EdgeRef edge = contactRef(rider);
        for (uint32_t i = 0; i < 2; ++i)
        {
            WheelPose &wheel = rider->wheelPoses[i];
            if ((i & 1u) != 0)
                wheel.spin = 0;
            else
                wheel.spin = ee::toInt(ee::toFloat(wheel.spin) * spinKept);
        }

        collision::edgeDirection(edge, &direction->x);
        bool onRail = true;
        collision::edgePoint(edge, 0, &toStart->x);
        collision::edgePoint(edge, 1, &toEnd->x);
        *toStart = *toStart - position;
        *toEnd = *toEnd - position;
        const ee::Float nextReach = *g_nextRailReach;
        if (zge::dot(*toStart, *toEnd) <= ee::Float(0.0f))
        {
            leanRowsToRail(rider, uint32_t(rider->trick));
        }
        else
        {
            // Past an end: onto the next rail, if one is near and turned less than 75 degrees from this one.
            const EdgeRef next = closestRailEdge(rider, rider->position, &step->x, nextReach);
            onRail = false;
            if (next != 0)
            {
                collision::edgeDirection(next, &nextDirection->x);
                if (zge::angleAcos(ee::abs(zge::dot(*nextDirection, *direction))) < *g_nextRailMostTurn)
                {
                    grindOnto(rider, next, 0, uint32_t(rider->trick));
                    onRail = true;
                }
            }
        }
        if (onRail)
        {
            *rayFrom = up * kRayHeight;
            *rayFrom = *rayFrom + position;
            for (uint32_t ray = 0; ray < 2; ++ray)
            {
                *step = *direction;
                if (zge::dot(velocity, *step) < ee::Float(0.0f))
                    negate(*step);
                *step = *step * kRayAhead;
                ee::Float reach = *g_railSideReach;
                if ((ray & 1u) == 0)
                    reach = -reach;
                *side = right * reach;
                *step = *step + *side;
                *step = *step + velocity;
                *step = *step + *rayFrom;
                if (collision::rayCastTouching(rider, &rayFrom->x, &step->x, kTouchGrind) == nullptr)
                    continue;
                // Blocked: pushed off the rail's side.
                *step = zge::matrixRow(*matrix(rider->snapRows), 0);
                const int32_t grindSide = collision::edgeGrindSide(edge);
                if (grindSide != 0)
                {
                    bool away = grindSide > 0;
                    if (zge::dot(*direction, zge::matrixRow(*matrix(rider->snapRows), 2)) < ee::Float(0.0f))
                        away = !away;
                    if (away)
                        negate(*step);
                }
                else if (zge::dot(*step, up) < ee::Float(0.0f))
                {
                    negate(*step);
                }
                *step = *step * *g_railBlockedPush;
                velocity = velocity + *step;
                onRail = false;
                setFlag(rider, kFlagFellOff);
                break;
            }
            if (onRail)
            {
                position = position + velocity;
                *vec4(rider->drawnPosition) = *vec4(rider->drawnPosition) + velocity;
                return;
            }
        }
        // Off the rail: lifted, into the air.
        *step = *g_worldUp;
        *step = *step * *g_offRailLift;
        velocity = velocity + *step;
        setPhysicsState(rider, uint32_t(kPhysicsAir));
        setFlag(rider, kFlagOffRailIntoAir);
    }
    READABLE_FUNCTION(grindMove, Verified);

    namespace
    {
        // stallLipBalance's contact kinds: a stall (7), a lip (8, 9).
        constexpr uint8_t kContactStall = 7;
        constexpr uint8_t kContactLipFirst = 8, kContactPastLip = 10;
        // A stall's tilt falls back by 0.35 of itself a frame; a lip's grows by its lean (the right row's y, at
        // least 0.1 either way) against the balance stat, 0.002 a turn a frame, and falls back by 0.35 of itself;
        // the up row tipped to within 15 degrees of level: into the air.
        inline ee::Global<ee::Float, sym::g_stallPitchFalloff> g_stallPitchFalloff;
        inline ee::Global<ee::Float, sym::g_lipLeanLeast> g_lipLeanLeast;
        inline ee::Global<ee::Float, sym::g_lipLeanLeastBack> g_lipLeanLeastBack;
        inline ee::Global<ee::Float, sym::g_lipTip> g_lipTip;
        inline ee::Global<ee::Float, sym::g_lipPitchFalloff> g_lipPitchFalloff;
        constexpr int32_t kLipFallAngle = 0xAAA;

        int16_t pitchFalloff(int16_t pitch, ee::Float share)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(pitch) - ee::toInt(ee::toFloat(pitch) * kTurnUnit * share * ee::Float(65536.0f)));
        }
    }

    // stallLipBalance (rider): states 4 and 6's turn: on a stall the tilt (the pitch) undone about the up row; on
    // a lip the tilt grows by the lean against the balance stat, the rows and the body turned by it about the
    // forward row, off it into the air once the up row has tipped to within 15 degrees of level. DM2's grindBalance
    // without its rail (BMX XXX balances a rail in its grind's sub-states).
    void stallLipBalance(Rider *rider)
    {
        ee::Local<zge::Matrix> turn;
        zge::matrixIdentity(turn);
        const uint8_t kind = rider->contactKind;
        if (kind == kContactStall)
        {
            zge::matrixRotateY(turn, turn, static_cast<zge::Angle>(-rider->pitch));
            rider->pitch = pitchFalloff(rider->pitch, *g_stallPitchFalloff);
            zge::matrixMultiply(matrix(rider->right), matrix(rider->right), turn);
            zge::matrixMultiply(matrix(rider->body), matrix(rider->body), turn);
            return;
        }
        if (kind < kContactLipFirst || kind >= kContactPastLip)
            return;
        ee::Float lean = rider->right[1];
        if (ee::abs(lean) < *g_lipLeanLeast)
            lean = ee::Float(0.0f) <= lean ? *g_lipLeanLeast : *g_lipLeanLeastBack;
        const int32_t pitch = static_cast<uint16_t>(rider->pitch) -
                              ee::toInt(lean * (ee::Float(1.0f) - rider->balanceStat + ee::Float(1.0f)) * *g_lipTip * ee::Float(65536.0f));
        rider->pitch = static_cast<int16_t>(pitch);
        zge::matrixRotateZ(turn, turn, static_cast<zge::Angle>(pitch));
        zge::matrixMultiply(matrix(rider->right), matrix(rider->right), turn);
        zge::matrixMultiply(matrix(rider->body), matrix(rider->body), turn);
        const ee::Float down = -rider->up[1];
        rider->pitch = pitchFalloff(rider->pitch, *g_lipPitchFalloff);
        if (zge::angleAsin(down) < kLipFallAngle)
            setPhysicsState(rider, uint32_t(kPhysicsAir));
    }
    READABLE_FUNCTION(stallLipBalance, Verified);

    namespace
    {
        // stallEnter's: a stall's or a lip's contact touches its object with word 4 (a rail's with kTouchGrind); the drop below
        // a lip found by a ray 100 down from the target's top. The sound's kind by the trick: 5 for a trick with
        // flag 0x100 (railMount's kTrickUntilted), 3 for tricks 0x3D and 0x3E, 4 for 0x43 .. 0x45, else 2; kinds 2
        // and 3 rumble the pad (the large motor, 200). Trick 0x47 keeps moving along the forward row (1.25 of
        // its speed along it, at least 0.1333 a frame).
        constexpr uint32_t kTouchStallOrLip = 4;
        constexpr ee::Float kLipProbeDepth(100.0f);
        constexpr uint32_t kTrickStallSound5 = 0x100u;
        constexpr uint8_t kTrickSound3First = 0x3D, kTrickSound3Last = 0x3E;
        constexpr uint8_t kTrickSound4First = 0x43, kTrickSound4Last = 0x45;
        constexpr uint32_t kStallRumbleMotor = 1;
        constexpr uint8_t kStallRumbleStrength = 200;
        constexpr uint8_t kTrickKeepsMoving = 0x47;
        inline ee::Global<ee::Float, sym::g_stallKeptSpeedShare> g_stallKeptSpeedShare;
        inline ee::Global<ee::Float, sym::g_stallKeptSpeedLeast> g_stallKeptSpeedLeast;
    }

    // stallEnter (rider, from, state): physics states 6 and 8's enter (a stall, a lip trick): the contact's object
    // touched; the snap over, the rows the target's, the angles still; on a lip the drop below it (a ray down) the
    // ground height, else 0; the position at the target; the sound by the trick and the rail's surface (and the
    // pad's rumble for some); the rider stopped, but for trick 0x47, which keeps moving along his forward row (the
    // alignment ended). Returns the state. DM2's, with the lips one contact kind on and BMX XXX's sounds.
    uint32_t stallEnter(Rider *rider, uint32_t /*from*/, uint32_t state)
    {
        ee::Local<ee::Float[4]> from;
        ee::Local<ee::Float[4]> to;
        ee::Local<ee::Float[4]> below;
        ee::Local<calls::Rumble> rumble;
        const uint8_t kind = rider->contactKind;
        collision::touchObject(rider, nullptr, kind >= kContactStall && kind < kContactPastLip ? kTouchStallOrLip : kTouchGrind);
        rider->snapFrames = 0;
        *matrix(rider->right) = *matrix(rider->snapRows);
        const uint8_t contact = rider->contactKind;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        if (contact >= kContactLipFirst && contact < kContactPastLip)
        {
            // A lip: a ray from the target's top (its up row added) down; the drop to the hit the ground height.
            const ee::Float *const target = rider->snapPoint;
            const ee::Float *const up = &rider->snapRows[4];
            const ee::Float y = target[1] + up[1];
            const ee::Float x = target[0] + up[0];
            const ee::Float z = target[2] + up[2];
            from[1] = y;
            to[0] = x;
            from[0] = x;
            to[2] = z;
            to[1] = y + kLipProbeDepth;
            from[2] = z;
            if (const collision::RayHit *hit = collision::rayCast(from, to))
            {
                collision::hitPoint(hit, below);
                rider->groundHeight = below[1] - from[1];
            }
        }
        else
        {
            rider->groundHeight = 0.0f;
        }
        *vec4(rider->position) = *vec4(rider->snapPoint);
        *vec4(rider->drawnPosition) = *vec4(rider->snapPoint);

        // The sound: by the trick, on a rail by its surface; the rumble.
        uint32_t surface = 0;
        if (rider->contactKind == kContactRailEdge)
        {
            const uint32_t wall = collision::edgeWallKind(contactRef(rider));
            surface = wall == 1 ? 7u : wall == 2 ? 2u : 0u;
        }
        const uint8_t trick = rider->trick;
        uint32_t soundKind = 2;
        if ((*g_trickFlags)[trick] & kTrickStallSound5)
            soundKind = 5;
        else if (trick >= kTrickSound3First && trick <= kTrickSound3Last)
            soundKind = 3;
        else if (trick >= kTrickSound4First && trick <= kTrickSound4Last)
            soundKind = 4;
        const int32_t code = surfaceSoundFlags(0u, 0u, soundKind, surface);
        soundEvent(kSoundGrindStart, rider->position, ee::Address(code), kSoundVolume, 0u);
        if (!hasFlag(rider, kFlagNoPlayer) && (soundKind == 2 || soundKind == 3))
        {
            rumble->motor = kStallRumbleMotor;
            rumble->strength = kStallRumbleStrength;
            rumble->timing = 0;
            calls::rumble(uint32_t(rider->player), rumble);
        }
        zge::Vec4 &velocity = *vec4(rider->velocity);
        if (rider->trick != kTrickKeepsMoving)
        {
            velocity = zge::kZero;
            *vec4(rider->force) = zge::kZero;
            rider->speed = 0.0f;
            return state;
        }
        // (Each scale brings its factor in by the register the dot product went out by: its upper lanes go too.)
        const zge::Vec4 &forward = *vec4(rider->forward);
        velocity = zge::scaledAfterMeasure(forward, zge::dot(velocity, forward));
        velocity = zge::scaledAfterMeasure(velocity, *g_stallKeptSpeedShare);
        rider->speed = zge::vec3Length(&velocity.xyz());
        if (rider->speed < *g_stallKeptSpeedLeast)
        {
            velocity = zge::scaledCarrying(forward, *g_stallKeptSpeedLeast, zge::scaleLeftover(zge::LeftBy::lastCall));
            rider->speed = *g_stallKeptSpeedLeast;
        }
        clearFlag(rider, kFlagSnapAlign);
        rider->alignAngles[1] = 0;
        rider->alignAngles[2] = 0;
        rider->alignAngles[0] = 0;
        rider->snapFrames = 0;
        return state;
    }
    READABLE_FUNCTION(stallEnter, Verified);

    namespace
    {
        // fallOffRail's: with ground beside a rail (a face of kinds 1 .. 3 below), the rider set 0.4 to the rail
        // rows' right side (the other on the mirrored side) and 0.1 down, falling a further 1/120 a frame; else he
        // tumbles off: 0.35 of the velocity along the ground kept, 0.05 up, pushed out from the rail along its
        // side (0.025) or, for a trick with rows by the right row, ahead (0.023) or behind (0.015), and down by
        // 16 frames' gravity.
        constexpr uint32_t kGroundFaceKinds = 4;
        inline ee::Global<ee::Float, sym::g_fallOffSide> g_fallOffSide;
        inline ee::Global<ee::Float, sym::g_fallOffDrop> g_fallOffDrop;
        inline ee::Global<ee::Float, sym::g_fallOffGroundDrop> g_fallOffGroundDrop;
        inline ee::Global<ee::Float, sym::g_fallOffKeep> g_fallOffKeep;
        inline ee::Global<ee::Float, sym::g_fallOffLift> g_fallOffLift;
        inline ee::Global<ee::Float, sym::g_fallOffAside> g_fallOffAside;
        inline ee::Global<ee::Float, sym::g_fallOffAhead> g_fallOffAhead;
        inline ee::Global<ee::Float, sym::g_fallOffBehind> g_fallOffBehind;
        constexpr ee::Float kPushFrames(16.0f);
    }

    // fallOffRail (rider): the fall the grind's and stall's balance tip over into. Grinding with ground beside the
    // rail below: onto it (physics state 0), beside the rail and a little down, both wheels rolling with the
    // rider's velocity, the rows turned along the rail the way the rider faces. Else off the rail's side into the
    // tumble (physics state 12, kFlagFellOff). DM2's grindBalance's fall, with BMX XXX's ground beside the rail.
    void fallOffRail(Rider *rider)
    {
        ee::Local<zge::Vec4> step;
        ee::Local<zge::Matrix> rows; // the rail's rows, flattened
        ee::Local<int32_t> kind;
        ee::Local<uint64_t> face;
        ee::Local<uint64_t> edge;
        ee::Local<uint32_t> object;
        zge::Vec4 &velocity = *vec4(rider->velocity);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        const zge::Matrix &railRows = *matrix(rider->snapRows);
        if (groundContactBelow(rider, kind, face, edge, object) && static_cast<uint32_t>(*kind) < kGroundFaceKinds && *kind != 0 &&
            physicsState(rider) == kPhysicsGrind)
        {
            // Onto the ground beside the rail.
            *step = railRows.row(0) * *g_fallOffSide;
            if (hasFlag(rider, kFlagMirrorSide))
                zge::vec3Negate(&step->xyz(), &step->xyz());
            step->y = *g_fallOffDrop;
            *vec4(rider->position) = *vec4(rider->position) + *step;
            *vec4(rider->drawnPosition) = *vec4(rider->position);
            WheelPose &front = rider->wheelPoses[0];
            WheelPose &back = rider->wheelPoses[1];
            *vec4(front.velocity) = velocity;
            *vec4(back.velocity) = velocity;
            const ee::Float drop = *g_fallOffGroundDrop;
            velocity.y = velocity.y + drop;
            front.velocity[1] = front.velocity[1] + drop;
            back.velocity[1] = back.velocity[1] + drop;
            front.ground = kWheelRolling;
            back.ground = kWheelRolling;
            if (ee::Float(0.0f) <= zge::dot(forward, railRows.row(2)))
                forward = railRows.row(2);
            else
                zge::vec3Negate(&forward.xyz(), &railRows.row(2).xyz());
            right = zge::cross(up, forward);
            right = zge::unit(right);
            up = zge::cross(forward, right);
            *matrix(rider->body) = *matrix(rider->right);
            *matrix(front.matrix) = *matrix(rider->right);
            *matrix(back.matrix) = *matrix(rider->right);
            setPhysicsState(rider, uint32_t(kPhysicsGround));
            return;
        }

        // Off the rail's side, tumbling.
        *step = velocity;
        step->y = 0.0f;
        *step = *step * *g_fallOffKeep;
        step->y = velocity.y - *g_fallOffLift;
        velocity = *step;
        *rows = railRows;
        zge::Vec4 &across = rows->row(0);
        zge::Vec4 &push = rows->row(1);
        zge::Vec4 &ahead = rows->row(2);
        const zge::Vec4 *out;
        ee::Float by;
        if (!((*g_trickFlags)[rider->trick] & kTrickRowsByRight))
        {
            across.y = 0.0f;
            across = zge::unit(across);
            push = zge::cross(ahead, across);
            if (zge::dot(across, up) < ee::Float(0.0f))
                zge::vec3Negate(&across.xyz(), &across.xyz());
            *step = push * (*g_gravity * kPushFrames);
            out = &across;
            by = *g_fallOffAside;
        }
        else
        {
            ahead.y = 0.0f;
            ahead = zge::unit(ahead);
            push = zge::cross(ahead, across);
            if (zge::dot(ahead, up) < ee::Float(0.0f))
                zge::vec3Negate(&ahead.xyz(), &ahead.xyz());
            *step = push * (*g_gravity * kPushFrames);
            out = &ahead;
            by = zge::dot(forward, push) < ee::Float(0.0f) ? *g_fallOffBehind : *g_fallOffAhead;
        }
        step->x = step->x + out->x * by;
        step->y = step->y + out->y * by;
        step->z = step->z + out->z * by;
        velocity = velocity + *step;
        setPhysicsState(rider, uint32_t(kPhysicsTumble));
        setFlag(rider, kFlagFellOff);
    }
    READABLE_FUNCTION(fallOffRail, Verified);
}
