// Aggressive Inline (SLUS-20327): button prompts for the device in use.
//
// The game shows PlayStation buttons in three ways: pictures in its texture
// libraries (ps2Buttons.zif: the footer prompts "X Select", "Triangle
// Back"; ParkEditorButton.zif), characters of fonts that are button
// pictures (fnt_ovr_small: \ ` | ~ for cross, square, triangle, circle in
// the tutorial's tips; fnt_ps2buttons: b x y l L r R s S in the trick
// lists; fnt_lvl_ed: 1-8, the park editor's L1 L2 R1 R2) and words ("the directional button", "the left analog stick",
// "R1"). With keyboard and mouse, an Xbox-style or a Nintendo-style gamepad
// they become that device's keys and buttons, following controls.ini, and
// change when the player switches device (PSPadBackend::activeDevice).
//
// Pictures: zge_prompts.cpp, shared with DM2 and BMX XXX, redraws the
// textures and font cells the tables below name when the game loads them
// (the key caps, mouse and Xbox-style shoulder labels drawn in the game's
// fnt_14pt, the Xbox face buttons from the disc's xboxButtons.zif, the
// Nintendo-style pads' drawn in the look of the game's own, glyph_art.h);
// the Tutorial's loading screens show a pad drawn here (pad_art.h) for
// them.
//
// Words: the two text primitives every label, menu and text box goes
// through (drawText: font, position, text: a line; drawWrappedText: wrapped
// text, text in a2) get the text with the device's words; the font (a0, its
// HFNT header in RAM: line height at +0, atlas name at +0x0A) tells which
// characters are pictures.

#include "aggressive_inline_language.h"
#include "game_overrides.h"
#include "sym/game_overrides/aggressive_inline_prompts.cpp.h"
#include "overrides/zge_prompts.h"
#include "host_backend.h"
#include "machine.h"
#include "runtime/game_files.h"
#include "runtime/glyph_art.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/pad_art.h"
#include "runtime/xbox_disc.h"
#include "runtime/zaxis_ui.h"
#include "runtime/session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ps2_aggressive_inline
{
    // Named, not anonymous: the unity build puts the settings file in the
    // same translation unit.
    namespace prompts
    {
        namespace gl = zx::glyphs;
        namespace hi = ps2_host_input;
        namespace zp = zge_host::prompts;
        using Device = PSPadBackend::InputDevice;
        using zp::Picture;
        using zp::Shows;
        using zp::firstKey;
        using zp::keyPhrase;
        using zp::padLabel;
        using zp::remapped;
        using zp::replaceWords;

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        std::string readString(const uint8_t *rdram, uint32_t address, size_t limit)
        {
            std::string out;
            for (size_t i = 0; i < limit; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!p || *p == 0u)
                    break;
                out.push_back(static_cast<char>(*p));
            }
            return out;
        }

        // ---- what each picture shows ------------------------------------------

        // Whole textures, by TXRH name.
        const zp::TexturePictures kTexturePictures = {
            // ps2Buttons.zif
            {"select_button", {Shows::Button, hi::kCross}},
            {"cancel_button", {Shows::Button, hi::kTriangle}},
            {"secondary_button", {Shows::Button, hi::kCircle}},
            {"Tird_Button", {Shows::Button, hi::kSquare}},
            {"shoulder_button", {Shows::Button, hi::kR1}},
            {"DPad_Button", {Shows::DPad, -1}},
            // ParkEditorButton.zif
            {"Button1", {Shows::Button, hi::kCross}},
            {"Button2", {Shows::Button, hi::kCircle}},
            {"Button3", {Shows::Button, hi::kSquare}},
            {"Button4", {Shows::Button, hi::kTriangle}},
            {"Button5", {Shows::DPad, -1}},
            {"ButtonL1Down", {Shows::ButtonLeft, hi::kL1, true}},
            {"ButtonL1Up", {Shows::ButtonLeft, hi::kL1, false}},
            {"ButtonR1Down", {Shows::ButtonRight, hi::kR1, true}},
            {"ButtonR1Up", {Shows::ButtonRight, hi::kR1, false}},
        };

        constexpr Picture button(int b) { return Picture{Shows::Button, b}; }
        // The park editor's L1/L2/R1/R2 at the top of its screen
        // (ParkEditor.zsc: guiLabels in ParkEditor.Icons, fnt_lvl_ed,
        // "1" L1, "2" L2, "3" R1, "4" R2, and while held "6" "7" "5" "8"):
        // grey keys as wide as the 31-pixel cell, 19 pixels tall.
        constexpr Picture shoulder(int b, bool held) { return Picture{Shows::Button, b, held, true}; }

        // Font atlases: the characters that are button pictures.
        const zp::FontPictures kFontPictures = {
            {"fnt_ovr_small_t0.zmp",
             {{'\\', button(hi::kCross)}, {'`', button(hi::kSquare)}, {'|', button(hi::kTriangle)}, {'~', button(hi::kCircle)}}},
            {"fnt_ps2buttons_t0.zmp",
             {{'b', button(hi::kCircle)}, {'x', button(hi::kSquare)}, {'y', button(hi::kTriangle)}, {'l', button(hi::kL1)},
              {'L', button(hi::kL2)}, {'r', button(hi::kR1)}, {'R', button(hi::kR2)}, {'s', button(hi::kStart)},
              {'S', button(hi::kSelect)}}},
            {"fnt_lvl_ed_t0.zmp",
             {{'1', shoulder(hi::kL1, false)}, {'2', shoulder(hi::kL2, false)}, {'3', shoulder(hi::kR1, false)},
              {'4', shoulder(hi::kR2, false)}, {'5', shoulder(hi::kR1, true)}, {'6', shoulder(hi::kL1, true)},
              {'7', shoulder(hi::kL2, true)}, {'8', shoulder(hi::kR2, true)}}},
        };

        // The game's own pictures: ps2Buttons.zif's (a remapped PlayStation
        // pad's buttons) and xboxButtons.zif's face buttons and d-pad. The
        // Nintendo-style pads' are drawn.
        constexpr zp::ArtSource kArt[] = {
            {Device::PlayStation, "btm_psx_x_wb.zmp", 0, hi::kCross},
            {Device::PlayStation, "btm_psx_tri_wb.zmp", 0, hi::kTriangle},
            {Device::PlayStation, "btm_psx_o_wb.zmp", 0, hi::kCircle},
            {Device::PlayStation, "btn_psz_sq.zmp", 0, hi::kSquare},
            {Device::PlayStation, "btn_r1_arrow_up.zmp", 0, hi::kR1},
            {Device::Xbox, "xbox_btn_a.tga", 0, hi::kCross},
            {Device::Xbox, "xbox_btn_b.tga", 0, hi::kCircle},
            {Device::Xbox, "xbox_btn_x.tga", 0, hi::kSquare},
            {Device::Xbox, "xbox_btn_y.tga", 0, hi::kTriangle},
            {Device::Xbox, "xbox_btn_dpad.tga", 0, zp::kDPadArt},
        };

        const zp::Game kGame{
            .tag = "[prompts]",
            .loadTextureChunk = sym::loadTextureChunk,
            .newLoadedTexture = sym::newLoadedTexture,
            .releaseLoadedTexture = sym::releaseLoadedTexture,
            .drawText = 0u, // the words' hooks below
            .textures = &kTexturePictures,
            .fonts = &kFontPictures,
            .art = kArt,
            .artCount = std::size(kArt),
            .labelFont = "fnt_14pt.zfn",
        };

        // ---- drawn gamepads (Nintendo-style; Xbox-style without the disc's) --

        // The place on a drawn pad of a physical button (PadButton).
        gl::PadControl padControlOf(int physical)
        {
            using C = gl::PadControl;
            switch (physical)
            {
            case hi::kCross:
                return C::South;
            case hi::kCircle:
                return C::East;
            case hi::kSquare:
                return C::West;
            case hi::kTriangle:
                return C::North;
            case hi::kL1:
                return C::L1;
            case hi::kR1:
                return C::R1;
            case hi::kL2:
                return C::L2;
            case hi::kR2:
                return C::R2;
            case hi::kSelect:
                return C::Select;
            case hi::kStart:
                return C::Start;
            case hi::kL3:
                return C::LeftStick;
            case hi::kR3:
                return C::RightStick;
            default:
                return C::DPad;
            }
        }

        // The control of a drawn pad that presses a game button ([gamepad]
        // remaps move it).
        gl::PadControl controlFor(int button)
        {
            const int physical = PSPadBackend::gamepadButtonFor(button);
            return padControlOf(physical < 0 ? button : physical);
        }

        // For each control of a pad as mapped by default, the one that
        // presses its game button now (the sticks and the d-pad stay).
        std::array<gl::PadControl, static_cast<size_t>(gl::PadControl::Count)> remappedControls()
        {
            std::array<gl::PadControl, static_cast<size_t>(gl::PadControl::Count)> out{};
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = static_cast<gl::PadControl>(i);
            for (int button : {hi::kCross, hi::kCircle, hi::kSquare, hi::kTriangle, hi::kL1, hi::kR1, hi::kL2, hi::kR2,
                               hi::kSelect, hi::kStart})
                out[static_cast<size_t>(padControlOf(button))] = controlFor(button);
            return out;
        }

        // ---- the Tutorial's loading screens for a drawn pad ------------------

        // TUTn.RAW: 640x448 RGB, a photo of the DualShock 2 with arrows and
        // captions above the LOADING bar (from row 340 down, kept as it is).
        constexpr int kScreenW = 640, kScreenH = 448, kScreenBar = 340;

        struct Callout
        {
            const char *text;        // wrapped to stay on screen; empty: another arrow of the caption before
            gl::PadControl control;  // where the arrow points on a pad as mapped by default
            float x, y;              // the caption's middle
            float fromX, fromY;      // the arrow's start, bent towards (bendX, bendY)
            float bendX, bendY;
            uint32_t color;
        };

        // The drawn pad's place on the screen (its width; the height follows).
        constexpr float kPadX = 135.0f, kPadY = 36.0f, kPadW = 370.0f;

        // What the four screens say, placed for the drawn pad. The face
        // buttons' captions keep the colours of the PlayStation symbols at
        // that place (green triangle, red circle, blue cross, pink square).
        // TUT1 swaps Action and Grabs and Tricks of TUT0, TUT2 Grind and
        // Action; TUT3 has no Challenges.
        std::vector<Callout> tutorialCallouts(int screen)
        {
            using C = gl::PadControl;
            const uint32_t white = gl::rgba(236, 236, 236);
            const uint32_t green = gl::rgba(86, 196, 156), red = gl::rgba(236, 64, 64);
            const uint32_t blue = gl::rgba(120, 132, 238), pink = gl::rgba(226, 92, 176);
            const char *north = "Grind", *east = "Action", *west = "Grabs and Tricks";
            if (screen == 1)
                std::swap(east, west);
            if (screen == 2)
                std::swap(north, east);
            std::vector<Callout> out = {
                {"Rotate L / R", C::L1, 320, 16, 266, 26, 256, 40, white},
                {"", C::R1, 320, 16, 374, 26, 384, 40, white},
                {"Cess slide", C::R2, 572, 30, 524, 32, 476, 24, white},
                {north, C::North, 564, 68, 520, 70, 470, 66, green},
                {east, C::East, 570, 108, 524, 110, 494, 108, red},
                {"Jump / Accelerate", C::South, 560, 156, 496, 160, 456, 162, blue},
                {west, C::West, 530, 270, 466, 258, 418, 206, pink},
                {"Pause", C::Start, 322, 274, 326, 260, 336, 206, white},
                {"Camera", C::RightStick, 392, 306, 386, 292, 378, 240, white},
                {"Forward", C::LeftStick, 100, 246, 94, 232, 112, 122, white},
                {"", C::DPad, 100, 246, 134, 238, 200, 236, white},
            };
            if (screen != 3)
                out.push_back({"Challenges", C::Select, 226, 224, 252, 210, 300, 192, white});
            return out;
        }

        // The screen above the bar as an image, the original picture blurred
        // away into a background (darker towards the middle, with a fine
        // grain like the original's).
        gl::Image screenBackground(const std::vector<uint8_t> &rgb, int screen)
        {
            gl::Image image(kScreenW, kScreenBar);
            for (int y = 0; y < kScreenBar; ++y)
                for (int x = 0; x < kScreenW; ++x)
                {
                    const uint8_t *p = &rgb[(static_cast<size_t>(y) * kScreenW + static_cast<size_t>(x)) * 3u];
                    image.pixels[static_cast<size_t>(y) * kScreenW + static_cast<size_t>(x)] = gl::rgba(p[0], p[1], p[2]);
                }
            gl::blur(image, 22);
            uint32_t seed = 0x2468ACEu + static_cast<uint32_t>(screen);
            for (int y = 0; y < kScreenBar; ++y)
                for (int x = 0; x < kScreenW; ++x)
                {
                    seed = seed * 1664525u + 1013904223u;
                    const float grain = static_cast<float>((seed >> 24) & 0xFFu) / 255.0f * 14.0f - 7.0f;
                    const float dx = (static_cast<float>(x) - 320.0f) / 320.0f, dy = (static_cast<float>(y) - 150.0f) / 190.0f;
                    const float shade = 0.82f + 0.12f * std::min(1.0f, dx * dx + dy * dy);
                    uint32_t &c = image.pixels[static_cast<size_t>(y) * kScreenW + static_cast<size_t>(x)];
                    uint32_t out = 0xFF000000u;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float v = static_cast<float>((c >> (k * 8)) & 0xFFu) * shade + grain;
                        out |= static_cast<uint32_t>(std::clamp(static_cast<int>(v), 0, 255)) << (k * 8);
                    }
                    c = out;
                }
            return image;
        }

        void putScreen(std::vector<uint8_t> &rgb, const gl::Image &image)
        {
            for (int y = 0; y < kScreenBar; ++y)
                for (int x = 0; x < kScreenW; ++x)
                {
                    const uint32_t c = image.pixels[static_cast<size_t>(y) * kScreenW + static_cast<size_t>(x)];
                    uint8_t *p = &rgb[(static_cast<size_t>(y) * kScreenW + static_cast<size_t>(x)) * 3u];
                    p[0] = static_cast<uint8_t>(c);
                    p[1] = static_cast<uint8_t>(c >> 8);
                    p[2] = static_cast<uint8_t>(c >> 16);
                }
        }

        // Repaints the picture above the bar: the original blurred into a
        // background, the drawn pad, the captions in the game's marker font
        // (fnt_ovr_small) with arrows to the controls, and the Forward /
        // Left / Right / Brake cross. `target`: the control that presses
        // each default control's game button (a remapped pad).
        bool composeTutorialScreen(std::vector<uint8_t> &rgb, int screen, const gl::Font &caption,
                                   const gl::Font &label, gl::PadStyle style,
                                   const std::array<gl::PadControl, static_cast<size_t>(gl::PadControl::Count)> &target)
        {
            if (rgb.size() != static_cast<size_t>(kScreenW) * kScreenH * 3u || caption.empty() || label.empty())
                return false;
            gl::Image image = screenBackground(rgb, screen);
            const float padH = kPadW / gl::kGamepadAspect;
            gl::drawGamepad(image, kPadX, kPadY, kPadW, padH, style, label);
            const uint32_t ink = gl::rgba(14, 14, 18);
            const float textH = 30.0f;
            for (const Callout &c : tutorialCallouts(screen))
            {
                const gl::PadControl control = target[static_cast<size_t>(c.control)];
                gl::Point to = gl::padAnchor(control, kPadX, kPadY, kPadW, padH);
                const float r = gl::padControlRadius(control, kPadX, kPadY, kPadW, padH);
                // Stop at the control's edge, coming from the bend.
                const float dx = to.x - c.bendX, dy = to.y - c.bendY, len = std::sqrt(dx * dx + dy * dy);
                if (len > r + 4.0f)
                    to = {to.x - dx / len * (r + 2.0f), to.y - dy / len * (r + 2.0f)};
                gl::drawArrow(image, {c.fromX, c.fromY}, {c.bendX, c.bendY}, to, 3.2f, c.color, ink);
            }
            for (const Callout &c : tutorialCallouts(screen))
            {
                // Lines as given, each wrapped at spaces to stay on screen.
                const float maxWidth = 2.0f * std::min(c.x - 6.0f, static_cast<float>(kScreenW) - 6.0f - c.x);
                std::vector<std::string> lines;
                std::string rest = c.text;
                // The port's own words in another language when the table has them.
                if (aggressive_inline::language::active() && !rest.empty())
                    if (std::string translated = aggressive_inline::language::translate(rest); !translated.empty())
                        rest = std::move(translated);
                while (!rest.empty())
                {
                    const size_t nl = rest.find('\n');
                    std::string line = rest.substr(0, nl);
                    rest = nl == std::string::npos ? std::string() : rest.substr(nl + 1);
                    while (gl::textWidth(caption, line, textH) > maxWidth && line.find(' ') != std::string::npos)
                    {
                        size_t cut = line.rfind(' ');
                        while (cut != std::string::npos && cut > 0 &&
                               gl::textWidth(caption, line.substr(0, cut), textH) > maxWidth)
                            cut = line.rfind(' ', cut - 1);
                        if (cut == std::string::npos || cut == 0)
                            break;
                        lines.push_back(line.substr(0, cut));
                        line = line.substr(cut + 1);
                    }
                    lines.push_back(line);
                }
                const float step = textH * 0.84f;
                float y = c.y - step * 0.5f * static_cast<float>(lines.size() - 1u);
                for (const std::string &line : lines)
                {
                    gl::drawOutlinedText(image, caption, line, c.x, y, textH, c.color, ink, 2.0f);
                    y += step;
                }
            }
            // Forward (above), Left, Right and Brake around the cross.
            const float crossX = 104.0f, crossY = 290.0f;
            gl::drawCrossArrows(image, crossX, crossY, 52.0f, gl::rgba(236, 236, 236), ink);
            const auto word = [](const char *english)
            {
                std::string translated = aggressive_inline::language::active() ? aggressive_inline::language::translate(english) : std::string();
                return translated.empty() ? std::string(english) : translated;
            };
            gl::drawOutlinedText(image, caption, word("Left"), crossX - 58.0f, crossY, textH, gl::rgba(236, 236, 236), ink, 2.0f);
            gl::drawOutlinedText(image, caption, word("Right"), crossX + 62.0f, crossY, textH, gl::rgba(236, 236, 236), ink, 2.0f);
            gl::drawOutlinedText(image, caption, word("Brake"), crossX, crossY + 40.0f, textH, gl::rgba(236, 236, 236), ink, 2.0f);
            putScreen(rgb, image);
            return true;
        }

        // The keyboard version of a Tutorial loading screen: the same
        // captions (wording, colours and swaps as tutorialCallouts), each
        // beside the key caps bound to its game button in controls.ini
        // (the first key; a mouse button as the mouse with it lit), drawn
        // as the prompts draw them; the movement keys as a cross with
        // Forward / Left / Right / Brake around it, and the mouse for the
        // camera with mouse-look.
        bool composeKeyboardScreen(std::vector<uint8_t> &rgb, int screen, const gl::Font &caption, const gl::Font &label)
        {
            if (rgb.size() != static_cast<size_t>(kScreenW) * kScreenH * 3u || caption.empty() || label.empty())
                return false;
            gl::Image image = screenBackground(rgb, screen);
            const uint32_t ink = gl::rgba(14, 14, 18), white = gl::rgba(236, 236, 236);
            const auto word = [](const std::string &english)
            {
                std::string translated = aggressive_inline::language::active() ? aggressive_inline::language::translate(english) : std::string();
                return translated.empty() ? english : translated;
            };
            // A caption with its left edge at x, narrowed to fit the screen.
            const auto captionAt = [&](const std::string &text, float x, float y, uint32_t color, float maxWidth)
            {
                const std::string t = word(text);
                float h = 30.0f;
                const float w0 = gl::textWidth(caption, t, h);
                if (w0 > maxWidth && w0 > 0.0f)
                    h *= maxWidth / w0;
                gl::drawOutlinedText(image, caption, t, x + gl::textWidth(caption, t, h) * 0.5f, y, h, color, ink, 2.0f);
            };
            // A key cap (or the mouse) for a host code, its left edge at x; returns its width.
            const float capH = 34.0f;
            const auto capWidth = [&](int code)
            {
                if (code < 0 || hi::isMouseCode(code))
                    return capH * 0.8f;
                const std::string name = hi::hostCodeLabel(code);
                if (name.size() <= 1u || name == "Space" || name == "Up" || name == "Down" || name == "Left" ||
                    name == "Right" || name == "Enter" || name == "Backspace")
                    return name == "Space" ? capH * 2.2f : capH;
                return std::min(110.0f, std::max(capH, gl::textWidth(label, name, 16.0f) + 16.0f));
            };
            const auto drawCode = [&](int code, float x, float cy, float w)
            {
                if (code >= 0 && hi::isMouseCode(code))
                    gl::drawMouse(image, x, cy - capH * 0.5f, w, capH, code - hi::kMouseCodeBase);
                else
                    gl::drawKeycap(image, x, cy - capH * 0.5f, w, capH, label, code < 0 ? "?" : hi::hostCodeLabel(code));
            };
            const auto stickKey = [](int direction)
            {
                const std::vector<int> keys = PSPadBackend::stickKeyBindings(direction);
                return keys.empty() ? -1 : keys.front();
            };

            // Movement: the left stick's keys as a cross.
            const float cap = 46.0f, cx = 150.0f, cy = 150.0f, gap = 4.0f;
            const int up = stickKey(hi::kLeftUp), left = stickKey(hi::kLeftLeft), down = stickKey(hi::kLeftDown),
                      right = stickKey(hi::kLeftRight);
            const auto bigCap = [&](int code, float x, float y)
            {
                if (code >= 0 && hi::isMouseCode(code))
                    gl::drawMouse(image, x, y, cap, cap, code - hi::kMouseCodeBase);
                else
                    gl::drawKeycap(image, x, y, cap, cap, label, code < 0 ? "?" : hi::hostCodeLabel(code));
            };
            bigCap(up, cx - cap * 0.5f, cy - cap * 1.5f - gap);
            bigCap(left, cx - cap * 1.5f - gap, cy - cap * 0.5f);
            bigCap(down, cx - cap * 0.5f, cy - cap * 0.5f);
            bigCap(right, cx + cap * 0.5f + gap, cy - cap * 0.5f);
            const auto centred = [&](const char *text, float x, float y)
            {
                const std::string t = word(text);
                gl::drawOutlinedText(image, caption, t, x, y, 30.0f, white, ink, 2.0f);
            };
            centred("Forward", cx, cy - cap * 1.5f - gap - 24.0f);
            centred("Left", cx - cap * 1.5f - gap - 4.0f - gl::textWidth(caption, word("Left"), 30.0f) * 0.5f, cy);
            centred("Right", cx + cap * 1.5f + gap + 6.0f + gl::textWidth(caption, word("Right"), 30.0f) * 0.5f, cy);
            centred("Brake", cx, cy + cap * 0.5f + 22.0f);
            // The camera: the mouse with mouse-look, else the right stick's keys.
            {
                const float my = 268.0f;
                const bool look = PSPadBackend::mouseLook();
                if (look)
                    gl::drawMouse(image, cx - 60.0f, my - 26.0f, 40.0f, 52.0f, -1);
                else
                {
                    const int keys[] = {stickKey(hi::kRightLeft), stickKey(hi::kRightUp), stickKey(hi::kRightDown),
                                        stickKey(hi::kRightRight)};
                    float x = cx - 110.0f;
                    for (int k : keys)
                    {
                        drawCode(k, x, my, capWidth(k));
                        x += capWidth(k) + 3.0f;
                    }
                }
                captionAt("Camera", cx - 10.0f, my, white, 140.0f);
            }

            // The buttons: key caps right-aligned at keysRight, captions after.
            const uint32_t green = gl::rgba(86, 196, 156), red = gl::rgba(236, 64, 64);
            const uint32_t blue = gl::rgba(120, 132, 238), pink = gl::rgba(226, 92, 176);
            struct Row
            {
                const char *text;
                std::vector<int> buttons;
                uint32_t color;
            };
            const char *north = "Grind", *east = "Action", *west = "Grabs and Tricks";
            if (screen == 1)
                std::swap(east, west);
            if (screen == 2)
                std::swap(north, east);
            std::vector<Row> rows = {
                {"Rotate L / R", {hi::kL1, hi::kR1}, white},
                {"Cess slide", {hi::kL2, hi::kR2}, white},
                {north, {hi::kTriangle}, green},
                {east, {hi::kCircle}, red},
                {"Jump / Accelerate", {hi::kCross}, blue},
                {west, {hi::kSquare}, pink},
                {"Pause", {hi::kStart}, white},
            };
            if (screen != 3)
                rows.push_back({"Challenges", {hi::kSelect}, white});
            const float keysRight = 420.0f, textLeft = 432.0f;
            const float top = 34.0f, step = rows.size() > 7u ? 38.0f : 42.0f;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const float y = top + step * static_cast<float>(i);
                float width = 0.0f;
                for (int b : rows[i].buttons)
                    width += capWidth(firstKey(b)) + 6.0f;
                float x = keysRight - width + 6.0f;
                for (int b : rows[i].buttons)
                {
                    const int code = firstKey(b);
                    drawCode(code, x, y, capWidth(code));
                    x += capWidth(code) + 6.0f;
                }
                captionAt(rows[i].text, textLeft, y, rows[i].color, static_cast<float>(kScreenW) - textLeft - 6.0f);
            }
            putScreen(rgb, image);
            return true;
        }

        // The PS2 disc's own files (no mods folder: a mod of these files is
        // served before any provider), read without the providers below.
        thread_local bool t_readingDisc = false;

        struct DiscFiles
        {
            std::mutex mutex;
            std::unique_ptr<zx::game_files::GameFileSystem> files;
        };

        bool readDisc(const std::string &path, std::vector<uint8_t> &bytes)
        {
            DiscFiles &disc = zx::session::state<DiscFiles>();
            std::lock_guard<std::mutex> lock(disc.mutex);
            std::unique_ptr<zx::game_files::GameFileSystem> &files = disc.files;
            if (!files)
            {
                const PS2Machine::IoPaths &paths = PS2Machine::getIoPaths();
                zx::game_files::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Machine::cdImageFileSystem();
                files = std::make_unique<zx::game_files::GameFileSystem>(std::move(roots));
            }
            t_readingDisc = true;
            std::shared_ptr<zx::game_files::GameFile> file = files->open(path);
            const bool ok = file && zx::game_files::readAll(*file, bytes);
            t_readingDisc = false;
            return ok;
        }

        // Would the Xbox release's own Tutorial screens tell the truth? They
        // show the Xbox layout of the time: Rotate L / R on the triggers,
        // Cess slide on the white (and black) button, Jump A, Action B,
        // Grabs X, Grind Y, Pause Start, no Challenges. The port presses L1
        // / R1 (Rotate) with LB / RB and L2 / R2 (Cess slide) with LT / RT
        // by default, so they only fit a [gamepad] that puts Rotate on the
        // triggers and Cess slide on the shoulders, with the face buttons
        // and Start as they are (Gamepad Layout's Modern Triggers:
        // hi::gamepadLayoutButtons).
        bool xboxScreensFit()
        {
            const auto on = [](int button, int physical) { return PSPadBackend::gamepadButtonFor(button) == physical; };
            return on(hi::kL1, hi::kL2) && on(hi::kR1, hi::kR2) && on(hi::kL2, hi::kL1) && on(hi::kR2, hi::kR1) &&
                   on(hi::kCross, hi::kCross) && on(hi::kCircle, hi::kCircle) && on(hi::kSquare, hi::kSquare) &&
                   on(hi::kTriangle, hi::kTriangle) && on(hi::kStart, hi::kStart);
        }

        // The loading screens made, by device, name and bindings.
        struct TutorialScreens
        {
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> made;
        };

        // The Tutorial's loading screens (LOADING/TUT/TUT0-3.RAW) for the
        // device in use, chosen each time the game opens one: the PS2
        // screen with a pad drawn here in place of the DualShock 2 (Xbox-
        // and Nintendo-style pads), its arrows to the controls that press
        // each game button, or the bound keys (keyboard and mouse); the
        // Xbox release's own pictures of its controller only where they
        // label the port's bindings right (xboxScreensFit). Null: the PS2
        // file (PlayStation pads).
        std::shared_ptr<const std::vector<uint8_t>> tutorialScreen(const std::string &path, bool xboxScreens)
        {
            if (t_readingDisc)
                return nullptr;
            const Device device = PSPadBackend::activeDevice();
            if (device == Device::PlayStation)
                return nullptr;
            const std::string name = path.substr(path.find_last_of('/') + 1u);
            if (name.size() != 8u || name.compare(0, 3, "TUT") != 0 || name[3] < '0' || name[3] > '3' ||
                name.compare(4, 4, ".RAW") != 0)
                return nullptr;
            TutorialScreens &screens = zx::session::state<TutorialScreens>();
            std::lock_guard<std::mutex> lock(screens.mutex);
            auto &cache = screens.made;
            if (device == Device::Keyboard)
            {
                auto &bytes = cache["keyboard " + name + " " + std::to_string(PSPadBackend::promptGeneration()) +
                                    (PSPadBackend::mouseLook() ? " look" : "")];
                if (bytes)
                    return bytes;
                std::vector<uint8_t> rgb;
                if (!readDisc("ASSETS/LOADING/TUT/" + name, rgb))
                    return nullptr;
                zx::zaxis::LibraryFont marker;
                if (!zp::atlasFont("fnt_ovr_small_t0.zmp", marker) ||
                    !composeKeyboardScreen(rgb, name[3] - '0', gl::fontFrom(marker), zp::labelFont()))
                    return nullptr;
                bytes = std::make_shared<const std::vector<uint8_t>>(std::move(rgb));
                std::cout << "[prompts] keyboard loading screen " << name << " (drawn)" << std::endl;
                return bytes;
            }
            if (device == Device::Xbox && xboxScreens && xboxScreensFit())
            {
                auto &bytes = cache["xbox " + name];
                if (!bytes)
                {
                    std::vector<uint8_t> data;
                    if (!ps2_xbox_disc::readFile("Loading/TUT/" + name, data))
                        return nullptr;
                    bytes = std::make_shared<const std::vector<uint8_t>>(std::move(data));
                    std::cout << "[prompts] Xbox controller loading screen " << name << std::endl;
                }
                return bytes;
            }
            const gl::PadStyle style = device == Device::Nintendo ? gl::PadStyle::Nintendo : gl::PadStyle::Xbox;
            const auto target = remappedControls();
            std::string key = (style == gl::PadStyle::Nintendo ? "nintendo " : "drawn xbox ") + name;
            for (gl::PadControl c : target)
                key += static_cast<char>('a' + static_cast<int>(c));
            auto &bytes = cache[key];
            if (bytes)
                return bytes;
            std::vector<uint8_t> rgb;
            if (!readDisc("ASSETS/LOADING/TUT/" + name, rgb))
                return nullptr;
            zx::zaxis::LibraryFont marker;
            if (!zp::atlasFont("fnt_ovr_small_t0.zmp", marker) ||
                !composeTutorialScreen(rgb, name[3] - '0', gl::fontFrom(marker), zp::labelFont(), style, target))
                return nullptr;
            bytes = std::make_shared<const std::vector<uint8_t>>(std::move(rgb));
            std::cout << "[prompts] " << (style == gl::PadStyle::Nintendo ? "Nintendo" : "Xbox") << "-style pad loading screen "
                      << name << " (drawn)" << std::endl;
            return bytes;
        }

        // ---- words ----------------------------------------------------------

        std::string keyName(int button)
        {
            const int code = firstKey(button);
            return code < 0 ? std::string() : hi::hostCodeLabel(code);
        }

        // The keys of a stick's directions, "WASD" if they are letters.
        std::string stickKeys(int up, int left, int down, int right)
        {
            std::string out;
            for (int direction : {up, left, down, right})
            {
                const std::vector<int> keys = PSPadBackend::stickKeyBindings(direction);
                std::string name = keys.empty() ? std::string() : hi::hostCodeLabel(keys.front());
                if (name.size() != 1u)
                    return {};
                out += name;
            }
            return out;
        }

        // The text as the device's player reads it. `buttonFont`: the
        // font's characters are all pictures (fnt_ps2buttons), left alone.
        std::string wordsFor(const std::string &text, Device device, bool buttonFont)
        {
            if (buttonFont)
                return text;
            std::string out = text;
            if (device == Device::PlayStation)
            {
                // A remapped pad: the shoulder buttons' names (in one pass,
                // so swapped buttons do not change back).
                static const std::pair<const char *, int> kShoulders[] = {
                    {"L1", hi::kL1}, {"R1", hi::kR1}, {"L2", hi::kL2}, {"R2", hi::kR2}};
                std::vector<std::string> labels;
                for (const auto &[name, button] : kShoulders)
                {
                    const int physical = PSPadBackend::gamepadButtonFor(button);
                    labels.push_back(padLabel(physical < 0 ? button : physical, device));
                    replaceWords(out, name, std::string("\x01") + static_cast<char>('0' + labels.size()));
                }
                for (size_t i = 0; i < labels.size(); ++i)
                {
                    const std::string mark = std::string("\x01") + static_cast<char>('1' + i);
                    for (size_t at = out.find(mark); at != std::string::npos; at = out.find(mark, at + labels[i].size()))
                        out.replace(at, mark.size(), labels[i]);
                }
                return out;
            }
            if (device == Device::Keyboard)
            {
                // Button pictures in a sentence: "the \\ button" -> "Space".
                static const std::pair<char, int> kGlyphs[] = {
                    {'\\', hi::kCross}, {'`', hi::kSquare}, {'|', hi::kTriangle}, {'~', hi::kCircle}};
                for (const auto &[glyph, button] : kGlyphs)
                {
                    const std::string phrase = std::string("the ") + glyph + " button";
                    if (out.find(phrase) == std::string::npos)
                        continue;
                    const std::string key = keyPhrase(button);
                    if (!key.empty())
                        replaceWords(out, phrase, key);
                }
                static const std::pair<const char *, int> kShoulders[] = {
                    {"L1", hi::kL1}, {"R1", hi::kR1}, {"L2", hi::kL2}, {"R2", hi::kR2}};
                for (const auto &[name, button] : kShoulders)
                {
                    if (out.find(name) == std::string::npos)
                        continue;
                    const std::string key = keyName(button);
                    if (key.empty())
                        continue;
                    replaceWords(out, std::string("the ") + name + " button", key);
                    replaceWords(out, name, key);
                }
                if (out.find("analog") != std::string::npos || out.find("directional") != std::string::npos)
                {
                    const std::string wasd = stickKeys(hi::kLeftUp, hi::kLeftLeft, hi::kLeftDown, hi::kLeftRight);
                    replaceWords(out, "the left analog stick", wasd.empty() ? "the movement keys" : "the " + wasd + " keys");
                    replaceWords(out, "left analog stick", wasd.empty() ? "movement keys" : wasd);
                    // The park editor's help: its zoom is the right stick's
                    // Y axis, which the wheel works there (noteMenus).
                    replaceWords(out, "right analog stick = zoom camera", "mouse wheel = zoom camera");
                    replaceWords(out, "the right analog stick", "the mouse");
                    replaceWords(out, "right analog stick", "mouse");
                    replaceWords(out, "the directional button", "the arrow keys");
                    replaceWords(out, "directional button", "arrow keys");
                }
                replaceWords(out, "Press any button", "Press any key");
                return out;
            }
            // Xbox and Nintendo gamepads.
            replaceWords(out, "directional button", "D-pad");
            replaceWords(out, "left analog stick", "left stick");
            replaceWords(out, "right analog stick", "right stick");
            static const std::pair<const char *, int> kShoulders[] = {
                {"L1", hi::kL1}, {"R1", hi::kR1}, {"L2", hi::kL2}, {"R2", hi::kR2}};
            for (const auto &[name, button] : kShoulders)
            {
                int physical = PSPadBackend::gamepadButtonFor(button);
                replaceWords(out, name, padLabel(physical < 0 ? button : physical, device));
            }
            return out;
        }

        // Guest buffers for changed text, used in turn.
        constexpr uint32_t kTextBufferSize = 0x800u;
        constexpr int kTextBufferCount = 8;
        struct TextBuffers
        {
            uint32_t base = 0;
            int next = 0;
        };
        TextBuffers &textBuffers() { return zx::session::state<TextBuffers>(); }

        struct TextHooks
        {
            PS2Machine::RecompiledFunction origLine = nullptr;
            PS2Machine::RecompiledFunction origWrapped = nullptr;
        };
        TextHooks &textHooks() { return zx::session::state<TextHooks>(); }

        // The game's font object (the first argument of the text
        // primitives): u8 line height, u8, u16 glyph count, u16 pages, u16,
        // +0x08 the first page (u16 width, u16 height, the atlas name at
        // +4), +0x0C the glyphs (12 bytes each as in the library's HFNT:
        // u16 character first).
        std::string fontAtlasName(const uint8_t *rdram, uint32_t font)
        {
            if (!font)
                return {};
            const uint32_t page = readWord(rdram, font + 0x08u);
            return page ? readString(rdram, page + 0x04u, 32) : std::string();
        }

        // The text with each character the font has no glyph for as the
        // nearest one it has: the letter without its accent. The Xbox fonts
        // have most of the translations' accented letters, not all (no
        // capital E acute in fnt_14pt and fnt_ovr_small, the Settings help
        // lines' fonts; fnt_sidebanner has few); the Xbox release writes
        // "Etes-vous", "Editeur" for that.
        std::string withGlyphs(const uint8_t *rdram, uint32_t font, const std::string &text)
        {
            if (!font ||
                std::all_of(text.begin(), text.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80u; }))
                return text;
            uint16_t count = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, font + 2u))
                std::memcpy(&count, p, sizeof(count));
            const uint32_t glyphs = readWord(rdram, font + 0x0Cu);
            if (count == 0u || count > 1024u || !glyphs)
                return text;
            std::array<bool, 256> has{};
            for (uint32_t i = 0; i < count; ++i)
            {
                uint16_t character = 0;
                if (const uint8_t *p = getConstMemPtr(rdram, glyphs + i * 12u))
                    std::memcpy(&character, p, sizeof(character));
                if (character < 256u)
                    has[character] = true;
            }
            // Latin-1 0xC0-0xFF without accents.
            static const char kBase[] = "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                const uint8_t code = static_cast<uint8_t>(c);
                if (code < 0x80u || has[code])
                    out.push_back(c);
                else if (code >= 0xC0u)
                    out.push_back(kBase[code - 0xC0u]);
                else if (code != 0xBFu && code != 0xA1u) // the Spanish opening marks: left out
                    out.push_back(c);
            }
            return out;
        }

        // The text argument changed for the device, or 0 to leave it.
        uint32_t textFor(uint8_t *rdram, uint32_t font, uint32_t text)
        {
            if (!text || !textBuffers().base)
                return 0u;
            const std::string original = readString(rdram, text, kTextBufferSize - 1u);
            if (original.empty())
                return 0u;
            const Device device = PSPadBackend::activeDevice();
            const bool language = aggressive_inline::language::active();
            const bool words = device != Device::PlayStation || remapped(hi::kL1) || remapped(hi::kR1) ||
                               remapped(hi::kL2) || remapped(hi::kR2);
            if (!words && !language)
                return 0u;
            const std::string atlas = fontAtlasName(rdram, font);
            const bool buttonFont = atlas == "fnt_ps2buttons_t0.zmp";
            std::string changed = original;
            // Another language: the text's translation (the button font's
            // characters are pictures, never words), in letters the font has.
            if (language && !buttonFont)
            {
                if (std::string translated = aggressive_inline::language::translate(original); !translated.empty())
                    changed = std::move(translated);
                changed = withGlyphs(rdram, font, changed);
            }
            if (words)
                changed = wordsFor(changed, device, buttonFont);
            if (changed == original || changed.size() + 1u > kTextBufferSize)
                return 0u;
            const uint32_t buffer = textBuffers().base + kTextBufferSize * static_cast<uint32_t>(textBuffers().next);
            textBuffers().next = (textBuffers().next + 1) % kTextBufferCount;
            for (size_t i = 0; i <= changed.size(); ++i)
                if (uint8_t *p = getMemPtr(rdram, buffer + static_cast<uint32_t>(i)))
                    *p = i < changed.size() ? static_cast<uint8_t>(changed[i]) : 0u;
            return buffer;
        }

        // Both primitives can yield to the scheduler (they flush GS
        // packets), so they are not called as nested guest calls: the text
        // register is changed and the original runs in the same frame (a
        // tail call). Its string stays in one of the buffers meanwhile.
        // Mouse wheel and click in menus: anything but play (game mode 2:
        // the front end, the pause menu, tip and message boxes).
        // In the park editor (its editor object g_parkEditorCore, set while
        // it runs) the wheel zooms the camera: the right stick's Y axis there.
        void noteMenus(const uint8_t *rdram)
        {
            const bool play = readWord(rdram, sym::g_gameMode) == 2u;
            PSPadBackend::setMenuMouse(!play);
            PSPadBackend::setWheelStick(play && readWord(rdram, sym::g_parkEditorCore) != 0u);
        }

        void textLineHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            zp::refresh(rdram, runtime);
            noteMenus(rdram);
            if (const uint32_t changed = textFor(rdram, getRegU32(ctx, 4), getRegU32(ctx, 6)))
                SET_GPR_U32(ctx, 6, changed);
            textHooks().origLine(rdram, ctx, runtime);
        }

        void textWrappedHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            zp::refresh(rdram, runtime);
            noteMenus(rdram);
            if (const uint32_t changed = textFor(rdram, getRegU32(ctx, 4), getRegU32(ctx, 6)))
                SET_GPR_U32(ctx, 6, changed);
            textHooks().origWrapped(rdram, ctx, runtime);
        }
    }

    void installPrompts(PS2Machine &runtime)
    {
        using namespace prompts;
        textBuffers().base = runtime.reserveAsyncCallbackStack(static_cast<uint32_t>(kTextBufferCount) * kTextBufferSize, 16u);
        textHooks().origLine = runtime.lookupFunction(sym::drawText);
        textHooks().origWrapped = runtime.lookupFunction(sym::drawWrappedText);
        if (!textBuffers().base || !textHooks().origLine || !textHooks().origWrapped || !zp::install(runtime, kGame))
        {
            std::cerr << "[prompts] not installed" << std::endl;
            return;
        }
        runtime.replaceFunction(sym::drawText, &textLineHook);
        runtime.replaceFunction(sym::drawWrappedText, &textWrappedHook);
        // The Tutorial's loading screens show the pad (tutorialScreen).
        const bool xboxScreens = ps2_xbox_disc::init() && ps2_xbox_disc::exists("Loading/TUT/TUT0.RAW");
        zx::game_files::registerProvider(
            "ASSETS/LOADING/TUT/", [xboxScreens](const std::string &path) { return tutorialScreen(path, xboxScreens); },
            "pad loading screens");
        zx::session::onEnd([]() { zx::game_files::removeProviders("pad loading screens"); });
    }
}
