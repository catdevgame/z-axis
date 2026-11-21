#pragma once

// Software drawing on a Surface: all the panel needs, nothing more. Colours
// are 0xAABBGGRR (bytes R, G, B, A in memory).

#include "collection/panel.h"
#include "collection/panel_file.h"

#include <cstdint>
#include <string_view>

namespace panel
{
    constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255)
    {
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    class Canvas
    {
    public:
        explicit Canvas(const Surface &surface) : s_(surface) {}
        // Drawing outside `clip` does nothing.
        void setClip(const Rect &clip);
        void clearClip();

        void fill(const Rect &rect, uint32_t colour);        // blends by the colour's alpha
        void roundedRect(const Rect &rect, float radius, uint32_t colour); // the same with round corners
        void disc(Point center, float radius, uint32_t colour);
        void ring(Point center, float radius, float width, uint32_t colour);
        void line(Point a, Point b, float width, uint32_t colour);
        void triangle(Point a, Point b, Point c, uint32_t colour);
        // An image scaled to `rect`, bilinear, multiplied by `tint`.
        void image(const Image &image, const Rect &rect, uint32_t tint = 0xFFFFFFFFu);
        // Part of an image (source in its pixels).
        void imagePart(const Image &image, const Rect &source, const Rect &rect, uint32_t tint);
        // The map (RGB565) as `view` shows it over `pane`: the mip that fits, bilinear.
        void map(const LevelFile &level, const MapView &view, const Rect &pane);

        // Returns the text's width. `scale` multiplies the font's pixels;
        // `tracking` is added after each glyph (letter-spaced caps).
        float text(const Font &font, std::string_view text, Point at, float scale, uint32_t colour, float tracking = 0.0f);
        static float textWidth(const Font &font, std::string_view text, float scale, float tracking = 0.0f);

    private:
        Surface s_;
        int clipX0_ = 0, clipY0_ = 0, clipX1_ = 0, clipY1_ = 0;
        bool hasClip_ = false;
        void bounds(int &x0, int &y0, int &x1, int &y1) const;
    };
}
