#pragma once

// Small pictures for on-screen button prompts, drawn at run time: keyboard
// key caps, mouse buttons and gamepad shoulder/menu buttons, with labels in
// one of the game's own fonts (zaxis_ui.h LibraryFont), and the palette
// reduction that puts them into a game's 8-bit textures. No game art lives
// here: the fonts and any button pictures come from the user's disc.
//
// Pixels are RGBA8 with R in the low byte and alpha 0-255, top row first.

#include "runtime/zaxis_ui.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zx::glyphs
{
    constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255u)
    {
        return (r & 0xFFu) | (g & 0xFFu) << 8 | (b & 0xFFu) << 16 | (a & 0xFFu) << 24;
    }

    struct Image
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> pixels;

        Image() = default;
        Image(int w, int h, uint32_t fill = 0u);
        uint32_t at(int x, int y) const;
        // Source-over of `color` with this much of the pixel covered (0-1).
        void blend(int x, int y, uint32_t color, float coverage);
    };

    // Shapes with smooth edges (4x4 samples per pixel). Coordinates are in
    // pixels, (0, 0) the top-left corner of the top-left pixel.
    void fillRoundRect(Image &image, float x0, float y0, float x1, float y1, float radius, uint32_t color);
    void fillEllipse(Image &image, float cx, float cy, float rx, float ry, uint32_t color);
    void fillPolygon(Image &image, const std::vector<float> &xy, uint32_t color); // x0 y0 x1 y1 ...
    void strokeLine(Image &image, float x0, float y0, float x1, float y1, float width, uint32_t color);
    // An ellipse turned by `angle` radians (clockwise on screen).
    void fillRotatedEllipse(Image &image, float cx, float cy, float rx, float ry, float angle, uint32_t color);
    // A quadratic curve from (x0, y0) bent towards (cx, cy) to (x1, y1).
    void strokeCurve(Image &image, float x0, float y0, float cx, float cy, float x1, float y1, float width,
                     uint32_t color);

    // Slightly 3D shapes, as the game's own pictures: draw the shape into
    // an image of the same size (any opaque colour; its alpha is the
    // coverage), then paint it with a vertical gradient over the shape's
    // height and a bevel: edges facing the upper left lighter, those facing
    // the lower right darker, `bevel` pixels deep.
    struct Shading
    {
        uint32_t top = 0u;
        uint32_t bottom = 0u;
        float bevel = 1.5f;
        float light = 0.35f; // how far lit edges go towards white (0-1)
        float shade = 0.4f;  // how far shaded edges go towards black
    };
    void paintShaded(Image &dst, const Image &shape, const Shading &shading);
    // The shape's soft shadow (its coverage blurred over `radius` pixels),
    // moved by (dx, dy).
    void paintShadow(Image &dst, const Image &shape, float dx, float dy, int radius, uint32_t color);
    // Blurs every channel over `radius` pixels (three box blurs each way;
    // the edges repeat).
    void blur(Image &image, int radius);

    // src (or its sub-rectangle) scaled into the rectangle, bilinear,
    // source-over.
    void drawImage(Image &dst, const Image &src, float x, float y, float w, float h);

    // Text in a game font: the light fill of its glyphs (alpha times
    // brightness; the dark outline is left out), centred on (cx, cy), the
    // line `height` pixels tall, narrowed to maxWidth if wider (0: no
    // limit). Characters the font lacks are skipped.
    struct Font
    {
        int lineHeight = 0;
        std::vector<zaxis::FontGlyph> glyphs;
        Image atlas;
        bool empty() const { return glyphs.empty() || atlas.pixels.empty(); }
    };
    Font fontFrom(const zaxis::LibraryFont &font);
    float textWidth(const Font &font, std::string_view text, float height);
    void drawText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                  uint32_t color, float maxWidth = 0.0f);
    // Bolder, for small labels (strokes thickened by up to half a pixel).
    void drawBoldText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                      uint32_t color, float maxWidth = 0.0f);
    // The same with a dark outline `outline` pixels wide around the
    // letters (the look of the game's marker-pen captions).
    void drawOutlinedText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                          uint32_t color, uint32_t outlineColor, float outline);

    // ---- prompt pictures, drawn into a rectangle of an image ----------------

    // A keyboard key with its label: a single character large, a longer
    // name smaller; "Space", "Enter", "Up"/"Down"/"Left"/"Right" and
    // "Backspace" as symbols. `pressed`: drawn held down, its top tinted.
    void drawKeycap(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label,
                    bool pressed = false);
    // A mouse with one button lit: 0 left, 1 right, 2 middle (wheel), 3+ side.
    void drawMouse(Image &image, float x, float y, float w, float h, int button);
    // A dark rounded button with a light label (gamepad shoulders and menu
    // buttons: "LB", "RT", ...); `pill` rounds it fully; `pressed` tints it.
    void drawPadLabel(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label,
                      bool pill, bool pressed = false);
    // A gamepad's d-pad cross in a dark circle.
    void drawDpad(Image &image, float x, float y, float w, float h);

    // Nintendo-style pad buttons in the look of the game's own button
    // pictures (round dark buttons, grey shoulder keys, slight 3D shading).
    // A round button with a light letter; "+" and "-" are drawn as the
    // symbols.
    void drawRoundButton(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label);
    // A grey shoulder or trigger key with a dark label ("L", "ZR").
    void drawShoulderButton(Image &image, float x, float y, float w, float h, const Font &font,
                            const std::string &label, bool pressed = false);
    // A grey d-pad cross.
    void drawGreyDpad(Image &image, float x, float y, float w, float h);

    // ---- 8-bit palettes ------------------------------------------------------

    struct Indexed
    {
        std::vector<uint8_t> indices;
        std::vector<uint32_t> palette;
    };
    // At most maxColors colours (median cut), each pixel the nearest one.
    Indexed quantize(const Image &image, size_t maxColors);
    // The nearest colour of a palette (alpha weighs like a colour channel;
    // fully transparent colours all match each other).
    size_t nearest(const std::vector<uint32_t> &palette, uint32_t color);

    // PS2 CLUT storage of a 256-colour palette (CSM1): entries 8-15 and
    // 16-23 of every 32 swap places. Its own inverse.
    inline size_t clutSlot(size_t index)
    {
        return (index & ~size_t{0x18}) | ((index & 0x8u) << 1) | ((index & 0x10u) >> 1);
    }
}
