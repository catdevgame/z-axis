// The slide off an edge: a skater landing or standing tilted off the
// world's up (its right axis not straight up or down) looks for ground on
// both sides, a little to its right and to its left; with none within
// Step Height (and 0.1) on either side it is moved off the edge, 1.25
// units sideways. The Ground and Stand enters run it first.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "moves.h"

#include "sym/aggressive/skater/slide_off.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSegmentQuery = 1;                // zge::CollisionKind::Segment
        constexpr uint32_t kSkipListedWorldPoints = 0xC;     // kQueryExcludeListed | kQueryWorldPoints

        inline ee::Global<ee::Float, sym::g_lookRight> g_lookRight;   // 0.35
        inline ee::Global<ee::Float, sym::g_lookLeft> g_lookLeft;    // -0.35
        inline ee::Global<ee::Float, sym::g_shortProbe> g_shortProbe;  // 1e-4: a probe this short is none
        inline ee::Global<ee::Float, sym::g_groundSlack> g_groundSlack; // 0.1: added to Step Height
        // (0, -1, 0, 0): the world's up (y points down).
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;

        // The segment the query walks.
        struct Probe
        {
            zge::Vec4 start, end;
        };

        constexpr ee::Float kProbeDepth = -5.0f;  // down the world's up
        constexpr ee::Float kNoGround = 99.0f;    // a side's distance with nothing under it
        constexpr ee::Float kSlideStep = 1.25f;   // the move off the edge

        ee::Float stepHeight() { return (*g_tuningCategories)[3].entries[8].value; } // Physics / Step Height
    }

    // slideOffEdge: (player, frame time) -> 1 when the skater is moved off
    // an edge (moveSkater to 1.25 along its level right, toward the side
    // whose ground is nearer), 0 when its right axis runs along the world's
    // up or ground lies within Step Height + 0.1 under a point 0.35 to its
    // right or left (probes 5 units down, the skater's own collision
    // skipped).
    int32_t slideOffEdge(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<Probe> probe;
        ee::Local<zge::QueryResults> results;
        ee::Local<uint32_t[2]> skip;     // the nodes to skip: the skater's scene object, 0
        ee::Local<zge::Vec4> side, work; // side: right, level
        ee::Local<ee::Float[2]> groundDistance; // per side, kNoGround for none
        const zge::Vec4 &right = player->axes.right;

        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        skip[0] = ee::Address(zge::sceneObject(scene)); // the skip list's word
        skip[1] = 0;

        if (ee::Float(1.0f) <= ee::abs(zge::dot(right, *g_worldUp)))
            return 0;

        // Right, level and of length 1.
        alongPlane(side, g_worldUp.get(), &right);
        *side = zge::unit(*side);

        for (uint32_t i = 0; i < 2; ++i)
        {
            *work = *side * (i == 0 ? *g_lookRight : *g_lookLeft);
            probe->start = player->position + *work;
            *work = *g_worldUp * kProbeDepth;
            probe->end = probe->start + *work;

            // The probe too short, or no ground: none on this side.
            const ee::Float epsilon = *g_shortProbe;
            bool tooShort = ee::abs(probe->end.x - probe->start.x) < epsilon;
            if (tooShort)
            {
                tooShort = ee::abs(probe->end.y - probe->start.y) < epsilon;
                if (tooShort)
                    tooShort = ee::abs(probe->end.z - probe->start.z) < epsilon;
            }
            if (tooShort || !zge::collisionQuery(kSegmentQuery, probe, kSkipListedWorldPoints, 0, 0, skip, results, 0))
            {
                groundDistance[i] = kNoGround;
                continue;
            }
            // The first face hit's point.
            *work = results->nodes[0]->faces->point;
            *work = *work - probe->start;
            const ee::Float distance = zge::vec3Length(&work->xyz());
            groundDistance[i] = distance;
            if (distance < stepHeight() + *g_groundSlack)
                return 0;
        }

        const ee::Float step = groundDistance[0] < groundDistance[1] ? kSlideStep : -kSlideStep;
        *work = *side * step;
        *work = player->position + *work;
        moveSkater(player, &player->position, work, frameTime);
        return 1;
    }
    READABLE_FUNCTION(slideOffEdge, Verified);
}
