// The ground under the skater (were the sweep's groundBelow and
// probeGround): a segment down from the position through the level's
// collision, the height above what it hits, the surface and object it is
// over (the "OverStart" / "OverEnd" events and the goals told), and the
// skater stood on it.
//

#include "skater.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/scene/query.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "moves.h"
#include "aggressive/level/objectives.h"

#include "sym/aggressive/skater/ground_below.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kOverEnd = sym::overEnd;   // "OverEnd"
        constexpr uint32_t kOverStart = sym::overStart; // "OverStart"
        // The skater's up axis on flat ground (0, -1, 0): the game's y points down.
        inline ee::Global<zge::Vec4, sym::g_worldUp> g_worldUp;
        inline constexpr ee::Float kGroundSearchDepth = -50.0f; // beyond the last height above it
        inline constexpr ee::Float kProbeBelow = 20.0f;
        // How far above the position the probe starts, and the
        // height the skater is put above the point hit.
        inline ee::Global<ee::Float, sym::g_probeAbove> g_probeAbove;
        inline ee::Global<ee::Float, sym::g_standHeight> g_standHeight;

        // The segment the query walks.
        struct Probe
        {
            zge::Vec4 start, end;
        };

        // The query's filter context, which no filter here reads: the upper
        // half of the vector last copied, as the engine's macro leaves it.
        ee::Qword upperHalf(const zge::Vec4 &v)
        {
            return ee::Qword{0, (uint64_t{v.w.bits()} << 32) | v.z.bits()};
        }

        // zge's collisionQuery by address: its filter context goes as the
        // whole quadword the original's register holds (the upper half of
        // the vector last copied), which the plain call's word cannot carry.
        int32_t collisionQuery(uint32_t kind, ee::Address shape, uint32_t flags, uint32_t filter, ee::Qword filterContext, ee::Address nodes, ee::Address results, uint32_t cache) { return ee::game<int32_t>(sym::collisionQuery, kind, shape, flags, filter, filterContext, nodes, results, cache); }
    }

    // groundBelow: the ground below: Player::heightAboveGround, and when
    // the surface or object under the skater changes, "OverEnd" to the old
    // one (with its surface kind), "OverStart" to the new, and the goals
    // told of the named object it belongs to.
    void groundBelow(ee::Ptr<Player> player)
    {
        ee::Local<Probe> segment; // start (the position), end
        ee::Local<zge::QueryResults> found;
        ee::Local<zge::Vec4> step, point;
        // The nodes the query skips (kQueryExcludeListed, null-terminated):
        // the skater's own scene object.
        ee::Local<uint32_t[2]> skipped;
        ee::Local<uint32_t> surfaceWord; // the events' data

        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        skipped[0] = ee::Address(zge::sceneObject(scene)); // the skip list's word
        skipped[1] = 0;
        // Down from the position, past the last height above the ground.
        segment->start = player->position;
        const ee::Float depth = kGroundSearchDepth - player->heightAboveGround;
        *step = *g_worldUp * depth;
        segment->end = segment->start + *step;
        if (!collisionQuery(kQuerySegment, segment, uint32_t{zge::kQueryWorldPoints} | zge::kQueryExcludeListed, 0, upperHalf(player->position), skipped, found, 0))
            return;

        *point = found->nodes[0]->faces->point;
        *step = player->position - *point;
        const ee::Float height = zge::vec3Length(&step->xyz());
        const ee::Ptr<zge::HitNode> under = found->nodes[0];
        player->heightAboveGround = height;
        if (player->overNode == under->node && player->overSurfaceKind == surfaceKind(player->overNode, under->faces->face))
            return; // over the same as before

        if (player->overObject)
        {
            *surfaceWord = player->overSurfaceKind;
            sendObjectEvent(player, player->overNode, kOverEnd, surfaceWord);
        }
        player->overNode = under->node;
        const uint32_t kind = surfaceKind(player->overNode, under->faces->face);
        *surfaceWord = kind;
        player->overSurfaceKind = kind;
        sendObjectEvent(player, player->overNode, kOverStart, surfaceWord);
        const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(ee::Ptr<zge::Node>(player->overObject), 0);
        if (!owner)
            return;
        const uint32_t name = zge::objectNameHash(owner);
        level::tellGoalsOver(player, name, *surfaceWord); // the level's goals hear which named object the skater is over
    }
    READABLE_FUNCTION_WAITS(groundBelow, Verified);

    // probeGround: a short probe from just above the position to 20 below
    // it; on a hit the skater is put g_standHeight above the point and
    // touches the surface.
    void probeGround(ee::Ptr<Player> player)
    {
        ee::Local<zge::Vec4> start;
        ee::Local<zge::Vec4> end;
        ee::Local<zge::QueryResults> found;
        ee::Local<zge::Vec4> point;
        *end = player->position;
        end->y = end->y + kProbeBelow;
        *start = player->position;
        start->y = start->y - *g_probeAbove;
        if (!collisionQuery(kQuerySegment, start, zge::kQueryWorldPoints, 0, upperHalf(player->position), 0, found, 0))
            return;
        *point = found->nodes[0]->faces->point;
        const ee::Ptr<zge::HitNode> under = found->nodes[0];
        player->position.y = point->y - *g_standHeight;
        skaterTouchSurface(player, under->node, under->faces->face);
    }
    READABLE_FUNCTION_WAITS(probeGround, Verified);
}
