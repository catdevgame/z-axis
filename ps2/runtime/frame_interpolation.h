#pragma once

// The window loop's side of frame interpolation (gs/gs_interpolate.h; the Display setting Frame
// Interpolation): which picture each display refresh shows.
//
// A refresh shows the game as it was one game frame before the refresh (its display time, in the guest's vblank
// ticks): the latched frame until the next frame's tick is reached, and in between the renderer's picture at that
// point between the two (GS::presentBetween). The next frame is taken from the frame queue when the display time
// reaches it. Only on a fixed-refresh display faster than the game (a refresh under 90 % of the game's frame),
// paced, on the GPU renderer; otherwise the loop runs as without it.
//
// The display's refresh is the one measured at the window's latches (host_pacing::noteLatch), or before any is
// measured its display mode's (ps2_host::displayRefreshNs). With Variable Refresh on as well, variable refresh wins
// (gs_interpolate::interpolates): each game frame is shown when it is ready and no picture between is made.
//
// The delay: with the guest's vblanks phase-locked to the refreshes (host_pacing.h: a 120 Hz display, each
// vblank kAfterLatch after a refresh) one game frame less kAfterLatch, so the refreshes fall half way between two
// frames and on the newer one; otherwise (a refresh in no whole ratio to the game's: 75, 144, 165 Hz) one game
// frame plus how late the renderer makes its frames after their vblanks (gs_interpolate::FrameLag, from the
// frames' finish times), so the next frame is there when the display time reaches the latched one. At 75 Hz four
// or five refreshes of five fall between two frames: nearly every refresh is a picture between.
//
// ZX_PERF_LOG=<seconds> prints an [interp] line per interval: pictures between, the refreshes that showed a
// frame as it is and why (gs_interpolate::Skip), the refresh intervals, how long after a frame was made it was
// first seen moving (first) and shown whole (whole), the frames' own lags after their vblanks (frame lag) and the
// delay's (lag), and the game's frames made and pushed out unshown.

#include "gs/gs_frontend.h"
#include "gs/gs_interpolate.h"

#include <chrono>
#include <cstdint>
#include <vector>

class PS2Machine;

namespace zx
{
    class FrameInterpolation
    {
    public:
        using Clock = std::chrono::steady_clock;

        // This refresh interpolates (the setting, the frame queue, paced, a fast enough display).
        bool active(PS2Machine &runtime);
        // The display time of a refresh latched at `latch`, in vblank ticks.
        double displayTick(Clock::time_point latch);
        // After the latch: `picture` becomes the picture between the latched frame and the next at displayTick
        // when the renderer makes one (`taken`: the frame latched at this refresh, if any).
        void between(PS2Machine &runtime, double displayTick, Clock::time_point latch, const GS::TakenFrame &taken,
                     GSGpuPicture &picture);
        // The refresh presented.
        void presented(Clock::time_point at);
        // The [interp] line for the interval of `span` seconds (and the renderer's counts).
        void report(PS2Machine &runtime, double span);

    private:
        // A frame's lag after its vblank (gs_interpolate::FrameLag), once per frame.
        void measureLag(const GS::TakenFrame &frame);
        static constexpr size_t kSkips = static_cast<size_t>(gs_interpolate::Skip::kCount);
        uint64_t m_between = 0;
        uint64_t m_skips[kSkips]{};
        gs_interpolate::FrameLag m_lag;   // the renderer's frames after their vblanks, for the delay
        uint64_t m_lastMeasuredFrame = 0; // the newest frame whose lag was measured
        bool m_modeReported = false;      // the display mode's refresh logged (once)
        std::vector<double> m_frameLagMs, m_makingMs; // each frame's own lag and the time making it took, ms
        // A frame's first moving picture and its whole one, ms after it was made.
        uint64_t m_firstSeenFrame = 0;
        std::vector<double> m_firstMs, m_wholeMs;
        int64_t m_pendingMadeNs = 0; // the frame first seen at this refresh: its made time (0: none)
        int64_t m_wholeMadeNs = 0;   // the frame latched at this refresh
        Clock::time_point m_lastPresent{};
        std::vector<double> m_intervals; // ms between presents
        uint64_t m_reportedMade = 0, m_reportedDropped = 0; // the frame queue's counts at the last report
    };
}
