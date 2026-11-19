#pragma once

// The PC's own work beside the game's: the native meshes and collision mirrored
// from the files, the Xbox textures, the draw, packet and collision traces. It
// reads more than the original does (game memory the original never touches,
// the host's files), so it runs in the game only: not while ps2xFnCheck replays
// one function from a recording, which holds just the memory the original read.

#include "ee/abi.h"

#include <cstdint>

namespace host_clock
{
    uint64_t vsyncTick();
    uint64_t eeCycle();
}

namespace zge::native
{
    // Whether the PC's extras run (the game, not a replay of one function).
    inline bool hostExtrasOn() { return !ee::t_ee.callHandler; }

    // Game memory as the running function sees it.
    inline const uint8_t *gameRam() { return ee::t_ee.frame ? ee::t_ee.frame->rdram : ee::g_ram; }

    // The emulated clock for traces: the vsync count and the EE cycle (0 outside
    // the running game). One clock for every game's code: it is defined outside
    // zge, which a game's instance renames (zge_dm2), so an instance's traces
    // (draw.cpp, query_trace.cpp) call this one.
    using host_clock::vsyncTick;
    using host_clock::eeCycle;
}
