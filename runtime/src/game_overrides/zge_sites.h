#pragma once

// The places a game override's hook acts at, counted (ZX_HOOK_SITES=1
// logs "[hook-site] NAME N" at 1, 10, 100 ... and every 1,000), so a check can
// say by a number that a hook still finds its place in the game: a caller
// rewritten as readable code, or another release, shows as 0. A hook knows
// its place by the caller it runs inside (the caller wrapped) and by its
// arguments, never by a return address.

namespace zge_host::sites
{
    void seen(const char *site);
}
