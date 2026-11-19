#include "MiniTest.h"
#include "runtime/ps2_photo.h"

#include <cmath>
#include <vector>

namespace ph = ps2x::photo;

namespace
{
    bool near(float a, float b, float eps = 1e-3f)
    {
        return std::fabs(a - b) <= eps;
    }

    bool near(const ph::Vec3 &a, const ph::Vec3 &b, float eps = 1e-3f)
    {
        return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
    }

    std::vector<uint8_t> grey(uint32_t width, uint32_t height, uint8_t value)
    {
        return std::vector<uint8_t>(static_cast<size_t>(width) * height * 4u, value);
    }
}

void register_ps2_photo_tests()
{
    MiniTest::Case("Photo", [](TestCase &tc)
    {
        tc.Run("lookFrom keeps the game camera's view and FOV (y down)", [](TestCase &t)
        {
            ph::FreeCamera cam;
            // Eye above and behind, looking down at the skater: y down, so
            // the target has the larger y.
            cam.lookFrom({1.0f, -2.0f, 3.0f}, {1.0f, -1.0f, 4.0f}, 75.0f);
            t.IsTrue(near(cam.pitch, -45.0f), "pitch: 45 degrees down");
            t.IsTrue(near(cam.yaw, 0.0f), "yaw: along +z");
            t.IsTrue(near(cam.fov, 75.0f), "fov");
            const ph::Vec3 dir = ph::normalized({0.0f, 1.0f, 1.0f});
            t.IsTrue(near(cam.forward(), dir), "forward points at the target");
            t.IsTrue(near(cam.target(), ph::add(cam.position, dir)), "target one unit ahead");
        });

        tc.Run("right is level and across forward; up has the world's up", [](TestCase &t)
        {
            ph::FreeCamera cam;
            cam.yaw = 30.0f;
            cam.pitch = 20.0f;
            t.IsTrue(near(ph::dot(cam.right(), cam.forward()), 0.0f), "right across forward");
            t.IsTrue(near(cam.right().y, 0.0f), "right level");
            t.IsTrue(ph::dot(cam.up(), cam.worldUp) > 0.0f, "up is up");
            t.IsTrue(near(ph::length(cam.up()), 1.0f), "unit up");
            t.IsTrue(cam.forward().y < 0.0f, "pitch up: towards -y");
        });

        tc.Run("update: moves, turns, rolls and zooms within limits", [](TestCase &t)
        {
            ph::FreeCamera cam;
            cam.speed = 10.0f;
            ph::Controls c;
            c.moveForward = 1.0f;
            cam.update(c, 0.1f);
            t.IsTrue(near(cam.position, {0.0f, 0.0f, 1.0f}), "one unit forward");
            c = {};
            c.moveUp = 1.0f;
            c.speedFactor = 4.0f;
            cam.update(c, 0.05f);
            t.IsTrue(near(cam.position, {0.0f, -2.0f, 1.0f}), "up is -y, four times faster");
            c = {};
            c.lookYaw = 190.0f;
            c.lookPitch = 120.0f;
            cam.update(c, 0.0f);
            t.IsTrue(near(cam.yaw, -170.0f), "yaw wraps");
            t.IsTrue(near(cam.pitch, 89.0f), "pitch stops short of straight up");
            c = {};
            c.roll = 1.0f;
            cam.update(c, 0.1f);
            t.IsTrue(near(cam.roll, 4.5f), "roll rate");
            for (int i = 0; i < 20; ++i)
                cam.update(c, 0.1f);
            t.IsTrue(near(cam.roll, 45.0f), "roll limit");
            c = {};
            c.zoom = 100.0f;
            cam.update(c, 0.0f);
            t.IsTrue(near(cam.fov, cam.minFov), "zoom limit");
            cam.update(c, 2.0f);
            t.IsTrue(near(cam.position, {0.0f, -2.0f, 1.0f}), "a long frame counts as 0.1 s at most, no move asked");
        });

        tc.Run("update: stays within maxDistance of home", [](TestCase &t)
        {
            ph::FreeCamera cam;
            cam.lookFrom({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 60.0f);
            cam.maxDistance = 3.0f;
            ph::Controls c;
            c.moveForward = 1.0f;
            c.speedFactor = 10.0f;
            for (int i = 0; i < 10; ++i)
                cam.update(c, 0.1f);
            t.IsTrue(near(cam.position, {0.0f, 0.0f, 3.0f}), "held at the limit");
        });

        tc.Run("rollRows: a quarter turn swaps right and up", [](TestCase &t)
        {
            ph::Vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, -1.0f, 0.0f};
            ph::rollRows(right, up, 90.0f);
            t.IsTrue(near(right, {0.0f, 1.0f, 0.0f}), "right turned");
            t.IsTrue(near(up, {1.0f, 0.0f, 0.0f}), "up turned");
            ph::rollRows(right, up, -90.0f);
            t.IsTrue(near(right, {1.0f, 0.0f, 0.0f}) && near(up, {0.0f, -1.0f, 0.0f}), "and back");
        });

        tc.Run("filters: letterbox bars, vignette corners, black and white", [](TestCase &t)
        {
            t.Equals(ph::letterboxRows(480, 4.0f / 3.0f), 106u, "4:3: (480 - 480 * 4/3 / 2.39) / 2");
            t.Equals(ph::letterboxRows(480, 2.5f), 0u, "wider than 2.39: none");
            std::vector<uint8_t> pic = grey(64, 48, 200);
            ph::applyFilters(pic.data(), 64, 48, 64 * 4, ph::kFilterLetterbox, 4.0f / 3.0f);
            t.Equals(static_cast<int>(pic[0]), 0, "top row black");
            t.Equals(static_cast<int>(pic[(47 * 64) * 4]), 0, "bottom row black");
            t.Equals(static_cast<int>(pic[(24 * 64 + 32) * 4]), 200, "middle kept");
            t.Equals(static_cast<int>(pic[3]), 200, "alpha kept");

            pic = grey(64, 48, 200);
            ph::applyFilters(pic.data(), 64, 48, 64 * 4, ph::kFilterVignette, 4.0f / 3.0f);
            t.Equals(static_cast<int>(pic[(24 * 64 + 32) * 4]), 200, "centre kept");
            t.IsTrue(pic[0] < 100, "corner darker");

            pic = {255, 0, 0, 7, 0, 0, 255, 7};
            ph::applyFilters(pic.data(), 2, 1, 8, ph::kFilterMono, 4.0f / 3.0f);
            t.IsTrue(pic[0] == pic[1] && pic[1] == pic[2], "grey");
            t.IsTrue(pic[0] > pic[4], "red brighter than blue");
            t.Equals(static_cast<int>(pic[3]), 7, "alpha kept");
            t.Equals(ph::presets().size(), static_cast<size_t>(6), "six presets");
            t.Equals(ph::presets().front().filters, 0u, "the first is none");
        });
    });
}
