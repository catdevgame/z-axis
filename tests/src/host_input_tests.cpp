#include "MiniTest.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_pad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ps2_host_input;

namespace
{
    int code(const char *name)
    {
        return hostCodeByName(name);
    }

    RawHostInput held(std::initializer_list<const char *> names)
    {
        RawHostInput raw;
        for (const char *name : names)
            raw.down.set(static_cast<size_t>(code(name)));
        return raw;
    }

    RawHostInput pressedNow(std::initializer_list<const char *> names)
    {
        RawHostInput raw = held(names);
        for (const char *name : names)
            raw.pressed.set(static_cast<size_t>(code(name)));
        return raw;
    }

    bool hasButton(const PadReport &report, int button)
    {
        return (report.buttons & (1u << button)) != 0u;
    }

    HostInputConfig configFrom(const std::string &ini, std::vector<std::string> *warnings = nullptr)
    {
        HostInputConfig config = HostInputConfig::defaults();
        config.applyIni(ini, warnings);
        return config;
    }

    // The mouse pushing a stick (the default before mouse-look).
    HostInputConfig stickMode(const std::string &extra = std::string())
    {
        return configFrom("[mouse]\nmode = stick\n" + extra);
    }

    bool near(float a, float b, float eps = 1e-3f)
    {
        return std::fabs(a - b) <= eps;
    }

    // Captures the cursor with a click and lets go of the button again.
    void capture(HostInputMapper &mapper)
    {
        mapper.map(pressedNow({"MOUSE_LEFT"}), 1.0);
        mapper.map(RawHostInput{}, 1.0);
    }
}

void register_host_input_tests()
{
    MiniTest::Case("HostInput", [](TestCase &tc)
                   {
        tc.Run("key names map to host codes and back", [](TestCase &t)
               {
            t.Equals(code("SPACE"), 32, "SPACE");
            t.Equals(code("space"), 32, "names are case-insensitive");
            t.Equals(code("W"), 87, "W");
            t.Equals(code("ESCAPE"), 256, "ESCAPE");
            t.Equals(code("ESC"), 256, "ESC alias");
            t.Equals(code("LEFT_ALT"), 342, "LEFT_ALT");
            t.Equals(code("OPTION"), 342, "OPTION alias");
            t.Equals(code("GRAVE"), 96, "GRAVE");
            t.Equals(code("KP_5"), 325, "KP_5");
            t.Equals(code("MOUSE_LEFT"), kMouseCodeBase, "MOUSE_LEFT");
            t.Equals(code("MOUSE_RIGHT"), kMouseCodeBase + 1, "MOUSE_RIGHT");
            t.Equals(code("NOPE"), -1, "unknown name");
            t.Equals(hostCodeName(256), std::string("ESCAPE"), "canonical name of an aliased code");
            t.Equals(hostCodeName(kMouseCodeBase + 2), std::string("MOUSE_MIDDLE"), "mouse name");
            t.IsTrue(isMouseCode(code("MOUSE_SIDE")), "mouse code");
            t.IsFalse(isMouseCode(code("Q")), "key is not a mouse code");
        });

        tc.Run("default ini parses cleanly to the documented defaults", [](TestCase &t)
               {
            HostInputConfig config;
            std::vector<std::string> warnings;
            config.applyIni(HostInputConfig::defaultIniText(), &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(0), "no warnings in the default file");
            const HostInputConfig d = HostInputConfig::defaults();
            t.Equals(d.buttons[kCross].size(), static_cast<size_t>(2), "cross has two keys");
            t.Equals(d.buttons[kCross][0].code, code("SPACE"), "cross = SPACE");
            t.Equals(d.buttons[kSquare][0].code, code("MOUSE_LEFT"), "square = MOUSE_LEFT");
            t.Equals(d.buttons[kTriangle][0].code, code("MOUSE_RIGHT"), "triangle = MOUSE_RIGHT");
            t.Equals(d.buttons[kCircle][0].code, code("F"), "circle = F");
            t.Equals(d.buttons[kStart][0].code, code("ESCAPE"), "start = ESCAPE");
            t.Equals(d.buttons[kL3].size(), static_cast<size_t>(0), "l3 unbound");
            t.Equals(d.stick[kLeftUp][0].code, code("W"), "left_up = W");
            t.Equals(d.stickModifier[0].code, code("LEFT_ALT"), "modifier = LEFT_ALT");
            t.IsTrue(d.mouseMotion == MouseMotion::RightStick, "mouse drives the right stick");
            t.IsTrue(d.mouseMode == MouseMode::Offset, "offset mode");
            t.IsFalse(d.mouseInvertX, "mouse right looks right");
            t.IsTrue(d.mouseInvertY, "mouse up looks up (stick Y inverted)");
            t.IsTrue(d.capture == CaptureMode::Click, "capture on click");
            t.Equals(d.releaseKeys.size(), static_cast<size_t>(2), "two release keys");
            t.IsTrue(d.gamepadEnabled, "gamepad on");
            t.IsTrue(d.mouseCamera == MouseCamera::Look, "mouse-look by default");
            t.IsTrue(near(d.lookDegreesPerPixel, 0.2f), "look_speed 0.2 deg/px");
            t.IsTrue(near(d.lookUpLimit, 15.0f) && near(d.lookDownLimit, 75.0f), "pitch limits");
            t.IsTrue(d.lookAutoRecentre == 0.0f, "no auto recentre");
            t.IsTrue(d.lookSnap, "snap");
            t.Equals(d.recentreKeys.size(), static_cast<size_t>(2), "two recentre keys");
            t.Equals(d.recentreKeys[0].code, code("R"), "recentre = R");
            t.Equals(d.recentreKeys[1].code, code("MOUSE_MIDDLE"), "and MOUSE_MIDDLE");
        });

        tc.Run("mouse mode keys: look / stick, stick_mode, old mode values", [](TestCase &t)
               {
            std::vector<std::string> warnings;
            HostInputConfig c = configFrom("[mouse]\nmode = stick\nstick_mode = velocity\nlook_speed = 0.5\n"
                                           "look_up_limit = 30\nlook_down_limit = 60\nauto_recentre = 2.5\n"
                                           "look_snap = false\nrecentre = HOME\n",
                                           &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(0), "no warnings");
            t.IsTrue(c.mouseCamera == MouseCamera::Stick, "mode = stick");
            t.IsTrue(c.mouseMode == MouseMode::Velocity, "stick_mode = velocity");
            t.IsTrue(near(c.lookDegreesPerPixel, 0.5f), "look_speed");
            t.IsTrue(near(c.lookUpLimit, 30.0f) && near(c.lookDownLimit, 60.0f), "limits");
            t.IsTrue(near(c.lookAutoRecentre, 2.5f), "auto_recentre");
            t.IsFalse(c.lookSnap, "look_snap");
            t.Equals(c.recentreKeys[0].code, code("HOME"), "recentre");
            // A controls.ini from before mouse-look says "mode = offset": that
            // was the stick filter, so it keeps look as the camera mode.
            c = configFrom("[mouse]\nmode = velocity\n", &warnings);
            t.IsTrue(c.mouseCamera == MouseCamera::Look, "old mode = velocity keeps look");
            t.IsTrue(c.mouseMode == MouseMode::Velocity, "and sets the stick filter");
            warnings.clear();
            configFrom("[mouse]\nmode = orbit\nlook_up_limit = 95\n", &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(2), "bad mode and out-of-range limit reported");
        });

        tc.Run("mouse-look: accumulators, no stick, limits, recentre", [](TestCase &t)
               {
            HostInputMapper mapper;
            capture(mapper);
            mapper.cameraFrame(-26.0f); // the camera runs
            RawHostInput raw;
            raw.mouseDx = 100.0f;
            raw.mouseDy = -50.0f; // mouse up
            PadReport report = mapper.map(raw, 1.0);
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "mouse does not push the right stick");
            t.Equals(report.sticks[kRy], static_cast<uint8_t>(0x80), "nor RY");
            t.IsTrue(mapper.lookYaw() == 0.0f, "motion waits for the camera frame");
            LookFrame f = mapper.cameraFrame(-26.0f);
            t.IsTrue(f.active, "active after motion");
            t.IsTrue(near(f.yaw, 20.0f), "100 px * 0.2 = 20 deg right");
            t.IsTrue(near(f.pitch, 10.0f), "mouse up 50 px = 10 deg up");
            t.IsTrue(near(f.snapYaw, 20.0f) && near(f.snapPitch, 10.0f), "this frame's motion, to turn the camera at once");
            report = mapper.map(RawHostInput{}, 1.0);
            f = mapper.cameraFrame(-26.0f);
            t.IsTrue(f.active && f.snapYaw == 0.0f && f.snapPitch == 0.0f, "still mouse: stays where it was put, nothing to turn");
            t.IsTrue(near(f.yaw, 20.0f) && near(f.pitch, 10.0f), "holds the angles");

            // Yaw goes past 60 and wraps at 180.
            raw = RawHostInput{};
            raw.mouseDx = 900.0f; // +180
            mapper.map(raw, 1.0);
            f = mapper.cameraFrame(-26.0f);
            t.IsTrue(near(f.yaw, -160.0f), "200 deg wraps to -160");
            t.IsTrue(near(f.snapYaw, 180.0f) || near(f.snapYaw, -180.0f), "turned by 180 this frame");

            // Pitch limit: base -26 + pitch <= 15 (up limit), so pitch <= 41.
            raw = RawHostInput{};
            raw.mouseDy = -1000.0f;
            mapper.map(raw, 1.0);
            f = mapper.cameraFrame(-26.0f);
            t.IsTrue(near(f.pitch, 41.0f), "up limit");
            t.IsTrue(near(f.snapPitch, 31.0f), "the turn is what the limit let through");
            raw.mouseDy = 2000.0f;
            mapper.map(raw, 1.0);
            f = mapper.cameraFrame(-26.0f);
            t.IsTrue(near(f.pitch, -49.0f), "down limit: -26 + p >= -75");

            // Recentre key.
            report = mapper.map(pressedNow({"R"}), 1.0);
            t.IsFalse(hasButton(report, kCircle), "R is no pad button");
            f = mapper.cameraFrame(-26.0f);
            t.IsFalse(f.active, "R recentres");
            t.IsTrue(mapper.lookYaw() == 0.0f && mapper.lookPitch() == 0.0f, "accumulators cleared");

            // Middle click recentres too (cursor captured).
            raw = RawHostInput{};
            raw.mouseDx = 50.0f;
            mapper.map(raw, 1.0);
            t.IsTrue(mapper.cameraFrame(-26.0f).active, "moved again");
            mapper.map(pressedNow({"MOUSE_MIDDLE"}), 1.0);
            t.IsFalse(mapper.cameraFrame(-26.0f).active, "MOUSE_MIDDLE recentres");

            // invert_x and sensitivity.
            HostInputMapper inv(configFrom("[mouse]\ninvert_x = true\ninvert_y = false\nsensitivity_x = 2\n"));
            capture(inv);
            inv.cameraFrame(-26.0f);
            raw = RawHostInput{};
            raw.mouseDx = 10.0f;
            raw.mouseDy = -10.0f;
            inv.map(raw, 1.0);
            f = inv.cameraFrame(-26.0f);
            t.IsTrue(near(f.yaw, -4.0f), "invert_x, sensitivity_x 2: 10 px = 4 deg left");
            t.IsTrue(near(f.pitch, -2.0f), "invert_y = false: mouse up looks down");
        });

        tc.Run("mouse-look: camera gaps, auto recentre, gamepad, uncaptured", [](TestCase &t)
               {
            HostInputMapper mapper(configFrom("[mouse]\nauto_recentre = 1\n"));
            capture(mapper);
            mapper.cameraFrame(-26.0f);
            RawHostInput raw;
            raw.mouseDx = 50.0f;
            mapper.map(raw, 1.0);
            t.IsTrue(near(mapper.cameraFrame(-26.0f).yaw, 10.0f), "10 deg");

            // A pad read with no camera frame since the last one (pause,
            // scripted camera): its motion is dropped.
            mapper.map(RawHostInput{}, 1.0);
            mapper.map(raw, 1.0); // no camera frame between these reads
            mapper.map(RawHostInput{}, 1.0);
            t.IsTrue(near(mapper.cameraFrame(-26.0f).yaw, 10.0f), "motion while the camera did not run is dropped");

            // Paused (camera running, motion not taken): dropped, angle kept.
            mapper.map(raw, 1.0);
            const LookFrame paused = mapper.cameraFrame(-26.0f, HostInputMapper::LookContext::Paused);
            t.IsTrue(paused.active && near(paused.yaw, 10.0f) && paused.snapYaw == 0.0f,
                     "paused: motion dropped, the view keeps its angle");

            // A camera cut / scripted camera recentres.
            HostInputMapper cut;
            capture(cut);
            cut.cameraFrame(-26.0f);
            cut.map(raw, 1.0);
            t.IsTrue(cut.cameraFrame(-26.0f).active, "moved");
            cut.map(raw, 1.0);
            t.IsFalse(cut.cameraFrame(-26.0f, HostInputMapper::LookContext::Cut).active, "cut: recentred");
            t.IsTrue(cut.lookYaw() == 0.0f, "accumulator cleared, motion dropped");

            // Auto recentre after 1 s (59.94 vsyncs) without motion.
            for (int i = 0; i < 58; ++i)
            {
                mapper.map(RawHostInput{}, 1.0);
                mapper.cameraFrame(-26.0f);
            }
            t.IsTrue(near(mapper.lookYaw(), 10.0f), "still held before the timeout");
            for (int i = 0; i < 3; ++i)
            {
                mapper.map(RawHostInput{}, 1.0);
                mapper.cameraFrame(-26.0f);
            }
            t.IsTrue(mapper.lookYaw() == 0.0f, "recentred after auto_recentre seconds");

            // The camera stops running for kLookResetReads pad reads: recentre.
            mapper.map(raw, 1.0);
            mapper.cameraFrame(-26.0f);
            t.IsTrue(mapper.lookYaw() != 0.0f, "moved");
            mapper.map(RawHostInput{}, 1.0); // covers the frame above
            for (int i = 0; i < HostInputMapper::kLookResetReads - 1; ++i)
                mapper.map(RawHostInput{}, 1.0);
            t.IsTrue(mapper.lookYaw() != 0.0f, "held through a short gap");
            mapper.map(RawHostInput{}, 1.0);
            t.IsTrue(mapper.lookYaw() == 0.0f, "recentred after a long camera gap");

            // A pushed gamepad right stick takes over and recentres.
            mapper.cameraFrame(-26.0f);
            mapper.map(raw, 1.0);
            t.IsTrue(mapper.cameraFrame(-26.0f).active, "moved");
            RawHostInput pad;
            pad.gamepad.present = true;
            pad.gamepad.axes[kRx] = 0.9f;
            PadReport report = mapper.map(pad, 1.0);
            t.IsTrue(report.sticks[kRx] > 0xF0, "gamepad pushes RX");
            t.IsFalse(mapper.cameraFrame(-26.0f).active, "gamepad stick: mouse-look off");

            // Not captured: motion ignored.
            HostInputMapper loose;
            loose.cameraFrame(-26.0f);
            loose.map(raw, 1.0);
            t.IsFalse(loose.cameraFrame(-26.0f).active, "no motion before the cursor is captured");

            // Stick mode never activates mouse-look.
            HostInputMapper stick(stickMode());
            capture(stick);
            stick.cameraFrame(-26.0f);
            report = stick.map(raw, 1.0);
            t.IsTrue(report.sticks[kRx] > 0x80, "stick mode pushes RX");
            t.IsFalse(stick.cameraFrame(-26.0f).active, "stick mode: no mouse-look");
        });

        tc.Run("orbitCameraOffset: yaw turns the view right, pitch tilts it, limits hold", [](TestCase &t)
               {
            // PS2 world: y points down, so up = (0, -1, 0). Skater facing +x,
            // camera 4 behind and 1.97 up: offset (-4, -1.97, 0).
            const Vec3 up{0.0f, -1.0f, 0.0f};
            const Vec3 base{-4.0f, -1.97f, 0.0f};
            const float basePitch = viewPitchOfOffset(base, up);
            t.IsTrue(near(basePitch, -26.22f, 0.01f), "base view pitch about -26 (looking down)");

            Vec3 o = base;
            t.IsTrue(orbitCameraOffset(o, up, 0.0f, 0.0f, 20.0f, 75.0f), "identity orbit");
            t.IsTrue(near(o.x, base.x) && near(o.y, base.y) && near(o.z, base.z), "zero yaw/pitch keeps the offset");

            // View right: facing +x, right is -z (RIGHT turns from +x to -z),
            // so the view direction gets a -z part and the camera moves to +z.
            o = base;
            orbitCameraOffset(o, up, 90.0f, 0.0f, 20.0f, 75.0f);
            t.IsTrue(near(o.x, 0.0f) && near(o.z, 4.0f) && near(o.y, -1.97f), "yaw 90: camera on the skater's left");
            o = base;
            orbitCameraOffset(o, up, 180.0f, 0.0f, 20.0f, 75.0f);
            t.IsTrue(near(o.x, 4.0f) && near(o.z, 0.0f), "yaw 180: camera in front");

            const float len = std::sqrt(4.0f * 4.0f + 1.97f * 1.97f);
            o = base;
            orbitCameraOffset(o, up, 0.0f, 26.22f, 20.0f, 75.0f);
            t.IsTrue(near(o.y, 0.0f, 0.01f) && near(o.x, -len, 0.01f), "pitch up 26: camera level with the look-at");
            o = base;
            orbitCameraOffset(o, up, 90.0f, 26.22f, 20.0f, 75.0f);
            t.IsTrue(near(o.y, 0.0f, 0.01f) && near(o.z, len, 0.01f), "pitch means the same at any yaw");
            o = base;
            orbitCameraOffset(o, up, 0.0f, 80.0f, 20.0f, 75.0f);
            t.IsTrue(near(viewPitchOfOffset(o, up), 20.0f, 0.01f), "up limit");
            o = base;
            orbitCameraOffset(o, up, 0.0f, -80.0f, 20.0f, 75.0f);
            t.IsTrue(near(viewPitchOfOffset(o, up), -75.0f, 0.01f), "down limit");
            const Vec3 steep{-0.5f, -3.0f, 0.0f}; // already below the down limit
            o = steep;
            orbitCameraOffset(o, up, 30.0f, 0.0f, 20.0f, 75.0f);
            t.IsTrue(near(viewPitchOfOffset(o, up), viewPitchOfOffset(steep, up)), "a yaw alone never moves the pitch");

            // Tilted skater (wall ride, ramp): up along +x.
            o = Vec3{0.0f, 0.0f, -4.0f};
            orbitCameraOffset(o, Vec3{1.0f, 0.0f, 0.0f}, 90.0f, 0.0f, 20.0f, 75.0f);
            t.IsTrue(near(o.x, 0.0f) && near(std::fabs(o.y), 4.0f) && near(o.z, 0.0f), "orbits about the skater's own up");
            o = Vec3{0.0f, -2.0f, 0.0f};
            t.IsFalse(orbitCameraOffset(o, up, 30.0f, 0.0f, 20.0f, 75.0f), "offset along up: degenerate, unchanged");
            t.IsTrue(o.y == -2.0f, "unchanged");
        });

        tc.Run("limitCameraDrop pulls a low camera in", [](TestCase &t)
               {
            const Vec3 up{0.0f, -1.0f, 0.0f};
            Vec3 o{-4.0f, 1.0f, 0.0f}; // 1.0 below the target
            t.IsTrue(limitCameraDrop(o, up, 0.5f, 1.5f, 0.0f), "pulled in");
            t.IsTrue(near(o.y, 0.5f) && near(o.x, -2.0f), "drop 0.5, same direction");
            o = Vec3{-4.0f, 1.0f, 0.0f};
            limitCameraDrop(o, up, 0.1f, 1.5f, 0.0f);
            t.IsTrue(near(std::sqrt(o.x * o.x + o.y * o.y), 1.5f), "no nearer than minLength");
            o = Vec3{-4.0f, 1.0f, 0.0f};
            t.IsFalse(limitCameraDrop(o, up, 0.5f, 1.5f, 1.0f), "a drop the game's camera had is kept");
            o = Vec3{-4.0f, -1.97f, 0.0f};
            t.IsFalse(limitCameraDrop(o, up, 0.5f, 1.5f, 0.0f), "camera above the target: unchanged");
        });

        tc.Run("lookRotation turns the current camera like the orbit", [](TestCase &t)
               {
            const Vec3 up{0.0f, -1.0f, 0.0f};
            const Vec3 base{-4.0f, -1.97f, 0.5f};
            for (float yaw : {-150.0f, -30.0f, 5.0f, 90.0f, 179.0f})
                for (float pitch : {-20.0f, 0.0f, 15.0f})
                {
                    Vec3 orbit = base;
                    orbitCameraOffset(orbit, up, yaw, pitch, 89.0f, 89.0f);
                    const Vec3 turned = quatRotate(lookRotation(base, up, yaw, pitch), base);
                    t.IsTrue(near(orbit.x, turned.x) && near(orbit.y, turned.y) && near(orbit.z, turned.z),
                             "rigid turn = orbit (yaw " + std::to_string(yaw) + ", pitch " + std::to_string(pitch) + ")");
                }
            // A camera orientation turned with it: its view axis (row 2 of the
            // game's matrix = q * (0,0,1)) still points at the target.
            const Vec3 view{4.0f, 1.97f, -0.5f};
            const float len = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
            // Orientation whose z axis is the view direction: rotate z onto it.
            const Vec3 z{0.0f, 0.0f, 1.0f};
            const Vec3 d{view.x / len, view.y / len, view.z / len};
            const Vec3 axis{z.y * d.z - z.z * d.y, z.z * d.x - z.x * d.z, z.x * d.y - z.y * d.x};
            const float al = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            const float angle = std::acos(d.z);
            const float s = std::sin(angle / 2.0f) / al;
            const Quat q{axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2.0f)};
            const Vec3 v0 = quatRotate(q, z);
            t.IsTrue(near(v0.x, d.x) && near(v0.y, d.y) && near(v0.z, d.z), "test orientation looks at the target");
            const Quat r = lookRotation(base, up, 70.0f, 10.0f);
            const Vec3 v1 = quatRotate(quatMultiply(r, q), z);
            const Vec3 o1 = quatRotate(r, base);
            t.IsTrue(near(v1.x, -o1.x / len) && near(v1.y, -o1.y / len) && near(v1.z, -o1.z / len),
                     "turned orientation still looks from the turned eye at the target");
            const Quat id = lookRotation(Vec3{0.0f, -3.0f, 0.0f}, up, 30.0f, 0.0f);
            t.IsTrue(id.w == 1.0f && id.x == 0.0f, "degenerate offset: identity");
        });

        tc.Run("ini rebinds, unbinds, keeps absent keys and reports mistakes", [](TestCase &t)
               {
            std::vector<std::string> warnings;
            const HostInputConfig config = configFrom(
                "[buttons]\n"
                "cross = J, mouse_middle   # comment\n"
                "square =\n"
                "r2 = X*0.5\n"
                "l1 = NOSUCHKEY, Q\n"
                "bogus = A\n"
                "[sticks]\n"
                "right_left = KP_4\n"
                "modifier_amount = 0.25\n"
                "[mouse]\n"
                "mode = velocity\n"
                "sensitivity = 2.5\n"
                "invert_y = yes\n"
                "decay = 1.5\n"
                "capture = never\n"
                "[weird]\n"
                "x = y\n",
                &warnings);
            t.Equals(config.buttons[kCross].size(), static_cast<size_t>(2), "cross replaced");
            t.Equals(config.buttons[kCross][0].code, code("J"), "cross = J");
            t.Equals(config.buttons[kCross][1].code, code("MOUSE_MIDDLE"), "and MOUSE_MIDDLE");
            t.Equals(config.buttons[kSquare].size(), static_cast<size_t>(0), "square unbound");
            t.Equals(config.buttons[kR2][0].code, code("X"), "r2 = X");
            t.IsTrue(std::fabs(config.buttons[kR2][0].value - 0.5f) < 1e-6f, "r2 at half pressure");
            t.Equals(config.buttons[kL1].size(), static_cast<size_t>(1), "bad name skipped, good kept");
            t.Equals(config.buttons[kTriangle][0].code, code("MOUSE_RIGHT"), "absent key keeps its default");
            t.Equals(config.stick[kRightLeft][0].code, code("KP_4"), "right_left = KP_4");
            t.IsTrue(std::fabs(config.stickModifierAmount - 0.25f) < 1e-6f, "modifier amount");
            t.IsTrue(config.mouseMode == MouseMode::Velocity, "velocity mode");
            t.IsTrue(std::fabs(config.mouseSensitivityX - 2.5f) < 1e-6f, "sensitivity sets x");
            t.IsTrue(std::fabs(config.mouseSensitivityY - 2.5f) < 1e-6f, "sensitivity sets y");
            t.IsTrue(config.mouseInvertY, "invert_y");
            t.IsTrue(std::fabs(config.mouseDecay - 0.85f) < 1e-6f, "out-of-range decay keeps the default");
            t.IsTrue(config.capture == CaptureMode::Never, "capture never");
            // NOSUCHKEY, bogus, decay range, [weird] section
            t.Equals(warnings.size(), static_cast<size_t>(4), "four warnings");
        });

        tc.Run("keys press buttons with pressure and push sticks", [](TestCase &t)
               {
            HostInputMapper mapper(configFrom("[buttons]\nr2 = C*0.5\n"));
            PadReport report = mapper.map(held({"SPACE", "Q", "C", "W", "D"}), 1.0);
            t.IsTrue(hasButton(report, kCross), "SPACE = cross");
            t.IsTrue(hasButton(report, kL1), "Q = L1");
            t.IsTrue(hasButton(report, kR2), "C = R2");
            t.IsFalse(hasButton(report, kSquare), "square not pressed");
            t.Equals(report.pressure[kCross], static_cast<uint8_t>(0xFF), "full pressure by default");
            t.Equals(report.pressure[kR2], static_cast<uint8_t>(128), "half pressure");
            t.Equals(report.pressure[kSquare], static_cast<uint8_t>(0), "released button has no pressure");
            t.Equals(report.sticks[kLy], stickByte(-1.0f), "W pushes the left stick up");
            t.Equals(report.sticks[kLx], stickByte(1.0f), "D pushes it right");
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "right stick centred");

            report = mapper.map(held({"W", "LEFT_ALT"}), 1.0);
            t.Equals(report.sticks[kLy], stickByte(-0.5f), "modifier: half push");
            report = mapper.map(held({"A", "D"}), 1.0);
            t.Equals(report.sticks[kLx], static_cast<uint8_t>(0x80), "opposite keys cancel");
            report = mapper.map(RawHostInput{}, 1.0);
            t.Equals(report.buttons, static_cast<uint16_t>(0), "nothing held, nothing pressed");
            t.Equals(report.sticks[kLx], static_cast<uint8_t>(0x80), "centred");
            t.Equals(stickByte(-1.0f), static_cast<uint8_t>(1), "stick byte as ZX_PAD_SCRIPT (-1)");
            t.Equals(stickByte(1.0f), static_cast<uint8_t>(255), "stick byte (+1)");
            t.Equals(stickByte(0.0f), static_cast<uint8_t>(0x80), "stick byte (0)");
        });

        tc.Run("focus loss releases keys and the cursor", [](TestCase &t)
               {
            HostInputMapper mapper;
            capture(mapper);
            t.IsTrue(mapper.captured(), "captured after a click");
            RawHostInput raw = held({"SPACE", "W", "MOUSE_RIGHT"});
            raw.mouseDx = 50.0f;
            PadReport report = mapper.map(raw, 1.0);
            t.IsTrue(hasButton(report, kCross), "focused: cross");
            t.IsTrue(hasButton(report, kTriangle), "focused and captured: mouse button");
            raw.focused = false;
            report = mapper.map(raw, 1.0);
            t.Equals(report.buttons, static_cast<uint16_t>(0), "unfocused: no buttons");
            t.Equals(report.sticks[kLy], static_cast<uint8_t>(0x80), "unfocused: stick centred");
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "unfocused: mouse ignored");
            t.IsFalse(mapper.captured(), "unfocused: cursor released");
            raw.focused = true;
            report = mapper.map(raw, 1.0);
            t.IsTrue(hasButton(report, kCross), "focus back: keys count again");
            t.IsFalse(hasButton(report, kTriangle), "focus back: mouse needs a new capture click");
        });

        tc.Run("capture on click swallows the click; release keys free the cursor", [](TestCase &t)
               {
            HostInputMapper mapper;
            RawHostInput raw;
            raw.mouseDx = 300.0f;
            PadReport report = mapper.map(raw, 1.0);
            t.IsFalse(mapper.captured(), "not captured at start");
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "motion ignored before capture");

            report = mapper.map(pressedNow({"MOUSE_LEFT"}), 1.0);
            t.IsTrue(mapper.captured(), "click captures");
            t.IsFalse(hasButton(report, kSquare), "capturing click is not square");
            report = mapper.map(held({"MOUSE_LEFT"}), 1.0);
            t.IsFalse(hasButton(report, kSquare), "still held: still swallowed");
            report = mapper.map(RawHostInput{}, 1.0);
            report = mapper.map(pressedNow({"MOUSE_LEFT"}), 1.0);
            t.IsTrue(hasButton(report, kSquare), "next click is square");

            report = mapper.map(pressedNow({"ESCAPE"}), 1.0);
            t.IsFalse(mapper.captured(), "ESCAPE releases");
            t.IsTrue(hasButton(report, kStart), "and is still Start");
            report = mapper.map(pressedNow({"MOUSE_RIGHT"}), 1.0);
            t.IsTrue(mapper.captured(), "click recaptures");
            t.IsFalse(hasButton(report, kTriangle), "recapturing click swallowed");
            mapper.map(RawHostInput{}, 1.0);
            mapper.map(pressedNow({"GRAVE"}), 1.0);
            t.IsFalse(mapper.captured(), "GRAVE releases");

            HostInputMapper never(configFrom("[mouse]\ncapture = never\n"));
            report = never.map(pressedNow({"MOUSE_LEFT"}), 1.0);
            t.IsFalse(never.captured(), "never captures");
            t.IsTrue(hasButton(report, kSquare), "never: mouse buttons work at once");

            HostInputMapper always(configFrom("[mouse]\ncapture = always\n"));
            always.map(RawHostInput{}, 1.0);
            t.IsTrue(always.captured(), "always: captured when focused");
            always.map(pressedNow({"GRAVE"}), 1.0);
            always.map(RawHostInput{}, 1.0);
            t.IsFalse(always.captured(), "always: a release key frees it until a click");
            RawHostInput away;
            away.focused = false;
            always.map(away, 1.0);
            always.map(RawHostInput{}, 1.0);
            t.IsTrue(always.captured(), "always: captured again after refocus");
        });

        tc.Run("mouse offset mode pushes the right stick and springs back", [](TestCase &t)
               {
            HostInputMapper mapper(stickMode());
            capture(mapper);
            RawHostInput raw;
            raw.mouseDx = 100.0f; // half of the 200 px to full
            PadReport report = mapper.map(raw, 1.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 0.5f) < 1e-6f, "100 px = half push");
            const float expected = 0.27f + 0.73f * 0.5f;
            t.Equals(report.sticks[kRx], stickByte(expected), "dead zone mapped output");
            t.Equals(report.sticks[kRy], static_cast<uint8_t>(0x80), "no vertical motion");

            raw.mouseDx = 1.0f;
            mapper.map(raw, 1.0);
            HostInputMapper small(stickMode());
            capture(small);
            report = small.map(raw, 1.0);
            t.IsTrue(std::abs(static_cast<int>(report.sticks[kRx]) - 127) > 32,
                     "one pixel already gets past the game's stick dead zone");

            raw.mouseDx = 1000.0f;
            mapper.map(raw, 1.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 1.0f) < 1e-6f, "clamped at full");

            // Still mouse: 0.85 per vsync.
            report = mapper.map(RawHostInput{}, 2.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 0.7225f) < 1e-4f, "decays per vsync");
            for (int i = 0; i < 60; ++i)
                report = mapper.map(RawHostInput{}, 1.0);
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "back to centre");

            HostInputMapper inverted(stickMode("invert_x = true\ninvert_y = false\nsensitivity_y = 2\n"));
            capture(inverted);
            raw.mouseDx = 100.0f;
            raw.mouseDy = 50.0f;
            inverted.map(raw, 1.0);
            t.IsTrue(std::fabs(inverted.mouseStickX() + 0.5f) < 1e-6f, "invert_x");
            t.IsTrue(std::fabs(inverted.mouseStickY() - 0.5f) < 1e-6f, "invert_y = false: 50 px down at sensitivity_y 2 = half push down");

            HostInputMapper defaults(stickMode());
            capture(defaults);
            raw.mouseDx = 0.0f;
            raw.mouseDy = -100.0f;
            defaults.map(raw, 1.0);
            t.IsTrue(std::fabs(defaults.mouseStickY() - 0.5f) < 1e-6f, "default: mouse up pushes the stick down (look up)");
            raw.mouseDx = 100.0f;

            HostInputMapper left(stickMode("motion = left_stick\n"));
            capture(left);
            raw.mouseDx = 200.0f;
            raw.mouseDy = 0.0f;
            report = left.map(raw, 1.0);
            t.Equals(report.sticks[kLx], stickByte(1.0f), "motion = left_stick");
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "right stick untouched");

            HostInputMapper none(stickMode("motion = none\n"));
            capture(none);
            report = none.map(raw, 1.0);
            t.Equals(report.sticks[kRx], static_cast<uint8_t>(0x80), "motion = none");
        });

        tc.Run("mouse velocity mode follows speed", [](TestCase &t)
               {
            HostInputMapper mapper(stickMode("stick_mode = velocity\n"));
            capture(mapper);
            RawHostInput raw;
            raw.mouseDx = 25.0f; // 25 px in one vsync = full
            mapper.map(raw, 1.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 1.0f) < 1e-6f, "25 px per vsync = full");
            raw.mouseDx = 25.0f;
            mapper.map(raw, 4.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 0.25f) < 1e-6f, "same pixels over 4 vsyncs = quarter");
            mapper.map(RawHostInput{}, 1.0);
            t.IsTrue(std::fabs(mapper.mouseStickX() - 0.2125f) < 1e-5f, "decays when still");
        });

        tc.Run("gamepad buttons add and a pushed gamepad stick wins", [](TestCase &t)
               {
            HostInputMapper mapper;
            RawHostInput raw = held({"W", "SPACE"});
            raw.gamepad.present = true;
            raw.gamepad.buttons = static_cast<uint16_t>(1u << kTriangle);
            raw.gamepad.axes[kLx] = 0.1f; // inside the threshold
            PadReport report = mapper.map(raw, 1.0);
            t.IsTrue(hasButton(report, kCross), "keyboard cross");
            t.IsTrue(hasButton(report, kTriangle), "gamepad triangle");
            t.Equals(report.pressure[kTriangle], static_cast<uint8_t>(0xFF), "gamepad pressure full");
            t.Equals(report.sticks[kLy], stickByte(-1.0f), "resting gamepad stick: keys drive it");

            raw.gamepad.axes[kLx] = -1.0f;
            raw.gamepad.axes[kLy] = 0.0f;
            report = mapper.map(raw, 1.0);
            t.Equals(report.sticks[kLx], static_cast<uint8_t>(1), "pushed gamepad stick wins (x)");
            t.Equals(report.sticks[kLy], static_cast<uint8_t>(128), "and its other axis too");

            HostInputMapper off(configFrom("[gamepad]\nenabled = false\n"));
            report = off.map(raw, 1.0);
            t.IsFalse(hasButton(report, kTriangle), "gamepad disabled");

            raw.focused = false;
            report = mapper.map(raw, 1.0);
            t.IsTrue(hasButton(report, kTriangle), "gamepad still works without focus");
            t.IsFalse(hasButton(report, kCross), "keyboard does not");
        });

        tc.Run("host input script parses and samples on the script clock", [](TestCase &t)
               {
            HostInputScript script;
            std::vector<std::string> warnings;
            script.parse("10v:KEY=W:20v, 1:KEY=SPACE, 40v:MOUSE=100/-50, 50v:MOUSE=60/0:30v, 70v:FOCUS=0, "
                         "80v:FOCUS=1, 5v:KEY=NOPE, junk, 9v:MOUSE=1",
                         &warnings);
            t.Equals(script.keys().size(), static_cast<size_t>(2), "two keys");
            t.Equals(script.motions().size(), static_cast<size_t>(2), "two motions");
            t.Equals(script.focus().size(), static_cast<size_t>(2), "two focus changes");
            t.Equals(warnings.size(), static_cast<size_t>(3), "three bad entries");
            t.IsTrue(std::fabs(script.keys()[1].start - 59.94) < 1e-9, "seconds convert to vsyncs");
            t.IsTrue(std::fabs(script.keys()[1].end - 59.94 * 1.25) < 1e-9, "default duration 0.25 s");

            RawHostInput raw = script.sample(-1.0, 5.0);
            t.IsFalse(raw.down.test(static_cast<size_t>(code("W"))), "W not yet");
            raw = script.sample(5.0, 10.0);
            t.IsTrue(raw.down.test(static_cast<size_t>(code("W"))), "W from 10v");
            t.IsTrue(raw.pressed.test(static_cast<size_t>(code("W"))), "W pressed edge");
            raw = script.sample(10.0, 29.0);
            t.IsTrue(raw.down.test(static_cast<size_t>(code("W"))), "W held");
            t.IsFalse(raw.pressed.test(static_cast<size_t>(code("W"))), "no new edge");
            raw = script.sample(29.0, 30.0);
            t.IsFalse(raw.down.test(static_cast<size_t>(code("W"))), "W released at 30v");

            HostInputScript tap;
            tap.parse("12v:KEY=SPACE:1v");
            raw = tap.sample(10.0, 20.0);
            t.IsTrue(raw.down.test(static_cast<size_t>(code("SPACE"))), "tap inside one sample still counts");

            raw = script.sample(39.0, 40.0);
            t.IsTrue(raw.mouseDx == 100.0f && raw.mouseDy == -50.0f, "instant motion");
            raw = script.sample(40.0, 41.0);
            t.IsTrue(raw.mouseDx == 0.0f, "instant motion only once");
            raw = script.sample(50.0, 60.0);
            t.IsTrue(std::fabs(raw.mouseDx - 20.0f) < 1e-4f, "spread motion: a third in 10 of 30 vsyncs");
            raw = script.sample(60.0, 200.0);
            t.IsTrue(std::fabs(raw.mouseDx - 40.0f) < 1e-4f, "rest of the spread motion");

            t.IsTrue(script.sample(60.0, 69.0).focused, "focused before 70v");
            t.IsFalse(script.sample(69.0, 75.0).focused, "unfocused from 70v");
            t.IsTrue(script.sample(75.0, 85.0).focused, "focused again from 80v");

            HostInputScript wheel;
            warnings.clear();
            wheel.parse("10v:WHEEL=1, 12v:WHEEL=-2, 14v:WHEEL=x, 16v:WHEEL=1:2v", &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(2), "bad wheel entries");
            t.IsTrue(wheel.sample(-1.0, 10.0).wheel == 1.0f, "a wheel step at 10v");
            t.IsTrue(wheel.sample(10.0, 11.0).wheel == 0.0f, "counted once");
            t.IsTrue(wheel.sample(11.0, 20.0).wheel == -2.0f, "two steps the other way");
        });

        tc.Run("config folder per platform", [](TestCase &t)
               {
            const std::string dir = userConfigDir("Aggressive Inline");
            t.IsFalse(dir.empty(), "home is known");
#if defined(__APPLE__)
            t.IsTrue(dir.find("/Library/Application Support/Aggressive Inline") != std::string::npos, "macOS path");
#endif
        });

        tc.Run("gamepad slots: connect order, a pad keeps its slot, ids are idempotent", [](TestCase &t)
               {
            GamepadSlots slots;
            t.Equals(slots.connect(11u), 0, "first pad is player 1");
            t.Equals(slots.connect(22u), 1, "second pad is player 2");
            t.Equals(slots.connect(33u), 2, "third pad is a spare");
            t.Equals(slots.connect(22u), 1, "a pad reported again keeps its slot");
            t.Equals(slots.count(), 3, "three pads");
            t.Equals(slots.connect(0u), -1, "id 0 is never a pad");
            t.Equals(slots.slotOf(33u), 2, "slotOf");
            t.Equals(slots.idAt(1), 22u, "idAt");
            t.Equals(slots.idAt(5), 0u, "free slot");
            t.Equals(slots.slotOf(99u), -1, "unknown id");
            t.IsFalse(slots.disconnect(99u), "unknown id not removed");
        });

        tc.Run("gamepad slots: removal rules", [](TestCase &t)
               {
            // Player 1's pad goes and one pad remains: it becomes player 1.
            GamepadSlots two;
            two.connect(1u);
            two.connect(2u);
            t.IsTrue(two.disconnect(1u), "removed");
            t.Equals(two.slotOf(2u), 0, "remaining pad moves up to player 1");
            t.Equals(two.connect(1u), 1, "the pad that comes back is player 2");

            // Player 2's pad goes: player 1 keeps theirs.
            GamepadSlots keep;
            keep.connect(1u);
            keep.connect(2u);
            keep.disconnect(2u);
            t.Equals(keep.slotOf(1u), 0, "player 1 unchanged");
            t.Equals(keep.count(), 1, "one pad");
            t.Equals(keep.connect(3u), 1, "a new pad becomes player 2");

            // With a spare, a player who loses their pad gets the spare and
            // the other player keeps theirs.
            GamepadSlots spare;
            spare.connect(1u);
            spare.connect(2u);
            spare.connect(3u);
            spare.connect(4u);
            spare.disconnect(1u);
            t.Equals(spare.slotOf(3u), 0, "first spare takes player 1");
            t.Equals(spare.slotOf(2u), 1, "player 2 keeps their pad");
            t.Equals(spare.slotOf(4u), 2, "the other spare moves up");
            spare.disconnect(2u);
            t.Equals(spare.slotOf(4u), 1, "the next spare takes player 2");
            t.Equals(spare.slotOf(3u), 0, "player 1 unchanged");

            // A spare going away moves nobody else.
            GamepadSlots spareGone;
            spareGone.connect(1u);
            spareGone.connect(2u);
            spareGone.connect(3u);
            spareGone.disconnect(3u);
            t.Equals(spareGone.slotOf(1u), 0, "player 1 unchanged");
            t.Equals(spareGone.slotOf(2u), 1, "player 2 unchanged");

            // The only pad goes and comes back: player 1 again.
            GamepadSlots one;
            one.connect(5u);
            one.disconnect(5u);
            t.Equals(one.count(), 0, "none");
            t.Equals(one.connect(6u), 0, "next pad is player 1");

            // Occupied slots are always 0..n-1.
            GamepadSlots many;
            for (uint32_t id = 1u; id <= 6u; ++id)
                many.connect(id);
            const uint32_t removals[] = {2u, 1u, 6u, 3u, 5u};
            for (uint32_t id : removals)
            {
                many.disconnect(id);
                for (int slot = 0; slot < many.count(); ++slot)
                    t.IsTrue(many.idAt(slot) != 0u, "no hole in the slots");
            }
            t.Equals(many.count(), 1, "one pad left");
            t.Equals(many.slotOf(4u), 0, "the last pad is player 1");
        });

        tc.Run("gamepad slots: swap players", [](TestCase &t)
               {
            GamepadSlots slots;
            slots.connect(1u);
            slots.swapPlayers();
            t.Equals(slots.slotOf(1u), 0, "one pad: nothing to swap");
            slots.connect(2u);
            slots.connect(3u);
            slots.swapPlayers();
            t.Equals(slots.slotOf(2u), 0, "pad 2 is player 1");
            t.Equals(slots.slotOf(1u), 1, "pad 1 is player 2");
            t.Equals(slots.slotOf(3u), 2, "spare unchanged");
        });

        tc.Run("port routing: port 1 has its own gamepad only with two pads, else no pad", [](TestCase &t)
               {
            PortSource p0 = routePort(0, 0, true);
            t.IsTrue(p0.connected, "port 0 always has a pad");
            t.IsTrue(p0.keyboardMouse, "port 0: keyboard and mouse");
            t.Equals(p0.gamepadSlot, -1, "port 0: no gamepad");
            t.IsFalse(p0.mirrorPort0, "port 0 is not a mirror");
            PortSource p1 = routePort(1, 0, true);
            t.IsFalse(p1.connected, "no gamepad: port 1 is empty");
            t.IsFalse(p1.mirrorPort0, "no gamepad: port 1 is not a copy of port 0");
            t.IsFalse(p1.keyboardMouse, "port 1 has no keyboard of its own");

            t.Equals(routePort(0, 1, true).gamepadSlot, 0, "one pad: port 0 reads it");
            t.IsFalse(routePort(1, 1, true).connected, "one pad: port 1 is empty (as a PS2 with one controller)");

            t.Equals(routePort(0, 2, true).gamepadSlot, 0, "two pads: port 0 reads slot 0");
            p1 = routePort(1, 2, true);
            t.IsTrue(p1.connected, "two pads: port 1 has a pad");
            t.Equals(p1.gamepadSlot, 1, "two pads: port 1 reads slot 1");
            t.IsFalse(p1.mirrorPort0, "two pads: port 1 is not a mirror");
            t.Equals(routePort(1, 3, true).gamepadSlot, 1, "spares do not change port 1");

            t.Equals(routePort(0, 2, false).gamepadSlot, -1, "gamepads off: port 0 has no gamepad");
            t.IsTrue(routePort(0, 2, false).keyboardMouse, "gamepads off: keyboard still on port 0");
            t.IsFalse(routePort(1, 2, false).connected, "gamepads off: port 1 is empty");
            t.IsFalse(routePort(2, 2, true).connected, "ports past 1 are empty (no multitap)");

            // ZX_PAD_PORT1=mirror: the old routing.
            p1 = routePort(1, 1, true, true);
            t.IsTrue(p1.connected && p1.mirrorPort0, "mirror: one pad, port 1 copies port 0");
            t.IsTrue(routePort(1, 0, false, true).mirrorPort0, "mirror: gamepads off, port 1 copies port 0");
            p1 = routePort(1, 2, true, true);
            t.IsTrue(p1.connected && p1.gamepadSlot == 1 && !p1.mirrorPort0, "mirror: two pads, port 1 still player 2's");
            t.IsFalse(routePort(2, 2, true, true).connected, "mirror: ports past 1 stay empty");
        });

        tc.Run("applyGamepad: the gamepad part of the mapping, alone (port 1)", [](TestCase &t)
               {
            HostInputConfig config = configFrom("[gamepad]\ncross = EAST\ncircle = SOUTH\nstick_threshold = 0.3\n");
            GamepadState pad;
            pad.present = true;
            pad.buttons = static_cast<uint16_t>((1u << kCross) | (1u << kStart)); // SOUTH and START
            pad.axes[kLx] = 0.25f; // inside the threshold
            pad.axes[kRy] = -1.0f;
            PadReport report;
            const bool right = applyGamepad(config, pad, report);
            t.IsTrue(right, "right stick pushed");
            t.IsTrue(hasButton(report, kCircle), "SOUTH presses circle (remapped)");
            t.IsFalse(hasButton(report, kCross), "SOUTH no longer presses cross");
            t.IsTrue(hasButton(report, kStart), "START");
            t.Equals(report.pressure[kCircle], static_cast<uint8_t>(0xFF), "full pressure");
            t.Equals(report.sticks[kLx], static_cast<uint8_t>(0x80), "left stick inside the dead zone");
            t.Equals(report.sticks[kRy], static_cast<uint8_t>(1), "right stick up");

            // The same as the port-0 mapper's gamepad part with no keys.
            RawHostInput raw;
            raw.gamepad = pad;
            HostInputMapper mapper(config);
            const PadReport mapped = mapper.map(raw, 1.0);
            t.Equals(mapped.buttons, report.buttons, "same buttons as the mapper");
            t.IsTrue(mapped.sticks == report.sticks, "same sticks as the mapper");
            t.IsTrue(mapped.pressure == report.pressure, "same pressure as the mapper");

            PadReport none;
            t.IsFalse(applyGamepad(configFrom("[gamepad]\nenabled = false\n"), pad, none), "gamepads off");
            t.Equals(none.buttons, static_cast<uint16_t>(0), "gamepads off: nothing pressed");
            GamepadState absent;
            absent.buttons = 0xFFFFu;
            t.IsFalse(applyGamepad(config, absent, none), "absent pad");
            t.Equals(none.buttons, static_cast<uint16_t>(0), "absent pad: nothing pressed");
        });

        tc.Run("pad backend writes the default controls.ini and reports an idle pad; port 1 is empty without a second gamepad", [](TestCase &t)
               {
            const std::filesystem::path path =
                std::filesystem::temp_directory_path() / ("zx-host-input-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / "controls.ini";
            std::error_code ec;
            std::filesystem::remove(path, ec);
#if defined(_WIN32)
            _putenv_s("ZX_CONTROLS_INI", path.string().c_str());
#else
            setenv("ZX_CONTROLS_INI", path.string().c_str(), 1);
#endif
            PSPadBackend backend;
            uint8_t data[32]{};
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState");
            t.Equals(data[2], static_cast<uint8_t>(0xFF), "no buttons (low)");
            t.Equals(data[3], static_cast<uint8_t>(0xFF), "no buttons (high)");
            for (int i = 4; i < 8; ++i)
                t.Equals(data[i], static_cast<uint8_t>(0x80), "sticks centred");
            for (int i = 8; i < 20; ++i)
                t.Equals(data[i], static_cast<uint8_t>(0), "no pressure");
            t.IsTrue(std::filesystem::exists(path), "controls.ini created");
            std::ifstream in(path);
            std::stringstream text;
            text << in.rdbuf();
            t.Equals(text.str(), HostInputConfig::defaultIniText(), "with the documented defaults");

            // Without gamepads port 1 has no pad (the stubs report it
            // DISCONNECTED) and, read anyway, sees none of port 0's input
            // (here, inside this test: the backend loads its controls once
            // per process, from the temporary file above).
            t.IsTrue(PSPadBackend::portConnected(0), "port 0 has a pad");
            t.IsFalse(PSPadBackend::portConnected(1), "no second gamepad: port 1 is empty");
            t.IsFalse(PSPadBackend::portConnected(2), "port 2 is empty");
            PSPadBackend::pressButton(kTriangle, 1);
            uint8_t port0[32]{};
            uint8_t port1[32]{};
            t.IsTrue(backend.readState(0, 0, port0, sizeof(port0)), "port 0");
            t.IsTrue(backend.readState(1, 0, port1, sizeof(port1)), "port 1");
            t.Equals(port0[3] & 0x10, 0, "port 0: triangle pressed (active low)");
            t.Equals(port1[2], static_cast<uint8_t>(0xFF), "port 1: nothing pressed (low)");
            t.Equals(port1[3], static_cast<uint8_t>(0xFF), "port 1: nothing pressed (high), not port 0's triangle");
            std::filesystem::remove_all(path.parent_path(), ec);
        });

        tc.Run("game controls: each game's own defaults and action names", [](TestCase &t)
               {
            const std::vector<const GameControls *> games = allGameControls();
            t.Equals(games.size(), static_cast<size_t>(3u), "three games");
            t.Equals(gameControls("ai").id, std::string("ai"), "ai");
            t.Equals(gameControls("dm2").title, std::string("Dave Mirra Freestyle BMX 2"), "dm2");
            t.Equals(gameControls("bmx").title, std::string("BMX XXX"), "bmx");
            t.Equals(gameControls("nope").id, std::string("ai"), "an unknown id: Aggressive Inline's");
            t.Equals(HostInputConfig::defaultIniText(), gameControls("ai").defaultIni, "the old defaults are AI's");

            const std::vector<std::string> keys = {"cross", "square", "triangle", "circle", "l1", "r1", "l2", "r2",
                                                   "start", "select", "up", "down", "left", "right", "l3", "r3",
                                                   "left_up", "left_down", "left_left", "left_right", "right_up",
                                                   "right_down", "right_left", "right_right", "modifier", "recentre",
                                                   "restart", "hide_hud", "screenshot", "photo_mode", "switch_game"};
            for (const GameControls *game : games)
            {
                std::vector<std::string> warnings;
                HostInputConfig config;
                config.applyIni(game->defaultIni, &warnings);
                t.IsTrue(warnings.empty(), game->id + ": its controls.ini parses without warnings");
                t.IsTrue(game->defaultIni.rfind("# " + game->title + " controls", 0) == 0, game->id + ": its own header");
                t.IsTrue(!game->labels.empty(), game->id + ": names its actions");
                for (const auto &[key, label] : game->labels)
                {
                    t.IsTrue(std::find(keys.begin(), keys.end(), key) != keys.end(), game->id + ": a row key: " + key);
                    t.IsTrue(!label.empty() && label.size() <= 20u, game->id + ": a short name for " + key);
                }
                const HostInputConfig d = HostInputConfig::defaults(game->defaultIni);
                t.Equals(formatBindings(d.buttons[kCross]), std::string("SPACE, K"), game->id + ": Cross on Space");
                t.Equals(formatBindings(d.hotkeys[kHotkeyScreenshot]), std::string("F12"), game->id + ": screenshot on F12");
                t.Equals(formatBindings(d.stick[kLeftUp]), std::string("W"), game->id + ": W pushes the left stick up");
            }

            const auto label = [](const GameControls &game, const std::string &key)
            {
                for (const auto &[k, v] : game.labels)
                    if (k == key)
                        return v;
                return std::string();
            };
            t.Equals(label(gameControls("ai"), "cross"), std::string("Jump"), "AI: Cross jumps");
            t.Equals(label(gameControls("dm2"), "cross"), std::string("Start / Jump"), "DM2: the game's caption");
            t.Equals(label(gameControls("dm2"), "circle"), std::string("Modifier"), "DM2: Circle is the modifier");
            t.Equals(label(gameControls("bmx"), "select"), std::string("Challenge List"), "BMX: Select");
            t.Equals(label(gameControls("dm2"), "restart"), std::string("Restart"), "DM2: a Restart key row (the run again)");
            t.Equals(label(gameControls("bmx"), "photo_mode"), std::string("Photo Mode"), "BMX: a Photo Mode key row");

            const HostInputConfig ai = HostInputConfig::defaults();
            t.Equals(formatBindings(ai.hotkeys[kHotkeyRestart]), std::string("BACKSPACE"), "AI keeps its Restart key");
            const HostInputConfig dm2 = HostInputConfig::defaults(gameControls("dm2").defaultIni);
            t.IsTrue(formatBindings(dm2.hotkeys[kHotkeyRestart]) == "BACKSPACE" &&
                         formatBindings(dm2.hotkeys[kHotkeyHideHud]) == "F9" &&
                         formatBindings(dm2.hotkeys[kHotkeyPhotoMode]) == "P",
                     "DM2: the shortcuts it acts on (Restart, Hide HUD, Photo Mode), AI's keys");
            t.IsTrue(dm2.stick[kRightUp].empty() && dm2.mouseCamera == MouseCamera::Look,
                     "DM2: no right stick keys (the rider reads the left one), mouse-look");
            const HostInputConfig bmx = HostInputConfig::defaults(gameControls("bmx").defaultIni);
            t.IsTrue(bmx.mouseCamera == MouseCamera::Stick && bmx.mouseMotion == MouseMotion::None && bmx.recentreKeys.empty(),
                     "BMX: stick mode with no motion by default, so the mouse moves nothing");
            t.Equals(formatBindings(bmx.buttons[kR3]), std::string("MOUSE_MIDDLE, V"), "BMX: R3 is its camera button");
            t.IsTrue(gameControls("dm2").defaultIni.find("Aggressive") == std::string::npos &&
                         gameControls("bmx").defaultIni.find("skater") == std::string::npos,
                     "no other game's words");
        });

        tc.Run("pad backend: a game's controls.ini starts from that game's defaults", [](TestCase &t)
               {
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path() / ("zx-game-controls-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path path = dir / "controls.ini";
#if defined(_WIN32)
            _putenv_s("ZX_CONTROLS_INI", path.string().c_str());
#else
            setenv("ZX_CONTROLS_INI", path.string().c_str(), 1);
#endif
            PSPadBackend::setGame("dm2");
            PSPadBackend backend;
            uint8_t data[32]{};
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState");
            std::ifstream in(path);
            std::stringstream text;
            text << in.rdbuf();
            t.Equals(text.str(), gameControls("dm2").defaultIni, "DM2's documented defaults written");
            t.Equals(formatBindings(PSPadBackend::hotkeyKeys(kHotkeyRestart)), std::string("BACKSPACE"), "and read: its Restart key");
            PSPadBackend::setGame("ai");
            t.Equals(formatBindings(PSPadBackend::hotkeyKeys(kHotkeyRestart)), std::string("BACKSPACE"),
                     "back to AI: its controls again (from the same file: an absent key keeps AI's default)");
            in.close();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        });
    });
}
