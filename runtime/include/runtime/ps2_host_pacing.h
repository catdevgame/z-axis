#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// The guest's vblank clock held in phase with the window's frames (paced runs).
//
// The window thread takes the picture once per display refresh (a vsynced
// present: 60 or 120 Hz) and the guest's vblank is paced on the host clock
// at the PS2's 16.667 ms. With nothing tying the two, a guest vblank (and
// the buffer swap a game makes right after it, well under a millisecond
// later) can sit just before the window's latch for minutes at a time: the
// swap then lands on either side of the latch from frame to frame, and the
// game's 60 frames a second are shown 25 ms, 8 ms, 25 ms ... apart on a 120 Hz
// display (or a frame twice on a 60 Hz one). With the frame queue (GS) every
// game frame waits for the window, and a host frame that took two refreshes
// leaves one frame waiting at each latch after it, shown a refresh late. When
// one has waited at kSlipAfter latches in a row (noteTaken), the next vblank
// comes one guest period later (once, host time only): the window shows the
// waiting frame in that period, so the backlog goes with no frame dropped or
// shown twice. (On the Thor the window misses a refresh a few times a second
// when the GPU is busy; small steps could not keep up with that.) So the pacer moves each vblank's
// host deadline a little (at most kMaxStep) toward kAfterLatch after a latch,
// as a phase-locked loop: the guest's vblanks keep the display's rate (a
// refresh within 1 % of one to four times theirs) and each swap is taken by
// the next latch, a whole refresh away from the race. kAfterLatch is
// measured: the vblank is handled about 1 ms after its deadline, and with
// the vblank half way through the refresh the window thread's present waited
// a second refresh more often in the front ends (24-55 times in 20 s; 5-13
// at kAfterLatch, 1-16 without the lock; b123, a 120 Hz M5 Pro).
//
// Only host time moves: guest time is EE cycles (EeScheduler), so what the
// game does is the same; the sound follows guest time and its host ring
// corrects the rate (ps2_spu2_audio.h). Unpaced runs, fast-forwarded
// loading screens, a window without vsync (no steady refresh) or one that
// stopped presenting are left alone. PS2X_VSYNC_PHASE=0: no correction.
namespace ps2x::host_pacing
{
    using Clock = std::chrono::steady_clock;

    inline std::atomic<int64_t> g_latchNs{0};  // the window's last picture latch, Clock ns
    inline std::atomic<int64_t> g_periodNs{0}; // its refresh period (a running mean), 0 not known
    inline std::atomic<uint32_t> g_steady{0};  // latches in a row at that period
    inline std::atomic<bool> g_slip{false};    // the next vblank one guest period later (noteTaken)
    inline uint32_t g_waitingRun = 0;           // window thread: latches in a row that left a frame waiting
    constexpr uint32_t kSlipAfter = 8;

    // Window thread, after each latch: the frames still waiting after it took
    // one (0 when it took none).
    inline void noteTaken(uint32_t waiting)
    {
        g_waitingRun = waiting != 0 ? g_waitingRun + 1u : 0u;
        if (g_waitingRun >= kSlipAfter)
        {
            g_waitingRun = 0;
            g_slip.store(true, std::memory_order_relaxed);
        }
    }

    constexpr auto kMaxStep = std::chrono::microseconds(200);
    constexpr auto kAfterLatch = std::chrono::microseconds(1400);
    constexpr uint32_t kSteadyLatches = 30;

    inline bool enabled()
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_VSYNC_PHASE");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    inline int64_t nanoseconds(Clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }

    // Window thread: the picture is being taken now (once per host frame).
    inline void noteLatch(Clock::time_point now)
    {
        const int64_t t = nanoseconds(now);
        const int64_t last = g_latchNs.exchange(t, std::memory_order_relaxed);
        if (last == 0)
            return;
        const int64_t dt = t - last;
        int64_t period = g_periodNs.load(std::memory_order_relaxed);
        if (period == 0)
        {
            if (dt > 2'000'000 && dt < 50'000'000)
                g_periodNs.store(dt, std::memory_order_relaxed);
            return;
        }
        // (a missed refresh or a stall is not a period, and starts the count again)
        if (std::llabs(dt - period) * 8 < period)
        {
            g_periodNs.store(period + (dt - period) / 16, std::memory_order_relaxed);
            const uint32_t steady = g_steady.load(std::memory_order_relaxed);
            if (steady < kSteadyLatches)
                g_steady.store(steady + 1u, std::memory_order_relaxed);
        }
        else
        {
            g_steady.store(0u, std::memory_order_relaxed);
            if (std::llabs(dt - period) * 2 > period && dt > 2'000'000 && dt < 50'000'000)
                g_periodNs.store(dt, std::memory_order_relaxed); // (the refresh rate changed)
        }
    }

    // Window thread: frames shown, for PS2X_PERF_LOG. Each host frame that
    // shows a new game frame calls shown(frame number, made), every other one
    // repeated(); the counts say how many refreshes each game frame stayed
    // on screen and how many game frames were never shown.
    struct ShownFrames
    {
        uint64_t lastFrame = 0;   // the game frame on screen (0: none yet)
        uint32_t refreshes = 0;   // refreshes it has had so far
        uint64_t byRefreshes[5]{}; // game frames shown 1, 2, 3, 4, 5+ refreshes
        uint64_t never = 0;       // game frames skipped between two shown ones

        void shown(uint64_t frame)
        {
            if (lastFrame != 0)
            {
                ++byRefreshes[std::min<uint32_t>(refreshes, 5u) - 1u];
                if (frame > lastFrame + 1u)
                    never += frame - lastFrame - 1u;
            }
            lastFrame = frame;
            refreshes = 1;
        }
        void repeated()
        {
            if (lastFrame != 0)
                ++refreshes;
        }
    };

    // EE thread: how much to move the host deadline `next` of the next guest
    // vblank (one guest period `period` after the last), toward kAfterLatch
    // after a latch. Zero when the window has no steady refresh in
    // a whole ratio to the guest's.
    inline Clock::duration phaseStep(Clock::time_point next, Clock::duration period);
    inline Clock::duration correction(Clock::time_point next, Clock::duration period)
    {
        // (a frame waited at kSlipAfter latches: one period later, lock or not)
        const Clock::duration slip = g_slip.exchange(false, std::memory_order_relaxed) ? period : Clock::duration::zero();
        return slip + phaseStep(next + slip, period);
    }
    inline Clock::duration phaseStep(Clock::time_point next, Clock::duration period)
    {
        if (!enabled() || g_steady.load(std::memory_order_relaxed) < kSteadyLatches)
            return Clock::duration::zero();
        const int64_t refresh = g_periodNs.load(std::memory_order_relaxed);
        const int64_t latch = g_latchNs.load(std::memory_order_relaxed);
        const int64_t guest = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
        if (refresh <= 0 || latch == 0 || nanoseconds(Clock::now()) - latch > 4 * refresh)
            return Clock::duration::zero();
        // (a refresh 1 to 4 times the guest's vblank rate, within 1 %)
        const double ratio = static_cast<double>(guest) / static_cast<double>(refresh);
        const double whole = std::round(ratio);
        if (whole < 1.0 || whole > 4.0 || std::fabs(ratio - whole) > 0.01 * whole)
            return Clock::duration::zero();
        const int64_t slot = refresh;
        int64_t phase = (nanoseconds(next) - latch) % slot;
        if (phase < 0)
            phase += slot;
        // (-slot/2, slot/2]: later than the target, or earlier
        int64_t error = phase - std::min<int64_t>(slot / 2, std::chrono::duration_cast<std::chrono::nanoseconds>(kAfterLatch).count());
        if (error > slot / 2)
            error -= slot;
        else if (error <= -slot / 2)
            error += slot;
        const int64_t limit = std::chrono::duration_cast<std::chrono::nanoseconds>(kMaxStep).count();
        const int64_t step = std::clamp<int64_t>(-error / 8, -limit, limit);
        return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(step));
    }
}
