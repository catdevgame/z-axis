#include "MiniTest.h"
#include "collection/settings.h"
#include "gs/gs_interpolate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gi = zx::gs_interpolate;

namespace
{
    // The window loop's frame interpolation as runtime/frame_interpolation.cpp has it, unlocked (a refresh of
    // `refreshNs`, in no whole ratio to the game's): the game's frame n made frameLagNs(n) after its vblank n * P,
    // each refresh showing the game at one frame plus FrameLag before it. After the first second: the refreshes,
    // the pictures between, those past the shown frame with no next one, and the most frames waiting after a latch.
    struct LoopRun
    {
        int refreshes = 0, between = 0, notReady = 0;
        int64_t mostWaiting = 0;
    };
    template <class FrameLagNs>
    LoopRun runLoop(int64_t refreshNs, FrameLagNs frameLagNs)
    {
        const int64_t period = 16683333;
        gi::FrameLag lag;
        LoopRun run;
        int64_t shown = -1, measured = -1;
        for (int64_t k = 1; k * refreshNs < 10'000'000'000LL; ++k)
        {
            const int64_t latch = k * refreshNs + 1234567;
            const double displayTick = static_cast<double>(latch - period - lag.lagNs) / static_cast<double>(period);
            auto made = [&](int64_t n) { return n * period + frameLagNs(n) <= latch; };
            auto seen = [&](int64_t n)
            {
                if (n > measured)
                {
                    measured = n;
                    lag.made(frameLagNs(n), period);
                }
            };
            if (shown < 0)
            {
                if (made(0))
                    seen(shown = 0);
                continue;
            }
            // (one frame taken a latch, once the display time reaches it)
            if (made(shown + 1) && displayTick >= static_cast<double>(shown + 1) - gi::kSnap)
                seen(++shown);
            if (shown < 60)
                continue;
            int64_t waiting = 0;
            while (made(shown + 1 + waiting))
                ++waiting;
            run.mostWaiting = std::max(run.mostWaiting, waiting);
            ++run.refreshes;
            if (made(shown + 1))
            {
                seen(shown + 1);
                const float t = gi::alphaBetween(static_cast<uint64_t>(shown), static_cast<uint64_t>(shown + 1), displayTick);
                run.between += t > gi::kSnap && t < 1.0f - gi::kSnap;
            }
            else
                run.notReady += displayTick > static_cast<double>(shown) + gi::kSnap;
        }
        return run;
    }

    void identity(gi::Matrix &m)
    {
        std::memset(m, 0, sizeof(m));
        for (int i = 0; i < 4; ++i)
            m[i][i] = 1.0f;
    }

    // Rotation by `degrees` about z (row vectors), then scale s, then translation (tx, ty, tz).
    void transform(gi::Matrix &m, float degrees, float s, float tx, float ty, float tz)
    {
        const float a = degrees * 3.14159265358979f / 180.0f;
        identity(m);
        m[0][0] = std::cos(a) * s;
        m[0][1] = std::sin(a) * s;
        m[1][0] = -std::sin(a) * s;
        m[1][1] = std::cos(a) * s;
        m[2][2] = s;
        m[3][0] = tx;
        m[3][1] = ty;
        m[3][2] = tz;
    }

    bool near(const gi::Matrix &a, const gi::Matrix &b, float eps)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(a[r][c] - b[r][c]) > eps)
                    return false;
        return true;
    }

    // A perspective camera's world -> window matrix: view (world -> camera) x projection (x, y scaled and offset
    // by w, Z = 16 (a + b / w) as the games' FTOI4 projection).
    void window(const gi::Matrix &view, gi::Matrix &out)
    {
        gi::Matrix p{};
        p[0][0] = 333.67f;
        p[1][1] = 194.64f;
        p[2][0] = 2048.0f;
        p[2][1] = 2048.0f;
        p[2][2] = 16.0f * -5.063f;
        p[2][3] = 1.0f;
        p[3][2] = 16.0f * 5125.5f;
        gi::multiply(view, p, out);
    }
}

void register_gs_interpolate_tests()
{
    MiniTest::Case("GSInterpolate", [](TestCase &tc)
    {
        tc.Run("affine split and joined again", [](TestCase &t)
        {
            gi::Matrix m, back;
            transform(m, 37.0f, 1.5f, 3.0f, -2.0f, 10.0f);
            gi::Affine a;
            t.IsTrue(gi::decompose(m, a), "decomposes");
            gi::compose(a, back);
            t.IsTrue(near(m, back, 1e-4f), "compose(decompose(m)) == m");
            gi::Matrix flat;
            transform(flat, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            flat[2][2] = 0.0f;
            t.IsTrue(!gi::decompose(flat, a), "a part scaled to nothing does not");
            gi::Matrix projective;
            identity(projective);
            projective[2][3] = 1.0f;
            t.IsTrue(!gi::decompose(projective, a), "nor a projection");
        });

        tc.Run("between: the ends, the rotation's half way, the shorter way round", [](TestCase &t)
        {
            gi::Matrix a, b, m, want;
            transform(a, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            transform(b, 90.0f, 2.0f, 10.0f, 0.0f, 0.0f);
            t.IsTrue(gi::betweenAffine(a, b, 0.0f, m) && near(m, a, 1e-5f), "t = 0 is a");
            t.IsTrue(gi::betweenAffine(a, b, 1.0f, m) && near(m, b, 1e-5f), "t = 1 is b");
            t.IsTrue(gi::betweenAffine(a, b, 0.5f, m), "half way");
            transform(want, 45.0f, 1.5f, 5.0f, 0.0f, 0.0f);
            t.IsTrue(near(m, want, 1e-4f), "45 degrees, scale 1.5, translation 5 (not a lerp of the matrices)");
            transform(a, 170.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            transform(b, -170.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            transform(want, 180.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            t.IsTrue(gi::betweenAffine(a, b, 0.5f, m) && near(m, want, 1e-4f), "170 to -170 goes through 180");
        });

        tc.Run("a camera moves on a straight line while it turns", [](TestCase &t)
        {
            // Views (world -> camera) of a camera at x = 0 and x = 4, turned 0 and 20 degrees.
            gi::Matrix camA, camB, viewA, viewB, view, cam;
            transform(camA, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            transform(camB, 20.0f, 1.0f, 4.0f, 0.0f, 0.0f);
            t.IsTrue(gi::invert(camA, viewA) && gi::invert(camB, viewB), "views");
            t.IsTrue(gi::betweenView(viewA, viewB, 0.25f, view) && gi::invert(view, cam), "between");
            t.IsTrue(std::fabs(cam[3][0] - 1.0f) < 1e-4f && std::fabs(cam[3][1]) < 1e-4f, "at x = 1, a quarter of the way");
        });

        tc.Run("cuts and jumps: the engine's 7 units and 20 degrees", [](TestCase &t)
        {
            gi::Matrix a, b, va, vb;
            transform(a, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            transform(b, 0.0f, 1.0f, 6.0f, 0.0f, 0.0f);
            gi::invert(a, va);
            gi::invert(b, vb);
            t.IsTrue(!gi::cameraCut(va, vb), "6 units is no cut");
            transform(b, 0.0f, 1.0f, 7.5f, 0.0f, 0.0f);
            gi::invert(b, vb);
            t.IsTrue(gi::cameraCut(va, vb), "7.5 units is");
            // Turns about y move the forward axis (row 2).
            gi::Matrix ry;
            identity(ry);
            const float c = std::cos(15.0f * 3.14159265f / 180.0f), s = std::sin(15.0f * 3.14159265f / 180.0f);
            ry[0][0] = c;
            ry[0][2] = -s;
            ry[2][0] = s;
            ry[2][2] = c;
            gi::invert(ry, vb);
            t.IsTrue(!gi::cameraCut(va, vb), "15 degrees is no cut");
            const float c2 = std::cos(25.0f * 3.14159265f / 180.0f), s2 = std::sin(25.0f * 3.14159265f / 180.0f);
            ry[0][0] = c2;
            ry[0][2] = -s2;
            ry[2][0] = s2;
            ry[2][2] = c2;
            gi::invert(ry, vb);
            t.IsTrue(gi::cameraCut(va, vb), "25 degrees is");
            transform(b, 0.0f, 1.0f, 0.0f, 8.0f, 0.0f);
            t.IsTrue(gi::jumped(a, b) && !gi::jumped(a, a), "an object 8 units away jumped");
        });

        tc.Run("GS-path vertices follow the camera", [](TestCase &t)
        {
            gi::Matrix cam, view, from, inverse, to;
            transform(cam, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            gi::invert(cam, view);
            window(view, from);
            t.IsTrue(gi::invert(from, inverse), "invertible");
            // A world point 50 units ahead.
            const float p[4] = {2.0f, 1.0f, 50.0f, 1.0f};
            float c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = p[0] * from[0][k] + p[1] * from[1][k] + p[2] * from[2][k] + from[3][k];
            float xyz[3] = {c[0] / c[3], c[1] / c[3], c[2] / c[3]};
            const float orig[3] = {xyz[0], xyz[1], xyz[2]};
            float ratio = 0.0f;
            t.IsTrue(gi::reproject(inverse, from, from, xyz, ratio), "the same camera");
            t.IsTrue(std::fabs(xyz[0] - orig[0]) < 1e-2f && std::fabs(xyz[1] - orig[1]) < 1e-2f &&
                         std::fabs(xyz[2] - orig[2]) < 1.0f && std::fabs(ratio - 1.0f) < 1e-4f,
                     "stays where it is");
            // The camera 1 unit to the right: the point moves left by 333.67 / 50 pixels.
            transform(cam, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
            gi::invert(cam, view);
            window(view, to);
            float moved[3] = {orig[0], orig[1], orig[2]};
            t.IsTrue(gi::reproject(inverse, from, to, moved, ratio), "a moved camera");
            t.IsTrue(std::fabs((orig[0] - moved[0]) - 333.67f / 50.0f) < 1e-2f, "6.67 pixels to the left");
        });

        tc.Run("the warp's matrix: one camera's GS window to the other's", [](TestCase &t)
        {
            // presentWarp's toA = B's window inverse x A's window, used in warp_fs on a row vector (X, Y, Z, 1) and
            // divided by its w: the same point as reproject gives, with no world point in between.
            gi::Matrix cam, view, fromB, inverseB, toA, rows;
            transform(cam, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            gi::invert(cam, view);
            window(view, fromB);
            t.IsTrue(gi::invert(fromB, inverseB), "invertible");
            transform(cam, 10.0f, 1.0f, 1.0f, 0.5f, -2.0f);
            gi::invert(cam, view);
            window(view, toA);
            gi::multiply(inverseB, toA, rows);
            const float p[4] = {2.0f, 1.0f, 50.0f, 1.0f};
            float c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = p[0] * fromB[0][k] + p[1] * fromB[1][k] + p[2] * fromB[2][k] + fromB[3][k];
            float xyz[3] = {c[0] / c[3], c[1] / c[3], c[2] / c[3]};
            float h[4];
            for (int k = 0; k < 4; ++k)
                h[k] = xyz[0] * rows[0][k] + xyz[1] * rows[1][k] + xyz[2] * rows[2][k] + rows[3][k];
            float ratio = 0.0f;
            t.IsTrue(h[3] > 0.0f && gi::reproject(inverseB, fromB, toA, xyz, ratio), "in front of both");
            t.IsTrue(std::fabs(h[0] / h[3] - xyz[0]) < 1e-2f && std::fabs(h[1] / h[3] - xyz[1]) < 1e-2f,
                     "where reproject puts it");
        });

        tc.Run("display time in vblank ticks and alpha", [](TestCase &t)
        {
            gi::VblankClock c;
            c.tick = 100;
            c.tickNs = 1000000000;
            c.periodNs = 16000000;
            t.IsTrue(std::fabs(c.tickAt(1000000000 + 8000000) - 100.5) < 1e-9, "half a period after tick 100");
            t.IsTrue(std::fabs(gi::alphaBetween(100, 101, 100.25) - 0.25f) < 1e-6f, "a quarter");
            t.IsTrue(std::fabs(gi::alphaBetween(100, 102, 101.0) - 0.5f) < 1e-6f, "a 30 fps frame: half way at one tick");
            t.Equals(gi::alphaBetween(100, 101, 99.0), 0.0f, "clamped below");
            t.Equals(gi::alphaBetween(100, 101, 102.0), 1.0f, "clamped above");
        });

        tc.Run("the frames' lag: a slow-falling maximum, at most a quarter frame above the quickest recent", [](TestCase &t)
        {
            const int64_t period = 16000000;
            gi::FrameLag lag;
            for (int i = 0; i < 30; ++i)
                lag.made(9000000, period);
            t.Equals(lag.lagNs, int64_t{9000000}, "steady frames: their lag");
            lag.made(8000000, period);
            t.Equals(lag.lagNs, int64_t{9000000 - 9000000 / 64}, "a quicker frame: falls slowly");
            lag.made(30000000, period);
            t.Equals(lag.lagNs, int64_t{8000000 + period / 4}, "one late frame: a quarter frame above the quickest recent");
            for (size_t i = 0; i < gi::FrameLag::kRecent; ++i)
                lag.made(20000000, period);
            t.Equals(lag.lagNs, int64_t{20000000}, "frames steadily later than a frame: no cap");
            lag.made(3000000, period);
            t.Equals(lag.lagNs, int64_t{3000000 + period / 4}, "then a quick one: a quarter frame above it");
            lag.made(-500000, period);
            t.Equals(lag.lagNs, int64_t{period / 4}, "made before its vblank's deadline: as at it");
        });

        tc.Run("75, 120 and 144 Hz unlocked: a picture between at nearly every refresh, no frame pushed out", [](TestCase &t)
        {
            for (const int64_t refreshNs : {int64_t{13333333}, int64_t{8333333}, int64_t{6944444}})
            {
                const std::string hz = std::to_string(1000000000 / refreshNs) + " Hz: ";
                // (frames made 20 ms after their vblanks, later than a frame: the quarter-frame cap had 3 % between)
                const LoopRun late = runLoop(refreshNs, [](int64_t) { return int64_t{20000000}; });
                t.IsTrue(late.between * 10 > late.refreshes * 9 && late.notReady == 0, hz + "late frames: between");
                t.IsTrue(late.mostWaiting <= 2, hz + "late frames: at most two waiting");
                // (frames 3 and 15 ms late by turns: some refreshes show a frame as it is, none is pushed out)
                const LoopRun uneven = runLoop(refreshNs, [](int64_t n) { return n % 2 ? int64_t{3000000} : int64_t{15000000}; });
                t.IsTrue(uneven.between * 10 > uneven.refreshes * 7, hz + "uneven frames: mostly between");
                t.IsTrue(uneven.mostWaiting <= 2, hz + "uneven frames: at most two waiting");
            }
        });

        tc.Run("pictures between only on a fixed-refresh display faster than the game", [](TestCase &t)
        {
            const int64_t frame = 16683333;
            for (const int64_t refreshNs : {int64_t{13333333}, int64_t{8333333}, int64_t{6944444}})
            {
                const std::string hz = std::to_string(1000000000 / refreshNs) + " Hz: ";
                t.IsTrue(gi::interpolates(false, refreshNs, frame), hz + "fixed refresh: on");
                t.IsTrue(!gi::interpolates(true, refreshNs, frame), hz + "variable refresh wins");
            }
            t.IsTrue(!gi::interpolates(false, 16666667, frame), "60 Hz: off");
            t.IsTrue(!gi::interpolates(false, 0, frame), "refresh not known: off");
        });

        tc.Run("settings.ini [display] frame_interpolation", [](TestCase &t)
        {
            std::vector<std::string> warnings;
            const zx::app::Settings off = zx::app::parseSettings("", &warnings);
            t.IsTrue(!off.frameInterpolation, "off by default");
            const zx::app::Settings on = zx::app::parseSettings("[display]\nframe_interpolation = true\n", &warnings);
            t.IsTrue(warnings.empty() && on.frameInterpolation, "read");
            t.IsTrue(zx::app::parseSettings(zx::app::formatSettings(on), &warnings).frameInterpolation, "written back");
        });
    });
}
