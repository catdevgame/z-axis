// Photo mode's host side, shared by the Z-Axis engine's games (zge_photo.h).

#include "zge_photo.h"

#include "machine.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/session.h"
#include "runtime/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace zge_host::photo
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace ph = zx::photo;
        namespace st = zx::settings;
        using Device = PSPadBackend::InputDevice;

        constexpr uint64_t kHelpFrames = 600u;

        std::string keyName(const std::vector<hi::Binding> &keys, const char *fallback)
        {
            for (const hi::Binding &b : keys)
                if (b.code >= 0)
                    return hi::hostCodeLabel(b.code);
            return fallback;
        }

        // A pad button by position, as the device names it.
        std::string padName(int physical, Device device)
        {
            const bool ps = device == Device::PlayStation, nin = device == Device::Nintendo;
            switch (physical)
            {
            case hi::kCross:
                return ps ? "Cross" : nin ? "B" : "A";
            case hi::kCircle:
                return ps ? "Circle" : nin ? "A" : "B";
            case hi::kSquare:
                return ps ? "Square" : nin ? "Y" : "X";
            case hi::kTriangle:
                return ps ? "Triangle" : nin ? "X" : "Y";
            case hi::kL1:
                return ps ? "L1" : nin ? "L" : "LB";
            case hi::kR1:
                return ps ? "R1" : nin ? "R" : "RB";
            case hi::kL2:
                return ps ? "L2" : nin ? "ZL" : "LT";
            case hi::kR2:
                return ps ? "R2" : nin ? "ZR" : "RT";
            case hi::kL3:
                return ps ? "L3" : "LS";
            case hi::kR3:
                return ps ? "R3" : "RS";
            case hi::kStart:
                return ps ? "Start" : nin ? "+" : "Start";
            case hi::kSelect:
                return ps ? "Select" : nin ? "-" : "Back";
            default:
                return "?";
            }
        }

        float axis(float v)
        {
            constexpr float kDead = 0.2f;
            if (std::fabs(v) < kDead)
                return 0.0f;
            return (v - std::copysign(kDead, v)) / (1.0f - kDead);
        }
    }

    std::string FreeLook::word(const std::string &english) const
    {
        return translate_ ? translate_(english) : english;
    }

    std::string FreeLook::speedText() const
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", camera_.speed);
        return text;
    }

    // The overlay's notice: the help (unless hidden) and a status line.
    void FreeLook::showNotice(bool force)
    {
        if (!force && vsync_ < noticeAt_ + 20u)
            return;
        noticeAt_ = vsync_;
        std::string text;
        const std::string filter = word(ph::presets()[preset_].name);
        const Device device = PSPadBackend::activeDevice();
        if (help_ && !helpAsked_ && vsync_ > startedAt_ + kHelpFrames)
            help_ = false;
        if (!help_)
            text = word("Photo Mode") + "   " + (device == Device::Keyboard ? std::string("H") : padName(hi::kSquare, device)) +
                   ": " + word("Help");
        else
        {
            std::vector<std::pair<std::string, std::string>> rows;
            if (device == Device::Keyboard)
            {
                rows = {
                    {word("Look"), word("Mouse")},
                    {word("Move"), "W A S D"},
                    {word("Up / Down"), "Space / Ctrl"},
                    {word("Roll"), "Q / E"},
                    {word("Level Horizon"), "Q + E"},
                    {word("Zoom"), word("Wheel") + ", Z / X"},
                    {word("Faster / Slower"), "Shift / Alt"},
                    {word("Speed") + " (" + speedText() + ")", "- / ="},
                    {word("Filter") + " (" + filter + ")", "F"},
                    {word("Reset Camera"), "R"},
                    {word("Grid"), "G"},
                    {word("Next Frame"), "."},
                    {word("Save Picture"), keyName(PSPadBackend::hotkeyKeys(hi::kHotkeyScreenshot), "F12")},
                    {word("Hide Help"), "H"},
                    {word("Back"), "Esc, " + keyName(PSPadBackend::hotkeyKeys(hi::kHotkeyPhotoMode), "P")},
                };
            }
            else
            {
                rows = {
                    {word("Move"), word("Left Stick")},
                    {word("Look"), word("Right Stick")},
                    {word("Up / Down"), padName(hi::kR2, device) + " / " + padName(hi::kL2, device)},
                    {word("Roll"), padName(hi::kL1, device) + " / " + padName(hi::kR1, device)},
                    {word("Level Horizon"), padName(hi::kL1, device) + " + " + padName(hi::kR1, device)},
                    {word("Zoom"), word("D-pad Up / Down")},
                    {word("Speed") + " (" + speedText() + ")", word("D-pad Left / Right")},
                    {word("Faster"), padName(hi::kR3, device)},
                    {word("Filter") + " (" + filter + ")", padName(hi::kTriangle, device)},
                    {word("Reset Camera"), padName(hi::kSelect, device)},
                    {word("Grid"), padName(hi::kL2, device) + " + " + padName(hi::kR2, device)},
                    {word("Next Frame"), padName(hi::kL3, device)},
                    {word("Save Picture"), padName(hi::kCross, device)},
                    {word("Hide Help"), padName(hi::kSquare, device)},
                    {word("Back"), padName(hi::kCircle, device) + ", " + padName(hi::kStart, device)},
                };
            }
            // Columns in characters (the overlay's font is fixed width; the
            // words are UTF-8).
            const auto columns = [](const std::string &utf8)
            {
                size_t n = 0;
                for (char c : utf8)
                    n += (static_cast<uint8_t>(c) & 0xC0u) != 0x80u ? 1u : 0u;
                return n;
            };
            text = word("Photo Mode") + "\n";
            size_t width = 0;
            for (const auto &row : rows)
                width = std::max(width, columns(row.first));
            for (const auto &row : rows)
                text += "\n" + row.first + std::string(width + 2u - columns(row.first), ' ') + row.second;
        }
        if (!status_.empty() && vsync_ < statusUntil_)
            text += (text.empty() ? "" : "\n\n") + status_;
        if (!text.empty())
            st::notify(text, 1.0);
    }

    void FreeLook::status(const std::string &line)
    {
        status_ = line;
        statusUntil_ = vsync_ + 120u;
        showNotice(true);
    }

    void FreeLook::setRuntime(PS2Machine *runtime)
    {
        runtime_ = runtime;
        zx::session::onEnd([]()
                           {
                               ph::setFramingGrid({});
                               PSPadBackend::setFreeInput(false);
                           });
    }

    void FreeLook::applyPreset()
    {
        const uint32_t filters = ph::presets()[preset_].filters;
        showGrid();
        if (!runtime_)
            return;
        if (filters == 0u || !active_.load())
        {
            runtime_->setPictureFilter({});
            return;
        }
        runtime_->setPictureFilter([filters](uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride, float aspect)
                                   { ph::applyFilters(rgba, width, height, stride, filters, aspect); });
    }

    // The grid over the window while photo mode is on and the player has it
    // on, at the thirds of the picture between the letterbox's bars when the
    // filter has them.
    void FreeLook::showGrid()
    {
        ph::FramingGrid grid;
        grid.shown = grid_ && active_.load();
        grid.letterbox = (ph::presets()[preset_].filters & ph::kFilterLetterbox) != 0u;
        ph::setFramingGrid(grid);
    }

    void FreeLook::begin(const ph::Vec3 &eye, const ph::Vec3 &target, float fieldOfView, float maxDistance)
    {
        camera_ = ph::FreeCamera{};
        camera_.lookFrom(eye, target, fieldOfView);
        camera_.maxDistance = maxDistance;
        cameraStart_ = camera_;
        exitRequest_ = false;
        stepRequest_ = false;
        framesStepped_ = 0;
        levelling_ = false;
        gridChord_ = false;
        help_ = true;
        helpAsked_ = false;
        startedAt_ = vsync_;
        lastFrame_ = std::chrono::steady_clock::now();
        PSPadBackend::setFreeInput(true);
        active_ = true;
        applyPreset();
        roll_ = camera_.roll;
        showNotice(true);
    }

    void FreeLook::end(bool clearNotice)
    {
        roll_ = 0.0f;
        PSPadBackend::setFreeInput(false);
        active_ = false;
        stepRequest_ = false;
        showGrid();
        if (runtime_)
            runtime_->setPictureFilter({});
        if (clearNotice)
            st::notify("", 0.0);
    }

    bool FreeLook::frame()
    {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - lastFrame_).count();
        lastFrame_ = now;
        const PSPadBackend::FreeInput in = PSPadBackend::takeFreeInput();
        const auto key = [&in](const char *name)
        {
            const int code = hi::hostCodeByName(name);
            return code >= 0 && in.down.test(static_cast<size_t>(code));
        };
        const auto tapped = [&in](const char *name)
        {
            const int code = hi::hostCodeByName(name);
            return code >= 0 && in.pressed.test(static_cast<size_t>(code));
        };
        const auto pad = [&in](int button) { return (in.padButtons >> button & 1u) != 0u; };
        const auto padTapped = [&in](int button) { return (in.padPressed >> button & 1u) != 0u; };

        if (exitRequest_ || tapped("ESCAPE") || padTapped(hi::kCircle) || padTapped(hi::kStart))
            return false;

        ph::Controls c;
        if (in.focused)
        {
            c.moveForward = (key("W") ? 1.0f : 0.0f) - (key("S") ? 1.0f : 0.0f);
            c.moveRight = (key("D") ? 1.0f : 0.0f) - (key("A") ? 1.0f : 0.0f);
            c.moveUp = (key("SPACE") ? 1.0f : 0.0f) - (key("LEFT_CONTROL") || key("RIGHT_CONTROL") ? 1.0f : 0.0f);
            c.roll = (key("E") ? 1.0f : 0.0f) - (key("Q") ? 1.0f : 0.0f);
            // (the player's Mouse Sensitivity and Invert Mouse Y are in the motion: takeFreeInput)
            constexpr float kDegreesPerPixel = 0.12f;
            c.lookYaw = in.mouseDx * kDegreesPerPixel;
            c.lookPitch = -in.mouseDy * kDegreesPerPixel;
            c.zoom = in.wheel + (tapped("Z") ? 1.0f : 0.0f) - (tapped("X") ? 1.0f : 0.0f);
            if (key("LEFT_SHIFT") || key("RIGHT_SHIFT"))
                c.speedFactor = 4.0f;
            else if (key("LEFT_ALT") || key("RIGHT_ALT"))
                c.speedFactor = 0.25f;
        }
        // The pad (and ZX_CONTROL / ZX_PAD_SCRIPT, which arrive the same way).
        c.moveForward += -axis(in.sticks[hi::kLy]);
        c.moveRight += axis(in.sticks[hi::kLx]);
        c.turnYaw = axis(in.sticks[hi::kRx]);
        c.turnPitch = -axis(in.sticks[hi::kRy]);
        c.moveUp += (pad(hi::kR2) ? 1.0f : 0.0f) - (pad(hi::kL2) ? 1.0f : 0.0f);
        c.roll += (pad(hi::kR1) ? 1.0f : 0.0f) - (pad(hi::kL1) ? 1.0f : 0.0f);
        c.zoom += (padTapped(hi::kUp) ? 1.0f : 0.0f) - (padTapped(hi::kDown) ? 1.0f : 0.0f);
        if (pad(hi::kR3))
            c.speedFactor = 4.0f;

        const float fovBefore = camera_.fov;
        camera_.update(c, seconds);
        // Both roll buttons at once: the horizon level again (the rest of
        // the camera stays).
        const bool levelling = (in.focused && key("Q") && key("E")) || (pad(hi::kL1) && pad(hi::kR1));
        if (levelling && !levelling_)
        {
            camera_.roll = 0.0f;
            status(word("Level Horizon"));
        }
        levelling_ = levelling;
        // Both up / down buttons at once (no move: they cancel): the grid on
        // or off. Every pad button has its own use already.
        const bool gridChord = pad(hi::kL2) && pad(hi::kR2);
        const bool gridAsked = tapped("G") || (gridChord && !gridChord_);
        gridChord_ = gridChord;
        if (camera_.fov != fovBefore)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "%s %.0f", word("Field of View").c_str(), camera_.fov);
            status(line);
        }
        int speedStep = (tapped("EQUAL") || padTapped(hi::kRight) ? 1 : 0) - (tapped("MINUS") || padTapped(hi::kLeft) ? 1 : 0);
        if (speedStep != 0)
        {
            camera_.speed = std::fmin(32.0f, std::fmax(0.5f, camera_.speed * (speedStep > 0 ? 1.5f : 1.0f / 1.5f)));
            status(word("Speed") + " " + speedText());
        }
        if (tapped("F") || padTapped(hi::kTriangle))
        {
            preset_ = (preset_ + 1u) % ph::presets().size();
            applyPreset();
            status(word("Filter") + ": " + word(ph::presets()[preset_].name));
        }
        if (tapped("R") || padTapped(hi::kSelect))
        {
            const float speed = camera_.speed;
            camera_ = cameraStart_;
            camera_.speed = speed;
            status(word("Reset Camera"));
        }
        if (gridAsked)
        {
            grid_ = !grid_;
            showGrid();
            status(word("Grid") + ": " + word(grid_ ? "On" : "Off"));
        }
        if (tapped("PERIOD") || padTapped(hi::kL3))
        {
            requestFrameStep();
            status(word("Next Frame") + " " + std::to_string(framesStepped_ + 1u));
        }
        if (tapped("H") || padTapped(hi::kSquare))
        {
            help_ = !help_;
            helpAsked_ = true;
            showNotice(true);
        }
        if (padTapped(hi::kCross))
            PSPadBackend::triggerHotkey(hi::kHotkeyScreenshot); // the Screenshot key's path (the app)
        roll_ = camera_.roll;
        showNotice(false);
        return true;
    }
}
