// The platform's cycle timers (zge/debug/cycle_counter.h): what the
// profiler's bars count with. The three functions that read COP0 (COUNT,
// readCycleCounter, and PCR0 / PCR1, performanceCounter0 / performanceCounter1) stay the
// recompiled code's (not lifted: MFC0 / MFPC).

#include "cycle_counter.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/debug/cycle_counter.cpp.h"

namespace zge
{

    namespace
    {
        // %hi(g_profilerTimersReady), kept in a saved register.
        constexpr uint32_t kCycleTimersHigh = ee::high(sym::g_profilerTimersReady);
        // The EE's clock: 300 MHz (cycles a second), so 300,000 a millisecond.
        constexpr uint32_t kCpuClockRate = 300000000u;
        constexpr uint64_t kCyclesPerMillisecond = 300000u;

        constexpr uint32_t kReadCount = sym::readCycleCounter;         // -> the 64-bit cycle count (COUNT and its wraps)
        constexpr uint32_t kReadPerformanceCounter0 = sym::performanceCounter0;
        constexpr uint32_t kReadPerformanceCounter1 = sym::performanceCounter1;
        constexpr uint32_t kScePcStart = sym::scePcStart;        // libpc's scePcStart
        constexpr uint32_t kScePcStop = sym::stopPerformanceCounters;         // libpc's scePcStop

        CycleTimers &timers() { return *g_cycleTimers; }
    }

    uint32_t cpuClockRate();
    uint64_t cpuCycles();

    // readCycleCounter: the 64-bit cycle count: COUNT in the low word, each wrap
    // seen (COUNT below the last read) added to the high one.
    uint64_t readCycleCounter()
    {
        const uint32_t count = ee::io::cop0Count();
        CycleTimers &t = timers();
        uint64_t cycles = t.cycles;
        if (count < t.lastCount)
            cycles += uint64_t{1} << 32;
        t.cycles = (cycles & ~uint64_t{0xFFFFFFFFu}) | count;
        t.lastCount = count;
        return t.cycles;
    }
    READABLE_FUNCTION(readCycleCounter, Verified);

    // performanceCounter0, performanceCounter1: MFPC 0 and 1; the recompiled code keeps one
    // word for both. samplePerformanceCounters' only; nothing calls it.
    int32_t performanceCounter0()
    {
        return static_cast<int32_t>(ee::io::cop0Perf());
    }
    READABLE_FUNCTION(performanceCounter0, Verified);

    int32_t performanceCounter1()
    {
        return static_cast<int32_t>(ee::io::cop0Perf());
    }
    READABLE_FUNCTION(performanceCounter1, Verified);

    // initCycleTimers: the timers cleared and ready. 1.
    uint32_t initCycleTimers()
    {
        memoryFill(kCycleTimersAddress, 0, kCycleTimersBytes);
        timers().ready = 1;
        return 1;
    }
    READABLE_FUNCTION(initCycleTimers, Verified);

    // shutDownCycleTimers: the timers cleared (ZGE2: only marked not ready). 1.
    uint32_t shutDownCycleTimers()
    {
        if (timers().ready != 0)
        {
            if constexpr (!game::readable::kIsDm2)
                memoryFill(kCycleTimersAddress, 0, kCycleTimersBytes);
            timers().ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION(shutDownCycleTimers, Verified);

    // cycleTimersNoop (no caller): nothing.
    void cycleTimersNoop()
    {
    }
    READABLE_FUNCTION(cycleTimersNoop, Verified);

    // startCycleCount: a count started: the cycle count now, kept at count
    // (and returned).
    uint64_t startCycleCount(uint32_t count)
    {
        const uint64_t now = cpuCycles();
        *ee::Ptr<uint64_t>(count) = now;
        return now;
    }
    READABLE_FUNCTION(startCycleCount, Verified);

    // cyclesSince: the cycles since a count was started (the difference's
    // low word, sign-extended).
    uint64_t cyclesSince(uint32_t count)
    {
        const uint64_t now = cpuCycles();
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(now - *ee::Ptr<uint64_t>(count)))));
    }
    READABLE_FUNCTION(cyclesSince, Verified);

    // cyclesToTime: cycles as frames of time at a frame rate. ZGE2 takes the
    // clock rate as an unsigned 64-bit count (libgcc's __floatdisf; with the
    // top bit set, as GCC converts an unsigned value: half of it, its low
    // bit kept, converted and doubled); ZGE3 as an int.
    ee::Float cyclesToTime(int32_t cycles, ee::Float frameRate)
    {
        ee::Float clockRate;
        if constexpr (game::readable::kIsDm2)
        {
            const uint64_t clock = cpuClockRate();
            if (static_cast<int64_t>(clock) >= 0)
                clockRate = sce::libgcc::__floatdisf(static_cast<int64_t>(clock));
            else
            {
                clockRate = sce::libgcc::__floatdisf(static_cast<int64_t>((clock >> 1) | (clock & 1u)));
                clockRate = clockRate + clockRate;
            }
        }
        else
            clockRate = ee::toFloat(static_cast<int32_t>(cpuClockRate()));
        const ee::Float cyclesPerFrame = clockRate / frameRate;
        return ee::toFloat(cycles) / cyclesPerFrame;
    }
    READABLE_FUNCTION(cyclesToTime, Verified);

    // resetCpuCycles: the cycle count's widening reset.
    void resetCpuCycles()
    {
        timers().cycles = 0;
    }
    READABLE_FUNCTION(resetCpuCycles, Verified);

    // endCpuCycles: nothing (the PS2's end of the cycle count).
    void endCpuCycles()
    {
    }
    READABLE_FUNCTION(endCpuCycles, Verified);

    // initCpuCycles, shutDownCpuCycles: the cycle count's set-up and shut-down
    // (tail jumps to the two above).
    void initCpuCycles()
    {
        game::tail(resetCpuCycles);
    }
    READABLE_FUNCTION(initCpuCycles, Verified);

    void shutDownCpuCycles()
    {
        game::tail(endCpuCycles);
    }
    READABLE_FUNCTION(shutDownCpuCycles, Verified);

    // cpuClockRate: the EE's clock rate (cycles a second).
    uint32_t cpuClockRate()
    {
        return kCpuClockRate;
    }
    READABLE_FUNCTION(cpuClockRate, Verified);

    // cpuCycles: the 64-bit cycle count now.
    uint64_t cpuCycles()
    {
        return readCycleCounter();
    }
    READABLE_FUNCTION(cpuCycles, Verified);

    // timeMilliseconds: the cycle count in milliseconds (the quotient's low
    // word, sign-extended).
    uint64_t timeMilliseconds()
    {
        const uint64_t now = cpuCycles();
        const uint64_t milliseconds = sce::libgcc::__udivdi3(now, kCyclesPerMillisecond);
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(milliseconds))));
    }
    READABLE_FUNCTION(timeMilliseconds, Verified);

    // ---- the performance counters (no caller) ---------------------------------------

    // resetPerformanceCounters: the sampled counters cleared; the state's address.
    uint32_t resetPerformanceCounters()
    {
        timers().performanceCounters[1] = 0;
        timers().performanceCounters[0] = 0;
        return kCycleTimersAddress;
    }
    READABLE_FUNCTION(resetPerformanceCounters, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t scePcStart(uint32_t control, uint32_t counter0, uint32_t counter1) { return ee::tailCall<int32_t>(kScePcStart, control, counter0, counter1); }
        }
    }

    // startPerformanceCounters: libpc's counters started: scePcStart(control, counter 0, counter 1).
    int32_t startPerformanceCounters()
    {
        return tail::scePcStart(0x800378DEu, timers().performanceCounters[0], timers().performanceCounters[1]);
    }
    READABLE_FUNCTION(startPerformanceCounters, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void stopPerformanceCounters() { ee::game(kScePcStop); }
    }

    // samplePerformanceCounters: libpc's counters stopped and both sampled; PCR1.
    int32_t samplePerformanceCounters()
    {
        stopPerformanceCounters();
        timers().performanceCounters[0] = performanceCounter0();
        const int32_t second = performanceCounter1();
        timers().performanceCounters[1] = second;
        return second;
    }
    READABLE_FUNCTION(samplePerformanceCounters, Verified);

    // performanceCounters: the sampled counters out (either pointer may be 0);
    // the upper half of the state's address.
    uint32_t performanceCounters(uint32_t first, uint32_t second)
    {
        if (first != 0)
            *ee::Ptr<int32_t>(first) = timers().performanceCounters[0];
        if (second != 0)
            *ee::Ptr<int32_t>(second) = timers().performanceCounters[1];
        return kCycleTimersHigh;
    }
    READABLE_FUNCTION(performanceCounters, Verified);

    // ---- the running bars' masks ------------------------------------------------------

    // setTimerMask, clearTimerMask: a profile bar's mask set (with bit 0) or
    // cleared (bit 0 kept set). The masks, or the state's address.
    uint32_t setTimerMask(uint32_t mask)
    {
        const uint32_t masks = timers().masks | 1u | mask;
        timers().masks = masks;
        return masks;
    }
    READABLE_FUNCTION(setTimerMask, Verified);

    uint32_t clearTimerMask(uint32_t mask)
    {
        timers().masks = (timers().masks & ~mask) | 1u;
        return kCycleTimersAddress;
    }
    READABLE_FUNCTION(clearTimerMask, Verified);

    // timerMasks (no caller): the masks.
    uint32_t timerMasks()
    {
        return timers().masks;
    }
    READABLE_FUNCTION(timerMasks, Verified);
}
