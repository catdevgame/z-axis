#include "MiniTest.h"
#include "runtime/ps2_host_pacing.h"

#include <chrono>
#include <cstdint>

namespace hp = ps2x::host_pacing;

namespace
{
    using Clock = hp::Clock;

    void reset()
    {
        hp::g_latchNs.store(0);
        hp::g_periodNs.store(0);
        hp::g_steady.store(0);
        hp::g_slip.store(false);
        hp::g_waitingRun = 0;
    }

    // Latches every `period` up to now; returns the last one.
    Clock::time_point latches(Clock::duration period, int count)
    {
        const Clock::time_point now = Clock::now();
        for (int i = count - 1; i >= 0; --i)
            hp::noteLatch(now - period * i);
        return now;
    }

    // Where `t` falls between two latches of `period` after `latch`, 0..1.
    double phaseOf(Clock::time_point t, Clock::time_point latch, Clock::duration period)
    {
        const int64_t p = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
        int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(t - latch).count() % p;
        if (d < 0)
            d += p;
        return static_cast<double>(d) / static_cast<double>(p);
    }
}

void register_ps2_host_pacing_tests()
{
    MiniTest::Case("HostPacing", [](TestCase &tc)
    {
        const auto guest = std::chrono::microseconds(16667);

        tc.Run("a vblank just before the 120 Hz latch moves to kAfterLatch after one, in small steps", [guest](TestCase &t)
        {
            reset();
            const auto refresh = std::chrono::nanoseconds(8333500);
            const Clock::time_point latch = latches(refresh, 40);
            Clock::time_point next = latch + std::chrono::nanoseconds(static_cast<int64_t>(0.95 * 8333500));
            int64_t largest = 0;
            for (int i = 0; i < 240; ++i)
            {
                const Clock::duration step = hp::correction(next, guest);
                largest = std::max<int64_t>(largest, std::llabs(std::chrono::duration_cast<std::chrono::nanoseconds>(step).count()));
                next += step + guest;
            }
            const double phase = phaseOf(next, latch, refresh);
            const double want = 1400000.0 / 8333500.0;
            t.IsTrue(phase > want - 0.02 && phase < want + 0.02, "the vblank sits kAfterLatch after a latch");
            t.IsTrue(largest <= 200000, "no step over 0.2 ms");
        });

        tc.Run("a 60 Hz display: the same, one latch per vblank", [guest](TestCase &t)
        {
            reset();
            const auto refresh = std::chrono::nanoseconds(16667000);
            const Clock::time_point latch = latches(refresh, 40);
            Clock::time_point next = latch + std::chrono::milliseconds(9);
            for (int i = 0; i < 400; ++i)
                next += hp::correction(next, guest) + guest;
            const double phase = phaseOf(next, latch, refresh);
            const double want = 1400000.0 / 16667000.0;
            t.IsTrue(phase > want - 0.02 && phase < want + 0.02, "the vblank sits kAfterLatch after a latch");
        });

        tc.Run("no steady refresh in a whole ratio, a stopped window or PS2X_VSYNC_PHASE=0's switch: no correction", [guest](TestCase &t)
        {
            reset();
            latches(std::chrono::microseconds(6944), 40); // 144 Hz
            t.IsTrue(hp::correction(Clock::now() + guest, guest) == Clock::duration::zero(), "144 Hz: none");
            reset();
            latches(std::chrono::nanoseconds(8333500), 10); // not steady yet
            t.IsTrue(hp::correction(Clock::now() + guest, guest) == Clock::duration::zero(), "10 latches: none");
            reset();
            const Clock::time_point now = Clock::now();
            for (int i = 40; i >= 1; --i)
                hp::noteLatch(now - std::chrono::milliseconds(100) - std::chrono::nanoseconds(8333500) * i);
            t.IsTrue(hp::correction(now + guest, guest) == Clock::duration::zero(), "no latch for 100 ms: none");
            reset();
        });

        tc.Run("a frame left waiting at 60 Hz: one vblank a period later, no frame dropped or shown twice", [guest](TestCase &t)
        {
            // The window takes one game frame per latch (the oldest waiting);
            // each frame is ready 3 ms after its vblank. A host frame that
            // took two refreshes left one more waiting than usual.
            reset();
            const int64_t refresh = 16667000;
            const Clock::time_point start = latches(std::chrono::nanoseconds(refresh), 40);
            const int64_t t0 = hp::nanoseconds(start);
            int64_t vblank = t0 + 1400000; // locked: kAfterLatch after a latch
            int64_t nextLatch = t0 + refresh;
            int waiting = 1;               // the extra frame
            int repeats = 0, most = 0, latchesSeen = 0;
            for (int i = 0; i < 600; ++i)
            {
                const int64_t ready = vblank + 3000000;
                while (nextLatch <= ready)
                {
                    // a latch before this frame is ready
                    hp::noteLatch(Clock::time_point(std::chrono::nanoseconds(nextLatch)));
                    const bool took = waiting > 0;
                    if (took)
                        --waiting;
                    else if (i > 0)
                        ++repeats;
                    hp::noteTaken(took ? static_cast<uint32_t>(waiting) : 0u);
                    nextLatch += refresh;
                    ++latchesSeen;
                }
                ++waiting;
                most = std::max(most, waiting);
                const Clock::time_point next(std::chrono::nanoseconds(vblank + std::chrono::duration_cast<std::chrono::nanoseconds>(guest).count()));
                vblank = hp::nanoseconds(next + hp::correction(next, guest));
            }
            t.IsTrue(repeats == 0, "no refresh without a new frame");
            t.IsTrue(most <= 2, "at most two frames waiting (none pushed out)");
            t.IsTrue(waiting <= 1, "none waiting at a latch at the end");
            const double phase = static_cast<double>((vblank - (nextLatch - refresh)) % refresh) / static_cast<double>(refresh);
            const double want = 1400000.0 / refresh;
            t.IsTrue(phase > want - 0.02 && phase < want + 0.02, "the vblank back at kAfterLatch after a latch");
            reset();
        });

        tc.Run("shown frames: refreshes per game frame and frames never shown", [](TestCase &t)
        {
            hp::ShownFrames shown;
            shown.repeated(); // (nothing on screen yet)
            shown.shown(1);
            shown.repeated();
            shown.shown(2);
            shown.shown(4);
            shown.repeated();
            shown.repeated();
            shown.shown(5);
            t.IsTrue(shown.byRefreshes[0] == 1 && shown.byRefreshes[1] == 1 && shown.byRefreshes[2] == 1, "1, 2 and 3 refreshes");
            t.IsTrue(shown.never == 1, "frame 3 never shown");
        });
    });
}
