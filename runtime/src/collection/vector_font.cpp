// The shipped font drawn into the menu's bitmap font (collection/vector_font.h).
#include "collection/vector_font.h"

// stb_truetype v1.28 (public domain / MIT), its functions local to this file
// (the ones not called here are not warned about).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace ps2x::collection
{
    bool bakeFont(const std::vector<uint8_t> &ttf, float pixelHeight, BitmapFont &out)
    {
        if (ttf.empty() || pixelHeight <= 0.0f)
            return false;
        const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
        stbtt_fontinfo info;
        if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset))
            return false;
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        if (ascent - descent <= 0)
            return false;
        const float scale = pixelHeight / static_cast<float>(ascent - descent);
        const int baseline = static_cast<int>(std::lround(ascent * scale));
        const int lineHeight = static_cast<int>(std::ceil(pixelHeight));

        // Each character's cell: its advance less the pixel Canvas::text adds.
        constexpr int kFirst = 0x20, kLast = 0x7E, kPageWidth = 1024;
        struct Cell
        {
            int glyph, width, bearing;
        };
        std::vector<Cell> cells;
        int x = 0, rows = 1;
        for (int c = kFirst; c <= kLast; ++c)
        {
            const int glyph = stbtt_FindGlyphIndex(&info, c);
            int advance = 0, bearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyph, &advance, &bearing);
            const int width = std::max(1, static_cast<int>(std::lround(advance * scale)) - 1);
            if (x + width > kPageWidth)
            {
                x = 0;
                ++rows;
            }
            cells.push_back({glyph, width, static_cast<int>(std::lround(bearing * scale))});
            x += width + 1;
        }

        BitmapFont font;
        font.height = lineHeight;
        font.page.width = kPageWidth;
        font.page.height = rows * (lineHeight + 1);
        font.page.pixels.assign(static_cast<size_t>(font.page.width) * font.page.height, rgba(255, 255, 255, 0));
        std::vector<uint8_t> coverage;
        int u = 0, v = 0;
        for (size_t i = 0; i < cells.size(); ++i)
        {
            const Cell &cell = cells[i];
            if (u + cell.width > kPageWidth)
            {
                u = 0;
                v += lineHeight + 1;
            }
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&info, cell.glyph, scale, scale, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            if (w > 0 && h > 0)
            {
                coverage.assign(static_cast<size_t>(w) * h, 0);
                stbtt_MakeGlyphBitmap(&info, coverage.data(), w, h, w, scale, scale, cell.glyph);
                // Into the cell, at the glyph's bearing and baseline, cut to the cell.
                for (int gy = 0; gy < h; ++gy)
                {
                    const int py = baseline + y0 + gy;
                    if (py < 0 || py >= lineHeight)
                        continue;
                    for (int gx = 0; gx < w; ++gx)
                    {
                        const int px = x0 + gx;
                        if (px < 0 || px >= cell.width)
                            continue;
                        const uint8_t a = coverage[static_cast<size_t>(gy) * w + gx];
                        uint32_t &pixel = font.page.pixels[static_cast<size_t>(v + py) * font.page.width + (u + px)];
                        pixel = rgba(255, 255, 255, std::max<uint8_t>(a, static_cast<uint8_t>(pixel >> 24)));
                    }
                }
            }
            BitmapFont::Glyph &glyph = font.glyphs[static_cast<size_t>(kFirst + i)];
            glyph.present = true;
            glyph.top = 0;
            glyph.u = u;
            glyph.v = v;
            glyph.width = cell.width;
            glyph.height = lineHeight;
            u += cell.width + 1;
        }
        out = std::move(font);
        return true;
    }
}
