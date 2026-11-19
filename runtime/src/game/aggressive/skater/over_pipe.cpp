// The skater held in the plane above a quarter pipe's or spine's lip (was
// the sweep's holdOverPipe; Air QPipe's and Air Spine's move call it): the
// plane through the lip edge, upright and square to the edge, the skater
// put back into it (Player::stateValue off it, plus a quarter), in Air
// Spine only a twentieth of the way from where it was, and in Air QPipe
// its velocity turned into the plane at the same speed.
//

#include "moves.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "moves.h"
#include "sym/aggressive/skater/over_pipe.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // The skater's up axis on flat ground (0, -1, 0): the game's y points down.
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        // The part of the way back to the plane Air Spine moves the skater
        // in a frame (0.05).
        inline ee::Global<ee::Float, sym::g_spineReturn> g_spineReturn;
        inline constexpr ee::Float kPlaneMargin = 0.25f; // beyond the state's offset
    }

    // holdOverPipe: the skater put into the plane over the lip; `normal`
    // (out) is the plane's normal, on the skater's up side.
    void holdOverPipe(ee::Ptr<Player> player, ee::Ptr<zge::Vec4> normal)
    {
        ee::Local<zge::Vec4> plane, offset, before, direction, moved;
        zge::Vec4 &position = player->position;

        // Square to the lip edge and upright, on the skater's up side.
        *normal = zge::cross(*g_worldUp, player->railDirection);
        if (zge::dot(player->axes.up, *normal) < ee::Float(0.0f))
            zge::vec3Negate(&normal->xyz(), &normal->xyz());

        // Into the plane through the edge's start, then out by the offset.
        *before = position;
        zge::planeThrough(plane, &player->railStart, normal);
        const ee::Float distance = zge::planeDistance(plane, &position);
        *offset = *normal * distance;
        position = position - *offset;
        *offset = *normal * (player->stateValue + kPlaneMargin);
        position = position + *offset;

        if (player->state == State::AirSpine)
        {
            // Only part of the way there this frame.
            *moved = position - *before;
            position = *before;
            *offset = *moved * *g_spineReturn;
            position = position + *offset;
        }
        if (player->state == State::AirQuarterPipe)
        {
            // The velocity turned into the plane, its speed kept.
            alongPlane(direction, normal, &player->velocity);
            *direction = zge::unit(*direction);
            player->velocity = zge::scaledAfterMeasure(*direction, player->speed);
        }
    }
    READABLE_FUNCTION_VU0(holdOverPipe, Verified);
}
