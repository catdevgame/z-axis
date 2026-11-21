#include "MiniTest.h"
#include "collection/settings.h"
#include "gs/gs_interpolate.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace gi = zx::gs_interpolate;

namespace
{
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
