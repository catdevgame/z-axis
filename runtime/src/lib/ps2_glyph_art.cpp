#include "runtime/ps2_glyph_art.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace zx::glyphs
{
    namespace
    {
        float channel(uint32_t color, int shift) { return static_cast<float>((color >> shift) & 0xFFu); }

        // Coverage of each pixel in the box by `inside`, 4x4 samples.
        void fillShape(Image &image, float x0, float y0, float x1, float y1, uint32_t color,
                       const std::function<bool(float, float)> &inside)
        {
            const int ix0 = std::max(0, static_cast<int>(std::floor(x0)));
            const int iy0 = std::max(0, static_cast<int>(std::floor(y0)));
            const int ix1 = std::min(image.width, static_cast<int>(std::ceil(x1)));
            const int iy1 = std::min(image.height, static_cast<int>(std::ceil(y1)));
            for (int y = iy0; y < iy1; ++y)
                for (int x = ix0; x < ix1; ++x)
                {
                    int hits = 0;
                    for (int sy = 0; sy < 4; ++sy)
                        for (int sx = 0; sx < 4; ++sx)
                            if (inside(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f,
                                       static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f))
                                ++hits;
                    if (hits)
                        image.blend(x, y, color, static_cast<float>(hits) / 16.0f);
                }
        }

        // Premultiplied bilinear sample of an image (outside: transparent).
        std::array<float, 4> sample(const Image &src, float u, float v,
                                    const std::function<std::array<float, 4>(uint32_t)> &texel)
        {
            const float fx = u - 0.5f, fy = v - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
            std::array<float, 4> out{};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const int x = x0 + i, y = y0 + j;
                    if (x < 0 || y < 0 || x >= src.width || y >= src.height)
                        continue;
                    const float w = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
                    const std::array<float, 4> t = texel(src.at(x, y));
                    for (int c = 0; c < 4; ++c)
                        out[static_cast<size_t>(c)] += t[static_cast<size_t>(c)] * w;
                }
            return out;
        }

        // Key names drawn as symbols.
        bool drawKeySymbol(Image &image, const std::string &label, float x, float y, float w, float h, uint32_t ink)
        {
            const float cx = x + w * 0.5f, cy = y + h * 0.5f;
            const float s = std::min(w, h);
            const float line = std::max(1.2f, s * 0.11f);
            if (label == "Space")
            {
                const float half = std::min(w * 0.36f, s * 0.42f);
                const float base = cy + s * 0.12f, top = cy - s * 0.08f;
                strokeLine(image, cx - half, base, cx + half, base, line, ink);
                strokeLine(image, cx - half, base, cx - half, top, line, ink);
                strokeLine(image, cx + half, base, cx + half, top, line, ink);
                return true;
            }
            if (label == "Enter")
            {
                const float right = cx + s * 0.26f, left = cx - s * 0.24f;
                const float top = cy - s * 0.24f, mid = cy + s * 0.10f;
                strokeLine(image, right, top, right, mid, line, ink);
                strokeLine(image, right, mid, left + s * 0.06f, mid, line, ink);
                fillPolygon(image, {left - s * 0.04f, mid, left + s * 0.14f, mid - s * 0.15f, left + s * 0.14f, mid + s * 0.15f}, ink);
                return true;
            }
            if (label == "Backspace")
            {
                const float right = cx + s * 0.26f, left = cx - s * 0.22f;
                strokeLine(image, right, cy, left + s * 0.06f, cy, line, ink);
                fillPolygon(image, {left - s * 0.04f, cy, left + s * 0.14f, cy - s * 0.15f, left + s * 0.14f, cy + s * 0.15f}, ink);
                return true;
            }
            const float a = s * 0.24f;
            if (label == "Up")
            {
                fillPolygon(image, {cx, cy - a, cx + a, cy + a * 0.7f, cx - a, cy + a * 0.7f}, ink);
                return true;
            }
            if (label == "Down")
            {
                fillPolygon(image, {cx, cy + a, cx - a, cy - a * 0.7f, cx + a, cy - a * 0.7f}, ink);
                return true;
            }
            if (label == "Left")
            {
                fillPolygon(image, {cx - a, cy, cx + a * 0.7f, cy - a, cx + a * 0.7f, cy + a}, ink);
                return true;
            }
            if (label == "Right")
            {
                fillPolygon(image, {cx + a, cy, cx - a * 0.7f, cy + a, cx - a * 0.7f, cy - a}, ink);
                return true;
            }
            return false;
        }
    }

    Image::Image(int w, int h, uint32_t fill)
        : width(std::max(0, w)), height(std::max(0, h)),
          pixels(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), fill)
    {
    }

    uint32_t Image::at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0u;
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }

    void Image::blend(int x, int y, uint32_t color, float coverage)
    {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage <= 0.0f)
            return;
        uint32_t &dst = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        const float sa = channel(color, 24) / 255.0f * std::min(coverage, 1.0f);
        const float da = channel(dst, 24) / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        if (oa <= 0.0f)
        {
            dst = 0u;
            return;
        }
        uint32_t out = 0u;
        for (int shift = 0; shift < 24; shift += 8)
        {
            const float c = (channel(color, shift) * sa + channel(dst, shift) * da * (1.0f - sa)) / oa;
            out |= static_cast<uint32_t>(std::clamp(std::lround(c), 0L, 255L)) << shift;
        }
        out |= static_cast<uint32_t>(std::clamp(std::lround(oa * 255.0f), 0L, 255L)) << 24;
        dst = out;
    }

    void fillRoundRect(Image &image, float x0, float y0, float x1, float y1, float radius, uint32_t color)
    {
        radius = std::max(0.0f, std::min(radius, std::min(x1 - x0, y1 - y0) * 0.5f));
        fillShape(image, x0, y0, x1, y1, color, [=](float x, float y)
                  {
                      if (x < x0 || x > x1 || y < y0 || y > y1)
                          return false;
                      const float cx = std::clamp(x, x0 + radius, x1 - radius);
                      const float cy = std::clamp(y, y0 + radius, y1 - radius);
                      return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
                  });
    }

    void fillEllipse(Image &image, float cx, float cy, float rx, float ry, uint32_t color)
    {
        if (rx <= 0.0f || ry <= 0.0f)
            return;
        fillShape(image, cx - rx, cy - ry, cx + rx, cy + ry, color, [=](float x, float y)
                  {
                      const float dx = (x - cx) / rx, dy = (y - cy) / ry;
                      return dx * dx + dy * dy <= 1.0f;
                  });
    }

    void fillPolygon(Image &image, const std::vector<float> &xy, uint32_t color)
    {
        if (xy.size() < 6u)
            return;
        float x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
        for (size_t i = 0; i + 1 < xy.size(); i += 2)
        {
            x0 = std::min(x0, xy[i]);
            x1 = std::max(x1, xy[i]);
            y0 = std::min(y0, xy[i + 1]);
            y1 = std::max(y1, xy[i + 1]);
        }
        fillShape(image, x0, y0, x1, y1, color, [&xy](float x, float y)
                  {
                      bool in = false;
                      const size_t n = xy.size() / 2u;
                      for (size_t i = 0, j = n - 1; i < n; j = i++)
                      {
                          const float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
                          if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                              in = !in;
                      }
                      return in;
                  });
    }

    void strokeLine(Image &image, float x0, float y0, float x1, float y1, float width, uint32_t color)
    {
        const float r = width * 0.5f;
        const float dx = x1 - x0, dy = y1 - y0;
        const float len2 = dx * dx + dy * dy;
        fillShape(image, std::min(x0, x1) - r, std::min(y0, y1) - r, std::max(x0, x1) + r, std::max(y0, y1) + r, color,
                  [=](float x, float y)
                  {
                      float t = len2 > 0.0f ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0.0f;
                      t = std::clamp(t, 0.0f, 1.0f);
                      const float px = x0 + t * dx - x, py = y0 + t * dy - y;
                      return px * px + py * py <= r * r;
                  });
    }

    void fillRotatedEllipse(Image &image, float cx, float cy, float rx, float ry, float angle, uint32_t color)
    {
        if (rx <= 0.0f || ry <= 0.0f)
            return;
        const float c = std::cos(angle), s = std::sin(angle);
        const float ex = std::sqrt(rx * rx * c * c + ry * ry * s * s);
        const float ey = std::sqrt(rx * rx * s * s + ry * ry * c * c);
        fillShape(image, cx - ex, cy - ey, cx + ex, cy + ey, color, [=](float x, float y)
                  {
                      const float dx = x - cx, dy = y - cy;
                      const float u = (dx * c + dy * s) / rx, v = (-dx * s + dy * c) / ry;
                      return u * u + v * v <= 1.0f;
                  });
    }

    void strokeCurve(Image &image, float x0, float y0, float cx, float cy, float x1, float y1, float width,
                     uint32_t color)
    {
        // The curve as a polyline; a point is inside within half the width
        // of any of its pieces (one pass, so no darker joints).
        constexpr int kPieces = 24;
        std::vector<float> xy;
        float bx0 = x0, by0 = y0, bx1 = x0, by1 = y0;
        for (int i = 0; i <= kPieces; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kPieces);
            const float a = (1.0f - t) * (1.0f - t), b = 2.0f * (1.0f - t) * t, d = t * t;
            const float px = a * x0 + b * cx + d * x1, py = a * y0 + b * cy + d * y1;
            xy.push_back(px);
            xy.push_back(py);
            bx0 = std::min(bx0, px);
            by0 = std::min(by0, py);
            bx1 = std::max(bx1, px);
            by1 = std::max(by1, py);
        }
        const float r = width * 0.5f;
        fillShape(image, bx0 - r, by0 - r, bx1 + r, by1 + r, color, [&xy, r](float x, float y)
                  {
                      for (size_t i = 0; i + 3 < xy.size(); i += 2)
                      {
                          const float ax = xy[i], ay = xy[i + 1], dx = xy[i + 2] - ax, dy = xy[i + 3] - ay;
                          const float len2 = dx * dx + dy * dy;
                          const float t = len2 > 0.0f ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                          const float px = ax + t * dx - x, py = ay + t * dy - y;
                          if (px * px + py * py <= r * r)
                              return true;
                      }
                      return false;
                  });
    }

    namespace
    {
        float coverage(const Image &shape, int x, int y) { return channel(shape.at(x, y), 24) / 255.0f; }

        uint32_t mix(uint32_t a, uint32_t b, float t)
        {
            uint32_t out = 0u;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const float c = channel(a, shift) + (channel(b, shift) - channel(a, shift)) * t;
                out |= static_cast<uint32_t>(std::clamp(std::lround(c), 0L, 255L)) << shift;
            }
            return out;
        }
    }

    void paintShaded(Image &dst, const Image &shape, const Shading &shading)
    {
        int top = shape.height, bottom = -1;
        for (int y = 0; y < shape.height; ++y)
            for (int x = 0; x < shape.width; ++x)
                if (shape.pixels[static_cast<size_t>(y) * static_cast<size_t>(shape.width) + static_cast<size_t>(x)] >> 24)
                {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                    break;
                }
        if (bottom < top)
            return;
        const float span = static_cast<float>(bottom - top + 1);
        const int d = std::max(1, static_cast<int>(std::lround(shading.bevel)));
        for (int y = top; y <= bottom; ++y)
            for (int x = 0; x < shape.width; ++x)
            {
                const float c = coverage(shape, x, y);
                if (c <= 0.0f)
                    continue;
                uint32_t color = mix(shading.top, shading.bottom, (static_cast<float>(y - top) + 0.5f) / span);
                const float lit = std::max(0.0f, c - coverage(shape, x - d, y - d));
                const float shaded = std::max(0.0f, c - coverage(shape, x + d, y + d));
                const uint32_t alpha = color & 0xFF000000u;
                if (lit > 0.0f)
                    color = (mix(color, rgba(255, 255, 255), lit * shading.light) & 0xFFFFFFu) | alpha;
                if (shaded > 0.0f)
                    color = (mix(color, rgba(0, 0, 0), shaded * shading.shade) & 0xFFFFFFu) | alpha;
                if (dst.width == shape.width && dst.height == shape.height)
                    dst.blend(x, y, color, c);
            }
    }

    void paintShadow(Image &dst, const Image &shape, float dx, float dy, int radius, uint32_t color)
    {
        const int w = shape.width, h = shape.height;
        if (w <= 0 || h <= 0 || dst.width != w || dst.height != h)
            return;
        std::vector<float> a(static_cast<size_t>(w) * static_cast<size_t>(h)), b(a.size());
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = channel(shape.pixels[i], 24) / 255.0f;
        radius = std::max(0, radius);
        // Two box blurs each way: close to a Gaussian.
        for (int pass = 0; pass < 2 && radius > 0; ++pass)
        {
            const float n = static_cast<float>(2 * radius + 1);
            for (int y = 0; y < h; ++y)
            {
                float sum = 0.0f;
                for (int x = -radius; x <= radius; ++x)
                    sum += x >= 0 && x < w ? a[static_cast<size_t>(y) * w + x] : 0.0f;
                for (int x = 0; x < w; ++x)
                {
                    b[static_cast<size_t>(y) * w + x] = sum / n;
                    const int add = x + radius + 1, drop = x - radius;
                    if (add < w)
                        sum += a[static_cast<size_t>(y) * w + add];
                    if (drop >= 0)
                        sum -= a[static_cast<size_t>(y) * w + drop];
                }
            }
            for (int x = 0; x < w; ++x)
            {
                float sum = 0.0f;
                for (int y = -radius; y <= radius; ++y)
                    sum += y >= 0 && y < h ? b[static_cast<size_t>(y) * w + x] : 0.0f;
                for (int y = 0; y < h; ++y)
                {
                    a[static_cast<size_t>(y) * w + x] = sum / n;
                    const int add = y + radius + 1, drop = y - radius;
                    if (add < h)
                        sum += b[static_cast<size_t>(add) * w + x];
                    if (drop >= 0)
                        sum -= b[static_cast<size_t>(drop) * w + x];
                }
            }
        }
        const int ox = static_cast<int>(std::lround(dx)), oy = static_cast<int>(std::lround(dy));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const int sx = x - ox, sy = y - oy;
                if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                const float c = a[static_cast<size_t>(sy) * w + sx];
                if (c > 0.002f)
                    dst.blend(x, y, color, c);
            }
    }

    void blur(Image &image, int radius)
    {
        const int w = image.width, h = image.height;
        if (radius <= 0 || w <= 0 || h <= 0)
            return;
        std::vector<float> a(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u), b(a.size());
        for (size_t i = 0; i < image.pixels.size(); ++i)
            for (int k = 0; k < 4; ++k)
                a[i * 4u + static_cast<size_t>(k)] = channel(image.pixels[i], k * 8);
        const float n = static_cast<float>(2 * radius + 1);
        // One box blur along a line of `count` pixels `step` apart.
        const auto line = [&](const std::vector<float> &src, std::vector<float> &dst, size_t start, size_t step, int count)
        {
            for (int k = 0; k < 4; ++k)
            {
                const auto at = [&](int i)
                { return src[(start + static_cast<size_t>(std::clamp(i, 0, count - 1)) * step) * 4u + static_cast<size_t>(k)]; };
                float sum = 0.0f;
                for (int i = -radius; i <= radius; ++i)
                    sum += at(i);
                for (int i = 0; i < count; ++i)
                {
                    dst[(start + static_cast<size_t>(i) * step) * 4u + static_cast<size_t>(k)] = sum / n;
                    sum += at(i + radius + 1) - at(i - radius);
                }
            }
        };
        for (int pass = 0; pass < 3; ++pass)
        {
            for (int y = 0; y < h; ++y)
                line(a, b, static_cast<size_t>(y) * static_cast<size_t>(w), 1u, w);
            for (int x = 0; x < w; ++x)
                line(b, a, static_cast<size_t>(x), static_cast<size_t>(w), h);
        }
        for (size_t i = 0; i < image.pixels.size(); ++i)
        {
            uint32_t c = 0u;
            for (int k = 0; k < 4; ++k)
                c |= static_cast<uint32_t>(std::clamp(std::lround(a[i * 4u + static_cast<size_t>(k)]), 0L, 255L)) << (k * 8);
            image.pixels[i] = c;
        }
    }

    void drawImage(Image &dst, const Image &src, float x, float y, float w, float h)
    {
        if (src.width <= 0 || src.height <= 0 || w <= 0.0f || h <= 0.0f)
            return;
        const auto texel = [](uint32_t c) -> std::array<float, 4>
        {
            const float a = channel(c, 24) / 255.0f;
            return {channel(c, 0) * a, channel(c, 8) * a, channel(c, 16) * a, a};
        };
        const int ix0 = std::max(0, static_cast<int>(std::floor(x))), iy0 = std::max(0, static_cast<int>(std::floor(y)));
        const int ix1 = std::min(dst.width, static_cast<int>(std::ceil(x + w)));
        const int iy1 = std::min(dst.height, static_cast<int>(std::ceil(y + h)));
        for (int py = iy0; py < iy1; ++py)
            for (int px = ix0; px < ix1; ++px)
            {
                const float u = (static_cast<float>(px) + 0.5f - x) / w * static_cast<float>(src.width);
                const float v = (static_cast<float>(py) + 0.5f - y) / h * static_cast<float>(src.height);
                const std::array<float, 4> s = sample(src, u, v, texel);
                if (s[3] <= 0.0f)
                    continue;
                const uint32_t color = rgba(static_cast<uint32_t>(std::lround(s[0] / s[3])),
                                            static_cast<uint32_t>(std::lround(s[1] / s[3])),
                                            static_cast<uint32_t>(std::lround(s[2] / s[3])), 255u);
                dst.blend(px, py, color, s[3]);
            }
    }

    Font fontFrom(const zaxis::LibraryFont &font)
    {
        Font out;
        out.lineHeight = font.lineHeight;
        out.glyphs = font.glyphs;
        out.atlas = Image(font.atlas.width, font.atlas.height);
        out.atlas.pixels = font.atlas.rgba;
        return out;
    }

    namespace
    {
        const zaxis::FontGlyph *findGlyph(const Font &font, char c)
        {
            for (const zaxis::FontGlyph &g : font.glyphs)
                if (g.character == static_cast<uint8_t>(c))
                    return &g;
            return nullptr;
        }

        // Glyphs carry a dark outline of about a pixel on each side, which
        // is not drawn: the advance leaves it out.
        float advance(const Font &font, char c, float scale)
        {
            if (const zaxis::FontGlyph *g = findGlyph(font, c))
                return std::max(1.0f, static_cast<float>(g->width) - 2.0f) * scale;
            return c == ' ' ? static_cast<float>(font.lineHeight) * 0.3f * scale : 0.0f;
        }
    }

    float textWidth(const Font &font, std::string_view text, float height)
    {
        if (font.lineHeight <= 0)
            return 0.0f;
        const float scale = height / static_cast<float>(font.lineHeight);
        float width = 0.0f;
        for (char c : text)
            width += advance(font, c, scale);
        return width;
    }

    void drawText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                  uint32_t color, float maxWidth)
    {
        if (font.empty() || font.lineHeight <= 0 || text.empty())
            return;
        const float scale = height / static_cast<float>(font.lineHeight);
        const float natural = textWidth(font, text, height);
        const float squeeze = maxWidth > 0.0f && natural > maxWidth ? maxWidth / natural : 1.0f;
        // The light fill only: alpha times brightness.
        const auto texel = [](uint32_t c) -> std::array<float, 4>
        {
            const float a = channel(c, 24) / 255.0f *
                            std::max({channel(c, 0), channel(c, 8), channel(c, 16)}) / 255.0f;
            return {0.0f, 0.0f, 0.0f, a};
        };
        // Glyph boxes span the line; centre on the caps (the font's line
        // height includes the space below the baseline).
        float x = cx - natural * squeeze * 0.5f;
        const float top = cy - height * 0.5f + height * 0.06f;
        for (char c : text)
        {
            const zaxis::FontGlyph *g = findGlyph(font, c);
            const float step = advance(font, c, scale) * squeeze;
            if (g)
            {
                const float gx = x - scale * squeeze; // the outline pixel
                const float gy = top + static_cast<float>(g->offset) * scale;
                const float gw = static_cast<float>(g->width) * scale * squeeze;
                const float gh = static_cast<float>(g->height) * scale;
                const int ix0 = std::max(0, static_cast<int>(std::floor(gx)));
                const int iy0 = std::max(0, static_cast<int>(std::floor(gy)));
                const int ix1 = std::min(image.width, static_cast<int>(std::ceil(gx + gw)));
                const int iy1 = std::min(image.height, static_cast<int>(std::ceil(gy + gh)));
                for (int py = iy0; py < iy1; ++py)
                    for (int px = ix0; px < ix1; ++px)
                    {
                        const float u = static_cast<float>(g->x) + (static_cast<float>(px) + 0.5f - gx) / gw * static_cast<float>(g->width);
                        const float v = static_cast<float>(g->y) + (static_cast<float>(py) + 0.5f - gy) / gh * static_cast<float>(g->height);
                        if (u < g->x || v < g->y || u > g->x + g->width || v > g->y + g->height)
                            continue;
                        const std::array<float, 4> s = sample(font.atlas, u, v, texel);
                        image.blend(px, py, color, std::min(1.0f, s[3] * 1.15f));
                    }
            }
            x += step;
        }
    }

    void drawOutlinedText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                          uint32_t color, uint32_t outlineColor, float outline)
    {
        // The letters' fill moved around a circle, under the letters.
        constexpr int kSteps = 12;
        for (int i = 0; i < kSteps; ++i)
        {
            const float a = static_cast<float>(i) * 6.2831853f / static_cast<float>(kSteps);
            drawText(image, font, text, cx + std::cos(a) * outline, cy + std::sin(a) * outline, height, outlineColor);
        }
        drawText(image, font, text, cx, cy, height, color);
    }

    void drawBoldText(Image &image, const Font &font, std::string_view text, float cx, float cy, float height,
                      uint32_t color, float maxWidth)
    {
        // The font's strokes are about a pixel wide at small sizes: the fill
        // moved by up to half a pixel each way thickens them (the game's own
        // button pictures are lettered boldly, and a picture shown at half
        // its height, as the loading screens are, keeps its crossbars).
        if (height < 24.0f)
        {
            const float d = std::clamp((24.0f - height) / 20.0f, 0.25f, 0.5f);
            drawText(image, font, text, cx - d, cy, height, color, maxWidth);
            drawText(image, font, text, cx + d, cy, height, color, maxWidth);
            drawText(image, font, text, cx, cy - d, height, color, maxWidth);
            drawText(image, font, text, cx, cy + d, height, color, maxWidth);
        }
        drawText(image, font, text, cx, cy, height, color, maxWidth);
    }

    void drawKeycap(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label,
                    bool pressed)
    {
        const float s = std::min(w, h);
        const float radius = s * 0.2f;
        // Held down: the top sits lower on a thinner lip.
        const float lip = pressed ? std::max(1.0f, h * 0.06f) : std::max(1.5f, h * 0.13f);
        const float sink = pressed ? std::max(1.0f, h * 0.07f) : 0.0f;
        fillRoundRect(image, x, y, x + w, y + h, radius, rgba(16, 16, 20));
        fillRoundRect(image, x + 1.0f, y + 1.0f, x + w - 1.0f, y + h - 1.0f, radius - 1.0f, rgba(126, 128, 138));
        fillRoundRect(image, x + 1.5f, y + 1.0f + sink, x + w - 1.5f, y + h - lip, radius - 1.0f,
                      pressed ? rgba(184, 212, 255) : rgba(238, 238, 242));
        const float fx = x + 1.5f, fy = y + 1.0f + sink, fw = w - 3.0f, fh = h - lip - 1.0f - sink;
        const uint32_t ink = rgba(26, 26, 32);
        if (drawKeySymbol(image, label, fx, fy, fw, fh, ink))
            return;
        const bool single = label.size() == 1u;
        const float height = single ? fh * 0.92f : fh * (label.size() <= 3u ? 0.66f : 0.56f);
        drawBoldText(image, font, label, fx + fw * 0.5f, fy + fh * 0.5f, height, ink, fw * 0.86f);
    }

    void drawMouse(Image &image, float x, float y, float w, float h, int button)
    {
        const float mw = std::min(w * 0.82f, h * 0.66f);
        const float mh = h * 0.94f;
        const float x0 = x + (w - mw) * 0.5f, y0 = y + (h - mh) * 0.5f;
        const float x1 = x0 + mw, y1 = y0 + mh;
        const float r = mw * 0.48f;
        const uint32_t outline = rgba(16, 16, 20), body = rgba(232, 232, 238), lit = rgba(255, 186, 36);
        fillRoundRect(image, x0, y0, x1, y1, r, outline);
        fillRoundRect(image, x0 + 1.2f, y0 + 1.2f, x1 - 1.2f, y1 - 1.2f, r - 1.2f, body);
        const float split = y0 + mh * 0.44f;
        const float midX = (x0 + x1) * 0.5f;
        // The lit button: its quarter of the body.
        if (button == 0 || button == 1)
        {
            const float bx0 = button == 0 ? x0 + 1.2f : midX + 0.6f;
            const float bx1 = button == 0 ? midX - 0.6f : x1 - 1.2f;
            Image mask(image.width, image.height);
            fillRoundRect(mask, x0 + 1.2f, y0 + 1.2f, x1 - 1.2f, y1 - 1.2f, r - 1.2f, lit);
            for (int py = std::max(0, static_cast<int>(y0)); py < std::min(image.height, static_cast<int>(std::ceil(split))); ++py)
                for (int px = std::max(0, static_cast<int>(bx0)); px < std::min(image.width, static_cast<int>(std::ceil(bx1))); ++px)
                {
                    const uint32_t m = mask.at(px, py);
                    if (!(m >> 24))
                        continue;
                    float cover = channel(m, 24) / 255.0f;
                    cover *= std::clamp(std::min(static_cast<float>(px) + 1.0f - bx0, bx1 - static_cast<float>(px)), 0.0f, 1.0f);
                    cover *= std::clamp(split - static_cast<float>(py), 0.0f, 1.0f);
                    image.blend(px, py, lit, cover);
                }
        }
        strokeLine(image, x0 + 1.0f, split, x1 - 1.0f, split, std::max(1.0f, mw * 0.07f), outline);
        strokeLine(image, midX, y0 + 1.0f, midX, split, std::max(1.0f, mw * 0.07f), outline);
        const float ww = mw * 0.16f;
        fillRoundRect(image, midX - ww, y0 + mh * 0.12f, midX + ww, y0 + mh * 0.32f, ww, button == 2 ? lit : outline);
    }

    void drawPadLabel(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label,
                      bool pill, bool pressed)
    {
        const float radius = pill ? std::min(w, h) * 0.5f : std::min(w, h) * 0.28f;
        fillRoundRect(image, x, y, x + w, y + h, radius, rgba(210, 210, 216));
        fillRoundRect(image, x + 1.2f, y + 1.2f, x + w - 1.2f, y + h - 1.2f, radius - 1.2f,
                      pressed ? rgba(56, 96, 168) : rgba(38, 38, 44));
        drawBoldText(image, font, label, x + w * 0.5f, y + h * 0.5f, h * (label.size() <= 2u ? 0.72f : 0.5f),
                  rgba(244, 244, 248), w * 0.8f);
    }

    void drawDpad(Image &image, float x, float y, float w, float h)
    {
        const float s = std::min(w, h);
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        fillEllipse(image, cx, cy, s * 0.48f, s * 0.48f, rgba(38, 38, 44));
        const float arm = s * 0.36f, half = s * 0.12f;
        fillRoundRect(image, cx - arm, cy - half, cx + arm, cy + half, half * 0.4f, rgba(226, 226, 232));
        fillRoundRect(image, cx - half, cy - arm, cx + half, cy + arm, half * 0.4f, rgba(226, 226, 232));
    }

    void drawRoundButton(Image &image, float x, float y, float w, float h, const Font &font, const std::string &label)
    {
        const float s = std::min(w, h);
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        const float r = s * 0.47f;
        Image shape(image.width, image.height);
        fillEllipse(shape, cx, cy, r, r, rgba(255, 255, 255));
        if (s >= 20.0f)
            paintShadow(image, shape, s * 0.03f, s * 0.04f, std::max(1, static_cast<int>(s * 0.04f)), rgba(0, 0, 0, 140));
        // A dark button, lit from the upper left, with a dished top.
        paintShaded(image, shape, {rgba(104, 104, 112), rgba(20, 20, 24), std::max(1.0f, s * 0.05f), 0.5f, 0.5f});
        Image dish(image.width, image.height);
        fillEllipse(dish, cx, cy, r * 0.78f, r * 0.78f, rgba(255, 255, 255));
        paintShaded(image, dish, {rgba(30, 30, 34), rgba(66, 66, 74), std::max(1.0f, s * 0.03f), 0.0f, 0.0f});
        const uint32_t ink = rgba(236, 236, 240);
        if (label == "+" || label == "-")
        {
            const float arm = s * 0.22f, line = std::max(1.5f, s * 0.11f);
            strokeLine(image, cx - arm, cy, cx + arm, cy, line, ink);
            if (label == "+")
                strokeLine(image, cx, cy - arm, cx, cy + arm, line, ink);
            return;
        }
        drawBoldText(image, font, label, cx, cy, s * (label.size() <= 1u ? 0.66f : 0.46f), ink, r * 1.5f);
    }

    void drawShoulderButton(Image &image, float x, float y, float w, float h, const Font &font,
                            const std::string &label, bool pressed)
    {
        const float s = std::min(w, h);
        const float radius = s * 0.34f;
        fillRoundRect(image, x, y, x + w, y + h, radius, rgba(18, 18, 22));
        Image shape(image.width, image.height);
        fillRoundRect(shape, x + 1.0f, y + 1.0f, x + w - 1.0f, y + h - 1.0f, radius - 1.0f, rgba(255, 255, 255));
        const Shading grey{rgba(206, 206, 212), rgba(112, 112, 120), std::max(1.0f, s * 0.07f), 0.55f, 0.45f};
        const Shading blue{rgba(186, 214, 255), rgba(78, 118, 190), std::max(1.0f, s * 0.07f), 0.55f, 0.45f};
        paintShaded(image, shape, pressed ? blue : grey);
        drawBoldText(image, font, label, x + w * 0.5f, y + h * 0.5f, h * (label.size() <= 2u ? 0.76f : 0.5f),
                  rgba(22, 22, 26), w * 0.82f);
    }

    void drawGreyDpad(Image &image, float x, float y, float w, float h)
    {
        const float s = std::min(w, h);
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        const float arm = s * 0.46f, half = s * 0.16f;
        Image shape(image.width, image.height);
        fillRoundRect(shape, cx - arm, cy - half, cx + arm, cy + half, half * 0.35f, rgba(255, 255, 255));
        fillRoundRect(shape, cx - half, cy - arm, cx + half, cy + arm, half * 0.35f, rgba(255, 255, 255));
        if (s >= 20.0f)
            paintShadow(image, shape, s * 0.03f, s * 0.04f, std::max(1, static_cast<int>(s * 0.04f)), rgba(0, 0, 0, 140));
        Image outline(image.width, image.height);
        fillRoundRect(outline, cx - arm - 1.0f, cy - half - 1.0f, cx + arm + 1.0f, cy + half + 1.0f, half * 0.35f + 1.0f,
                      rgba(255, 255, 255));
        fillRoundRect(outline, cx - half - 1.0f, cy - arm - 1.0f, cx + half + 1.0f, cy + arm + 1.0f, half * 0.35f + 1.0f,
                      rgba(255, 255, 255));
        paintShaded(image, outline, {rgba(18, 18, 22), rgba(18, 18, 22), 1.0f, 0.0f, 0.0f});
        paintShaded(image, shape, {rgba(196, 196, 204), rgba(108, 108, 118), std::max(1.0f, s * 0.04f), 0.5f, 0.45f});
        // An arrow on each arm and a dimple in the middle.
        const uint32_t ink = rgba(48, 48, 56);
        const float t = half * 0.55f, tip = arm * 0.86f, base = arm * 0.56f;
        fillPolygon(image, {cx, cy - tip, cx - t, cy - base, cx + t, cy - base}, ink);
        fillPolygon(image, {cx, cy + tip, cx + t, cy + base, cx - t, cy + base}, ink);
        fillPolygon(image, {cx - tip, cy, cx - base, cy + t, cx - base, cy - t}, ink);
        fillPolygon(image, {cx + tip, cy, cx + base, cy - t, cx + base, cy + t}, ink);
        fillEllipse(image, cx, cy, half * 0.45f, half * 0.45f, rgba(132, 132, 142));
    }

    // ---- palettes ------------------------------------------------------------

    size_t nearest(const std::vector<uint32_t> &palette, uint32_t color)
    {
        size_t best = 0;
        float bestDistance = 1e30f;
        const float ca = channel(color, 24);
        for (size_t i = 0; i < palette.size(); ++i)
        {
            const uint32_t p = palette[i];
            const float pa = channel(p, 24);
            float d = (pa - ca) * (pa - ca) * 2.0f;
            // Colour matters as much as it shows.
            const float weight = std::min(pa, ca) / 255.0f;
            for (int shift = 0; shift < 24; shift += 8)
            {
                const float diff = channel(p, shift) - channel(color, shift);
                d += diff * diff * weight;
            }
            if (d < bestDistance)
            {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    Indexed quantize(const Image &image, size_t maxColors)
    {
        Indexed out;
        maxColors = std::max<size_t>(1u, maxColors);
        // Transparent pixels are one colour.
        std::vector<uint32_t> colors;
        colors.reserve(image.pixels.size());
        for (uint32_t c : image.pixels)
            colors.push_back((c >> 24) == 0u ? 0u : c);
        std::vector<uint32_t> unique = colors;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        if (unique.size() <= maxColors)
        {
            out.palette = unique;
        }
        else
        {
            // Median cut over the pixels (RGBA), widest channel first.
            std::vector<std::vector<uint32_t>> boxes{colors};
            while (boxes.size() < maxColors)
            {
                size_t pick = boxes.size();
                int pickShift = 0;
                float widest = 0.0f;
                for (size_t b = 0; b < boxes.size(); ++b)
                {
                    if (boxes[b].size() < 2u)
                        continue;
                    for (int shift = 0; shift < 32; shift += 8)
                    {
                        float lo = 255.0f, hi = 0.0f;
                        for (uint32_t c : boxes[b])
                        {
                            lo = std::min(lo, channel(c, shift));
                            hi = std::max(hi, channel(c, shift));
                        }
                        if (hi - lo > widest)
                        {
                            widest = hi - lo;
                            pick = b;
                            pickShift = shift;
                        }
                    }
                }
                if (pick == boxes.size() || widest <= 0.0f)
                    break;
                std::vector<uint32_t> &box = boxes[pick];
                std::sort(box.begin(), box.end(), [pickShift](uint32_t a, uint32_t b)
                          { return ((a >> pickShift) & 0xFFu) < ((b >> pickShift) & 0xFFu); });
                std::vector<uint32_t> upper(box.begin() + static_cast<std::ptrdiff_t>(box.size() / 2u), box.end());
                box.resize(box.size() / 2u);
                boxes.push_back(std::move(upper));
            }
            for (const std::vector<uint32_t> &box : boxes)
            {
                if (box.empty())
                    continue;
                double sum[4] = {};
                for (uint32_t c : box)
                    for (int k = 0; k < 4; ++k)
                        sum[k] += channel(c, k * 8);
                uint32_t color = 0u;
                for (int k = 0; k < 4; ++k)
                    color |= static_cast<uint32_t>(std::lround(sum[k] / static_cast<double>(box.size()))) << (k * 8);
                out.palette.push_back((color >> 24) == 0u ? 0u : color);
            }
        }
        out.indices.reserve(colors.size());
        for (uint32_t c : colors)
            out.indices.push_back(static_cast<uint8_t>(nearest(out.palette, c)));
        return out;
    }
}
