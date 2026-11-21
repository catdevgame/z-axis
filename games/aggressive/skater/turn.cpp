// The skater's turn for a frame (groundTurn, the state table's first
// callback for Ground, Stand, Air QPipe, Manual and Cess Slide): the
// steering's turn rate about the up axis and the pitch rate about the
// right axis; on the ground (Ground, Manual, Cess Slide) without steering,
// a slope turns the skater to face down it (Physics: Turndown Min Angle,
// Turndown Min Speed, Turndown Speed).

#include "moves.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "sym/aggressive/skater/turn.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Below this the steering's turn rate and a flattened axis's length
        // count as none (1e-5).
        inline ee::Global<ee::Float, sym::g_turnEpsilon> g_turnEpsilon;
        // Up against world up below this: on a slope (0.85).
        inline ee::Global<ee::Float, sym::g_slopeCosine> g_slopeCosine;
        inline ee::Global<Vec4, sym::g_worldUp> g_worldUp; // (0, -1, 0, 0): y grows downward

        enum PhysicsEntry : uint32_t
        {
            kTurndownMinAngle = 41, // facing across the slope more than this: turned down
            kTurndownMinSpeed = 42, // or facing up it and slower than this
            kTurndownSpeed = 43,    // degrees a second
        };
        ee::Float physics(uint32_t entry)
        {
            return (*g_tuningCategories)[3].entries[entry].value;
        }

        // Whether the skater should turn to face down the slope it is on.
        bool turnsDownhill(ee::Ptr<Player> player, Vec4 &flatNormal, Vec4 &flatForward)
        {
            const PlayerAxes &axes = player->axes;
            if (!(zge::dot(axes.up, *g_worldUp) < *g_slopeCosine))
                return false;

            // Forward and the surface's normal flattened onto the ground plane.
            flatForward = axes.forward;
            flatForward.y = 0.0f;
            flatNormal = player->surfaceNormal;
            flatNormal.y = 0.0f;
            const ee::Float epsilon = *g_turnEpsilon;
            if (ee::abs(zge::dot(flatForward, flatForward)) < epsilon || ee::abs(zge::dot(flatNormal, flatNormal)) < epsilon)
                return false;
            flatForward = zge::unit(flatForward);
            flatNormal = zge::unit(flatNormal);

            // Across the slope, or up it too slowly.
            if (ee::abs(zge::dot(flatForward, flatNormal)) < physics(kTurndownMinAngle))
                return true;
            return ee::Float(0.0f) < zge::dot(axes.forward, *g_worldUp) && player->speed < physics(kTurndownMinSpeed);
        }
    }

    void groundTurn(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> turnAxis;
        ee::Local<Vec4> flatNormal;
        ee::Local<Vec4> flatForward;
        PlayerAxes &axes = player->axes;
        zge::Matrix &rows = player->orientation;

        if (player->state == State::Ground)
        {
            player->spinDegreesClockwise = 0.0f;
            player->spinDegreesCounterClockwise = 0.0f;
        }

        // The steering (about -up) and the pitch (about right).
        if (player->turnRate != 0.0f)
        {
            vec3Negate(turnAxis, axes.up);
            rotateOrientation(&rows, &turnAxis->xyz(), player->turnRate * frameTime);
        }
        if (player->pitchRate != 0.0f)
            rotateOrientation(&rows, &axes.right.xyz(), -(player->pitchRate * frameTime));

        const State state = player->state;
        const bool onGround = state == State::Ground || state == State::Manual || state == State::CessSlide;
        if (onGround && ee::abs(player->turnRate) < *g_turnEpsilon && turnsDownhill(player, flatNormal, flatForward))
        {
            // About up: the side the right axis tilts to decides the way round.
            const ee::Float rightUphill = zge::dot(axes.right, *g_worldUp);
            ee::Float turn = physics(kTurndownSpeed) * frameTime;
            if (rightUphill < ee::Float(0.0f))
                turn = -turn;
            rotateOrientation(&rows, &axes.up.xyz(), turn);
        }

        // No forward axis at all: start again from upright, facing +x.
        Vec4 &forward = axes.forward;
        if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f)
        {
            axes.up = *g_worldUp;
            forward.x = 1.0f;
            forward.y = 0.0f;
            forward.z = 0.0f;
            orthonormalize(&axes.right, &axes.up, &forward);
        }
    }
    READABLE_FUNCTION(groundTurn, Verified);
}
