// The skater leaves the ground (was the sweep's leaveGround; Ground's move
// and controls call it when the skater takes off): which air state it
// goes into, from the surface it leaves.
//
//   - a quarter pipe (surface physics 1), the skater tilted past Physics /
//     Bounce Angle QPipe: Air QPipe, along the pipe's lip edge (Player's
//     rail fields: pipeEdgeOf fills them), the edge's direction turned so
//     that its square points away from the face. Holding the bowl
//     jump-out (kActionBowlJumpOut) over any edge but kind 9 goes into
//     the plain Air instead, pushed over the lip at the speed times
//     Physics / Bowl Jumpout;
//   - a spine (2), the skater tilted past 0.8 and heading up (forward
//     against world up): Air Spine, along the spine's edge the same way;
//   - anything else, or no object: Air.
//

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "aggressive/skater/skater.h"
#include "sym/aggressive/skater/leave_ground.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // The skater's up axis on flat ground (0, -1, 0): the game's y points down.
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp;
        // A spine's tilt (up against world up) below which the
        // skater goes into Air Spine (0.8).
        inline ee::Global<ee::Float, sym::g_spineTilt> g_spineTilt;

        enum SurfacePhysics : uint32_t
        {
            kSurfaceQuarterPipe = 1,
            kSurfaceSpine = 2,
        };
        enum PhysicsEntry : uint32_t
        {
            kBounceAngleQPipe = 12,
            kBowlJumpout = 38,
        };
        constexpr uint32_t kEdgeNoJumpOut = 9; // an edge kind the bowl jump-out does not leave by
        constexpr int32_t kRightAngle = 0x4000; // 90 degrees as an angle
    }

    // leaveGround: the air state the skater takes off into (Air, Air QPipe
    // or Air Spine), set with setSkaterState.
    void leaveGround(ee::Ptr<Player> player)
    {
        ee::Local<Vec4> square; // across the lip: world up x the edge
        ee::Local<Vec4> push;   // the bowl jump-out's

        Vec4 &velocity = player->velocity;
        Vec4 &edgeDirection = player->railDirection;
        const Vec4 &up = player->axes.up;
        const Vec4 &forward = player->axes.forward;
        const Vec4 &surfaceNormal = player->surfaceNormal;
        RailRecord &edge = railRecordOf(*player);

        // The edge's direction turned so that `square` leans into the surface.
        const auto turnEdgeIntoSurface = [&] {
            *square = zge::cross(*g_worldUp, edgeDirection);
            if (zge::dot(square, surfaceNormal) < ee::Float(0.0f))
                vec3Negate(edgeDirection, edgeDirection);
        };

        if (!player->object)
        {
            changeState(player, State::Air);
            return;
        }

        const uint32_t kind = surfacePhysics(player->objectNode, player->surface);
        if (kind == kSurfaceQuarterPipe)
        {
            const ee::Float tilt = zge::dot(up, *g_worldUp);
            const int32_t bounce = static_cast<short>(zge::angleAcos(tuningValue(kTuningPhysics, kBounceAngleQPipe)));
            const ee::Float steepest = zge::angleCos(static_cast<zge::Angle>(kRightAngle - bounce));
            if (!(tilt < steepest))
            {
                changeState(player, State::Air);
                return;
            }
            pipeEdgeOf(player->objectNode, player->surface, &edge);
            turnEdgeIntoSurface();
            *square = zge::cross(*g_worldUp, edgeDirection);
            if ((player->controls.actions & kActionBowlJumpOut) && railEdgeKind(player->grindRail, player->grindSegment) != kEdgeNoJumpOut)
            {
                // The bowl jump-out: into the air, pushed back off the lip.
                changeState(player, State::Air);
                vec3Negate(push, square);
                *push = *push * (player->speed * tuningValue(kTuningPhysics, kBowlJumpout));
                velocity = velocity + *push;
                return;
            }
            changeState(player, State::AirQuarterPipe);
            return;
        }
        if (kind == kSurfaceSpine && zge::dot(up, *g_worldUp) < *g_spineTilt)
        {
            pipeEdgeOf(player->objectNode, player->surface, &edge);
            if (ee::Float(0.0f) < zge::dot(*g_worldUp, forward))
            {
                turnEdgeIntoSurface();
                changeState(player, State::AirSpine);
                return;
            }
        }
        changeState(player, State::Air);
    }
    READABLE_FUNCTION_WAITS(leaveGround, Verified);
}
