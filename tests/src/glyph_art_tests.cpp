#include "MiniTest.h"
#include "runtime/glyph_art.h"
#include "runtime/host_input.h"
#include "runtime/pad_art.h"
#include "runtime/zaxis_ui.h"

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace gl = zx::glyphs;
namespace hi = ps2_host_input;

namespace
{
    void putU32(std::vector<uint8_t> &out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void putChunk(std::vector<uint8_t> &out, const char *tag, const std::vector<uint8_t> &data)
    {
        out.insert(out.end(), tag, tag + 4);
        putU32(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    }

    // A 2x2 8-bit BMP: red, green / blue, transparent (bottom-up rows).
    std::vector<uint8_t> tinyBmp()
    {
        std::vector<uint8_t> b = {'B', 'M'};
        putU32(b, 0);              // file size (unused)
        putU32(b, 0);              // reserved
        putU32(b, 54u + 1024u);    // pixel offset
        putU32(b, 40);             // header size
        putU32(b, 2);              // width
        putU32(b, 2);              // height (bottom-up)
        b.push_back(1), b.push_back(0);
        b.push_back(8), b.push_back(0);
        for (int i = 0; i < 6; ++i)
            putU32(b, 0);
        std::vector<uint8_t> palette(1024, 0);
        const uint8_t bgra[4][4] = {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {0, 0, 0, 0}};
        for (int i = 0; i < 4; ++i)
            std::memcpy(palette.data() + 4 * i, bgra[i], 4);
        b.insert(b.end(), palette.begin(), palette.end());
        // Bottom row first: blue, transparent; then red, green. Rows padded to 4.
        b.insert(b.end(), {2, 3, 0, 0, 0, 1, 0, 0});
        return b;
    }
}

void register_glyph_art_tests()
{
    MiniTest::Case("GlyphArt", [](TestCase &tc)
    {
        tc.Run("CLUT slots: CSM1 swaps 8-15 and 16-23 of every 32, its own inverse", [](TestCase &t)
        {
            t.Equals(gl::clutSlot(8), static_cast<size_t>(16), "8 -> 16");
            t.Equals(gl::clutSlot(23), static_cast<size_t>(15), "23 -> 15");
            t.Equals(gl::clutSlot(40), static_cast<size_t>(48), "next block");
            t.Equals(gl::clutSlot(5), static_cast<size_t>(5), "0-7 stay");
            bool inverse = true;
            for (size_t i = 0; i < 256; ++i)
                inverse = inverse && gl::clutSlot(gl::clutSlot(i)) == i;
            t.IsTrue(inverse, "inverse");
        });

        tc.Run("quantize: exact with few colours, bounded with many", [](TestCase &t)
        {
            gl::Image small(4, 1);
            small.pixels = {gl::rgba(255, 0, 0), gl::rgba(0, 255, 0), gl::rgba(255, 0, 0), 0u};
            const gl::Indexed exact = gl::quantize(small, 16);
            t.Equals(exact.palette.size(), static_cast<size_t>(3), "three colours");
            t.Equals(exact.palette[exact.indices[0]], gl::rgba(255, 0, 0), "red kept");
            t.Equals(exact.indices[0], exact.indices[2], "same colour, same index");
            gl::Image ramp(64, 64);
            for (int i = 0; i < 64 * 64; ++i)
                ramp.pixels[static_cast<size_t>(i)] = gl::rgba(static_cast<uint32_t>(i & 255), static_cast<uint32_t>(i >> 4), 77u);
            const gl::Indexed reduced = gl::quantize(ramp, 16);
            t.IsTrue(reduced.palette.size() <= 16u, "at most 16 colours");
            std::set<uint8_t> used(reduced.indices.begin(), reduced.indices.end());
            t.IsTrue(used.size() > 8u, "most of them used");
        });

        tc.Run("drawing stays inside the image and covers the shape", [](TestCase &t)
        {
            gl::Image image(32, 32);
            gl::Font none;
            gl::drawKeycap(image, 2, 2, 28, 24, none, "Space");
            gl::Image mouse(16, 16);
            gl::drawMouse(mouse, -10, -10, 60, 60, 1); // clipped, no writes outside
            t.IsTrue((image.at(16, 14) >> 24) == 255u, "key cap face opaque");
            t.Equals(image.at(0, 31) >> 24, 0u, "corner outside the cap left clear");
        });

        tc.Run("library reader: BMP materials and their names", [](TestCase &t)
        {
            std::vector<uint8_t> txrh(0x34, 0);
            std::memcpy(txrh.data() + 0x14, "select_button", 13);
            std::vector<uint8_t> txrc, umtl, uast, hast, ulib, root;
            putChunk(txrc, "TXRH", txrh);
            putChunk(txrc, "TZMP", tinyBmp());
            putChunk(umtl, "HMTL", std::vector<uint8_t>(4, 0));
            putChunk(umtl, "TXRC", txrc);
            const std::string source = "S:\\\\Art\\\\btn_x.zmp";
            hast.assign(source.begin(), source.end());
            hast.push_back(0);
            putChunk(uast, "HAST", hast);
            putChunk(uast, "UMTL", umtl);
            putChunk(ulib, "HLIB", std::vector<uint8_t>(8, 0));
            putChunk(ulib, "UAST", uast);
            putChunk(root, "ULIB", ulib);
            std::vector<zx::zaxis::LibraryTexture> textures;
            t.IsTrue(zx::zaxis::readLibrary(root, &textures), "a ULIB");
            t.Equals(textures.size(), static_cast<size_t>(1), "one texture");
            t.Equals(textures[0].name, std::string("select_button"), "TXRH name");
            t.Equals(zx::zaxis::sourceFileName(textures[0].source), std::string("btn_x.zmp"), "file of the source path");
            t.Equals(textures[0].rgba[0], gl::rgba(255, 0, 0), "top-left red");
            t.Equals(textures[0].rgba[2], gl::rgba(0, 0, 255), "bottom-left blue");
            t.Equals(textures[0].rgba[3] >> 24, 0u, "transparent kept");
        });

        tc.Run("key labels for prompts", [](TestCase &t)
        {
            t.Equals(hi::hostCodeLabel(hi::hostCodeByName("SPACE")), std::string("Space"), "Space");
            t.Equals(hi::hostCodeLabel(hi::hostCodeByName("K")), std::string("K"), "a letter as it is");
            t.Equals(hi::hostCodeLabel(hi::hostCodeByName("MOUSE_RIGHT")), std::string("Right Click"), "mouse");
            t.Equals(hi::hostCodeLabel(hi::hostCodeByName("ESCAPE")), std::string("Esc"), "Esc");
        });

        tc.Run("shaded shapes: gradient top to bottom, lit upper-left edge, soft offset shadow", [](TestCase &t)
        {
            gl::Image shape(40, 40);
            gl::fillRoundRect(shape, 10.0f, 10.0f, 30.0f, 30.0f, 0.0f, gl::rgba(255, 255, 255));
            gl::Image out(40, 40);
            gl::paintShaded(out, shape, {gl::rgba(200, 200, 200), gl::rgba(40, 40, 40), 2.0f, 0.5f, 0.5f});
            t.IsTrue((out.at(20, 12) & 0xFFu) > (out.at(20, 27) & 0xFFu), "lighter at the top");
            t.IsTrue((out.at(10, 20) & 0xFFu) > (out.at(14, 20) & 0xFFu), "the left edge lit");
            t.IsTrue((out.at(29, 20) & 0xFFu) < (out.at(25, 20) & 0xFFu), "the right edge shaded");
            t.Equals(out.at(5, 5) >> 24, 0u, "nothing outside");
            gl::Image shadow(40, 40);
            gl::paintShadow(shadow, shape, 4.0f, 4.0f, 2, gl::rgba(0, 0, 0));
            t.IsTrue((shadow.at(32, 32) >> 24) > 0u, "below right of the shape");
            t.Equals(shadow.at(8, 8) >> 24, 0u, "not above left");
            t.IsTrue((shadow.at(20, 20) >> 24) == 255u, "solid under the middle");
        });

        tc.Run("blur keeps a flat image and spreads a dot", [](TestCase &t)
        {
            gl::Image flat(16, 16, gl::rgba(100, 150, 200));
            gl::blur(flat, 3);
            t.Equals(flat.at(0, 0), gl::rgba(100, 150, 200), "edge unchanged");
            t.Equals(flat.at(8, 8), gl::rgba(100, 150, 200), "middle unchanged");
            gl::Image dot(21, 21, gl::rgba(0, 0, 0));
            dot.pixels[10 * 21 + 10] = gl::rgba(255, 255, 255);
            gl::blur(dot, 2);
            t.IsTrue((dot.at(10, 10) & 0xFFu) < 255u && (dot.at(12, 10) & 0xFFu) > 0u, "spread");
            t.Equals(dot.at(10, 12) & 0xFFu, dot.at(12, 10) & 0xFFu, "the same each way");
        });

        tc.Run("a turned ellipse and a curve", [](TestCase &t)
        {
            gl::Image upright(40, 40);
            gl::fillRotatedEllipse(upright, 20.0f, 20.0f, 4.0f, 16.0f, 0.0f, gl::rgba(255, 255, 255));
            gl::Image turned(40, 40);
            gl::fillRotatedEllipse(turned, 20.0f, 20.0f, 4.0f, 16.0f, 1.5707963f, gl::rgba(255, 255, 255));
            t.IsTrue((upright.at(20, 6) >> 24) > 0u && (upright.at(34, 20) >> 24) == 0u, "tall");
            t.IsTrue((turned.at(34, 20) >> 24) > 0u && (turned.at(20, 6) >> 24) == 0u, "wide when turned");
            gl::Image curve(40, 40);
            gl::strokeCurve(curve, 2.0f, 30.0f, 20.0f, 0.0f, 38.0f, 30.0f, 3.0f, gl::rgba(255, 255, 255));
            t.IsTrue((curve.at(20, 15) >> 24) > 0u, "bent towards the control point (half way)");
            t.Equals(curve.at(20, 30) >> 24, 0u, "not the straight line");
        });

        tc.Run("gamepad layout: controls by place, apart, inside the picture", [](TestCase &t)
        {
            using C = gl::PadControl;
            const float x = 10.0f, y = 20.0f, w = 320.0f, h = 200.0f;
            const gl::Point south = gl::padAnchor(C::South, x, y, w, h), north = gl::padAnchor(C::North, x, y, w, h);
            const gl::Point west = gl::padAnchor(C::West, x, y, w, h), east = gl::padAnchor(C::East, x, y, w, h);
            t.IsTrue(north.y < south.y && west.x < east.x, "the face buttons' places");
            t.IsTrue(gl::padAnchor(C::LeftStick, x, y, w, h).x < gl::padAnchor(C::RightStick, x, y, w, h).x, "sticks");
            t.IsTrue(gl::padAnchor(C::Select, x, y, w, h).x < gl::padAnchor(C::Start, x, y, w, h).x, "- before +");
            t.IsTrue(gl::padAnchor(C::L2, x, y, w, h).y < gl::padAnchor(C::L1, x, y, w, h).y, "triggers behind");
            bool apart = true, inside = true;
            for (int i = 0; i < static_cast<int>(C::Count); ++i)
            {
                const gl::Point a = gl::padAnchor(static_cast<C>(i), x, y, w, h);
                const float ra = gl::padControlRadius(static_cast<C>(i), x, y, w, h);
                inside = inside && a.x - ra >= x && a.x + ra <= x + w && a.y - ra >= y && a.y + ra <= y + h;
                for (int j = i + 1; j < static_cast<int>(C::Count); ++j)
                {
                    const gl::Point b = gl::padAnchor(static_cast<C>(j), x, y, w, h);
                    const float rb = gl::padControlRadius(static_cast<C>(j), x, y, w, h);
                    apart = apart && std::hypot(a.x - b.x, a.y - b.y) >= ra + rb;
                }
            }
            t.IsTrue(apart, "no two controls overlap");
            t.IsTrue(inside, "all inside the rectangle");
            t.Equals(gl::padControlLabel(C::South, gl::PadStyle::Nintendo), std::string("B"), "Nintendo south B");
            t.Equals(gl::padControlLabel(C::North, gl::PadStyle::Nintendo), std::string("X"), "Nintendo north X");
            t.Equals(gl::padControlLabel(C::South, gl::PadStyle::Xbox), std::string("A"), "Xbox south A");
            t.Equals(gl::padControlLabel(C::R2, gl::PadStyle::Nintendo), std::string("ZR"), "ZR");
            t.Equals(gl::padControlLabel(C::Start, gl::PadStyle::Nintendo), std::string("+"), "+");
            gl::Image image(400, 300);
            gl::drawGamepad(image, x, y, w, h, gl::PadStyle::Nintendo, gl::Font{});
            bool outside = false;
            const float pw = std::min(w, h * gl::kGamepadAspect), ph = pw / gl::kGamepadAspect;
            const float x0 = x + (w - pw) * 0.5f, y0 = y + (h - ph) * 0.5f;
            for (int py = 0; py < image.height; ++py)
                for (int px = 0; px < image.width; ++px)
                    if ((image.at(px, py) >> 24) > 8u &&
                        (px < x0 - 8.0f || px > x0 + pw + 8.0f || py < y0 - 8.0f || py > y0 + ph + 8.0f))
                        outside = true;
            t.IsFalse(outside, "drawn within its fitted rectangle (and its shadow)");
            t.IsTrue((image.at(static_cast<int>(south.x), static_cast<int>(south.y)) >> 24) == 255u, "a button there");
        });

        tc.Run("arrows and pressed prompts", [](TestCase &t)
        {
            gl::Image image(60, 60);
            gl::drawArrow(image, {5.0f, 50.0f}, {30.0f, 50.0f}, {50.0f, 20.0f}, 3.0f, gl::rgba(255, 0, 0), gl::rgba(0, 0, 0));
            const uint32_t tip = image.at(48, 23);
            t.IsTrue((tip >> 24) > 0u && (tip & 0xFFu) > 128u, "the head at the end, in the colour");
            t.IsTrue((image.at(5, 50) >> 24) > 0u, "the start");
            gl::Image up(32, 20), down(32, 20);
            gl::drawKeycap(up, 0.0f, 0.0f, 32.0f, 20.0f, gl::Font{}, "Q", false);
            gl::drawKeycap(down, 0.0f, 0.0f, 32.0f, 20.0f, gl::Font{}, "Q", true);
            t.IsTrue(up.pixels != down.pixels, "a held key looks held");
            t.IsTrue(((down.at(16, 10) >> 16) & 0xFFu) > (down.at(16, 10) & 0xFFu), "tinted blue");
        });
    });
}
