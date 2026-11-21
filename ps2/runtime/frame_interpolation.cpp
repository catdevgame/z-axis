// The window loop's side of frame interpolation (runtime/frame_interpolation.h).
#include "runtime/frame_interpolation.h"

#include "machine.h"
#include "runtime/host_pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace zx
{
    namespace gi = gs_interpolate;

    namespace
    {
        int64_t nanoseconds(FrameInterpolation::Clock::time_point t)
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }

        bool paced()
        {
            static const bool unpaced = []
            {
                const char *v = std::getenv("ZX_UNPACED");
                return v && v[0] == '1';
            }();
            return !unpaced;
        }

        // Percentile p (0..1) of v (sorted in place).
        double percentile(std::vector<double> &v, double p)
        {
            if (v.empty())
                return 0.0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1u, static_cast<size_t>(p * static_cast<double>(v.size())))];
        }
    }

    bool FrameInterpolation::active(PS2Machine &runtime)
    {
        if (!gi::enabled() || !paced() || !runtime.gs().frameQueue())
            return false;
        // A display faster than the game's frames: its refresh under 90 % of a vblank.
        const int64_t refresh = host_pacing::g_periodNs.load(std::memory_order_relaxed);
        const gi::VblankClock clock = gi::vblankClock();
        return refresh > 0 && clock.tickNs != 0 && refresh * 10 < clock.periodNs * 9;
    }

    double FrameInterpolation::displayTick(Clock::time_point latch)
    {
        const gi::VblankClock clock = gi::vblankClock();
        const int64_t refresh = host_pacing::g_periodNs.load(std::memory_order_relaxed);
        // Phase-locked: a whole number of refreshes per vblank (within 1 %) and the lock steady.
        bool locked = false;
        if (refresh > 0 && host_pacing::enabled() &&
            host_pacing::g_steady.load(std::memory_order_relaxed) >= host_pacing::kSteadyLatches)
        {
            const double ratio = static_cast<double>(clock.periodNs) / static_cast<double>(refresh);
            locked = std::fabs(ratio - std::round(ratio)) < 0.01 * ratio && std::round(ratio) >= 2.0;
        }
        const int64_t afterLatch = std::chrono::duration_cast<std::chrono::nanoseconds>(host_pacing::kAfterLatch).count();
        const int64_t delay = locked ? clock.periodNs - afterLatch : clock.periodNs + m_lagNs;
        return clock.tickAt(nanoseconds(latch) - delay);
    }

    void FrameInterpolation::between(PS2Machine &runtime, double displayTick, Clock::time_point latch,
                                     const GS::TakenFrame &taken, GSGpuPicture &picture)
    {
        GS &gs = runtime.gs();
        m_pendingMadeNs = 0;
        m_wholeMadeNs = taken.frame != 0u ? taken.madeNs : 0;
        const GS::TakenFrame shown = gs.shownPresentationFrame();
        GS::TakenFrame next;
        auto skip = [&](gi::Skip why)
        { ++m_skips[static_cast<size_t>(why)]; };
        if (shown.frame == 0u)
        {
            skip(gi::Skip::NoPair);
            return;
        }
        if (!gs.peekPresentationFrame(next) || next.vsync <= shown.vsync)
        {
            // The display time past the latched frame, with nothing newer to go to.
            skip(displayTick > static_cast<double>(shown.vsync) + gi::kSnap ? gi::Skip::NotReady : gi::Skip::Exact);
            return;
        }
        if (next.frame != m_lastNextFrame)
        {
            // The renderer's lag: the frame made after its vblank's host time.
            m_lastNextFrame = next.frame;
            const gi::VblankClock clock = gi::vblankClock();
            const int64_t vblankNs = clock.tickNs - static_cast<int64_t>(clock.tick - next.vsync) * clock.periodNs;
            const int64_t lag = std::clamp<int64_t>(next.madeNs - vblankNs, 0, clock.periodNs);
            m_lagNs = std::max(lag, m_lagNs - m_lagNs / 64);
        }
        const float t = gi::alphaBetween(shown.vsync, next.vsync, displayTick);
        if (t <= gi::kSnap || t >= 1.0f - gi::kSnap)
        {
            skip(gi::Skip::Exact);
            return;
        }
        GSGpuPicture between;
        const gi::Skip why = gs.presentBetween(shown.frame, next.frame, t, between);
        if (why != gi::Skip::kCount)
        {
            skip(why);
            return;
        }
        picture = between;
        ++m_between;
        if (next.frame != m_firstSeenFrame)
        {
            m_firstSeenFrame = next.frame;
            m_pendingMadeNs = next.madeNs;
        }
        (void)latch;
    }

    void FrameInterpolation::presented(Clock::time_point at)
    {
        if (!gi::enabled())
            return;
        const int64_t now = nanoseconds(at);
        if (m_lastPresent != Clock::time_point{} && m_intervals.size() < 200000u)
            m_intervals.push_back(std::chrono::duration<double, std::milli>(at - m_lastPresent).count());
        m_lastPresent = at;
        if (m_pendingMadeNs && m_firstMs.size() < 200000u)
            m_firstMs.push_back(static_cast<double>(now - m_pendingMadeNs) * 1e-6);
        if (m_wholeMadeNs && m_wholeMs.size() < 200000u)
            m_wholeMs.push_back(static_cast<double>(now - m_wholeMadeNs) * 1e-6);
        m_pendingMadeNs = m_wholeMadeNs = 0;
    }

    void FrameInterpolation::report(PS2Machine &runtime, double span)
    {
        std::string line = "[interp] between=" + std::to_string(m_between);
        char buf[160];
        std::snprintf(buf, sizeof(buf), " (%.1f/s)", span > 0.0 ? static_cast<double>(m_between) / span : 0.0);
        line += buf;
        line += " shown-as-is:";
        for (size_t i = 0; i < kSkips; ++i)
            line += std::string(" ") + gi::skipName(static_cast<gi::Skip>(i)) + "=" + std::to_string(m_skips[i]);
        std::snprintf(buf, sizeof(buf), " | refresh ms p50=%.2f p99=%.2f max=%.2f n=%zu", percentile(m_intervals, 0.5),
                      percentile(m_intervals, 0.99), m_intervals.empty() ? 0.0 : m_intervals.back(), m_intervals.size());
        line += buf;
        std::snprintf(buf, sizeof(buf), " | first ms p50=%.1f p95=%.1f whole ms p50=%.1f p95=%.1f lag=%.1f",
                      percentile(m_firstMs, 0.5), percentile(m_firstMs, 0.95), percentile(m_wholeMs, 0.5),
                      percentile(m_wholeMs, 0.95), static_cast<double>(m_lagNs) * 1e-6);
        line += buf;
        std::fprintf(stderr, "%s\n", line.c_str());
        const std::string renderer = runtime.gs().interpolationReport();
        if (!renderer.empty())
            std::fprintf(stderr, "[interp] renderer %s\n", renderer.c_str());
        m_between = 0;
        for (uint64_t &n : m_skips)
            n = 0;
        m_intervals.clear();
        m_firstMs.clear();
        m_wholeMs.clear();
    }
}
