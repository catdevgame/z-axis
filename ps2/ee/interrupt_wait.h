#pragma once

// A wait on something an interrupt handler or another thread sets: the game
// spins on a flag (a DMA transfer's end, a frame packet sent) until it
// changes. The spin's branch back is the original's safe point (ee::loopBack
// on the loop's head), so the interrupt handler that ends the wait gets to
// run there, as it does in the original:
//
//     ee::waitForInterrupt(here::loop, [&] { return display.framePacketSent != 0; });

#include "abi.h"

#include <cstdint>

namespace ee
{
    // Until done() holds, checked first: each check that fails is one pass of
    // the original's loop, back to its head `loopHead` (a points:: name).
    template <typename Done>
    void waitForInterrupt(uint32_t loopHead, Done done)
    {
        while (!done())
            loopBack(loopHead);
    }
}
