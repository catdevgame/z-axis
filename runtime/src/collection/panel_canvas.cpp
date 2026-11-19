#include "collection/panel_canvas.h"

#include <algorithm>
#include <cmath>

namespace panel
{
    namespace
    {
        inline uint32_t blend(uint32_t under, uint32_t over, uint32_t alpha)
        {
            // alpha 0 .. 256, both colours' channels; the result opaque
            const uint32_t rbUnder = under & 0x00FF00FFu, gUnder = (under >> 8) & 0xFFu;
            const uint32_t rbOver = over & 0x00FF00FFu, gOver = (over >> 8) & 0xFFu;
            const uint32_t rb = (rbUnder * (256u - alpha) + rbOver * alpha) >> 8;
            const uint32_t g = (gUnder * (256u - alpha) + gOver * alpha) >> 8;
            return 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
        }

        inline uint32_t expand565(uint16_t c) { return (c | (uint32_t(c) << 16)) & 0x07E0F81Fu; }
        inline uint32_t rgb565ToRgba(uint32_t packed)
        {
            // packed: 5 of blue low, 6 of green from bit 21, 5 of red from bit 11
            const uint32_t c = (packed | (packed >> 16)) & 0xFFFFu;
            const uint32_t r = (c >> 8) & 0xF8u, g = (c >> 3) & 0xFCu, b = (c << 3) & 0xF8u;
            return 0xFF000000u | (r | (r >> 5)) | ((g | (g >> 6)) << 8) | ((b | (b >> 5)) << 16);
        }
    }

    void Canvas::setClip(const Rect &clip)
    {
        hasClip_ = true;
        clipX0_ = static_cast<int>(clip.x);
        clipY0_ = static_cast<int>(clip.y);
        clipX1_ = static_cast<int>(clip.x + clip.width);
        clipY1_ = static_cast<int>(clip.y + clip.height);
    }

    void Canvas::clearClip() { hasClip_ = false; }

    void Canvas::bounds(int &x0, int &y0, int &x1, int &y1) const
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, s_.width);
        y1 = std::min(y1, s_.height);
        if (hasClip_)
        {
            x0 = std::max(x0, clipX0_);
            y0 = std::max(y0, clipY0_);
            x1 = std::min(x1, clipX1_);
            y1 = std::min(y1, clipY1_);
        }
    }

    void Canvas::fill(const Rect &rect, uint32_t colour)
    {
        int x0 = static_cast<int>(rect.x), y0 = static_cast<int>(rect.y);
        int x1 = static_cast<int>(rect.x + rect.width), y1 = static_cast<int>(rect.y + rect.height);
        bounds(x0, y0, x1, y1);
        const uint32_t a = colour >> 24;
        const uint32_t alpha = a + (a >> 7);
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            if (alpha >= 256)
                std::fill(row + x0, row + x1, colour);
            else
                for (int x = x0; x < x1; ++x)
                    row[x] = blend(row[x], colour, alpha);
        }
    }

    void Canvas::roundedRect(const Rect &rect, float radius, uint32_t colour)
    {
        const float r = std::clamp(radius, 0.0f, std::min(rect.width, rect.height) * 0.5f);
        if (r < 1.0f)
        {
            fill(rect, colour);
            return;
        }
        int x0 = static_cast<int>(rect.x), y0 = static_cast<int>(rect.y);
        int x1 = static_cast<int>(rect.x + rect.width), y1 = static_cast<int>(rect.y + rect.height);
        bounds(x0, y0, x1, y1);
        const uint32_t a8 = colour >> 24;
        const uint32_t alpha = a8 + (a8 >> 7);
        const float a = static_cast<float>(a8) / 255.0f;
        // the box the corner circles' centres lie on
        const float cx0 = rect.x + r, cx1 = rect.x + rect.width - r;
        const float cy0 = rect.y + r, cy1 = rect.y + rect.height - r;
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            const float py = static_cast<float>(y) + 0.5f;
            const bool band = py < cy0 || py > cy1;   // a row through the corners
            if (!band)
            {
                if (alpha >= 256)
                    std::fill(row + x0, row + x1, colour);
                else
                    for (int x = x0; x < x1; ++x)
                        row[x] = blend(row[x], colour, alpha);
                continue;
            }
            const float dy = py < cy0 ? cy0 - py : py - cy1;
            for (int x = x0; x < x1; ++x)
            {
                const float px = static_cast<float>(x) + 0.5f;
                const float dx = px < cx0 ? cx0 - px : px > cx1 ? px - cx1 : 0.0f;
                const float d = std::hypot(dx, dy) - r;
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f) * a;
                if (cover > 0)
                    row[x] = blend(row[x], colour, static_cast<uint32_t>(cover * 256.0f));
            }
        }
    }

    void Canvas::disc(Point c, float radius, uint32_t colour)
    {
        int x0 = static_cast<int>(c.x - radius - 1), y0 = static_cast<int>(c.y - radius - 1);
        int x1 = static_cast<int>(c.x + radius + 2), y1 = static_cast<int>(c.y + radius + 2);
        bounds(x0, y0, x1, y1);
        const float a = static_cast<float>(colour >> 24) / 255.0f;
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            for (int x = x0; x < x1; ++x)
            {
                const float d = std::hypot(static_cast<float>(x) + 0.5f - c.x, static_cast<float>(y) + 0.5f - c.y);
                const float cover = std::clamp(radius + 0.5f - d, 0.0f, 1.0f) * a;
                if (cover > 0)
                    row[x] = blend(row[x], colour, static_cast<uint32_t>(cover * 256.0f));
            }
        }
    }

    void Canvas::ring(Point c, float radius, float width, uint32_t colour)
    {
        int x0 = static_cast<int>(c.x - radius - width - 1), y0 = static_cast<int>(c.y - radius - width - 1);
        int x1 = static_cast<int>(c.x + radius + width + 2), y1 = static_cast<int>(c.y + radius + width + 2);
        bounds(x0, y0, x1, y1);
        const float a = static_cast<float>(colour >> 24) / 255.0f;
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            for (int x = x0; x < x1; ++x)
            {
                const float d = std::hypot(static_cast<float>(x) + 0.5f - c.x, static_cast<float>(y) + 0.5f - c.y);
                const float cover = std::clamp(width * 0.5f + 0.5f - std::fabs(d - radius), 0.0f, 1.0f) * a;
                if (cover > 0)
                    row[x] = blend(row[x], colour, static_cast<uint32_t>(cover * 256.0f));
            }
        }
    }

    void Canvas::line(Point a, Point b, float width, uint32_t colour)
    {
        int x0 = static_cast<int>(std::min(a.x, b.x) - width - 1), y0 = static_cast<int>(std::min(a.y, b.y) - width - 1);
        int x1 = static_cast<int>(std::max(a.x, b.x) + width + 2), y1 = static_cast<int>(std::max(a.y, b.y) + width + 2);
        bounds(x0, y0, x1, y1);
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float length2 = std::max(dx * dx + dy * dy, 1e-3f);
        const float alpha = static_cast<float>(colour >> 24) / 255.0f;
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            for (int x = x0; x < x1; ++x)
            {
                const float px = static_cast<float>(x) + 0.5f - a.x, py = static_cast<float>(y) + 0.5f - a.y;
                const float t = std::clamp((px * dx + py * dy) / length2, 0.0f, 1.0f);
                const float d = std::hypot(px - t * dx, py - t * dy);
                const float cover = std::clamp(width * 0.5f + 0.5f - d, 0.0f, 1.0f) * alpha;
                if (cover > 0)
                    row[x] = blend(row[x], colour, static_cast<uint32_t>(cover * 256.0f));
            }
        }
    }

    void Canvas::triangle(Point a, Point b, Point c, uint32_t colour)
    {
        int x0 = static_cast<int>(std::min({a.x, b.x, c.x})), y0 = static_cast<int>(std::min({a.y, b.y, c.y}));
        int x1 = static_cast<int>(std::max({a.x, b.x, c.x})) + 2, y1 = static_cast<int>(std::max({a.y, b.y, c.y})) + 2;
        bounds(x0, y0, x1, y1);
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-3f)
            return;
        const float sign = area > 0 ? 1.0f : -1.0f;
        const auto edge = [&](Point p, Point q, float x, float y) {
            const float ex = q.x - p.x, ey = q.y - p.y;
            return sign * (ex * (y - p.y) - ey * (x - p.x)) / std::max(std::hypot(ex, ey), 1e-3f);
        };
        const float alpha = static_cast<float>(colour >> 24) / 255.0f;
        for (int y = y0; y < y1; ++y)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            for (int x = x0; x < x1; ++x)
            {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const float d = std::min({edge(a, b, fx, fy), edge(b, c, fx, fy), edge(c, a, fx, fy)});
                const float cover = std::clamp(d + 0.5f, 0.0f, 1.0f) * alpha;
                if (cover > 0)
                    row[x] = blend(row[x], colour, static_cast<uint32_t>(cover * 256.0f));
            }
        }
    }

    void Canvas::imagePart(const Image &image, const Rect &source, const Rect &rect, uint32_t tint)
    {
        if (image.width == 0 || rect.width <= 0 || rect.height <= 0)
            return;
        int x0 = static_cast<int>(rect.x), y0 = static_cast<int>(rect.y);
        int x1 = static_cast<int>(std::ceil(rect.x + rect.width)), y1 = static_cast<int>(std::ceil(rect.y + rect.height));
        bounds(x0, y0, x1, y1);
        const int32_t stepX = static_cast<int32_t>(source.width / rect.width * 65536.0f);
        const int32_t stepY = static_cast<int32_t>(source.height / rect.height * 65536.0f);
        const int32_t startU = static_cast<int32_t>((source.x + (static_cast<float>(x0) + 0.5f - rect.x) * source.width / rect.width - 0.5f) * 65536.0f);
        int32_t v = static_cast<int32_t>((source.y + (static_cast<float>(y0) + 0.5f - rect.y) * source.height / rect.height - 0.5f) * 65536.0f);
        const uint32_t tr = tint & 255u, tg = (tint >> 8) & 255u, tb = (tint >> 16) & 255u, ta = tint >> 24;
        const uint8_t *px = image.rgba.data();
        const int w = image.width, h = image.height;
        // the source rectangle's own edges, so a font page's neighbours never bleed in
        const int minU = static_cast<int>(source.x), maxU = static_cast<int>(source.x + source.width) - 1;
        const int minV = static_cast<int>(source.y), maxV = static_cast<int>(source.y + source.height) - 1;
        for (int y = y0; y < y1; ++y, v += stepY)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            const int vi = v >> 16, fy = (v >> 8) & 0xFF;
            const int v0 = std::clamp(vi, minV, maxV), v1 = std::clamp(vi + 1, minV, maxV);
            const uint8_t *r0 = px + size_t(std::min(v0, h - 1)) * w * 4;
            const uint8_t *r1 = px + size_t(std::min(v1, h - 1)) * w * 4;
            int32_t u = startU;
            for (int x = x0; x < x1; ++x, u += stepX)
            {
                const int ui = u >> 16, fx = (u >> 8) & 0xFF;
                const int u0 = std::clamp(ui, minU, maxU), u1 = std::clamp(ui + 1, minU, maxU);
                const uint8_t *a = r0 + u0 * 4, *b = r0 + u1 * 4, *c = r1 + u0 * 4, *d = r1 + u1 * 4;
                const uint32_t aa = a[3], ab = b[3], ac = c[3], ad = d[3];
                if ((aa | ab | ac | ad) == 0)
                    continue;
                // premultiplied mix, so a clear texel's colour never fringes
                const uint32_t w0 = (256 - fx) * (256 - fy) >> 8, w1 = fx * (256 - fy) >> 8, w2 = (256 - fx) * fy >> 8, w3 = fx * fy >> 8;
                const uint32_t pa = aa * w0 + ab * w1 + ac * w2 + ad * w3;
                if (pa < 256)
                    continue;
                const uint32_t pr = a[0] * aa * w0 + b[0] * ab * w1 + c[0] * ac * w2 + d[0] * ad * w3;
                const uint32_t pg = a[1] * aa * w0 + b[1] * ab * w1 + c[1] * ac * w2 + d[1] * ad * w3;
                const uint32_t pb = a[2] * aa * w0 + b[2] * ab * w1 + c[2] * ac * w2 + d[2] * ad * w3;
                const uint32_t cr = (pr / pa) * tr >> 8, cg = (pg / pa) * tg >> 8, cb = (pb / pa) * tb >> 8;
                const uint32_t alpha = (pa >> 8) * (ta + 1) >> 8;
                row[x] = blend(row[x], cr | (cg << 8) | (cb << 16), alpha + (alpha >> 7));
            }
        }
    }

    void Canvas::image(const Image &image, const Rect &rect, uint32_t tint)
    {
        imagePart(image, {0, 0, static_cast<float>(image.width), static_cast<float>(image.height)}, rect, tint);
    }

    void Canvas::map(const LevelFile &level, const MapView &view, const Rect &pane)
    {
        if (level.mips.empty())
            return;
        size_t mip = 0;
        float zoom = view.zoom;
        while (mip + 1 < level.mips.size() && zoom < 0.6f)
        {
            ++mip;
            zoom *= 2.0f;
        }
        const MapLevel &m = level.mips[mip];
        const float scale = 1.0f / static_cast<float>(1u << mip);
        int x0 = static_cast<int>(pane.x), y0 = static_cast<int>(pane.y);
        int x1 = static_cast<int>(pane.x + pane.width), y1 = static_cast<int>(pane.y + pane.height);
        bounds(x0, y0, x1, y1);
        const int32_t step = static_cast<int32_t>(65536.0f / zoom);
        const Point first = screenToMap(view, pane, {static_cast<float>(x0) + 0.5f, static_cast<float>(y0) + 0.5f});
        const int32_t startU = static_cast<int32_t>((first.x * scale - 0.5f) * 65536.0f);
        int32_t v = static_cast<int32_t>((first.y * scale - 0.5f) * 65536.0f);
        const int maxU = m.width - 1, maxV = m.height - 1;
        const uint32_t outside = rgba(10, 13, 19);
        for (int y = y0; y < y1; ++y, v += step)
        {
            uint32_t *row = s_.pixels + size_t(y) * s_.stride;
            const int vi = v >> 16;
            if (vi < 0 || vi >= maxV)
            {
                std::fill(row + x0, row + x1, outside);
                continue;
            }
            const uint32_t fy = (static_cast<uint32_t>(v) >> 11) & 31u;
            const uint16_t *a = m.pixels.data() + size_t(vi) * m.width;
            const uint16_t *b = a + m.width;
            int32_t u = startU;
            for (int x = x0; x < x1; ++x, u += step)
            {
                const int ui = u >> 16;
                if (ui < 0 || ui >= maxU)
                {
                    row[x] = outside;
                    continue;
                }
                const uint32_t fx = (static_cast<uint32_t>(u) >> 11) & 31u;
                const uint32_t top = (expand565(a[ui]) * (32u - fx) + expand565(a[ui + 1]) * fx) >> 5 & 0x07E0F81Fu;
                const uint32_t bottom = (expand565(b[ui]) * (32u - fx) + expand565(b[ui + 1]) * fx) >> 5 & 0x07E0F81Fu;
                const uint32_t mixed = (top * (32u - fy) + bottom * fy) >> 5 & 0x07E0F81Fu;
                row[x] = rgb565ToRgba(mixed);
            }
        }
    }

    float Canvas::textWidth(const Font &font, std::string_view text, float scale, float tracking)
    {
        float width = 0;
        for (char c : text)
        {
            const uint8_t u = static_cast<uint8_t>(c);
            width += font.has[u] ? (static_cast<float>(font.glyphs[u].width) + 1.0f) * scale
                                 : static_cast<float>(font.lineHeight) * 0.35f * scale;
            width += tracking;
        }
        return text.empty() ? 0.0f : width - tracking;
    }

    float Canvas::text(const Font &font, std::string_view text, Point at, float scale, uint32_t colour, float tracking)
    {
        float x = at.x;
        for (char c : text)
        {
            const uint8_t u = static_cast<uint8_t>(c);
            if (!font.has[u])
            {
                x += static_cast<float>(font.lineHeight) * 0.35f * scale + tracking;
                continue;
            }
            const GlyphRecord &g = font.glyphs[u];
            imagePart(font.pages[g.page],
                      {static_cast<float>(g.x), static_cast<float>(g.y), static_cast<float>(g.width), static_cast<float>(g.height)},
                      {x, at.y, static_cast<float>(g.width) * scale, static_cast<float>(g.height) * scale}, colour);
            x += (static_cast<float>(g.width) + 1.0f) * scale + tracking;
        }
        return text.empty() ? 0.0f : x - at.x - tracking;
    }
}
