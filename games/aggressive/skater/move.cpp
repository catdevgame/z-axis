// The skater's move for a frame: a swept collision test of the step, split
// into its forward and its sideways part.

#include "moves.h"
#include "aggressive/tuning.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/query.h"
#include "sym/aggressive/skater/move.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // Steps shorter than these on every axis are not tested (1e-4).
        inline ee::Global<ee::Float, sym::g_minForwardStep> g_minForwardStep;
        inline ee::Global<ee::Float, sym::g_minSideStep> g_minSideStep;
        constexpr uint32_t kNoContact = 0xFFFFFFFFu;

        constexpr uint32_t kStepHeight = 8;   // Physics / Step Height

        // Whether a segment is shorter than `limit` on every axis (FPU).
        bool tooShort(const zge::Vec4 &start, const zge::Vec4 &end, ee::Float limit)
        {
            return ee::abs(end.x - start.x) < limit && ee::abs(end.y - start.y) < limit &&
                   ee::abs(end.z - start.z) < limit;
        }
    }

    int32_t moveSkater(ee::Ptr<Player> player, ee::Ptr<Vec4> from, ee::Ptr<Vec4> to, ee::Float frameTime)
    {
        ee::Local<zge::QuerySegment> segment;
        ee::Local<zge::QueryResults> hit;
        ee::Local<uint32_t[2]> skipped; // the nodes the query skips: the skater's own, then 0
        ee::Local<Vec4> mid;
        ee::Local<Vec4> step;
        ee::Local<Vec4> alongForward;
        ee::Local<Vec4> sideways;

        skipped[0] = ee::Address(zge::sceneObject(zge::sceneOf(player->node))); // the skip list's word
        skipped[1] = 0;

        // The step, its part along the forward axis, and the rest.
        const Vec4 &forward = player->axes.forward;
        *step = *to - *from;
        const ee::Float along = zge::dot(step, forward);
        *alongForward = forward * along;
        *sideways = *step - *alongForward;
        segment->start = *from;
        segment->end = *from + *alongForward;
        *mid = segment->end;

        player->groundNormal = Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        player->contact = kNoContact;
        player->contactObject = 0;

        // Whether the segment, long enough to test, hits anything.
        const auto hits = [&](ee::Float minStep) {
            const uint32_t flags = uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed;
            return !tooShort(segment->start, segment->end, minStep) &&
                   zge::collisionQuery(kQuerySegment, segment, flags, 0, 0, skipped, hit, 0) != 0;
        };

        int32_t result = 0;
        if (hits(*g_minForwardStep))
            result = skaterCollisionResponse(player, hit, frameTime);
        else
        {
            segment->start = mid;
            segment->end = *mid + *sideways;
            if (hits(*g_minSideStep))
                result = skaterCollisionResponse(player, hit, frameTime);
            else
            {
                player->position = segment->end;
                player->airTime = player->airTime + frameTime; // the time moved without a hit
            }
        }

        // On the ground, in a manual or cess slide, or in the air: a contact
        // found on the way is kept.
        const State state = player->state;
        const bool keeps = state == State::Ground || state == State::Manual || state == State::CessSlide || state == State::Air ||
                           state == State::AirQuarterPipe || state == State::AirSpine;
        if (keeps && player->contact != kNoContact)
            keepContact(player);
        return result;
    }
    READABLE_FUNCTION(moveSkater, Verified);

    // moveByVelocity (player, frame time): the frame's sweep by the velocity:
    // from Physics / Step Height along the skater's up above its position,
    // to the position moved by its velocity over the frame (and by the
    // moving surface that carries it, Player::carriedMove), then moveSkater.
    // On VU0, as the original (from's w is VU0's leftover sum).
    int32_t moveByVelocity(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Vec4> move;
        ee::Local<Vec4> from;
        ee::Local<Vec4> to;
        ee::Local<Vec4> lift;
        const Vec4 &position = player->position;
        const ee::Float stepHeight = (*g_tuningCategories)[3].entries[kStepHeight].value;
        *lift = player->axes.up * stepHeight;
        *from = *lift + position;
        *move = player->velocity * frameTime;
        *to = position + *move;
        if (player->onMovingSurface == 1)
            *to = *to + player->carriedMove;
        return moveSkater(player, from, to, frameTime);
    }
    READABLE_FUNCTION_WAITS(moveByVelocity, Verified);
}
