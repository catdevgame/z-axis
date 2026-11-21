#include "runtime/ps2_zaxis_xbox.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace zx::zaxis::xbox
{
    namespace
    {
        uint32_t u32(const uint8_t *p)
        {
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }

        uint16_t u16(const uint8_t *p)
        {
            uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return v;
        }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        void put16(std::vector<uint8_t> &out, uint16_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }

        void putTag(std::vector<uint8_t> &out, const char *tag)
        {
            out.insert(out.end(), tag, tag + 4);
        }

        uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
        {
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        // RGB565 colours of a DXT colour block, as RGBA (alpha 255).
        void dxtColours(const uint8_t *block, bool allowTransparent, uint32_t colours[4])
        {
            const uint16_t c0 = u16(block), c1 = u16(block + 2);
            auto expand = [](uint16_t c, uint32_t &r, uint32_t &g, uint32_t &b)
            {
                r = ((c >> 11) & 31u) * 255u / 31u;
                g = ((c >> 5) & 63u) * 255u / 63u;
                b = (c & 31u) * 255u / 31u;
            };
            uint32_t r0, g0, b0, r1, g1, b1;
            expand(c0, r0, g0, b0);
            expand(c1, r1, g1, b1);
            colours[0] = rgba(r0, g0, b0, 255u);
            colours[1] = rgba(r1, g1, b1, 255u);
            if (c0 > c1 || !allowTransparent)
            {
                colours[2] = rgba((2u * r0 + r1) / 3u, (2u * g0 + g1) / 3u, (2u * b0 + b1) / 3u, 255u);
                colours[3] = rgba((r0 + 2u * r1) / 3u, (g0 + 2u * g1) / 3u, (b0 + 2u * b1) / 3u, 255u);
            }
            else
            {
                colours[2] = rgba((r0 + r1) / 2u, (g0 + g1) / 2u, (b0 + b1) / 2u, 255u);
                colours[3] = 0u;
            }
        }

        void decodeDxt(const uint8_t *data, int width, int height, int kind, Picture &out)
        {
            const int bw = std::max(1, (width + 3) / 4), bh = std::max(1, (height + 3) / 4);
            const size_t blockSize = kind == 1 ? 8u : 16u;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                {
                    const uint8_t *block = data + (static_cast<size_t>(by) * bw + bx) * blockSize;
                    const uint8_t *colourBlock = kind == 1 ? block : block + 8;
                    uint32_t colours[4];
                    dxtColours(colourBlock, kind == 1, colours);
                    uint8_t alpha[16];
                    if (kind == 3)
                    {
                        for (int i = 0; i < 16; ++i)
                        {
                            const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xFu;
                            alpha[i] = static_cast<uint8_t>(nibble * 17u);
                        }
                    }
                    else if (kind == 5)
                    {
                        const uint32_t a0 = block[0], a1 = block[1];
                        uint32_t table[8] = {a0, a1};
                        if (a0 > a1)
                            for (uint32_t i = 1; i < 7; ++i)
                                table[i + 1] = ((7u - i) * a0 + i * a1) / 7u;
                        else
                        {
                            for (uint32_t i = 1; i < 5; ++i)
                                table[i + 1] = ((5u - i) * a0 + i * a1) / 5u;
                            table[6] = 0u;
                            table[7] = 255u;
                        }
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i)
                            bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                        for (int i = 0; i < 16; ++i)
                            alpha[i] = static_cast<uint8_t>(table[(bits >> (3 * i)) & 7u]);
                    }
                    const uint32_t indices = u32(colourBlock + 4);
                    for (int i = 0; i < 16; ++i)
                    {
                        const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x >= width || y >= height)
                            continue;
                        uint32_t c = colours[(indices >> (2 * i)) & 3u];
                        if (kind != 1)
                            c = (c & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha[i]) << 24);
                        out.rgba[static_cast<size_t>(y) * width + x] = c;
                    }
                }
        }

        // Morton order of the Xbox's swizzled textures: x and y bits
        // interleaved (x first) while both have bits left.
        size_t swizzledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
        {
            size_t offset = 0;
            uint32_t bit = 0;
            for (uint32_t mask = 1; mask < width || mask < height; mask <<= 1)
            {
                if (mask < width)
                {
                    if (x & mask)
                        offset |= size_t(1) << bit;
                    ++bit;
                }
                if (mask < height)
                {
                    if (y & mask)
                        offset |= size_t(1) << bit;
                    ++bit;
                }
            }
            return offset;
        }

        int channelWeight(int channel) { return channel == 0 ? 2 : channel == 1 ? 4 : 3; }

        int distance(uint32_t a, uint32_t b, bool withAlpha)
        {
            int d = 0;
            for (int c = 0; c < (withAlpha ? 4 : 3); ++c)
            {
                const int x = static_cast<int>((a >> (8 * c)) & 0xFFu) - static_cast<int>((b >> (8 * c)) & 0xFFu);
                d += x * x * channelWeight(c);
            }
            return d;
        }
    }

    bool decodeTexture(const uint8_t *txpr, size_t size, Picture &out, std::string *error)
    {
        auto fail = [&](const char *why)
        {
            if (error)
                *error = why;
            return false;
        };
        if (size < 4u + 32u || std::memcmp(txpr + 4, "XPR0", 4) != 0)
            return fail("no XPR0 resource");
        const uint8_t *xpr = txpr + 4;
        const size_t xprSize = size - 4u;
        const uint32_t headerSize = u32(xpr + 8);
        const uint32_t dataOffset = u32(xpr + 16);
        const uint32_t format = u32(xpr + 24);
        const uint32_t sizeWord = u32(xpr + 28);
        const uint32_t kind = (format >> 8) & 0xFFu;
        int width = 0, height = 0;
        uint32_t pitch = 0;
        if (sizeWord != 0u)
        {
            width = static_cast<int>((sizeWord & 0xFFFu) + 1u);
            height = static_cast<int>(((sizeWord >> 12) & 0xFFFu) + 1u);
            pitch = (((sizeWord >> 24) & 0xFFu) + 1u) * 64u;
        }
        else
        {
            width = 1 << ((format >> 20) & 0xFu);
            height = 1 << ((format >> 24) & 0xFu);
        }
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
            return fail("bad texture size");
        const size_t start = static_cast<size_t>(headerSize) + dataOffset;
        if (start >= xprSize)
            return fail("texture data outside the resource");
        const uint8_t *data = xpr + start;
        const size_t available = xprSize - start;
        out.width = width;
        out.height = height;
        out.rgba.assign(static_cast<size_t>(width) * height, 0u);
        const size_t blocks = static_cast<size_t>(std::max(1, (width + 3) / 4)) * std::max(1, (height + 3) / 4);
        switch (kind)
        {
        case 0x0C: // DXT1
            if (available < blocks * 8u)
                return fail("DXT1 data cut short");
            decodeDxt(data, width, height, 1, out);
            return true;
        case 0x0E: // DXT2/3
            if (available < blocks * 16u)
                return fail("DXT3 data cut short");
            decodeDxt(data, width, height, 3, out);
            return true;
        case 0x0F: // DXT4/5
            if (available < blocks * 16u)
                return fail("DXT5 data cut short");
            decodeDxt(data, width, height, 5, out);
            return true;
        case 0x06: // A8R8G8B8, swizzled
        case 0x07: // X8R8G8B8, swizzled
        case 0x12: // A8R8G8B8, linear
        case 0x1E: // X8R8G8B8, linear
        {
            const bool linear = kind == 0x12 || kind == 0x1E;
            const size_t rowBytes = linear && pitch ? pitch : static_cast<size_t>(width) * 4u;
            if (available < rowBytes * height)
                return fail("ARGB data cut short");
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const size_t at = linear ? y * rowBytes + x * 4u
                                             : swizzledOffset(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                                              static_cast<uint32_t>(width), static_cast<uint32_t>(height)) *
                                                   4u;
                    const uint32_t argb = u32(data + at);
                    const uint32_t a = (kind == 0x07 || kind == 0x1E) ? 255u : (argb >> 24);
                    out.rgba[static_cast<size_t>(y) * width + x] =
                        rgba((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu, a);
                }
            return true;
        }
        default:
            return fail("unsupported texture format");
        }
    }

    void palettize(const Picture &picture, bool withAlpha, Palettized &out)
    {
        // Unique colours and how often each is used.
        std::unordered_map<uint32_t, uint32_t> counts;
        counts.reserve(picture.rgba.size() / 4u + 16u);
        for (uint32_t c : picture.rgba)
            ++counts[withAlpha ? c : (c | 0xFF000000u)];
        struct Colour
        {
            uint32_t value;
            uint32_t count;
        };
        std::vector<Colour> colours;
        colours.reserve(counts.size());
        for (const auto &entry : counts)
            colours.push_back({entry.first, entry.second});
        std::sort(colours.begin(), colours.end(), [](const Colour &a, const Colour &b) { return a.value < b.value; });
        const int channels = withAlpha ? 4 : 3;
        std::vector<uint32_t> palette;
        if (colours.size() <= 256u)
        {
            for (const Colour &c : colours)
                palette.push_back(c.value);
        }
        else
        {
            // Median cut: split the box with the widest weighted range at
            // its weighted median.
            struct Box
            {
                size_t begin, end;
            };
            std::vector<Box> boxes{{0u, colours.size()}};
            auto channelOf = [](uint32_t v, int c) { return static_cast<int>((v >> (8 * c)) & 0xFFu); };
            while (boxes.size() < 256u)
            {
                double best = 0.0;
                int bestBox = -1, bestChannel = 0;
                for (size_t i = 0; i < boxes.size(); ++i)
                {
                    const Box &box = boxes[i];
                    if (box.end - box.begin < 2u)
                        continue;
                    uint64_t weight = 0;
                    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                    for (size_t k = box.begin; k < box.end; ++k)
                    {
                        weight += colours[k].count;
                        for (int c = 0; c < channels; ++c)
                        {
                            lo[c] = std::min(lo[c], channelOf(colours[k].value, c));
                            hi[c] = std::max(hi[c], channelOf(colours[k].value, c));
                        }
                    }
                    for (int c = 0; c < channels; ++c)
                    {
                        const double score = static_cast<double>(hi[c] - lo[c]) * channelWeight(c) *
                                             std::sqrt(static_cast<double>(weight));
                        if (score > best)
                        {
                            best = score;
                            bestBox = static_cast<int>(i);
                            bestChannel = c;
                        }
                    }
                }
                if (bestBox < 0)
                    break;
                Box box = boxes[static_cast<size_t>(bestBox)];
                std::sort(colours.begin() + static_cast<std::ptrdiff_t>(box.begin),
                          colours.begin() + static_cast<std::ptrdiff_t>(box.end),
                          [&](const Colour &a, const Colour &b)
                          {
                              const int ca = channelOf(a.value, bestChannel), cb = channelOf(b.value, bestChannel);
                              return ca < cb || (ca == cb && a.value < b.value);
                          });
                uint64_t total = 0, running = 0;
                for (size_t k = box.begin; k < box.end; ++k)
                    total += colours[k].count;
                size_t split = box.begin + 1u;
                for (size_t k = box.begin; k + 1u < box.end; ++k)
                {
                    running += colours[k].count;
                    split = k + 1u;
                    if (running * 2u >= total)
                        break;
                }
                boxes[static_cast<size_t>(bestBox)] = {box.begin, split};
                boxes.push_back({split, box.end});
            }
            for (const Box &box : boxes)
            {
                double sum[4] = {0, 0, 0, 0};
                double weight = 0;
                for (size_t k = box.begin; k < box.end; ++k)
                {
                    weight += colours[k].count;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += static_cast<double>(channelOf(colours[k].value, c)) * colours[k].count;
                }
                uint32_t v = 0;
                for (int c = 0; c < 4; ++c)
                    v |= static_cast<uint32_t>(std::clamp(static_cast<int>(sum[c] / weight + 0.5), 0, 255)) << (8 * c);
                palette.push_back(v);
            }
            // K-means refinement on the unique colours.
            std::vector<uint8_t> nearest(colours.size());
            for (int pass = 0; pass < 3; ++pass)
            {
                std::vector<std::array<double, 5>> sums(palette.size(), {0, 0, 0, 0, 0});
                for (size_t k = 0; k < colours.size(); ++k)
                {
                    int bestD = 1 << 30, best = 0;
                    for (size_t p = 0; p < palette.size(); ++p)
                    {
                        const int d = distance(colours[k].value, palette[p], withAlpha);
                        if (d < bestD)
                        {
                            bestD = d;
                            best = static_cast<int>(p);
                        }
                    }
                    nearest[k] = static_cast<uint8_t>(best);
                    auto &s = sums[static_cast<size_t>(best)];
                    for (int c = 0; c < 4; ++c)
                        s[c] += static_cast<double>(channelOf(colours[k].value, c)) * colours[k].count;
                    s[4] += colours[k].count;
                }
                for (size_t p = 0; p < palette.size(); ++p)
                {
                    if (sums[p][4] <= 0.0)
                        continue;
                    uint32_t v = 0;
                    for (int c = 0; c < 4; ++c)
                        v |= static_cast<uint32_t>(std::clamp(static_cast<int>(sums[p][c] / sums[p][4] + 0.5), 0, 255))
                             << (8 * c);
                    palette[p] = v;
                }
            }
        }
        // Each unique colour's nearest entry, then every pixel.
        std::unordered_map<uint32_t, uint8_t> lookup;
        lookup.reserve(colours.size());
        for (const Colour &c : colours)
        {
            int bestD = 1 << 30, best = 0;
            for (size_t p = 0; p < palette.size(); ++p)
            {
                const int d = distance(c.value, palette[p], withAlpha);
                if (d < bestD)
                {
                    bestD = d;
                    best = static_cast<int>(p);
                    if (d == 0)
                        break;
                }
            }
            lookup.emplace(c.value, static_cast<uint8_t>(best));
        }
        out.palette = palette;
        out.palette.resize(256u, withAlpha ? 0u : 0xFF000000u);
        out.index.resize(picture.rgba.size());
        for (size_t i = 0; i < picture.rgba.size(); ++i)
            out.index[i] = lookup[withAlpha ? picture.rgba[i] : (picture.rgba[i] | 0xFF000000u)];
    }

    std::vector<uint8_t> writeBmp8(int width, int height, const Palettized &image, bool keepAlpha)
    {
        const uint32_t rowBytes = (static_cast<uint32_t>(width) + 3u) & ~3u;
        const uint32_t pixelBytes = rowBytes * static_cast<uint32_t>(height);
        const uint32_t offset = 14u + 40u + 1024u;
        std::vector<uint8_t> out;
        out.reserve(offset + pixelBytes);
        out.push_back('B');
        out.push_back('M');
        put32(out, offset + pixelBytes);
        put32(out, 0u);
        put32(out, offset);
        put32(out, 40u);
        put32(out, static_cast<uint32_t>(width));
        put32(out, static_cast<uint32_t>(height)); // bottom-up
        put16(out, 1u);
        put16(out, 8u);
        put32(out, 0u); // BI_RGB
        put32(out, pixelBytes);
        put32(out, 0xB12u);
        put32(out, 0xB12u);
        put32(out, 0u);
        put32(out, 0u);
        for (size_t i = 0; i < 256u; ++i)
        {
            const uint32_t c = i < image.palette.size() ? image.palette[i] : 0u;
            out.push_back(static_cast<uint8_t>(c >> 16)); // B
            out.push_back(static_cast<uint8_t>(c >> 8));  // G
            out.push_back(static_cast<uint8_t>(c));       // R
            out.push_back(keepAlpha ? static_cast<uint8_t>(c >> 24) : 0u);
        }
        for (int y = height - 1; y >= 0; --y)
        {
            const uint8_t *row = image.index.data() + static_cast<size_t>(y) * width;
            out.insert(out.end(), row, row + width);
            out.insert(out.end(), rowBytes - static_cast<uint32_t>(width), 0u);
        }
        return out;
    }

    void shrink(Picture &picture, int maxSide)
    {
        if (maxSide <= 0)
            return;
        while (picture.width > maxSide || picture.height > maxSide)
        {
            const int w = std::max(1, picture.width / 2), h = std::max(1, picture.height / 2);
            const int sx = picture.width / w, sy = picture.height / h;
            std::vector<uint32_t> out(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    uint32_t sum[4] = {0, 0, 0, 0}, alphaSum = 0;
                    for (int dy = 0; dy < sy; ++dy)
                        for (int dx = 0; dx < sx; ++dx)
                        {
                            const uint32_t c = picture.rgba[static_cast<size_t>(y * sy + dy) * picture.width + (x * sx + dx)];
                            const uint32_t a = c >> 24;
                            for (int k = 0; k < 3; ++k)
                                sum[k] += ((c >> (8 * k)) & 0xFFu) * (a + 1u);
                            sum[3] += a;
                            alphaSum += a + 1u;
                        }
                    const uint32_t n = static_cast<uint32_t>(sx * sy);
                    uint32_t v = ((sum[3] + n / 2u) / n) << 24;
                    for (int k = 0; k < 3; ++k)
                        v |= ((sum[k] + alphaSum / 2u) / alphaSum) << (8 * k);
                    out[static_cast<size_t>(y) * w + x] = v;
                }
            picture.width = w;
            picture.height = h;
            picture.rgba = std::move(out);
        }
    }

    namespace
    {
        // The DXT3 levels of a TXPR's texture as they are (Xbox DXT data is
        // not swizzled), mip 0 first; false if it is not DXT3.
        bool dxt3Levels(const uint8_t *txpr, size_t size, int &width, int &height, int &mips, std::vector<uint8_t> &out)
        {
            if (size < 4u + 32u || std::memcmp(txpr + 4, "XPR0", 4) != 0)
                return false;
            const uint8_t *xpr = txpr + 4;
            const size_t xprSize = size - 4u;
            const uint32_t format = u32(xpr + 24);
            if (((format >> 8) & 0xFFu) != 0x0Eu || u32(xpr + 28) != 0u)
                return false;
            width = 1 << ((format >> 20) & 0xFu);
            height = 1 << ((format >> 24) & 0xFu);
            mips = std::max<int>(1, static_cast<int>((format >> 16) & 0xFu));
            const size_t start = static_cast<size_t>(u32(xpr + 8)) + u32(xpr + 16);
            size_t bytes = 0;
            int levels = 0;
            for (int w = width, h = height; levels < mips; ++levels, w = std::max(1, w / 2), h = std::max(1, h / 2))
            {
                const size_t level = static_cast<size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * 16u;
                if (start + bytes + level > xprSize)
                    break; // a short chain: the levels there are
                bytes += level;
            }
            if (levels == 0)
                return false;
            mips = levels;
            out.assign(xpr + start, xpr + start + bytes);
            return true;
        }

        uint64_t fnv1a64(const uint8_t *p, size_t n)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < n; ++i)
                h = (h ^ p[i]) * 0x100000001B3ull;
            return h;
        }
    }

    std::vector<int> budgetSides(const std::vector<std::pair<int, int>> &sizes, int maxSide, size_t budget,
                                 size_t *texels)
    {
        // A picture's texels once shrink() has made neither side over `side`.
        auto shrunk = [](std::pair<int, int> size, int side)
        {
            while (side > 0 && (size.first > side || size.second > side))
                size = {std::max(1, size.first / 2), std::max(1, size.second / 2)};
            return static_cast<size_t>(size.first) * static_cast<size_t>(size.second);
        };
        std::vector<int> sides(sizes.size(), maxSide);
        std::vector<size_t> bytes(sizes.size());
        size_t total = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            if (sides[i] <= 0) // no limit: the texture's own size
                sides[i] = std::max(sizes[i].first, sizes[i].second);
            total += bytes[i] = shrunk(sizes[i], sides[i]);
        }
        while (budget && total > budget)
        {
            size_t best = sizes.size();
            for (size_t i = 0; i < sizes.size(); ++i)
                if (sides[i] > 16 && (best == sizes.size() || bytes[i] > bytes[best]))
                    best = i;
            if (best == sizes.size())
                break;
            sides[best] /= 2;
            const size_t now = shrunk(sizes[best], sides[best]);
            total = total - bytes[best] + now;
            bytes[best] = now;
        }
        if (texels)
            *texels = total;
        return sides;
    }

    bool convertTextureLibrary(const std::vector<uint8_t> &xboxTxr, std::vector<uint8_t> &ps2Txr,
                               std::vector<TextureReport> *report, std::string *error, unsigned threads, int maxSide,
                               std::vector<HostTexture> *hostTextures, size_t texelBudget)
    {
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        if (xboxTxr.size() < 8u || std::memcmp(xboxTxr.data(), "FORM", 4) != 0)
            return fail("not a texture library (no FORM)");
        struct Entry
        {
            const uint8_t *header = nullptr; // TXRH payload, 52 bytes
            const uint8_t *picture = nullptr; // TXPR payload
            size_t pictureSize = 0;
            TextureReport report;
            std::vector<uint8_t> chunk; // the PS2 TXRC entry
            HostTexture host;           // host.data empty: none
        };
        std::vector<Entry> entries;
        size_t at = 8u;
        while (at + 8u <= xboxTxr.size())
        {
            const uint32_t size = u32(&xboxTxr[at + 4]);
            if (std::memcmp(&xboxTxr[at], "TXRC", 4) != 0 || at + 8u + size > xboxTxr.size())
                return fail("bad TXRC entry at " + std::to_string(at));
            Entry entry;
            size_t sub = at + 8u;
            while (sub + 8u <= at + 8u + size)
            {
                const uint32_t subSize = u32(&xboxTxr[sub + 4]);
                if (sub + 8u + subSize > at + 8u + size)
                    break;
                if (std::memcmp(&xboxTxr[sub], "TXRH", 4) == 0 && subSize >= 52u)
                    entry.header = &xboxTxr[sub + 8u];
                else if (std::memcmp(&xboxTxr[sub], "TXPR", 4) == 0 && !entry.picture)
                {
                    entry.picture = &xboxTxr[sub + 8u];
                    entry.pictureSize = subSize;
                }
                sub += 8u + subSize;
            }
            if (!entry.header)
                return fail("TXRC entry without TXRH at " + std::to_string(at));
            entry.report.id = u32(entry.header);
            entry.report.width = u16(entry.header + 4);
            entry.report.height = u16(entry.header + 6);
            const char *name = reinterpret_cast<const char *>(entry.header + 0x14);
            entry.report.name.assign(name, strnlen(name, 52u - 0x14u));
            entries.push_back(std::move(entry));
            at += 8u + size;
        }

        // Each texture's own limit: maxSide, or less to keep to the budget
        // (the sizes from the TXRH, as the Xbox texture has them).
        std::vector<int> sides(entries.size(), maxSide);
        if (texelBudget)
        {
            std::vector<std::pair<int, int>> sizes;
            for (const Entry &entry : entries)
                sizes.emplace_back(std::max(1, entry.report.width), std::max(1, entry.report.height));
            sides = budgetSides(sizes, maxSide, texelBudget);
        }

        if (threads == 0u)
            threads = std::max(1u, std::thread::hardware_concurrency());
        threads = std::min<unsigned>(threads, static_cast<unsigned>(std::max<size_t>(1u, entries.size())));
        std::atomic<size_t> next{0};
        auto work = [&]()
        {
            for (size_t i = next++; i < entries.size(); i = next++)
            {
                Entry &entry = entries[i];
                Picture picture;
                std::string why;
                if (!entry.picture)
                {
                    entry.report.error = "no TXPR";
                    continue;
                }
                if (!decodeTexture(entry.picture, entry.pictureSize, picture, &why))
                {
                    entry.report.error = why;
                    continue;
                }
                entry.report.sourceWidth = picture.width;
                entry.report.sourceHeight = picture.height;
                if (hostTextures)
                {
                    HostTexture &host = entry.host;
                    host.name = entry.report.name;
                    host.dxt3 = dxt3Levels(entry.picture, entry.pictureSize, host.pictureWidth, host.pictureHeight,
                                           host.mips, host.data);
                    if (!host.dxt3)
                    {
                        host.pictureWidth = picture.width;
                        host.pictureHeight = picture.height;
                        host.mips = 1;
                        host.data.resize(picture.rgba.size() * 4u);
                        std::memcpy(host.data.data(), picture.rgba.data(), host.data.size());
                    }
                }
                shrink(picture, sides[i]);
                size_t translucent = 0, clear = 0;
                for (uint32_t c : picture.rgba)
                {
                    translucent += (c >> 24) < 0xFFu ? 1u : 0u;
                    clear += (c >> 24) < 64u ? 1u : 0u;
                }
                entry.report.alpha = translucent * 1000u > picture.rgba.size(); // over 0.1%
                entry.report.cutout = entry.report.alpha && clear * 100u >= picture.rgba.size() * 3u;
                entry.report.width = picture.width;
                entry.report.height = picture.height;
                Palettized image;
                palettize(picture, entry.report.alpha, image);
                if (!entry.host.data.empty())
                {
                    entry.host.width = picture.width;
                    entry.host.height = picture.height;
                    entry.host.indexHash = fnv1a64(image.index.data(), image.index.size());
                }
                std::vector<uint8_t> bmp = writeBmp8(picture.width, picture.height, image, entry.report.alpha);
                bmp.resize((bmp.size() + 3u) & ~size_t(3));
                std::vector<uint8_t> &chunk = entry.chunk;
                putTag(chunk, "TXRC");
                put32(chunk, static_cast<uint32_t>(8u + 52u + 8u + bmp.size()));
                putTag(chunk, "TXRH");
                put32(chunk, 52u);
                chunk.insert(chunk.end(), entry.header, entry.header + 52);
                const uint16_t size[2] = {static_cast<uint16_t>(picture.width), static_cast<uint16_t>(picture.height)};
                std::memcpy(&chunk[16 + 4], size, sizeof(size)); // the TXRH's width and height
                putTag(chunk, entry.report.alpha ? "TZMP" : "TBMP");
                put32(chunk, static_cast<uint32_t>(bmp.size()));
                chunk.insert(chunk.end(), bmp.begin(), bmp.end());
            }
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t)
            pool.emplace_back(work);
        work();
        for (std::thread &t : pool)
            t.join();

        ps2Txr.clear();
        putTag(ps2Txr, "FORM");
        put32(ps2Txr, 0u);
        for (Entry &entry : entries)
        {
            ps2Txr.insert(ps2Txr.end(), entry.chunk.begin(), entry.chunk.end());
            if (report)
                report->push_back(entry.report);
            if (hostTextures && !entry.host.data.empty() && !entry.chunk.empty())
                hostTextures->push_back(std::move(entry.host));
        }
        const uint32_t formSize = static_cast<uint32_t>(ps2Txr.size() - 8u);
        std::memcpy(&ps2Txr[4], &formSize, 4);
        return true;
    }

    void convertLayer(uint8_t *layer, bool textureAlpha, bool grass)
    {
        uint32_t w[7];
        std::memcpy(w, layer, sizeof(w));
        uint32_t hdr = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
        const uint32_t tex = w[4], w5 = w[5];
        if (w2 == 0u && w3 == 0u && tex == 0u)
        {
            // An untextured layer.
            const uint32_t plain[7] = {0x1Cu, 0u, 0x49FC02F0u, 0x1C003101u, 0u, 0u, 0u};
            std::memcpy(layer, plain, sizeof(plain));
            return;
        }
        const uint32_t flags = (hdr >> 16) & 0xFFu;
        bool blended = (flags & 0x07u) != 0u;
        if (w2 == 0u)
        {
            // A textured layer without render state (the Zoo's exporter
            // wrote none; the Xbox engine blends by the texture's alpha):
            // the state the PS2 levels use most for the same blend mode
            // (flags bits 0-2) and alpha test (w1 bits 2-4).
            // w2's top byte and w3's low bits are GS ALPHA (A, B, C in
            // bits 25-30, D in w3 bits 0-1), w3 bits 10-28 GS TEST (ATE,
            // ATST, AREF, AFAIL, DATE, DATM, ZTE, ZTST); left at 0, the
            // blend byte drew these layers black.
            // - The base of a grass material: opaque, no alpha test, depth
            //   GEQUAL, as the Tutorial's grass (its alpha is the shells').
            // - A decal (the Zoo's shadows and dirt: only w3 bit 28 set):
            //   blended like the PS2 levels' alpha textures.
            // - An unblended layer whose texture is a cut-out: alpha
            //   blending, and an alpha test (GREATER 0x10, the CLUT's alpha
            //   is halved) so the clear texels write no depth: the trees,
            //   fences and signs no longer hide what is drawn after them.
            //   Other alpha textures (masks for Xbox effects) draw opaque.
            static const uint8_t kBlend[8] = {0x49, 0x49, 0x49, 0x11, 0x05, 0x09, 0x49, 0x49};
            const uint32_t test = w1 & 0xFFu;
            const bool decal = (w3 & 0x10000000u) != 0u;
            uint32_t blend = kBlend[flags & 7u];
            bool cutout = false;
            if (grass)
            {
                blended = false;
                blend = 0x49u;
            }
            else if (!blended && (textureAlpha || decal))
            {
                blend = textureAlpha ? 0x09u : 0x05u;
                blended = true;
                cutout = textureAlpha;
            }
            w1 = 0x40000000u | (w1 & 0x00FFFFFFu);
            w2 = (blend << 24) | 0x00F70000u;
            if (grass)
                w3 = 0x14000001u;
            else if (decal)
                w3 = 0x1C000001u; // as the PS2 levels' alpha textures (their shadows' 0x14400501 skips the near clip)
            else if (cutout)
                w3 = 0x1C043401u; // ZTE GREATER; ATE, ATST GREATER, AREF 0x10, AFAIL KEEP
            else
                w3 = (test & 0x08u) || textureAlpha ? 0x1C000001u : (test & 0x10u) ? 0x1C400401u : 0x14400401u;
        }
        const uint32_t first = blended ? 0x3Cu : 0x1Cu;
        // The clamp region: the Xbox's u16 at w1 bits 8-23 (texels * 4 - 4)
        // is the PS2's at hdr bits 8-23 in 1/16 texel steps.
        const uint32_t region = (w1 >> 8) & 0xFFFFu;
        const uint32_t ps2Region = region == 0u ? 0u : ((region + 4u) << 4) - 4u;
        const uint32_t out[7] = {first | (ps2Region << 8), w1 & 0xFFFFFF00u, w2 | 0x000082F0u, w3 | 0x00000100u, tex, w5, 0u};
        std::memcpy(layer, out, sizeof(out));
    }

    namespace
    {
        // The MATI layers of one TGEO object (its payload, [at, end)) in
        // place; the number of materials converted.
        long convertObjectMaterials(std::vector<uint8_t> &geo, size_t at, size_t end,
                                    const std::unordered_set<uint32_t> *alphaTextures)
        {
            constexpr uint32_t kPs2MaterialFlags = 0x1u | 0x2u | 0x4u | 0x20u | 0x400u;
            long converted = 0;
            size_t sub = at;
            while (sub + 8u <= end)
            {
                const uint32_t subSize = u32(&geo[sub + 4]);
                if (sub + 8u + subSize > end)
                    break;
                if (std::memcmp(&geo[sub], "TMTB", 4) == 0)
                {
                    size_t m = sub + 8u;
                    while (m + 8u <= sub + 8u + subSize)
                    {
                        const uint32_t mSize = u32(&geo[m + 4]);
                        if (m + 8u + mSize > sub + 8u + subSize)
                            break;
                        if (std::memcmp(&geo[m], "MATI", 4) == 0 && mSize >= 0x30u + 28u)
                        {
                            uint8_t *mati = &geo[m + 8u];
                            // Material flags the PS2 build knows (0x1,
                            // 0x2, 0x4, 0x20, 0x400 in its levels).
                            // The material type is flags >> 2: types
                            // 1 and 2 (0x4, 0x8) are grass shells on
                            // both (the PS2 levels only use 1). The
                            // Xbox-only effects (0x10, 0x40: caustics,
                            // shine; with 0x4/0x8 its water, caustics
                            // 0x54, tree shadows 0x454) are dropped,
                            // and the grass with them (on the PS2 those
                            // layers vanish or draw black). A material
                            // without render state (the Zoo's) with
                            // 0x4 or 0x8 on an unblended base layer is
                            // the PS2's grass (0x4), its base opaque.
                            const uint32_t xboxFlags = u32(mati);
                            uint32_t flags = xboxFlags & kPs2MaterialFlags;
                            const bool noState = u32(mati + 0x38) == 0u;
                            const bool unblended = ((u32(mati + 0x30) >> 16) & 7u) == 0u;
                            bool grass = false;
                            if (xboxFlags & 0x58u)
                                flags &= ~0x4u;
                            if (noState && (xboxFlags & 0x50u) == 0u && (xboxFlags & 0x0Cu) != 0u && unblended)
                            {
                                flags |= 0x4u;
                                grass = true;
                            }
                            std::memcpy(mati, &flags, 4);
                            const uint32_t layers = std::max<uint32_t>(1u, u32(mati + 0x2C));
                            for (uint32_t l = 0; l < layers && 0x30u + 28u * (l + 1u) <= mSize; ++l)
                            {
                                uint8_t *layer = mati + 0x30u + 28u * l;
                                convertLayer(layer, alphaTextures && alphaTextures->count(u32(layer + 0x10)) != 0u,
                                             grass && l == 0u);
                            }
                            ++converted;
                        }
                        m += 8u + mSize;
                    }
                }
                sub += 8u + subSize;
            }
            return converted;
        }
    }

    long convertMaterials(std::vector<uint8_t> &geo, const std::unordered_set<uint32_t> *alphaTextures)
    {
        if (geo.size() < 8u || std::memcmp(geo.data(), "FORM", 4) != 0)
            return -1;
        long converted = 0;
        size_t at = 8u;
        while (at + 8u <= geo.size())
        {
            const uint32_t size = u32(&geo[at + 4]);
            const size_t end = std::min(geo.size(), at + 8u + size);
            if (std::memcmp(&geo[at], "TGEO", 4) == 0)
                converted += convertObjectMaterials(geo, at + 8u, end, alphaTextures);
            at += 8u + size;
        }
        return converted;
    }

    std::vector<std::vector<uint16_t>> restrip(const std::vector<uint16_t> &xbox, size_t maxEntries)
    {
        // One PS2 entry per Xbox entry: no triangle where the Xbox draws
        // none (strip start, degenerate), else the winding flag from the
        // entry's position in the D3D strip.
        std::vector<uint16_t> entries(xbox.size());
        for (size_t k = 0; k < xbox.size(); ++k)
        {
            const uint16_t index = xbox[k] & 0x3FFFu;
            bool draw = k >= 2u && (xbox[k] & 0x8000u) == 0u;
            if (draw)
            {
                const uint16_t a = xbox[k - 2] & 0x3FFFu, b = xbox[k - 1] & 0x3FFFu;
                draw = a != b && b != index && a != index;
            }
            entries[k] = draw ? static_cast<uint16_t>(index | ((k & 1u) ? 0x4000u : 0u))
                              : static_cast<uint16_t>(index | 0x8000u);
        }
        std::vector<std::vector<uint16_t>> out;
        maxEntries = std::max<size_t>(maxEntries, 4u);
        size_t k = 0;
        while (k < entries.size())
        {
            std::vector<uint16_t> group;
            // A group that continues a strip starts with its last two
            // vertices again, drawing nothing.
            const bool starts = (entries[k] & 0x8000u) != 0u &&
                                (k + 1u >= entries.size() || (entries[k + 1] & 0x8000u) != 0u);
            if (k >= 2u && !starts)
            {
                group.push_back(static_cast<uint16_t>((entries[k - 2] & 0x3FFFu) | 0x8000u));
                group.push_back(static_cast<uint16_t>((entries[k - 1] & 0x3FFFu) | 0x8000u));
            }
            while (k < entries.size() && group.size() < maxEntries)
                group.push_back(entries[k++]);
            out.push_back(std::move(group));
        }
        return out;
    }

    namespace
    {
        size_t openChunk(std::vector<uint8_t> &buffer, const char *tag)
        {
            const size_t at = buffer.size();
            putTag(buffer, tag);
            put32(buffer, 0u);
            return at;
        }

        void closeChunk(std::vector<uint8_t> &buffer, size_t at)
        {
            const uint32_t size = static_cast<uint32_t>(buffer.size() - at - 8u);
            std::memcpy(&buffer[at + 4], &size, 4);
        }

        void copyChunk(const std::vector<uint8_t> &from, size_t at, size_t size, std::vector<uint8_t> &out)
        {
            out.insert(out.end(), from.begin() + static_cast<std::ptrdiff_t>(at),
                       from.begin() + static_cast<std::ptrdiff_t>(at + 8u + size));
        }

        // One TGEO object (the chunk at `at`, payload `size` bytes) to `out`
        // with its strips cut into the PS2 renderer's groups (and the
        // TGPH group counts to match). False if its chunks do not add up.
        bool convertObject(const std::vector<uint8_t> &geo, size_t at, size_t size, std::vector<uint8_t> &out,
                           GeometryReport &counts, size_t maxGroupEntries)
        {
            const size_t object = openChunk(out, "TGEO");
            size_t sub = at + 8u;
            const size_t end = at + 8u + size;
            while (sub + 8u <= end)
            {
                const uint32_t subSize = u32(&geo[sub + 4]);
                if (sub + 8u + subSize > end)
                    return false;
                if (std::memcmp(&geo[sub], "TGPT", 4) != 0)
                {
                    copyChunk(geo, sub, subSize, out);
                    sub += 8u + subSize;
                    continue;
                }
                const size_t part = openChunk(out, "TGPT");
                size_t headerAt = 0;
                uint32_t groupCount = 0;
                bool restripped = false;
                size_t c = sub + 8u;
                const size_t partEnd = sub + 8u + subSize;
                while (c + 8u <= partEnd)
                {
                    const uint32_t cSize = u32(&geo[c + 4]);
                    if (c + 8u + cSize > partEnd)
                        return false;
                    if (std::memcmp(&geo[c], "TGPH", 4) == 0 && cSize >= 16u)
                    {
                        headerAt = out.size() + 8u;
                        copyChunk(geo, c, cSize, out);
                    }
                    else if (std::memcmp(&geo[c], "TGFM", 4) == 0)
                    {
                        const size_t fm = openChunk(out, "TGFM");
                        size_t g = c + 8u;
                        while (g + 4u <= c + 8u + cSize)
                        {
                            const uint16_t submesh = u16(&geo[g]);
                            const uint16_t count = u16(&geo[g + 2]);
                            if (g + 4u + 2u * count > c + 8u + cSize)
                                return false;
                            std::vector<uint16_t> strip(count);
                            for (uint16_t i = 0; i < count; ++i)
                                strip[i] = u16(&geo[g + 4u + 2u * i]);
                            g += 4u + 2u * count + ((count & 1u) ? 2u : 0u);
                            ++counts.groupsIn;
                            for (const std::vector<uint16_t> &group : restrip(strip, maxGroupEntries))
                            {
                                put16(out, submesh);
                                put16(out, static_cast<uint16_t>(group.size()));
                                for (uint16_t index : group)
                                    put16(out, index);
                                if (group.size() & 1u)
                                    put16(out, 0u);
                                ++groupCount;
                                ++counts.groupsOut;
                            }
                        }
                        closeChunk(out, fm);
                        restripped = true;
                    }
                    else
                    {
                        copyChunk(geo, c, cSize, out);
                    }
                    c += 8u + cSize;
                }
                if (restripped && headerAt)
                {
                    std::memcpy(&out[headerAt + 8u], &groupCount, 4);
                    ++counts.objects;
                }
                closeChunk(out, part);
                sub += 8u + subSize;
            }
            closeChunk(out, object);
            return true;
        }
    }

    bool convertGeometry(std::vector<uint8_t> &geo, GeometryReport *report, const std::unordered_set<uint32_t> *alphaTextures,
                         size_t maxGroupEntries)
    {
        if (geo.size() < 8u || std::memcmp(geo.data(), "FORM", 4) != 0)
            return false;
        GeometryReport counts;
        std::vector<uint8_t> out;
        out.reserve(geo.size() + geo.size() / 16u);
        const size_t form = openChunk(out, "FORM");
        size_t at = 8u;
        while (at + 8u <= geo.size())
        {
            const uint32_t size = u32(&geo[at + 4]);
            if (at + 8u + size > geo.size())
                return false;
            if (std::memcmp(&geo[at], "TGEO", 4) != 0)
                copyChunk(geo, at, size, out);
            else if (!convertObject(geo, at, size, out, counts, maxGroupEntries))
                return false;
            at += 8u + size;
        }
        closeChunk(out, form);
        counts.materials = convertMaterials(out, alphaTextures);
        if (counts.materials < 0)
            return false;
        geo = std::move(out);
        if (report)
            *report = counts;
        return true;
    }

    bool convertModel(std::vector<uint8_t> &zit, ModelReport *report, std::string *error, int maxSide,
                      std::vector<HostTexture> *hostTextures, size_t maxGroupEntries)
    {
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        if (zit.size() < 16u || std::memcmp(zit.data(), "FMDL", 4) != 0 || std::memcmp(zit.data() + 8, "MODL", 4) != 0)
            return fail("not a model (no FMDL/MODL)");
        const size_t modelEnd = 16u + u32(&zit[12]);
        if (modelEnd > zit.size() || 8u + u32(&zit[4]) > zit.size())
            return fail("model chunks run past the file");
        ModelReport counts;
        // Textures first: the materials depend on which have alpha.
        std::unordered_set<uint32_t> cutouts;
        std::vector<std::vector<uint8_t>> textures; // the PS2 TXRC chunk of each Xbox one, in order (empty: left out)
        for (size_t at = 16u; at + 8u <= modelEnd;)
        {
            const uint32_t size = u32(&zit[at + 4]);
            if (at + 8u + size > modelEnd)
                return fail("bad chunk at " + std::to_string(at));
            if (std::memcmp(&zit[at], "TXRC", 4) == 0)
            {
                std::vector<uint8_t> library, converted;
                putTag(library, "FORM");
                put32(library, 8u + size);
                copyChunk(zit, at, size, library);
                std::vector<TextureReport> textureReport;
                std::string why;
                if (!convertTextureLibrary(library, converted, &textureReport, &why, 1u, maxSide, hostTextures))
                    return fail("texture at " + std::to_string(at) + ": " + why);
                for (const TextureReport &t : textureReport)
                {
                    if (t.cutout)
                        cutouts.insert(t.id);
                    if (!t.error.empty())
                        ++counts.texturesLeftOut;
                    if (report)
                        report->textures.push_back(t);
                }
                textures.emplace_back(converted.begin() + 8, converted.end());
            }
            at += 8u + size;
        }
        std::vector<uint8_t> out;
        out.reserve(zit.size());
        const size_t fmdl = openChunk(out, "FMDL");
        const size_t modl = openChunk(out, "MODL");
        size_t texture = 0;
        for (size_t at = 16u; at + 8u <= modelEnd;)
        {
            const uint32_t size = u32(&zit[at + 4]);
            if (std::memcmp(&zit[at], "TXRC", 4) == 0)
            {
                out.insert(out.end(), textures[texture].begin(), textures[texture].end());
                ++texture;
            }
            else if (std::memcmp(&zit[at], "TGEO", 4) == 0)
            {
                const size_t object = out.size();
                if (!convertObject(zit, at, size, out, counts.geometry, maxGroupEntries))
                    return fail("bad object at " + std::to_string(at));
                counts.geometry.materials += convertObjectMaterials(out, object + 8u, out.size(), &cutouts);
            }
            else
                copyChunk(zit, at, size, out);
            at += 8u + size;
        }
        closeChunk(out, modl);
        // Anything after the MODL (none in the Zoo's) as it is.
        out.insert(out.end(), zit.begin() + static_cast<std::ptrdiff_t>(modelEnd),
                   zit.begin() + static_cast<std::ptrdiff_t>(std::min(zit.size(), 8u + size_t{u32(&zit[4])})));
        closeChunk(out, fmdl);
        zit = std::move(out);
        if (report)
        {
            report->texturesLeftOut = counts.texturesLeftOut;
            report->geometry = counts.geometry;
        }
        return true;
    }
}
