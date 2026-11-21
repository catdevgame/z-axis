#pragma once

// The collision query's trace and timer (host side, a developer's tool; not
// game logic). ZX_COLLISION_TRACE=<file>: a line per query (outside
// zx-fncheck): its number, kind, flags, the result and every HitNode {node,
// part, faces: index, point and distance as bits}, and the EE cycles the
// query took. Two runs of the same input (ZX_NATIVE_COLLISION off and on)
// give the same file when every query gave the same results in the same
// emulated time. ZX_COLLISION_TIME=1: the host time per query, logged
// every 100000 queries ([collision-time]).

#include <cstdint>
#include <functional>

namespace zge
{
    // Whether either is on.
    bool collisionQueryTraced();
    // The query run, traced and timed; its result.
    bool traceCollisionQuery(uint32_t kind, uint32_t flags, const std::function<bool()> &query);
}
