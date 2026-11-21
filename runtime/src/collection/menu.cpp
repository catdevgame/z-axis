#include "collection/menu.h"

#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace zx::collection
{
    namespace
    {
        uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
        int16_t s16(const uint8_t *p) { return static_cast<int16_t>(u16(p)); }
        uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
        int32_t s32(const uint8_t *p) { return static_cast<int32_t>(u32(p)); }

        uint8_t channel(uint32_t colour, int index) { return static_cast<uint8_t>(colour >> (8 * index)); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A rectangle grown (or shrunk) about its centre.
        Rect scaled(const Rect &rect, float factor)
        {
            const int width = static_cast<int>(std::lround(rect.width * factor));
            const int height = static_cast<int>(std::lround(rect.height * factor));
            return {rect.x - (width - rect.width) / 2, rect.y - (height - rect.height) / 2, width, height};
        }

        Rect intersect(const Rect &a, const Rect &b)
        {
            const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
            const int x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
            return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
        }

        // Bilinear sample of the picture at (x, y) in texel units (centres at
        // .5), clamped to `within`, as four floats 0-255.
        void sample(const Image &picture, const Rect &within, float x, float y, float out[4])
        {
            const float fx = std::clamp(x - 0.5f, static_cast<float>(within.x), static_cast<float>(within.x + within.width - 1));
            const float fy = std::clamp(y - 0.5f, static_cast<float>(within.y), static_cast<float>(within.y + within.height - 1));
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, within.x + within.width - 1), y1 = std::min(y0 + 1, within.y + within.height - 1);
            const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
            const uint32_t c00 = picture.at(x0, y0), c10 = picture.at(x1, y0), c01 = picture.at(x0, y1), c11 = picture.at(x1, y1);
            for (int i = 0; i < 4; ++i)
            {
                const float top = channel(c00, i) + (channel(c10, i) - channel(c00, i)) * ax;
                const float bottom = channel(c01, i) + (channel(c11, i) - channel(c01, i)) * ax;
                out[i] = top + (bottom - top) * ay;
            }
        }

        // The source part of the picture drawn scaled with its top-left at
        // (left, top), within clip; colours times tint (0-1 each), alpha
        // times opacity.
        void blit(Image &target, const Image &picture, const Rect &source, float left, float top, float scaleX, float scaleY,
                  const Rect &clip, const float tint[3], float opacity)
        {
            if (picture.empty() || source.empty() || scaleX <= 0.0f || scaleY <= 0.0f)
                return;
            const Rect drawn{static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)),
                             static_cast<int>(std::ceil(left + source.width * scaleX)) - static_cast<int>(std::floor(left)),
                             static_cast<int>(std::ceil(top + source.height * scaleY)) - static_cast<int>(std::floor(top))};
            const Rect area = intersect(intersect(drawn, clip), {0, 0, target.width, target.height});
            for (int y = area.y; y < area.y + area.height; ++y)
            {
                const float sy = source.y + (static_cast<float>(y) + 0.5f - top) / scaleY;
                if (sy < source.y || sy > source.y + source.height)
                    continue;
                uint32_t *row = target.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(target.width);
                for (int x = area.x; x < area.x + area.width; ++x)
                {
                    const float sx = source.x + (static_cast<float>(x) + 0.5f - left) / scaleX;
                    if (sx < source.x || sx > source.x + source.width)
                        continue;
                    float c[4];
                    sample(picture, source, sx, sy, c);
                    const float a = c[3] / 255.0f * opacity;
                    if (a <= 0.0f)
                        continue;
                    uint32_t &out = row[x];
                    uint8_t result[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        const float value = std::min(255.0f, c[i] * tint[i]);
                        result[i] = static_cast<uint8_t>(std::lround(channel(out, i) + (value - channel(out, i)) * a));
                    }
                    out = rgba(result[0], result[1], result[2]);
                }
            }
        }

        // The mean colour of a part of a picture (every fourth texel each way).
        uint32_t meanColour(const Image &picture, const Rect &part)
        {
            const Rect area = intersect(part, {0, 0, picture.width, picture.height});
            uint64_t sum[3] = {}, count = 0;
            for (int y = area.y; y < area.y + area.height; y += 4)
                for (int x = area.x; x < area.x + area.width; x += 4, ++count)
                    for (int i = 0; i < 3; ++i)
                        sum[i] += channel(picture.at(x, y), i);
            if (count == 0)
                return rgba(0, 0, 0);
            return rgba(static_cast<uint8_t>(sum[0] / count), static_cast<uint8_t>(sum[1] / count),
                        static_cast<uint8_t>(sum[2] / count));
        }

        constexpr ArtSource kArt[] = {
            // Its loading screen (front_end.cpp's kMainLoadPicture), the logo
            // above the LOADING... band.
            {"ai", "ASSETS/LOADING/MAINLOAD.RAW", ArtSource::Raw, 640, 448, 24, "", {0, 0, 640, 348}},
            // Its legal screen (the game's first picture, a field of 224
            // rows): the title over the copyright lines.
            {"dm2", "ASSETS/LEGAL1.RAW", ArtSource::RawField, 512, 224, 32, "", {0, 0, 512, 214}},
            // Its start screen.
            {"bmx", "ASSETS/UILIB.ZAL", ArtSource::LibraryImage, 0, 0, 0, "start_center_04", {}},
        };
    }

    bool readRawImage(const std::vector<uint8_t> &file, int width, int height, int bitsPerPixel, Image &out)
    {
        const int bytes = bitsPerPixel == 32 ? 4 : 3;
        if (width <= 0 || height <= 0 || file.size() < static_cast<size_t>(width) * height * bytes)
            return false;
        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<size_t>(width) * height);
        for (size_t i = 0; i < out.pixels.size(); ++i)
        {
            const uint8_t *p = file.data() + i * bytes;
            out.pixels[i] = rgba(p[0], p[1], p[2]);
        }
        return true;
    }

    bool readBmpImage(const std::vector<uint8_t> &file, Image &out)
    {
        const uint8_t *d = file.data();
        const size_t size = file.size();
        if (size < 54 || d[0] != 'B' || d[1] != 'M')
            return false;
        const uint32_t pixels = u32(d + 10);
        const uint32_t header = u32(d + 14);
        const int32_t width = s32(d + 18);
        const int32_t height = s32(d + 22);
        const int bits = u16(d + 28);
        const uint32_t compression = u32(d + 30);
        // Palettes of 4 and 8 bits (the engine's), and plain 24 and 32-bit
        // pixels (a map's preview from any image editor), uncompressed
        // (BI_RGB, or BI_BITFIELDS with the usual B G R A order).
        if ((bits != 4 && bits != 8 && bits != 24 && bits != 32) || width <= 0 || height == 0 || (compression != 0 && compression != 3))
            return false;
        const int rows = std::abs(height);
        const size_t stride = ((static_cast<size_t>(width) * bits + 31) / 32) * 4;
        if (pixels + stride * rows > size)
            return false;
        std::array<uint32_t, 256> palette{};
        if (bits <= 8)
        {
            uint32_t colours = u32(d + 14 + 32);
            if (colours == 0 || colours > 256)
                colours = 1u << bits;
            if (14 + header + 4 * colours > size)
                return false;
            for (uint32_t i = 0; i < colours; ++i)
            {
                const uint8_t *c = d + 14 + header + 4 * i; // B G R A
                palette[i] = rgba(c[2], c[1], c[0], c[3]);
            }
        }
        out.width = width;
        out.height = rows;
        out.pixels.resize(static_cast<size_t>(width) * rows);
        for (int y = 0; y < rows; ++y)
        {
            // Bottom-up unless the height is negative.
            const uint8_t *row = d + pixels + stride * (height > 0 ? rows - 1 - y : y);
            for (int x = 0; x < width; ++x)
            {
                uint32_t &pixel = out.pixels[static_cast<size_t>(y) * width + x];
                if (bits == 24)
                    pixel = rgba(row[3 * x + 2], row[3 * x + 1], row[3 * x]);
                else if (bits == 32)
                    pixel = rgba(row[4 * x + 2], row[4 * x + 1], row[4 * x]); // the alpha of a plain 32-bit BMP is unused
                else
                {
                    const int index = bits == 8 ? row[x] : ((x & 1) ? row[x >> 1] & 15 : row[x >> 1] >> 4);
                    pixel = palette[static_cast<size_t>(index)];
                }
            }
        }
        return true;
    }

    std::vector<std::pair<size_t, size_t>> archiveFiles(const std::vector<uint8_t> &file)
    {
        std::vector<std::pair<size_t, size_t>> files;
        if (file.size() < 8 || std::memcmp(file.data(), "ZDIR", 4) != 0)
        {
            files.emplace_back(0, file.size());
            return files;
        }
        const size_t end = std::min(file.size(), static_cast<size_t>(8) + u32(file.data() + 4));
        const uint8_t *offsets = nullptr, *lengths = nullptr;
        size_t count = 0;
        for (size_t p = 8; p + 8 <= end;)
        {
            const uint32_t size = u32(file.data() + p + 4);
            if (p + 8 + size > end)
                break;
            if (std::memcmp(file.data() + p, "OFST", 4) == 0)
                offsets = file.data() + p + 8, count = size / 4;
            else if (std::memcmp(file.data() + p, "DLEN", 4) == 0)
                lengths = file.data() + p + 8, count = std::min<size_t>(count ? count : size / 4, size / 4);
            p += 8 + size;
        }
        if (!offsets || !lengths)
            return files;
        for (size_t i = 0; i < count; ++i)
        {
            const size_t offset = u32(offsets + 4 * i);
            const size_t length = u32(lengths + 4 * i) & 0x0FFFFFFFu;
            if (offset + length <= file.size())
                files.emplace_back(offset, length);
        }
        return files;
    }

    Image doubleRows(const Image &image)
    {
        Image out;
        out.width = image.width;
        out.height = image.height * 2;
        out.pixels.resize(static_cast<size_t>(out.width) * out.height);
        for (int y = 0; y < out.height; ++y)
            std::memcpy(out.pixels.data() + static_cast<size_t>(y) * out.width,
                        image.pixels.data() + static_cast<size_t>(y / 2) * image.width, sizeof(uint32_t) * out.width);
        return out;
    }

    bool readFont(const std::vector<uint8_t> &file, Image page, BitmapFont &out)
    {
        constexpr size_t kHeader = 6, kPage = 0x28, kGlyph = 0x0C;
        if (file.size() < kHeader || page.empty())
            return false;
        const uint8_t *d = file.data();
        const int height = d[0];
        const size_t glyphs = u16(d + 2);
        const size_t pages = u16(d + 4);
        if (height == 0 || pages != 1 || file.size() < kHeader + pages * kPage + glyphs * kGlyph)
            return false;
        BitmapFont font;
        font.height = height;
        const uint8_t *g = d + kHeader + pages * kPage;
        for (size_t i = 0; i < glyphs; ++i, g += kGlyph)
        {
            BitmapFont::Glyph glyph;
            glyph.present = true;
            glyph.top = s16(g + 2);
            glyph.u = u16(g + 4);
            glyph.v = u16(g + 6);
            glyph.width = u16(g + 8);
            glyph.height = u16(g + 10);
            if (g[1] != 0 || glyph.u + glyph.width > page.width || glyph.v + glyph.height > page.height)
                return false;
            font.glyphs[g[0]] = glyph;
        }
        font.page = std::move(page);
        out = std::move(font);
        return true;
    }

    // ---- Canvas ---------------------------------------------------------------

    Canvas::Canvas(int width, int height)
    {
        m_image.width = width;
        m_image.height = height;
        m_image.pixels.assign(static_cast<size_t>(width) * height, rgba(0, 0, 0));
    }

    void Canvas::clear(uint32_t colour)
    {
        std::fill(m_image.pixels.begin(), m_image.pixels.end(), colour | 0xFF000000u);
    }

    void Canvas::fill(const Rect &rect, uint32_t colour)
    {
        const Rect area = intersect(rect, {0, 0, width(), height()});
        const float a = channel(colour, 3) / 255.0f;
        for (int y = area.y; y < area.y + area.height; ++y)
            for (int x = area.x; x < area.x + area.width; ++x)
            {
                uint32_t &out = m_image.pixels[static_cast<size_t>(y) * width() + x];
                uint8_t result[3];
                for (int i = 0; i < 3; ++i)
                    result[i] = static_cast<uint8_t>(std::lround(channel(out, i) + (channel(colour, i) - channel(out, i)) * a));
                out = rgba(result[0], result[1], result[2]);
            }
    }

    void Canvas::frame(const Rect &rect, int thickness, uint32_t colour)
    {
        fill({rect.x, rect.y, rect.width, thickness}, colour);
        fill({rect.x, rect.y + rect.height - thickness, rect.width, thickness}, colour);
        fill({rect.x, rect.y + thickness, thickness, rect.height - 2 * thickness}, colour);
        fill({rect.x + rect.width - thickness, rect.y + thickness, thickness, rect.height - 2 * thickness}, colour);
    }

    void Canvas::draw(const Image &picture, const Rect &source, const Rect &target, Fit fit, float brightness, float opacity,
                      Rect clip)
    {
        if (picture.empty() || source.empty() || target.empty())
            return;
        const float sx = static_cast<float>(target.width) / source.width;
        const float sy = static_cast<float>(target.height) / source.height;
        const float scale = fit == Fit::Cover ? std::max(sx, sy) : std::min(sx, sy);
        const float left = target.x + (target.width - source.width * scale) / 2.0f;
        const float top = target.y + (target.height - source.height * scale) / 2.0f;
        const float tint[3] = {brightness, brightness, brightness};
        blit(m_image, picture, source, left, top, scale, scale, clip.empty() ? target : intersect(clip, target), tint, opacity);
    }

    namespace
    {
        // The percent sign is drawn, not taken from the font (the games'
        // fonts have none, the shipped one's reads as "o/o"): two rings and a
        // stroke over the rows the font's "0" fills, a little wider than it.
        struct DigitRows
        {
            int top = 0, bottom = 0, width = 0;
        };
        DigitRows digitRows(const BitmapFont &font)
        {
            DigitRows rows{font.height / 5, font.height * 4 / 5, font.height / 2};
            const BitmapFont::Glyph &zero = font.glyphs[static_cast<uint8_t>('0')];
            if (!zero.present)
                return rows;
            int top = -1, bottom = -1;
            for (int y = 0; y < zero.height; ++y)
                for (int x = 0; x < zero.width; ++x)
                    if (zero.u + x < font.page.width && zero.v + y < font.page.height && (font.page.at(zero.u + x, zero.v + y) >> 24) > 96)
                    {
                        top = top < 0 ? y : top;
                        bottom = y;
                        break;
                    }
            if (top >= 0)
                rows = {zero.top + top, zero.top + bottom + 1, zero.width};
            return rows;
        }
        float percentWidth(const BitmapFont &font) { return digitRows(font).width * 1.25f; }

        float distanceToSegment(float px, float py, float ax, float ay, float bx, float by)
        {
            const float dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
            const float t = length > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / length, 0.0f, 1.0f) : 0.0f;
            return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
        }
    }

    void Canvas::percentSign(float x, float top, float width, float height, uint32_t colour)
    {
        const float stroke = std::max(1.0f, height * 0.1f), radius = std::min(width * 0.2f, height * 0.18f);
        const float rings[2][2] = {{x + width * 0.26f, top + radius + stroke / 2}, {x + width * 0.74f, top + height - radius - stroke / 2}};
        const uint8_t alpha = static_cast<uint8_t>(colour >> 24);
        for (int py = static_cast<int>(top); py <= static_cast<int>(top + height); ++py)
            for (int px = static_cast<int>(x); px <= static_cast<int>(x + width); ++px)
            {
                const float cx = px + 0.5f, cy = py + 0.5f;
                float d = distanceToSegment(cx, cy, x + width * 0.86f, top, x + width * 0.14f, top + height) - stroke / 2;
                for (const auto &ring : rings)
                    d = std::min(d, std::fabs(std::hypot(cx - ring[0], cy - ring[1]) - radius) - stroke / 2);
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (cover > 0.0f)
                    fill({px, py, 1, 1}, (colour & 0x00FFFFFFu) | static_cast<uint32_t>(std::lround(alpha * cover)) << 24);
            }
    }

    int Canvas::textWidth(const BitmapFont &font, std::string_view line, float scale)
    {
        if (font.empty())
            return 0;
        float width = 0.0f;
        for (const char c : line)
        {
            const BitmapFont::Glyph &glyph = font.glyphs[static_cast<uint8_t>(c)];
            // A character the font lacks (the space) advances half a line.
            width += (c == '%' ? percentWidth(font) + 1 : glyph.present ? glyph.width + 1 : font.height / 2) * scale;
        }
        return static_cast<int>(std::lround(width));
    }

    int Canvas::text(const BitmapFont &font, int x, int y, std::string_view line, float scale, uint32_t colour)
    {
        if (font.empty())
            return 0;
        const float tint[3] = {channel(colour, 0) / 255.0f, channel(colour, 1) / 255.0f, channel(colour, 2) / 255.0f};
        const float opacity = channel(colour, 3) / 255.0f;
        float pen = static_cast<float>(x);
        for (const char c : line)
        {
            const BitmapFont::Glyph &glyph = font.glyphs[static_cast<uint8_t>(c)];
            if (c == '%')
            {
                const DigitRows rows = digitRows(font);
                percentSign(pen, y + rows.top * scale, percentWidth(font) * scale, (rows.bottom - rows.top) * scale, colour);
                pen += (percentWidth(font) + 1) * scale;
                continue;
            }
            if (!glyph.present)
            {
                pen += font.height / 2 * scale;
                continue;
            }
            blit(m_image, font.page, {glyph.u, glyph.v, glyph.width, glyph.height}, pen, y + glyph.top * scale, scale, scale,
                 {0, 0, width(), height()}, tint, opacity);
            pen += (glyph.width + 1) * scale;
        }
        return static_cast<int>(std::lround(pen)) - x;
    }

    // ---- input and layout -------------------------------------------------------

    MenuInput menuInput(const std::vector<int> &keysPressed, uint16_t padButtonsPressed)
    {
        namespace hi = ps2_host_input;
        static const int kLeft = hi::hostCodeByName("Left"), kRight = hi::hostCodeByName("Right");
        static const int kUp = hi::hostCodeByName("Up"), kDown = hi::hostCodeByName("Down");
        static const int kEnter = hi::hostCodeByName("Enter"), kKeypadEnter = hi::hostCodeByName("KP_Enter");
        static const int kSpace = hi::hostCodeByName("Space"), kEscape = hi::hostCodeByName("Escape");
        MenuInput in;
        for (const int key : keysPressed)
        {
            in.left = in.left || key == kLeft;
            in.right = in.right || key == kRight;
            in.up = in.up || key == kUp;
            in.down = in.down || key == kDown;
            in.confirm = in.confirm || key == kEnter || (kKeypadEnter >= 0 && key == kKeypadEnter) || key == kSpace;
            in.back = in.back || key == kEscape;
        }
        auto pad = [padButtonsPressed](int button) { return (padButtonsPressed & (1u << button)) != 0; };
        in.left = in.left || pad(hi::kLeft);
        in.right = in.right || pad(hi::kRight);
        in.up = in.up || pad(hi::kUp);
        in.down = in.down || pad(hi::kDown);
        in.confirm = in.confirm || pad(hi::kCross) || pad(hi::kStart);
        in.back = in.back || pad(hi::kCircle);
        return in;
    }

    std::string buttonHint(HintDevice device, const char *choose, const char *back)
    {
        const char *chooseButton = "Enter", *backButton = "Esc";
        switch (device)
        {
        case HintDevice::Touch:
            return {};
        case HintDevice::PlayStation:
            chooseButton = "Cross", backButton = "Circle";
            break;
        case HintDevice::Xbox:
            chooseButton = "A", backButton = "B";
            break;
        case HintDevice::Nintendo:
            chooseButton = "B", backButton = "A";
            break;
        case HintDevice::Keyboard:
            break;
        }
        std::string line = choose ? std::string(chooseButton) + " " + choose : std::string();
        if (back)
            line += (line.empty() ? std::string() : std::string("   ")) + backButton + " " + back;
        return line;
    }

    Layout layout(int width, int height, size_t games)
    {
        Layout out;
        const int n = static_cast<int>(std::max<size_t>(games, 1));
        const int margin = width * 3 / 80, gap = width / 24;
        // The cards: as wide as the title pictures' shape allows in a row,
        // never more than a fifth of the width, a gap between them wider
        // than the chosen one grows (by 1.2) into.
        const int tileWidth = std::min(width / 5, (width - 2 * margin - (n - 1) * gap) / n);
        const int tileHeight = tileWidth * 10 / 17; // AI's 640 x 348 title, about 1.7 to 1
        const int rowWidth = n * tileWidth + (n - 1) * gap;
        const int tileTop = height * 71 / 100;
        const int top = height * 3 / 100, line = height / 14;
        out.title = {margin, top, width / 2, line};
        // Settings, then Quit, at the right of the title's line.
        const int itemWidth = width / 7;
        out.quit = {width - margin - itemWidth, top, itemWidth, line};
        out.settings = {out.quit.x - itemWidth - gap, top, itemWidth, line};
        for (int i = 0; i < static_cast<int>(games); ++i)
            out.tiles.push_back({(width - rowWidth) / 2 + i * (tileWidth + gap), tileTop + (height * 15 / 100 - tileHeight) / 2,
                                 tileWidth, tileHeight});
        out.name = {0, height * 49 / 100, width, height / 14};
        out.lastPlayed = {0, out.name.y + out.name.height, width, height / 24};
        // The riders stand at the sides, from under the title to the cards.
        const int riderTop = out.title.y + out.title.height + height / 40;
        const int riderHeight = tileTop - riderTop - height / 60;
        const int riderWidth = width * 3 / 10;
        out.riderLeft = {margin, riderTop, riderWidth, riderHeight};
        out.riderRight = {width - margin - riderWidth, riderTop, riderWidth, riderHeight};
        out.hint = {0, height * 92 / 100, width, height / 30};
        return out;
    }

    // ---- the settings page -----------------------------------------------------

    std::string SettingsRow::valueText(int value) const
    {
        switch (kind)
        {
        case Toggle:
            return value != 0 ? "On" : "Off";
        case Choice:
            return choices.empty() ? std::string()
                                   : choices[static_cast<size_t>(std::clamp(value, 0, static_cast<int>(choices.size()) - 1))];
        case Range:
            return std::to_string(value) + unit;
        case Page:
        case Action:
        case Check:
        case Note:
            return summary ? summary() : std::string();
        case Heading:
            return {};
        }
        return {};
    }

    int SettingsRow::moved(int value, int steps) const
    {
        switch (kind)
        {
        case Toggle:
        case Check:
            return steps == 0 ? value : (value != 0 ? 0 : 1);
        case Choice:
            return choices.empty() ? 0 : std::clamp(value + steps, 0, static_cast<int>(choices.size()) - 1);
        case Range:
            return std::clamp(value + steps * std::max(step, 1), minValue, maxValue);
        case Page:
        case Action:
        case Heading:
        case Note:
            return value;
        }
        return value;
    }

    PageLayout pageLayout(int width, int height, size_t rows, bool picture)
    {
        PageLayout out;
        out.title = {0, height * 7 / 100, width, height / 12};
        const int backWidth = width / 6, backHeight = height / 16;
        out.back = {(width - backWidth) / 2, height * 85 / 100, backWidth, backHeight};
        const int margin = width / 8, helpGap = height / 40, helpHeight = height / 24;
        const int rowsTop = height * 22 / 100;
        // With a picture the rows keep the left 55% and the picture a 4:3
        // box at the right, from the rows' top.
        int rowsWidth = width - 2 * margin;
        if (picture)
        {
            const int gap = width / 40;
            const int pictureWidth = (width - 2 * margin) * 2 / 5, pictureHeight = pictureWidth * 3 / 4;
            rowsWidth = width - 2 * margin - pictureWidth - gap;
            out.picture = {margin + rowsWidth + gap, rowsTop, pictureWidth, pictureHeight};
        }
        // The rows' room: from their top to the help, which ends above Back.
        const int room = std::max(out.back.y - helpHeight - helpGap - rowsTop, 1);
        const int count = static_cast<int>(rows);
        int pitch = height / 18, rowHeight = height / 20;
        if (count * pitch > room)
        {
            // Closer, down to a line a little taller than the text; past
            // that the page shows as many as fit and scrolls.
            pitch = std::max(room / count, height / 22);
            rowHeight = std::min(rowHeight, pitch - 2);
        }
        const int shown = std::min(count, std::max(room / pitch, 1));
        int top = rowsTop;
        for (int i = 0; i < shown; ++i, top += pitch)
            out.rows.push_back({margin, top, rowsWidth, rowHeight});
        out.labelX = margin + width / 40;
        out.valueX = margin + rowsWidth - width / 40;
        out.help = {0, std::max(top, out.picture.y + out.picture.height) + helpGap, width, helpHeight};
        out.notice = {0, out.help.y + out.help.height + helpGap, width, std::min(helpHeight, std::max(out.back.y - helpGap - (out.help.y + out.help.height + helpGap), 0))};
        return out;
    }

    FittedLine fitLine(const BitmapFont &font, const std::string &line, int width, float scale, float least)
    {
        FittedLine out{line, scale, Canvas::textWidth(font, line, scale)};
        if (out.width <= width || out.width <= 0)
            return out;
        out.scale = std::max(least, scale * static_cast<float>(width) / static_cast<float>(out.width));
        out.width = Canvas::textWidth(font, line, out.scale);
        // (rounding: down a step at a time until it fits)
        while (out.width > width && out.scale > least)
        {
            out.scale = std::max(least, out.scale * 0.98f);
            out.width = Canvas::textWidth(font, line, out.scale);
        }
        if (out.width <= width)
            return out;
        // Still too wide: cut, with "..." in place of what is left out.
        std::string cut = line;
        while (!cut.empty() && Canvas::textWidth(font, cut + "...", out.scale) > width)
            cut.pop_back();
        out.text = cut.empty() ? std::string() : cut + "...";
        out.width = Canvas::textWidth(font, out.text, out.scale);
        return out;
    }

    RowText layRowText(const BitmapFont &font, const std::string &label, const std::string &value, int left, int right, float scale,
                       int gap)
    {
        const int room = std::max(right - left, 0);
        const float least = 0.6f * scale;
        RowText out;
        // Both at one scale, the largest at which they fit side by side.
        float s = scale;
        auto total = [&](float at) { return Canvas::textWidth(font, label, at) + gap + Canvas::textWidth(font, value, at); };
        while (total(s) > room && s > least)
            s = std::max(least, s * 0.97f);
        out.value = {value, s, Canvas::textWidth(font, value, s)};
        if (const int labelWidth = Canvas::textWidth(font, label, s); total(s) > room && labelWidth <= room / 2)
        {
            // A short label whole and the long value cut short ("Dave Mirra BMX 2", a disc's long file name).
            out.label = {label, s, labelWidth};
            out.value = fitLine(font, value, std::max(room - gap - labelWidth, 0), s, s);
            out.labelX = left;
            out.valueX = right - out.value.width;
            return out;
        }
        // The label in what the value leaves (cut short if it must be).
        out.label = fitLine(font, label, std::max(room - gap - out.value.width, 0), s, s);
        out.labelX = left;
        out.valueX = right - out.value.width;
        return out;
    }

    // ---- Custom Freeplay -------------------------------------------------------

    const char *playerType(std::string_view game)
    {
        if (game == "ai")
            return "Inline skater";
        if (game == "dm2" || game == "bmx")
            return "BMX rider";
        return "Player";
    }

    const char *shortGameName(std::string_view game)
    {
        if (game == "ai")
            return "Aggressive Inline";
        if (game == "dm2")
            return "Dave Mirra BMX 2";
        if (game == "bmx")
            return "BMX XXX";
        return "another game";
    }

    std::vector<SettingsRow> freeplayMapRows(const std::vector<MapChoice> &maps, const std::string &gameTitle,
                                             const std::string &folder, bool lends, const std::string &why)
    {
        std::vector<SettingsRow> rows;
        SettingsRow heading;
        heading.kind = SettingsRow::Heading;
        heading.label = gameTitle;
        rows.push_back(std::move(heading));
        if (!lends)
        {
            SettingsRow none;
            none.kind = SettingsRow::Note;
            none.label = why.empty() ? gameTitle + " lends no level slot to a custom map yet." : why;
            rows.push_back(std::move(none));
            return rows;
        }
        if (maps.empty())
        {
            SettingsRow none;
            none.kind = SettingsRow::Note;
            none.label = "No maps in the " + folder + " folder.";
            rows.push_back(std::move(none));
            return rows;
        }
        for (const MapChoice &map : maps)
        {
            SettingsRow row;
            row.label = map.name;
            row.picture = map.preview;
            if (map.playable)
            {
                row.kind = SettingsRow::Action;
                // (one line: the help is drawn centred, as wide as the page)
                // The same line whichever game starts it (converting is the
                // notice's: "Converting ...").
                row.help = "From " + map.builtFor + (map.author.empty() ? "." : ", by " + map.author + ".");
                row.summary = []() { return std::string("Start"); };
                row.activate = [game = map.game, index = map.map]()
                {
                    MenuAction action;
                    action.kind = MenuAction::Boot;
                    action.game = game;
                    action.map = index;
                    return action;
                };
            }
            else
            {
                row.kind = SettingsRow::Note;
                row.label = map.name + (map.why.empty() ? ", from " + map.builtFor : ": " + map.why);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // ---- the Mods page -------------------------------------------------------

    namespace
    {
        // A help line from words ("game files, music"): capital first, full stop.
        std::string sentence(std::string text)
        {
            if (text.empty())
                return text;
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            return text + ".";
        }

        // A mod's state in the games it is for: "On", "Off", "On in AI, DM2".
        std::string modState(const ModEntry &mod)
        {
            if (mod.map)
                return "Map";
            if (!mod.problem.empty() || mod.games.empty())
                return "Not read";
            std::vector<std::string> read;
            for (const ModEntry::Game &game : mod.games)
                if (game.read)
                    read.push_back(game.title);
            if (read.empty())
                return mod.games.size() == 1 && !mod.games[0].on ? "Off" : "Not read";
            if (read.size() == mod.games.size())
                return "On";
            if (read.size() == 1)
                return "On in " + read[0];
            return "On in " + std::to_string(read.size()) + " of " + std::to_string(mod.games.size());
        }
    }

    std::vector<SettingsRow> modsPageRows(const std::vector<ModEntry> &mods, const std::string &folder)
    {
        std::vector<SettingsRow> rows;
        if (mods.empty())
        {
            SettingsRow none;
            none.kind = SettingsRow::Note;
            none.label = "No mods in the " + folder + " folder.";
            rows.push_back(std::move(none));
            return rows;
        }
        for (size_t m = 0; m < mods.size(); ++m)
        {
            const ModEntry &mod = mods[m];
            SettingsRow row;
            row.kind = SettingsRow::Page;
            row.opens = MenuPage::Mod;
            row.label = std::to_string(m + 1) + ". " + mod.title + (mod.version.empty() ? "" : " " + mod.version);
            row.summary = [state = modState(mod)]() { return state; };
            row.help = sentence(mod.contents);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    std::vector<SettingsRow> modPageRows(const std::shared_ptr<std::vector<ModEntry>> &mods, size_t m,
                                         std::function<void(size_t game, bool on)> switched, std::function<void(int steps)> moved)
    {
        std::vector<SettingsRow> rows;
        if (m >= mods->size())
            return rows;
        const ModEntry &mod = (*mods)[m];
        auto note = [&rows](std::string text)
        {
            SettingsRow row;
            row.kind = SettingsRow::Note;
            row.label = std::move(text);
            rows.push_back(std::move(row));
        };
        SettingsRow heading;
        heading.kind = SettingsRow::Heading;
        heading.label = mod.title + (mod.version.empty() ? "" : " " + mod.version);
        rows.push_back(std::move(heading));
        note(mod.where + (mod.author.empty() ? "" : ", by " + mod.author));
        if (!mod.contents.empty())
            note(sentence(mod.contents));
        if (!mod.problem.empty())
            note("Not read, " + mod.problem + ".");
        if (mod.map)
            note("A map for Custom Freeplay.");
        auto onSwitch = std::make_shared<std::function<void(size_t, bool)>>(std::move(switched));
        if (mod.problem.empty() && !mod.map)
            for (size_t g = 0; g < mod.games.size(); ++g)
            {
                const ModEntry::Game &game = mod.games[g];
                SettingsRow row;
                row.kind = SettingsRow::Toggle;
                row.label = game.title;
                row.help = game.read || game.why == "Off" ? "Applies at next start." : "Not read, " + game.why + ".";
                row.current = [mods, m, g]() { return (*mods)[m].games[g].on ? 1 : 0; };
                row.apply = [mods, m, g, onSwitch](int value)
                {
                    (*mods)[m].games[g].on = value != 0;
                    if (*onSwitch)
                        (*onSwitch)(g, value != 0);
                };
                rows.push_back(std::move(row));
                if (game.on && !game.read)
                    note(game.title + ": not read, " + game.why);
                if (game.read && !game.winners.empty())
                    note(game.title + ": " + game.winners);
                if (game.read)
                    for (const std::string &entry : game.entriesNotRead)
                        note(game.title + ": " + entry + ", not read.");
            }
        auto onMove = std::make_shared<std::function<void(int)>>(std::move(moved));
        const std::string place = std::to_string(m + 1) + " of " + std::to_string(mods->size());
        for (const int steps : {-1, 1})
        {
            SettingsRow row;
            row.kind = SettingsRow::Action;
            row.label = steps < 0 ? "Move Up" : "Move Down";
            row.summary = [place]() { return place; };
            row.help = steps < 0 ? "Mods below it win on the same file." : "Its files win over mods above it.";
            row.activate = [onMove, steps]()
            {
                if (*onMove)
                    (*onMove)(steps);
                return MenuAction{};
            };
            rows.push_back(std::move(row));
        }
        for (const std::string &text : mod.notes)
            note(text);
        return rows;
    }

    std::string modsSummary(const std::vector<ModEntry> &mods)
    {
        size_t all = 0, on = 0;
        for (const ModEntry &mod : mods)
        {
            if (mod.map)
                continue;
            ++all;
            on += std::any_of(mod.games.begin(), mod.games.end(), [](const ModEntry::Game &g) { return g.read; }) ? 1u : 0u;
        }
        if (all == 0)
            return "None";
        return std::to_string(on) + " of " + std::to_string(all) + " on";
    }

    std::string lastPlayedText(std::time_t time)
    {
        char day[32] = {};
        if (const std::tm *local = std::localtime(&time); local && std::strftime(day, sizeof(day), "%e %B %Y", local) > 0)
        {
            const char *text = day;
            while (*text == ' ')
                ++text;
            return std::string("Last played ") + text;
        }
        return "Last played";
    }

    // ---- Shell -------------------------------------------------------------------

    const char *stateName(State state)
    {
        switch (state)
        {
        case State::Menu:
            return "menu";
        case State::Game:
            return "game";
        case State::Quit:
            return "quit";
        }
        return "?";
    }

    Shell::Shell(size_t games, bool freeplay) : m_games(games), m_freeplay(freeplay)
    {
        for (size_t i = 0; i < games; ++i)
            m_players.push_back(static_cast<int>(i));
    }

    int Shell::playerPlace(int game) const
    {
        const auto it = std::find(m_players.begin(), m_players.end(), game);
        return it == m_players.end() ? -1 : static_cast<int>(it - m_players.begin());
    }

    void Shell::keepPlayerOffered()
    {
        if (playerPlace(m_freeplayGame) >= 0)
            return;
        if (m_players.empty())
            m_playerOnBack = true;
        else
            m_freeplayGame = m_players.front();
    }

    void Shell::setFreeplayPlayers(std::vector<int> games)
    {
        games.erase(std::remove_if(games.begin(), games.end(), [this](int g) { return g < 0 || g >= static_cast<int>(m_games); }),
                    games.end());
        if (games == m_players)
            return;
        m_players = std::move(games);
        if (m_page == MenuPage::FreeplayPlayer)
            keepPlayerOffered();
        ++m_revision;
    }

    void Shell::start(int game)
    {
        m_state = State::Menu;
        m_selection = 0;
        if (game >= 0)
            boot(game);
    }

    void Shell::setFreeplay(bool on)
    {
        if (on == m_freeplay)
            return;
        // The items after the games move by one: the selection keeps its
        // item; on the tile that goes it moves to the game last played.
        const bool onQuit = m_selection == quitItem(), onSettings = m_selection == settingsItem();
        const bool onFreeplay = m_freeplay && m_selection == freeplayItem();
        m_freeplay = on;
        if (onQuit)
            m_selection = quitItem();
        else if (onSettings)
            m_selection = settingsItem();
        else if (onFreeplay || m_selection >= tileCount())
            m_selection = m_lastGame >= 0 ? m_lastGame : 0;
        if (!on && (m_page == MenuPage::FreeplayPlayer || m_page == MenuPage::FreeplayMap))
        {
            m_page = MenuPage::Tiles;
            m_selection = m_lastGame >= 0 ? m_lastGame : 0;
        }
        ++m_revision;
    }

    MenuAction Shell::input(const MenuInput &in, const Layout &layout, const std::vector<SettingsRow> &rows,
                            const PageLayout &page)
    {
        if (m_state != State::Menu)
            return {};
        switch (m_page)
        {
        case MenuPage::Tiles:
            return tilesInput(in, layout);
        case MenuPage::FreeplayPlayer:
            return playerInput(in, layout, page);
        case MenuPage::Settings:
        case MenuPage::Mods:
        case MenuPage::Games:
        case MenuPage::Mod:
        case MenuPage::FreeplayMap:
        case MenuPage::Recordings:
            break;
        }
        return pageInput(in, rows, page);
    }

    MenuAction Shell::tilesInput(const MenuInput &in, const Layout &layout)
    {
        MenuAction action;
        const int tiles = tileCount();
        const int was = m_selection;
        auto itemAt = [&](float x, float y)
        {
            for (int i = 0; i < tiles && i < static_cast<int>(layout.tiles.size()); ++i)
                if (layout.tiles[static_cast<size_t>(i)].contains(x, y))
                    return i;
            if (layout.quit.contains(x, y))
                return quitItem();
            return layout.settings.contains(x, y) ? settingsItem() : -1;
        };
        if (in.pointerMoved || in.click)
            if (const int item = itemAt(in.pointerX, in.pointerY); item >= 0)
                m_selection = item;
        // Left and Right move along the cards, and between Settings and
        // Quit (Settings at the left); Up goes from the cards to Settings
        // above them, Down back; Escape goes to Quit.
        const bool onCards = m_selection < tiles;
        if (in.left && m_selection > 0 && onCards)
            --m_selection;
        if (in.right && m_selection < tiles - 1)
            ++m_selection;
        if (in.left && m_selection == quitItem())
            m_selection = settingsItem();
        if (in.right && m_selection == settingsItem())
            m_selection = quitItem();
        if (in.back && onCards)
            m_selection = quitItem();
        else if (in.up && onCards)
            m_selection = settingsItem();
        else if (in.down && !onCards && tiles > 0)
            m_selection = m_map >= 0 && m_freeplay ? freeplayItem() : m_lastGame >= 0 ? m_lastGame : 0;
        const bool clicked = in.click && itemAt(in.pointerX, in.pointerY) == m_selection;
        if (in.confirm || clicked)
        {
            if (m_selection == quitItem())
                action.kind = MenuAction::Quit;
            else if (m_selection == settingsItem())
                openPage(MenuPage::Settings);
            else if (m_selection == freeplayItem())
                openPage(MenuPage::FreeplayPlayer);
            else
            {
                action.kind = MenuAction::Boot;
                action.game = m_selection;
            }
        }
        if (m_selection != was)
            ++m_revision;
        return action;
    }

    void Shell::choose(int game)
    {
        if (m_state != State::Menu || game < 0 || game >= static_cast<int>(m_games))
            return;
        if (m_page == MenuPage::FreeplayPlayer)
        {
            if (playerPlace(game) < 0)
                return;
            m_freeplayGame = game;
            m_playerOnBack = false;
        }
        else if (m_page == MenuPage::Tiles)
            m_selection = game;
        else
            return;
        ++m_revision;
    }

    void Shell::showPage(MenuPage page)
    {
        if (m_state == State::Menu && m_page == MenuPage::Settings && page == MenuPage::Games)
        {
            // (from Settings: its Back comes back there)
            openPage(page);
            return;
        }
        if (m_state != State::Menu || m_page != MenuPage::Tiles)
            return;
        if (page == MenuPage::FreeplayPlayer && !m_freeplay)
            return;
        if (page == MenuPage::Settings || page == MenuPage::Mods || page == MenuPage::Games || page == MenuPage::Recordings)
        {
            // (Mods, Games and Recordings with Settings behind them: their Back comes back to Settings, then the tiles)
            m_selection = settingsItem();
            openPage(MenuPage::Settings);
            if (page != MenuPage::Settings)
                openPage(page);
        }
        else if (page == MenuPage::FreeplayPlayer)
        {
            m_selection = freeplayItem();
            openPage(page);
        }
    }

    MenuAction Shell::playerInput(const MenuInput &in, const Layout &layout, const PageLayout &page)
    {
        const int players = static_cast<int>(m_players.size());
        const int was = m_freeplayGame;
        const bool wasOnBack = m_playerOnBack;
        // The game of the player tile at (x, y) (the layout's tiles are the players'), -1 none.
        auto tileAt = [&](float x, float y)
        {
            for (int i = 0; i < players && i < static_cast<int>(layout.tiles.size()); ++i)
                if (layout.tiles[static_cast<size_t>(i)].contains(x, y))
                    return m_players[static_cast<size_t>(i)];
            return -1;
        };
        const int place = playerPlace(m_freeplayGame);
        if (in.pointerMoved || in.click)
        {
            if (const int tile = tileAt(in.pointerX, in.pointerY); tile >= 0)
            {
                m_freeplayGame = tile;
                m_playerOnBack = false;
            }
            else if (page.back.contains(in.pointerX, in.pointerY))
                m_playerOnBack = true;
        }
        if (in.left && !m_playerOnBack && place > 0)
            m_freeplayGame = m_players[static_cast<size_t>(place - 1)];
        if (in.right && !m_playerOnBack && place >= 0 && place < players - 1)
            m_freeplayGame = m_players[static_cast<size_t>(place + 1)];
        if (in.down)
            m_playerOnBack = true;
        if (in.up && players > 0)
        {
            m_playerOnBack = false;
            keepPlayerOffered();
        }
        const bool clickedTile = in.click && tileAt(in.pointerX, in.pointerY) == m_freeplayGame && !m_playerOnBack;
        const bool clickedBack = in.click && page.back.contains(in.pointerX, in.pointerY);
        if (in.back || clickedBack || (in.confirm && m_playerOnBack))
        {
            closePage();
            return {};
        }
        if ((in.confirm && !m_playerOnBack) || clickedTile)
        {
            openPage(MenuPage::FreeplayMap);
            return {};
        }
        if (m_freeplayGame != was || m_playerOnBack != wasOnBack)
            ++m_revision;
        return {};
    }

    int Shell::chosenRow(const std::vector<SettingsRow> &rows) const
    {
        const int backRow = static_cast<int>(rows.size());
        int row = std::clamp(m_pageRow, 0, backRow);
        while (row < backRow && !rows[static_cast<size_t>(row)].choosable())
            ++row;
        return row;
    }

    void Shell::openPage(MenuPage page)
    {
        if ((page == MenuPage::Mods || page == MenuPage::Games || page == MenuPage::Recordings) && m_page == MenuPage::Settings)
            m_settingsRow = m_pageRow;
        if (page == MenuPage::Mod)
            m_modsRow = m_pageRow;
        if (page == MenuPage::FreeplayPlayer)
        {
            m_playerOnBack = false;
            keepPlayerOffered();
        }
        m_page = page;
        m_pageRow = 0;
        m_pageFirst = 0;
        ++m_revision;
    }

    void Shell::closePage()
    {
        switch (m_page)
        {
        case MenuPage::Mods:
        case MenuPage::Games:
        case MenuPage::Recordings:
            // Back to the Settings row that opened it.
            m_page = MenuPage::Settings;
            m_pageRow = m_settingsRow;
            m_pageFirst = 0;
            break;
        case MenuPage::Mod:
            // Back to the mod's row on the Mods page.
            m_page = MenuPage::Mods;
            m_pageRow = m_modsRow;
            m_pageFirst = 0;
            break;
        case MenuPage::FreeplayMap:
            // Back to the player's tile.
            m_page = MenuPage::FreeplayPlayer;
            m_playerOnBack = false;
            break;
        case MenuPage::FreeplayPlayer:
            m_page = MenuPage::Tiles;
            m_selection = freeplayItem();
            break;
        case MenuPage::Settings:
        case MenuPage::Tiles:
            m_page = MenuPage::Tiles;
            m_selection = settingsItem();
            break;
        }
        ++m_revision;
    }

    MenuAction Shell::pageInput(const MenuInput &in, const std::vector<SettingsRow> &rows, const PageLayout &page)
    {
        MenuAction action;
        const int backRow = static_cast<int>(rows.size());
        const int shown = static_cast<int>(page.rows.size());
        const int was = m_pageRow, wasFirst = m_pageFirst;
        auto choosable = [&](int row) { return row == backRow || rows[static_cast<size_t>(row)].choosable(); };
        m_pageRow = chosenRow(rows);
        auto rowAt = [&](float x, float y)
        {
            for (int slot = 0; slot < shown; ++slot)
                if (const int row = m_pageFirst + slot; row < backRow && page.rows[static_cast<size_t>(slot)].contains(x, y))
                    return choosable(row) ? row : -1;
            return page.back.contains(x, y) ? backRow : -1;
        };
        if (in.pointerMoved || in.click)
            if (const int row = rowAt(in.pointerX, in.pointerY); row >= 0)
                m_pageRow = row;
        if (in.up)
            for (int row = m_pageRow - 1; row >= 0; --row)
                if (choosable(row))
                {
                    m_pageRow = row;
                    break;
                }
        if (in.down)
            for (int row = m_pageRow + 1; row <= backRow; ++row)
                if (choosable(row))
                {
                    m_pageRow = row;
                    break;
                }
        const bool clicked = in.click && rowAt(in.pointerX, in.pointerY) == m_pageRow;
        if (in.back || ((in.confirm || clicked) && m_pageRow == backRow))
        {
            closePage();
            return action;
        }
        if (m_pageRow < backRow)
        {
            const SettingsRow &row = rows[static_cast<size_t>(m_pageRow)];
            if (row.kind == SettingsRow::Page)
            {
                if (in.right || in.confirm || clicked)
                {
                    openPage(row.opens);
                    return action;
                }
            }
            else if (row.kind == SettingsRow::Action)
            {
                if ((in.confirm || clicked) && row.activate)
                    action = row.activate();
            }
            else
            {
                if (in.left)
                    changeRow(rows, -1);
                if (in.right || in.confirm || clicked)
                    changeRow(rows, 1);
            }
        }
        // The chosen row in view (Back is always shown), the Heading over it
        // too when it is the first row under one.
        if (shown > 0 && m_pageRow < backRow)
        {
            if (m_pageRow < m_pageFirst)
                m_pageFirst = m_pageRow;
            if (m_pageRow >= m_pageFirst + shown)
                m_pageFirst = m_pageRow - shown + 1;
            if (m_pageRow == m_pageFirst && m_pageRow > 0 && rows[static_cast<size_t>(m_pageRow - 1)].kind == SettingsRow::Heading)
                --m_pageFirst;
            m_pageFirst = std::clamp(m_pageFirst, 0, std::max(backRow - shown, 0));
        }
        if (m_pageRow != was || m_pageFirst != wasFirst)
            ++m_revision;
        return action;
    }

    void Shell::changeRow(const std::vector<SettingsRow> &rows, int steps)
    {
        if (m_pageRow < 0 || m_pageRow >= static_cast<int>(rows.size()))
            return;
        const SettingsRow &row = rows[static_cast<size_t>(m_pageRow)];
        if (!row.current || !row.apply)
            return;
        const int value = row.current();
        const int next = row.moved(value, steps);
        if (next == value)
            return;
        row.apply(next);
        ++m_revision;
    }

    bool Shell::boot(int game, int map)
    {
        if (m_state != State::Menu || game < 0 || game >= static_cast<int>(m_games))
            return false;
        m_state = State::Game;
        m_game = game;
        m_lastGame = game;
        m_map = map;
        m_playback = -1;
        m_page = MenuPage::Tiles;
        m_selection = map >= 0 && m_freeplay ? freeplayItem() : game;
        ++m_boots;
        return true;
    }

    bool Shell::play(int game, int recording)
    {
        if (m_state != State::Menu || game < 0 || game >= static_cast<int>(m_games) || recording < 0)
            return false;
        m_state = State::Game;
        m_game = game;
        m_playback = recording;
        ++m_boots;
        return true;
    }

    void Shell::gameEnded(GameEnd end)
    {
        if (m_state != State::Game)
            return;
        m_game = -1;
        m_state = end == GameEnd::Quit ? State::Quit : State::Menu;
        if (m_playback >= 0)
        {
            // A playback: back on the page and row it was started from.
            m_playback = -1;
            ++m_revision;
            return;
        }
        // Back on the game just played, or on Custom Freeplay after a map.
        m_selection = m_map >= 0 && m_freeplay ? freeplayItem() : m_lastGame >= 0 ? m_lastGame : 0;
    }

    void Shell::quit()
    {
        m_state = State::Quit;
        m_game = -1;
    }

    // ---- MenuScript ------------------------------------------------------------

    bool MenuScript::parse(const std::string &spec, std::string *error)
    {
        std::vector<Step> steps;
        size_t start = 0;
        while (start <= spec.size())
        {
            const size_t comma = spec.find(',', start);
            std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (item.empty())
                continue;
            Step step;
            const std::string name = lower(item);
            if (name.rfind("drop:", 0) == 0)
            {
                step.drop = item.substr(5);
                if (step.drop.empty())
                {
                    if (error)
                        *error = "'" + item + "' names no file or folder";
                    return false;
                }
                steps.push_back(step);
                continue;
            }
            if (name == "wait")
            {
                steps.push_back(step);
                continue;
            }
            if (name.rfind("game:", 0) == 0)
            {
                step.game = name.substr(5);
                if (step.game.empty())
                {
                    if (error)
                        *error = "'" + item + "' names no game";
                    return false;
                }
                steps.push_back(step);
                continue;
            }
            if (name.rfind("pad:", 0) == 0)
            {
                for (int b = 0; b < ps2_host_input::kPadButtonCount; ++b)
                    if (name.substr(4) == lower(ps2_host_input::padButtonName(b)))
                        step.padButton = b;
            }
            else
                step.key = ps2_host_input::hostCodeByName(item);
            if (step.key < 0 && step.padButton < 0)
            {
                if (error)
                    *error = "'" + item + "' is not a key, pad:BUTTON, game:ID, drop:PATH or wait";
                return false;
            }
            steps.push_back(step);
        }
        m_steps = std::move(steps);
        m_next = 0;
        m_visitFrame = 0;
        return true;
    }

    bool MenuScript::next(std::vector<int> &keys, uint16_t &padButtons) { return next(keys, padButtons, Cards{}); }

    bool MenuScript::next(std::vector<int> &keys, uint16_t &padButtons, const Cards &cards, std::string *problem,
                          std::vector<std::string> *dropped)
    {
        ++m_visitFrame;
        if (m_next >= m_steps.size() || m_visitFrame % kFramesPerStep != 0)
            return false;
        if (!m_steps[m_next].game.empty())
        {
            // game:<id>: a move toward its card, or on to the next step.
            const std::string &id = m_steps[m_next].game;
            const auto it = std::find(cards.games.begin(), cards.games.end(), id);
            const int target = it == cards.games.end() ? -1 : static_cast<int>(it - cards.games.begin());
            if (target < 0 || target == cards.chosen || m_moves > static_cast<int>(cards.games.size()) + 2)
            {
                if (problem && target < 0)
                    *problem = "game:" + id + ": the menu shows no such game";
                else if (problem && target != cards.chosen)
                    *problem = "game:" + id + ": its card was not reached";
                m_moves = 0;
                ++m_next;
                return true;
            }
            ++m_moves;
            keys.push_back(ps2_host_input::hostCodeByName(cards.chosen < 0 ? "Down" : target < cards.chosen ? "Left" : "Right"));
            return true;
        }
        const Step &step = m_steps[m_next++];
        if (!step.drop.empty() && dropped)
            dropped->push_back(step.drop);
        if (step.key >= 0)
            keys.push_back(step.key);
        if (step.padButton >= 0)
            padButtons = static_cast<uint16_t>(padButtons | (1u << step.padButton));
        return true;
    }

    // ---- art and drawing -------------------------------------------------------

    const ArtSource *artSource(std::string_view game)
    {
        for (const ArtSource &art : kArt)
            if (game == art.game)
                return &art;
        return nullptr;
    }

    namespace
    {
        uint32_t scaledColour(uint32_t colour, float factor)
        {
            return rgba(static_cast<uint8_t>(std::min(255.0f, channel(colour, 0) * factor)),
                        static_cast<uint8_t>(std::min(255.0f, channel(colour, 1) * factor)),
                        static_cast<uint8_t>(std::min(255.0f, channel(colour, 2) * factor)));
        }

        uint32_t withAlpha(uint32_t colour, uint8_t alpha) { return (colour & 0x00FFFFFFu) | static_cast<uint32_t>(alpha) << 24; }

        // Rows `from` to `to` darkened from `fromAlpha` to `toAlpha` of black.
        void shade(Canvas &canvas, int from, int to, int fromAlpha, int toAlpha)
        {
            const int rows = std::max(1, to - from);
            for (int y = std::max(0, from); y < std::min(to, canvas.height()); ++y)
            {
                const int alpha = fromAlpha + (toAlpha - fromAlpha) * (y - from) / rows;
                if (alpha > 0)
                    canvas.fill({0, y, canvas.width(), 1}, rgba(0, 0, 0, static_cast<uint8_t>(std::min(alpha, 255))));
            }
        }

        // With no picture of any game: the games' colours as diagonal
        // stripes over a dark ground (ours alone, nothing from a disc).
        void stripes(Canvas &canvas, const std::vector<Tile> &tiles)
        {
            canvas.clear(rgba(14, 15, 19));
            const int width = canvas.width(), height = canvas.height();
            const int band = width / 10;
            for (size_t i = 0; i < tiles.size(); ++i)
            {
                const uint32_t colour = withAlpha(tiles[i].colour, 46);
                for (int y = 0; y < height; ++y)
                {
                    // (each band runs from the bottom left up to the right)
                    const int x = width / 6 + static_cast<int>(i) * band + (height - y) * 3 / 4 - height / 2;
                    canvas.fill({x, y, band, 1}, colour);
                }
            }
        }

        // A cut-out standing on the bottom of its room, as large as fits.
        Rect standing(const Image &picture, const Rect &room)
        {
            if (picture.empty() || room.empty())
                return {};
            const float scale = std::min(static_cast<float>(room.width) / picture.width, static_cast<float>(room.height) / picture.height);
            const int width = static_cast<int>(picture.width * scale), height = static_cast<int>(picture.height * scale);
            return {room.x + (room.width - width) / 2, room.y + room.height - height, width, height};
        }

        Rect between(const Rect &from, const Rect &to, float t)
        {
            auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
            return {mix(from.x, to.x), mix(from.y, to.y), mix(from.width, to.width), mix(from.height, to.height)};
        }
    }

    void drawMenu(Canvas &canvas, const Layout &layout, const std::vector<Tile> &tiles, const BitmapFont &font,
                  const Shell &shell, const std::string &title, const std::string &notice,
                  const std::vector<SettingsRow> &rows, const PageLayout &page, HintDevice device, const Backdrop &backdrop,
                  const std::string &subtitle, const char *escapeVerb)
    {
        const uint32_t kWhite = rgba(255, 255, 255), kDim = kHelpColour, kAmber = rgba(255, 196, 64);
        canvas.clear(rgba(10, 10, 14));
        const int selection = shell.selection();
        const bool onGame = selection >= 0 && selection < static_cast<int>(shell.games());
        const bool onQuit = selection == shell.quitItem(), onSettings = selection == shell.settingsItem();
        const bool onFreeplayPage = shell.page() == MenuPage::FreeplayPlayer || shell.page() == MenuPage::FreeplayMap;
        const bool onTiles = shell.page() == MenuPage::Tiles;
        const int shown = onGame ? selection : onFreeplayPage ? shell.freeplayGame() : std::max(shell.lastGame(), 0);
        const Tile *shownTile = shown >= 0 && shown < static_cast<int>(tiles.size()) ? &tiles[static_cast<size_t>(shown)] : nullptr;
        // The shown game's colour marks what is chosen; the Custom Freeplay
        // tile and Settings and Quit keep white.
        const uint32_t accent = shownTile && (onGame || !onTiles) ? shownTile->colour : kWhite;
        auto whole = [](const Image &art) { return Rect{0, 0, art.width, art.height}; };
        auto focusOf = [&](const Tile &tile) { return tile.focus.empty() ? whole(tile.art) : tile.focus; };
        const Rect screen{0, 0, canvas.width(), canvas.height()};
        const float unit = canvas.height() / 448.0f; // the games' screen height

        // Behind everything: the shown game's menu movie or its still, its
        // tile picture dark when it has neither, or the games' stripes.
        if (backdrop.picture && !backdrop.picture->empty())
            canvas.draw(*backdrop.picture, backdrop.part.empty() ? whole(*backdrop.picture) : backdrop.part, screen, Fit::Cover,
                        onTiles ? 0.62f : 0.32f);
        else if (shownTile && !shownTile->art.empty())
            canvas.draw(shownTile->art, focusOf(*shownTile), screen, Fit::Cover, 0.22f);
        else
            stripes(canvas, tiles);
        // (the text's lines over it kept readable: darker at the top and under the cards)
        shade(canvas, 0, canvas.height() / 6, 150, 0);
        shade(canvas, canvas.height() * 3 / 5, canvas.height(), 0, 200);

        // A line with a drop shadow, its left at x.
        auto line = [&](int x, int y, std::string_view text, float scale, uint32_t colour)
        {
            const int drop = std::max(1, static_cast<int>(unit));
            canvas.text(font, x + drop, y + drop, text, scale, rgba(0, 0, 0, 200));
            return canvas.text(font, x, y, text, scale, colour);
        };
        auto centred = [&](const Rect &rect, std::string_view text, float scale, uint32_t colour)
        {
            // A line wider than its rectangle comes smaller (the built-in font, before AI's is there, is wider).
            if (const int wide = Canvas::textWidth(font, text, scale); wide > rect.width && rect.width > 0)
                scale *= static_cast<float>(rect.width) / static_cast<float>(wide);
            const int width = Canvas::textWidth(font, text, scale);
            const int y = rect.y + (rect.height - static_cast<int>(font.height * scale)) / 2;
            line(rect.x + (rect.width - width) / 2, y, text, scale, colour);
        };
        // An item's band and frame, as the chosen one is marked.
        auto marked = [&](const Rect &rect)
        {
            canvas.fill(rect, withAlpha(accent, 40));
            canvas.frame(rect, std::max(1, static_cast<int>(unit)), accent);
        };

        // A row's mark in its square: a box, with a tick or a cross in it.
        auto stroke = [&](float x0, float y0, float x1, float y1, int thick, uint32_t colour)
        {
            const int steps = std::max(1, static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0))));
            for (int i = 0; i <= steps; ++i)
            {
                const float t = static_cast<float>(i) / steps;
                canvas.fill({static_cast<int>(x0 + (x1 - x0) * t) - thick / 2, static_cast<int>(y0 + (y1 - y0) * t) - thick / 2, thick, thick},
                            colour);
            }
        };
        auto drawMark = [&](const Rect &square, SettingsRow::Mark mark, uint32_t colour)
        {
            const int thin = std::max(1, static_cast<int>(unit)), thick = std::max(2, static_cast<int>(2 * unit));
            canvas.frame(square, thin, colour);
            const float x = static_cast<float>(square.x), y = static_cast<float>(square.y), size = static_cast<float>(square.width);
            if (mark == SettingsRow::Mark::Tick)
            {
                stroke(x + size * 0.22f, y + size * 0.52f, x + size * 0.42f, y + size * 0.74f, thick, colour);
                stroke(x + size * 0.42f, y + size * 0.74f, x + size * 0.80f, y + size * 0.26f, thick, colour);
            }
            else if (mark == SettingsRow::Mark::Cross)
            {
                stroke(x + size * 0.28f, y + size * 0.28f, x + size * 0.72f, y + size * 0.72f, thick, colour);
                stroke(x + size * 0.72f, y + size * 0.28f, x + size * 0.28f, y + size * 0.72f, thick, colour);
            }
        };

        // The pages' Back line.
        auto backLine = [&](const Rect &rect, bool on)
        {
            if (rect.empty())
                return; // (a page with no Back: a setup running)
            if (on)
                marked(rect);
            centred(rect, "Back", 1.25f * unit, on ? kWhite : kDim);
        };
        auto noticeLine = [&]()
        {
            if (!notice.empty())
            {
                const FittedLine fitted = fitLine(font, notice, layout.hint.width - static_cast<int>(16 * unit), 1.0f * unit, 0.6f * unit);
                // (under the name and its line, above the cards)
                centred({layout.hint.x, layout.lastPlayed.y + layout.lastPlayed.height, layout.hint.width, layout.hint.height}, fitted.text,
                        fitted.scale, kAmber);
            }
        };
        // A game's card: its picture (the focus whole, on the picture's own
        // mean colour), or for a game not set up its colour with its name
        // and Set Up.
        auto gameTile = [&](const Tile &tile, const Rect &rect, bool chosen)
        {
            canvas.fill(rect, rgba(0, 0, 0));
            if (tile.art.empty())
            {
                canvas.fill(rect, scaledColour(tile.colour, chosen ? 0.32f : 0.2f));
                // (the name made to fit the tile: the built-in font is wider than the games' own)
                const int inset = 2 * static_cast<int>(4.0f * unit);
                const FittedLine name = fitLine(font, tile.cardName.empty() ? tile.name : tile.cardName, rect.width - inset, 0.9f * unit,
                                                0.4f * unit);
                const int lineHeight = static_cast<int>(font.height * name.scale);
                const Rect upper{rect.x, rect.y + rect.height / 2 - lineHeight, rect.width, lineHeight};
                centred(upper, name.text, name.scale, chosen ? kWhite : kDim);
                if (!tile.installed)
                    centred({rect.x, rect.y + rect.height / 2 + lineHeight / 4, rect.width, lineHeight}, "Set Up", name.scale * 0.85f, kDim);
                return;
            }
            const float brightness = chosen ? 1.0f : 0.55f;
            if (tile.focus.empty())
                canvas.draw(tile.art, whole(tile.art), rect, Fit::Cover, brightness);
            else
            {
                const uint32_t mean = meanColour(tile.art, tile.focus);
                canvas.fill(rect, scaledColour(mean, brightness));
                canvas.draw(tile.art, tile.focus, rect, Fit::Contain, brightness);
            }
        };
        auto tileFrame = [&](const Rect &rect, bool chosen, uint32_t colour)
        {
            if (chosen)
                canvas.frame(rect, std::max(2, static_cast<int>(3 * unit)), colour);
            else
                canvas.frame(rect, std::max(1, static_cast<int>(unit)), rgba(255, 255, 255, 50));
        };
        // The chosen card grows; the others keep their place.
        auto cardRect = [&](size_t i, bool chosen) { return chosen ? scaled(layout.tiles[i], 1.2f) : layout.tiles[i]; };
        // The shown game's riders at the sides (the tiles and the player page).
        auto riders = [&]()
        {
            const Rect rooms[2] = {layout.riderLeft, layout.riderRight};
            for (int side = 0; side < 2; ++side)
                if (const Image *rider = backdrop.riders[side]; rider && !rider->empty())
                    canvas.draw(*rider, whole(*rider), standing(*rider, rooms[side]), Fit::Contain, 1.0f, backdrop.riderOpacity);
        };
        // The game booting: its card grown from its place to the screen.
        auto launch = [&]()
        {
            if (backdrop.launch <= 0.0f || !shownTile || shown >= static_cast<int>(layout.tiles.size()))
                return;
            const Rect rect = between(cardRect(static_cast<size_t>(shown), true), screen, std::min(1.0f, backdrop.launch));
            gameTile(*shownTile, rect, true);
            tileFrame(rect, true, shownTile->colour);
        };
        auto fade = [&]()
        {
            if (backdrop.black > 0.0f)
                canvas.fill(screen, rgba(0, 0, 0, static_cast<uint8_t>(std::lround(std::min(1.0f, backdrop.black) * 255.0f))));
        };

        if (shell.page() == MenuPage::FreeplayPlayer)
        {
            // Custom Freeplay's player page: the games' cards again, each
            // for its player; the chosen one's player and game named over
            // them, Back below.
            riders();
            centred(page.title, kFreeplayTitle, 2.0f * unit, kWhite);
            centred({0, page.title.y + page.title.height, canvas.width(), canvas.height() / 24}, "Choose a player", 1.0f * unit, kDim);
            const int chosenGame = shell.freeplayGame();
            const bool onBack = shell.freeplayOnBack();
            const std::vector<int> &players = shell.freeplayPlayers();
            for (size_t i = 0; i < players.size() && i < layout.tiles.size(); ++i)
            {
                const int game = players[i];
                if (game < 0 || game >= static_cast<int>(tiles.size()))
                    continue;
                const Tile &tile = tiles[static_cast<size_t>(game)];
                const bool chosen = game == chosenGame && !onBack;
                const Rect rect = cardRect(i, chosen);
                gameTile(tile, rect, chosen);
                tileFrame(rect, chosen, tile.colour);
            }
            if (std::find(players.begin(), players.end(), chosenGame) != players.end() && chosenGame < static_cast<int>(tiles.size()))
            {
                const Tile &tile = tiles[static_cast<size_t>(chosenGame)];
                centred(layout.name, tile.player.empty() ? tile.name : tile.player, 1.5f * unit, kWhite);
                if (!tile.player.empty())
                    centred(layout.lastPlayed, tile.name, 0.9f * unit, kDim);
            }
            backLine(page.back, onBack);
            noticeLine();
            centred(layout.hint, buttonHint(device, "Select", "Back"), 0.9f * unit, kDim);
            fade();
            return;
        }

        if (!onTiles)
        {
            // The app's settings page (or the Mods page it opens, or Custom
            // Freeplay's maps): the title over a rule in the shown game's
            // colour, a row per setting, the chosen one marked with its
            // value between arrows, its help under the rows, Back at the
            // bottom. A game's name over its rows in amber; a page longer
            // than its lines scrolls, with dots where rows are out of view.
            // A page with pictures shows the chosen row's beside the rows.
            const bool freeplay = shell.page() == MenuPage::FreeplayMap;
            const bool mods = shell.page() == MenuPage::Mods || shell.page() == MenuPage::Mod;
            centred(page.title,
                    freeplay                           ? kFreeplayTitle
                    : mods                             ? "Mods"
                    : shell.page() == MenuPage::Games ? "Games"
                    : shell.page() == MenuPage::Recordings ? "Recordings"
                                                      : "Settings",
                    2.0f * unit, kWhite);
            canvas.fill({0, page.title.y + page.title.height + static_cast<int>(2 * unit), canvas.width(), std::max(2, static_cast<int>(unit))},
                        accent);
            if (freeplay || !subtitle.empty())
                centred({0, page.title.y + page.title.height + static_cast<int>(4 * unit), canvas.width(), canvas.height() / 24},
                        freeplay ? "Choose a map" : subtitle, 1.0f * unit, kDim);
            const int backRow = static_cast<int>(rows.size());
            const int chosenRow = shell.chosenRow(rows);
            const int first = shell.pageFirst();
            std::string help;
            std::shared_ptr<const Image> picture;
            for (size_t slot = 0; slot < page.rows.size() && first + slot < rows.size(); ++slot)
            {
                const int index = first + static_cast<int>(slot);
                const SettingsRow &row = rows[static_cast<size_t>(index)];
                const Rect &rect = page.rows[slot];
                const bool chosen = index == chosenRow;
                if (chosen)
                {
                    marked(rect);
                    help = row.help;
                    picture = row.picture;
                }
                // Rows closer than the usual spacing (a long page) in smaller text.
                const float scale = 1.1f * unit * std::min(1.0f, rect.height / (canvas.height() / 20.0f));
                // Every line in its row's rectangle: smaller, then cut short,
                // never into the next text or the picture beside the rows.
                const int rowRight = rect.x + rect.width;
                auto lineY = [&](float at) { return rect.y + (rect.height - static_cast<int>(font.height * at)) / 2; };
                if (row.kind == SettingsRow::Heading)
                {
                    const int headingX = page.labelX - static_cast<int>(8 * unit);
                    const FittedLine heading = fitLine(font, row.label, rowRight - headingX, scale, 0.6f * scale);
                    line(headingX, lineY(heading.scale), heading.text, heading.scale, kAmber);
                    continue;
                }
                const uint32_t colour = chosen ? kWhite : kDim;
                if (row.steady)
                {
                    // A list's line: one size whatever its text, the label
                    // `indent` steps in (after its mark), the value from its
                    // column; a text too long is cut, never made smaller.
                    const float at = 0.62f * unit;
                    const int textHeight = static_cast<int>(font.height * at);
                    int x = page.labelX + row.indent * textHeight * 3 / 2;
                    const int column = page.labelX + (page.valueX - page.labelX) * 9 / 20;
                    const int gap = static_cast<int>(12 * unit);
                    if (const float done = row.progress ? row.progress() : -1.0f; done >= 0.0f)
                    {
                        const int thick = std::max(2, static_cast<int>(2 * unit));
                        const Rect bar{x, rect.y + rect.height - thick - 1, page.valueX - x, thick};
                        canvas.fill(bar, rgba(255, 255, 255, 40));
                        canvas.fill({bar.x, bar.y, static_cast<int>(bar.width * std::min(done, 1.0f)), bar.height}, accent);
                    }
                    SettingsRow::Mark mark = row.mark ? row.mark() : SettingsRow::Mark::None;
                    if (row.kind == SettingsRow::Check && !row.mark)
                        mark = row.current && row.current() != 0 ? SettingsRow::Mark::Tick : SettingsRow::Mark::Box;
                    if (mark != SettingsRow::Mark::None)
                    {
                        const int box = textHeight * 4 / 5;
                        drawMark({x, rect.y + (rect.height - box) / 2, box, box}, mark, colour);
                        x += box + gap / 2;
                    }
                    // (a game's own line bright, the lines under it dim, the chosen one white)
                    const uint32_t labelColour = chosen || row.indent == 0 ? kWhite : kDim;
                    const FittedLine label = fitLine(font, row.label, std::max(column - gap - x, 0), at, at);
                    line(x, lineY(at), label.text, at, labelColour);
                    if (const std::string value = row.valueText(row.current ? row.current() : 0); !value.empty())
                    {
                        const int valueX = std::max(column, x + label.width + gap);
                        const FittedLine fitted = fitLine(font, value, std::max(page.valueX - valueX, 0), at, at);
                        line(valueX, lineY(at), fitted.text, at, colour);
                    }
                    continue;
                }
                // Work under way: a bar along the row's foot.
                if (const float done = row.progress ? row.progress() : -1.0f; done >= 0.0f)
                {
                    const int thick = std::max(2, static_cast<int>(2 * unit));
                    const Rect bar{page.labelX, rect.y + rect.height - thick - 1, page.valueX - page.labelX, thick};
                    canvas.fill(bar, rgba(255, 255, 255, 40));
                    canvas.fill({bar.x, bar.y, static_cast<int>(bar.width * std::min(done, 1.0f)), bar.height}, accent);
                }
                // A mark at the label's left (a Check's tick box), the label after it.
                SettingsRow::Mark mark = row.mark ? row.mark() : SettingsRow::Mark::None;
                if (row.kind == SettingsRow::Check && !row.mark)
                    mark = row.current && row.current() != 0 ? SettingsRow::Mark::Tick : SettingsRow::Mark::Box;
                int labelX = page.labelX;
                if (mark != SettingsRow::Mark::None)
                {
                    const int box = static_cast<int>(font.height * scale * 0.7f);
                    const Rect square{labelX, rect.y + (rect.height - box) / 2, box, box};
                    drawMark(square, mark, colour);
                    labelX += box + static_cast<int>(8 * unit);
                }
                if (row.kind == SettingsRow::Note)
                {
                    const std::string value = row.valueText(0);
                    if (value.empty())
                    {
                        const FittedLine note = fitLine(font, row.label, rowRight - labelX, 0.82f * scale, 0.6f * scale);
                        line(labelX, lineY(note.scale), note.text, note.scale, colour);
                        continue;
                    }
                    const RowText laid = layRowText(font, row.label, value, labelX, page.valueX, 0.82f * scale, static_cast<int>(12 * unit));
                    line(laid.labelX, lineY(laid.label.scale), laid.label.text, laid.label.scale, colour);
                    line(laid.valueX, lineY(laid.value.scale), laid.value.text, laid.value.scale, colour);
                    continue;
                }
                const int value = row.current ? row.current() : 0;
                const std::string valueText = row.valueText(value);
                const bool arrows = row.kind != SettingsRow::Page && row.kind != SettingsRow::Action && row.kind != SettingsRow::Check;
                const bool pointer = row.kind == SettingsRow::Page || row.kind == SettingsRow::Action;
                const std::string text = !chosen ? valueText : arrows ? "< " + valueText + " >" : pointer ? valueText + " >" : valueText;
                const RowText laid = layRowText(font, row.label, text, labelX, page.valueX, scale, static_cast<int>(12 * unit));
                line(laid.labelX, lineY(laid.label.scale), laid.label.text, laid.label.scale, colour);
                line(laid.valueX, lineY(laid.value.scale), laid.value.text, laid.value.scale, colour);
            }
            if (!page.rows.empty())
            {
                const Rect &top = page.rows.front(), &bottom = page.rows.back();
                if (first > 0)
                    centred({top.x, top.y - top.height / 2 - 4, top.width, top.height / 2}, "...", 0.7f * unit, kDim);
                if (first + static_cast<int>(page.rows.size()) < backRow)
                    centred({bottom.x, bottom.y + bottom.height, bottom.width, page.help.y - bottom.y - bottom.height}, "...",
                            0.7f * unit, kDim);
            }
            if (!page.picture.empty())
            {
                // The chosen row's picture in its box (the whole picture,
                // centred), or the box alone.
                canvas.fill(page.picture, rgba(0, 0, 0, 120));
                if (picture && !picture->empty())
                    canvas.draw(*picture, whole(*picture), page.picture, Fit::Contain);
                else
                    centred(page.picture, "No Preview", 0.9f * unit, kDim);
                canvas.frame(page.picture, std::max(1, static_cast<int>(unit)), rgba(255, 255, 255, 60));
            }
            // (a page too full for the notice's own line: the notice in the help's place)
            const bool noticeOnHelp = !notice.empty() && page.notice.height < page.help.height / 2;
            if (!help.empty() && !noticeOnHelp)
            {
                const FittedLine fitted = fitLine(font, help, page.help.width - static_cast<int>(16 * unit), 0.9f * unit, 0.6f * unit);
                centred(page.help, fitted.text, fitted.scale, kDim);
            }
            backLine(page.back, chosenRow == backRow);
            if (noticeOnHelp)
            {
                const FittedLine fitted = fitLine(font, notice, page.help.width - static_cast<int>(16 * unit), 1.0f * unit, 0.6f * unit);
                centred(page.help, fitted.text, fitted.scale, kAmber);
            }
            else if (!notice.empty())
            {
                // (the page's own line for it, between the help and Back)
                const FittedLine fitted = fitLine(font, notice, page.notice.width - static_cast<int>(16 * unit), 1.0f * unit, 0.6f * unit);
                centred(page.notice, fitted.text, fitted.scale, kAmber);
            }
            // One hint line for what the chosen row does (the page's Back
            // stands for the back button; a page with none names what the
            // back button does there).
            const char *verb = nullptr;
            if (escapeVerb)
                ; // (a setup running: only the back button does anything)
            else if (chosenRow >= backRow || freeplay)
                verb = chosenRow >= backRow ? "Select" : "Start";
            else
                switch (rows[static_cast<size_t>(chosenRow)].kind)
                {
                case SettingsRow::Page:
                case SettingsRow::Action:
                    verb = "Select";
                    break;
                case SettingsRow::Check:
                    verb = "Tick";
                    break;
                case SettingsRow::Note:
                case SettingsRow::Heading:
                    verb = nullptr;
                    break;
                default:
                    verb = "Change";
                    break;
                }
            centred(layout.hint, buttonHint(device, verb, escapeVerb), 0.9f * unit, kDim);
            fade();
            return;
        }

        // The cabinet: the title at the top left, Settings and Quit at the
        // top right, the riders at the sides, the chosen game's name, the
        // cards.
        riders();
        {
            const FittedLine fitted = fitLine(font, title, layout.title.width, 1.5f * unit, 0.8f * unit);
            line(layout.title.x, layout.title.y + (layout.title.height - static_cast<int>(font.height * fitted.scale)) / 2, fitted.text,
                 fitted.scale, kWhite);
        }
        if (onSettings)
            marked(layout.settings);
        centred(layout.settings, "Settings", 1.1f * unit, onSettings ? kWhite : kDim);
        if (onQuit)
            marked(layout.quit);
        centred(layout.quit, "Quit", 1.1f * unit, onQuit ? kWhite : kDim);

        // The cards, the chosen one last (it grows over its neighbours).
        const int freeplayItem = shell.freeplayItem();
        auto freeplayTile = [&](bool chosen)
        {
            // The Custom Freeplay tile: the games' pictures side by side,
            // a band with its name over them.
            const Rect rect = cardRect(static_cast<size_t>(freeplayItem), chosen);
            canvas.fill(rect, rgba(0, 0, 0));
            const int parts = static_cast<int>(std::max<size_t>(tiles.size(), 1));
            for (int i = 0; i < parts && i < static_cast<int>(tiles.size()); ++i)
            {
                const Rect part{rect.x + rect.width * i / parts, rect.y, rect.width * (i + 1) / parts - rect.width * i / parts, rect.height};
                const Tile &tile = tiles[static_cast<size_t>(i)];
                if (tile.art.empty())
                    canvas.fill(part, scaledColour(tile.colour, 0.25f));
                else
                    canvas.draw(tile.art, focusOf(tile), part, Fit::Cover, chosen ? 0.8f : 0.4f);
            }
            const Rect band{rect.x, rect.y + rect.height * 2 / 5, rect.width, rect.height / 5};
            canvas.fill(band, rgba(0, 0, 0, 170));
            // The caption as large as the tile allows.
            const FittedLine caption = fitLine(font, kFreeplayTitle, rect.width - static_cast<int>(8 * unit), 0.9f * unit, 0.3f * unit);
            centred(band, caption.text, caption.scale, chosen ? kWhite : kDim);
            tileFrame(rect, chosen, kWhite);
        };
        for (size_t i = 0; i < tiles.size() && i < layout.tiles.size(); ++i)
            if (static_cast<int>(i) != selection)
            {
                gameTile(tiles[i], cardRect(i, false), false);
                tileFrame(cardRect(i, false), false, tiles[i].colour);
            }
        const bool freeplayShown = freeplayItem >= 0 && freeplayItem < static_cast<int>(layout.tiles.size());
        if (freeplayShown && selection != freeplayItem)
            freeplayTile(false);
        if (onGame && selection < static_cast<int>(tiles.size()) && selection < static_cast<int>(layout.tiles.size()))
        {
            const size_t i = static_cast<size_t>(selection);
            gameTile(tiles[i], cardRect(i, true), true);
            tileFrame(cardRect(i, true), true, tiles[i].colour);
        }
        if (freeplayShown && selection == freeplayItem)
            freeplayTile(true);

        if (onGame && selection < static_cast<int>(tiles.size()))
        {
            const Tile &tile = tiles[static_cast<size_t>(selection)];
            centred(layout.name, tile.name, 1.5f * unit, kWhite);
            if (!tile.installed)
                centred(layout.lastPlayed, "Set it up from your disc.", 0.9f * unit, kDim);
            else if (!tile.lastPlayed.empty())
                centred(layout.lastPlayed, tile.lastPlayed, 0.9f * unit, kDim);
        }
        else if (selection == freeplayItem && freeplayItem >= 0)
        {
            centred(layout.name, kFreeplayTitle, 1.5f * unit, kWhite);
            centred(layout.lastPlayed, "Any player on any map", 0.9f * unit, kDim);
        }

        noticeLine();
        const bool setUp = onGame && selection < static_cast<int>(tiles.size()) && !tiles[static_cast<size_t>(selection)].installed;
        centred(layout.hint, buttonHint(device, onGame ? (setUp ? "Set Up" : "Play") : "Select"), 0.9f * unit, kDim);
        launch();
        fade();
    }

    void drawIntro(Canvas &canvas, const Image &picture, const BitmapFont &font, const std::string &hint, float black)
    {
        canvas.clear(rgba(0, 0, 0));
        if (!picture.empty())
            canvas.draw(picture, {0, 0, picture.width, picture.height}, {0, 0, canvas.width(), canvas.height()}, Fit::Contain);
        if (black > 0.0f)
            canvas.fill({0, 0, canvas.width(), canvas.height()}, rgba(0, 0, 0, static_cast<uint8_t>(std::lround(std::min(1.0f, black) * 255.0f))));
        if (!hint.empty() && !font.empty())
        {
            const float scale = 0.8f * canvas.height() / 448.0f;
            const int width = Canvas::textWidth(font, hint, scale);
            canvas.text(font, (canvas.width() - width) / 2, canvas.height() * 94 / 100 - static_cast<int>(font.height * scale) / 2, hint,
                        scale, rgba(150, 150, 160, 160));
        }
    }
}
