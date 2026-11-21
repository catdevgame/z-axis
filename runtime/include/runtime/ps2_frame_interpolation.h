#pragma once

// The window loop's side of frame interpolation (runtime/gs/gs_interpolate.h; the Display setting Frame
// Interpolation): which picture each display refresh shows.
//
// A refresh shows the game as it was one game frame before the refresh (its display time, in the guest's vblank
// ticks): the latched frame until the next frame's tick is reached, and in between the renderer's picture at that
// point between the two (GS::presentBetween). The next frame is taken from the frame queue when the display time
// reaches it. Only on a display faster than the game (a refresh under 90 % of the game's frame), paced, on the
// GPU renderer; otherwise the loop runs as without it.
//
// The delay: with the guest's vblanks phase-locked to the refreshes (ps2_host_pacing.h: a 120 Hz display, each
// vblank kAfterLatch after a refresh) one game frame less kAfterLatch, so the refreshes fall half way between two
// frames and on the newer one; otherwise one game frame plus the time the renderer takes to make a frame after its
// vblank (measured), so the next frame is there when the display time reaches the latched one.
//
// ZX_PERF_LOG=<seconds> prints an [interp] line per interval: pictures between, the refreshes that showed a
// frame as it is and why (gs_interpolate::Skip), the refresh intervals, and how long after a frame was made it was
// first seen moving (first) and shown whole (whole).

#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_interpolate.h"

#include <chrono>
#include <cstdint>
#include <vector>

class PS2Runtime;

namespace zx
{
    class FrameInterpolation
    {
    public:
        using Clock = std::chrono::steady_clock;

        // This refresh interpolates (the setting, the frame queue, paced, a fast enough display).
        bool active(PS2Runtime &runtime);
        // The display time of a refresh latched at `latch`, in vblank ticks.
        double displayTick(Clock::time_point latch);
        // After the latch: `picture` becomes the picture between the latched frame and the next at displayTick
        // when the renderer makes one (`taken`: the frame latched at this refresh, if any).
        void between(PS2Runtime &runtime, double displayTick, Clock::time_point latch, const GS::TakenFrame &taken,
                     GSGpuPicture &picture);
        // The refresh presented.
        void presented(Clock::time_point at);
        // The [interp] line for the interval of `span` seconds (and the renderer's counts).
        void report(PS2Runtime &runtime, double span);

    private:
        static constexpr size_t kSkips = static_cast<size_t>(gs_interpolate::Skip::kCount);
        uint64_t m_between = 0;
        uint64_t m_skips[kSkips]{};
        int64_t m_lagNs = 0;          // the renderer's frame after its vblank (a slow-falling maximum)
        uint64_t m_lastNextFrame = 0; // the next frame whose lag was measured
        // A frame's first moving picture and its whole one, ms after it was made.
        uint64_t m_firstSeenFrame = 0;
        std::vector<double> m_firstMs, m_wholeMs;
        int64_t m_pendingMadeNs = 0; // the frame first seen at this refresh: its made time (0: none)
        int64_t m_wholeMadeNs = 0;   // the frame latched at this refresh
        Clock::time_point m_lastPresent{};
        std::vector<double> m_intervals; // ms between presents
    };
}
