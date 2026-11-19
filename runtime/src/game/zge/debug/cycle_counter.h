#pragma once

// The platform's cycle timers (the PS2 layer under zprofile.c;
// zge/debug/cycle_counter.cpp): the EE's cycle count widened to 64 bits,
// counts started and read back, cycles as frames of time or milliseconds,
// and the CPU's performance counters (no caller in the game).

#include "ee/ee.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/debug/cycle_counter.h.h"

namespace zge
{
    // The timers' state (g_profilerTimersReady, 0x20 bytes).
    struct CycleTimers
    {
        uint32_t ready;             // +0x00
        uint32_t _04;
        // +0x08 the cycle count: the low word the last COUNT read, the high
        // word its wraps (readCycleCounter, not lifted: MFC0).
        uint64_t cycles;
        uint32_t lastCount;         // +0x10 COUNT at the last read
        int32_t performanceCounters[2]; // +0x14 PCR0 and PCR1 as last sampled
        // +0x1C the running profile bars' masks, with bit 0 set [unknown:
        // what reads them; only timerMasks, which has no caller].
        uint32_t masks;
    };
    static_assert(offsetof(CycleTimers, cycles) == 0x08 && offsetof(CycleTimers, masks) == 0x1C && sizeof(CycleTimers) == 0x20);

    // The bytes initCycleTimers clears: ZGE2's state ends at +0x18 (DM2's
    // initCycleTimers), before the bars' masks ZGE3 added (setTimerMask).
    constexpr uint32_t kCycleTimersBytes = game::readable::perGame(static_cast<uint32_t>(sizeof(CycleTimers)), 0x18u);

    constexpr uint32_t kCycleTimersAddress = sym::g_profilerTimersReady;
    inline ee::Global<CycleTimers, kCycleTimersAddress> g_cycleTimers;

    uint32_t initCycleTimers();
    uint32_t shutDownCycleTimers();
    uint64_t startCycleCount(uint32_t count);
    uint64_t cyclesSince(uint32_t count);
    ee::Float cyclesToTime(int32_t cycles, ee::Float frameRate);
    uint32_t setTimerMask(uint32_t mask);
    uint32_t clearTimerMask(uint32_t mask);
}

namespace zge
{
    // (cycle_counter.cpp)
    void initCpuCycles();
    void shutDownCpuCycles();
    uint64_t timeMilliseconds();
}
