#pragma once

// A delay loop of the game's C: `for (i = 0; i < rounds; i++) ;` with a
// volatile counter on the stack. It does nothing but let EE time pass, and
// that time is the game's: the other threads, the vsync and the timers see
// it. (zge/movie/ps2/mpeg_sample.cpp's outputMovieFrame waits so before each
// picture goes to the texture.)

#include "ee/abi.h"
#include "ee/plain.h"

#include <cstdint>

namespace ee
{
    // `rounds` turns of the empty loop: its branch back is taken one fewer
    // times than it turns (the first test is ahead of the loop), each charged
    // and checked as the original's; the counter's stack word is left at
    // `rounds`, as the original leaves it.
    inline void delayLoop(Local<int32_t> &counter, int32_t rounds)
    {
        if (rounds > 1)
            chargeLoop(static_cast<uint64_t>(rounds - 1));
        *counter = rounds > 0 ? rounds : 0;
    }
}
