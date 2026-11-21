// The Ground state's move for a frame (groundMove, the state table's
// third callback for Ground, Stand, Manual and Cess Slide): last frame's
// position, velocity and orientation kept, the skater held on a quarter
// pipe's lip, the ground followed and the move made with its collisions,
// a launch once the skater has been off the ground for Air Transition
// Time, and a manual's distance counted.
//
// Like groundPhysics (ground.cpp) its vector code stores a vector whose w
// a callee left (planeDistance); it reaches the position's w.

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/collision/closest.h"

#include <cstring>
#include "sym/aggressive/skater/ground_move.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr int32_t kSurfaceLip = 1;
        constexpr ee::Float kQuarterOut = 0.25f; // how far out of the lip's plane the skater is put
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp; // (0, -1, 0, 0): y grows downward

        enum PhysicsEntry : uint32_t
        {
            kBounceAngleQPipe = 12,
            kAirTransitionTime = 17,
        };
        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }

        // The vectors holdOnLip works with.
        struct LipWork
        {
            ee::Local<Vec4> &across, &toEdge, &alongLip, &plane, &push;
        };

        // On a quarter pipe's lip, steep enough (up within Bounce Angle
        // QPipe of level): the skater is put back on the plane through the
        // lip's edge, a quarter of the way out of it, and moves along it.
        void holdOnLip(ee::Ptr<Player> player, const LipWork &lip)
        {
            const Vec4 &up = player->axes.up;
            const ee::Float upright = zge::dot(up, *g_worldUp);
            const zge::Angle steepest = static_cast<zge::Angle>(0x4000 - static_cast<short>(zge::angleAcos(physics(kBounceAngleQPipe))));
            if (!(upright < zge::angleCos(steepest)))
                return;

            pipeEdgeOf(player->objectNode, player->surface, &(railRecordOf(*player)));
            Vec4 &edge = player->railDirection;
            Vec4 &position = player->position;
            auto &[across, toEdge, alongLip, plane, push] = lip;

            // The edge runs so that up x edge faces out of the surface.
            *across = zge::cross(*g_worldUp, edge);
            if (zge::dot(across, player->surfaceNormal) < ee::Float(0.0f))
                vec3Negate(edge, edge);

            zge::closestOnSegment(toEdge, &player->railStart, &player->railEnd, &position);
            *toEdge = zge::unit(*toEdge - position);
            *across = zge::unit(zge::cross(*g_worldUp, edge));
            if (zge::dot(up, across) < ee::Float(0.0f))
                vec3Negate(across, across);
            if (!(ee::Float(0.0f) < zge::dot(toEdge, across)))
                return;

            // Onto the plane through the edge, then a quarter out of it.
            zge::planeThrough(plane, &player->railStart, across);
            const ee::Float distance = zge::planeDistance(plane, &position);
            *push = *across * distance;
            position = position - *push;
            *push = *across * kQuarterOut;
            position = position + *push;

            // The velocity along the lip at the same speed.
            alongPlane(alongLip, across, &player->velocity);
            *alongLip = zge::unit(alongLip);
            player->velocity = *alongLip * player->speed;
        }
    }

    void groundMove(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> across;
        ee::Local<Vec4> offset; // to the lip's edge; in a manual, the frame's step
        ee::Local<Vec4> alongLip;
        ee::Local<Vec4> plane;
        ee::Local<Vec4> push;
        ee::Float &airTime = player->airTime; // off the ground (moveSkater adds to it)

        player->lastPosition = player->position;
        player->lastVelocity = player->velocity;
        player->lastOrientation = player->orientation;

        if (player->state != State::Vault && ee::Float(0.0f) < airTime && player->object &&
            surfacePhysics(player->objectNode, player->surface) == kSurfaceLip)
            holdOnLip(player, {across, offset, alongLip, plane, push});

        carryRider(player, frameTime);
        skaterMoveCollide(player, frameTime, 0);
        moveByVelocity(player, frameTime);

        // Off the ground long enough: Ground, Manual and Cess Slide take off.
        if (physics(kAirTransitionTime) < airTime)
        {
            const State state = player->state;
            if (state == State::Ground || state == State::Manual || state == State::CessSlide)
                leaveGround(player);
        }
        const State state = player->state;
        if (state == State::Stand)
        {
            airTime = 0.0f;
            return;
        }
        if (state != State::Manual)
            return;

        // A manual counts the distance: in all, and this manual's.
        *offset = player->lastPosition - player->position;
        player->distance = player->distance + vec3Length(offset);
        player->stateDistance = player->stateDistance + vec3Length(offset);
    }
    READABLE_FUNCTION(groundMove, Verified);
}
