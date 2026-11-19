// The converter's textures: fitted to a game's budgets, palettized and
// written as the games' 8-bit BMPs (ai_level_convert.py's Texture,
// fit_textures, palettize, bmp8, texture_chunk; dm2_level.py bmp8).
#include "converter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ps2x::app::levels
{
    namespace
    {
        int pow2Below(int n)
        {
            int p = 1;
            while (p * 2 <= n)
                p *= 2;
            return p;
        }

        uint32_t key(const Rgba &c) { return uint32_t(c[0]) | uint32_t(c[1]) << 8 | uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24; }

        std::string size(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        // AI's and BMX XXX's BMP of a TBMP/TZMP chunk: a 40-byte info header,
        // a 256-colour BGRA palette (alpha 0 in a TBMP), rows bottom-up, the
        // whole padded as the reference pads it.
        Bytes aiBmp8(int width, int height, const std::vector<Rgba> &palette, const std::vector<uint8_t> &indices, bool withAlpha)
        {
            const size_t rowBytes = (static_cast<size_t>(width) + 3) & ~size_t(3);
            const size_t offset = 14 + 40 + 1024;
            ByteWriter body;
            body.u32(40).s32(width).s32(height).u16(1).u16(8).u32(0).u32(0).s32(2834).s32(2834).u32(0).u32(0);
            for (size_t i = 0; i < 256; ++i)
            {
                const Rgba c = i < palette.size() ? palette[i] : Rgba{0, 0, 0, 255};
                body.u8(c[2]).u8(c[1]).u8(c[0]).u8(withAlpha ? c[3] : 0);
            }
            for (int y = height - 1; y >= 0; --y)
            {
                body.bytes(indices.data() + static_cast<size_t>(y) * static_cast<size_t>(width), static_cast<size_t>(width));
                body.zeros(rowBytes - static_cast<size_t>(width));
            }
            const size_t total = (offset + body.size() - 54 + 3) & ~size_t(3);
            ByteWriter out;
            out.u8('B').u8('M').u32(static_cast<uint32_t>(total)).u16(0).u16(0).u32(static_cast<uint32_t>(offset)).bytes(body.data());
            if (out.size() < total)
                out.zeros(total - out.size());
            return out.take();
        }
    }

    std::vector<std::string> Report::text() const
    {
        std::vector<std::string> out = lines;
        if (!done.empty())
        {
            out.push_back("done to fit:");
            for (const std::string &d : done)
                out.push_back("  " + d);
        }
        if (!errors.empty())
        {
            out.push_back("over budget:");
            for (const std::string &e : errors)
                out.push_back("  " + e);
        }
        return out;
    }

    bool Texture::hasAlpha() const
    {
        return std::any_of(pixels.begin(), pixels.end(), [](const Rgba &p) { return p[3] < 255; });
    }

    bool Texture::cutOut() const
    {
        size_t clear = 0, partial = 0;
        for (const Rgba &p : pixels)
        {
            if (p[3] < kClearAlpha)
                ++clear;
            else if (p[3] <= kSolidAlpha)
                ++partial;
        }
        return clear > 0 && partial * 100 < static_cast<size_t>(kCutOutPartial) * texels();
    }

    void Texture::halve()
    {
        const bool cut = cutOut();
        const int w = width / 2, h = height / 2;
        std::vector<Rgba> out;
        out.reserve(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const Rgba *four[4] = {&at(2 * x, 2 * y), &at(2 * x + 1, 2 * y), &at(2 * x, 2 * y + 1), &at(2 * x + 1, 2 * y + 1)};
                Rgba p{};
                for (int c = 0; c < 4; ++c)
                    p[c] = static_cast<uint8_t>(((*four[0])[c] + (*four[1])[c] + (*four[2])[c] + (*four[3])[c] + 2) / 4);
                if (cut)
                {
                    // A cut-out stays one: its solid texels' colour, solid or clear by coverage.
                    const int weight = (*four[0])[3] + (*four[1])[3] + (*four[2])[3] + (*four[3])[3];
                    if (weight)
                        for (int c = 0; c < 3; ++c)
                        {
                            int sum = 0;
                            for (const Rgba *q : four)
                                sum += (*q)[c] * (*q)[3];
                            p[c] = static_cast<uint8_t>((sum + weight / 2) / weight);
                        }
                    p[3] = p[3] >= kCutOutKeep ? 255 : 0;
                }
                out.push_back(p);
            }
        width = w;
        height = h;
        pixels = std::move(out);
    }

    void Texture::resample(int w, int h)
    {
        std::vector<Rgba> out;
        out.reserve(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.push_back(at(x * width / w, y * height / h));
        width = w;
        height = h;
        pixels = std::move(out);
    }

    void fitTextures(std::vector<Texture *> &textures, Report &report, int side, int budgetKb)
    {
        for (Texture *t : textures)
        {
            const int w = pow2Below(t->width), h = pow2Below(t->height);
            if (w != t->width || h != t->height)
            {
                report.did("texture " + t->name + " " + size(t->width, t->height) + " resampled to " + size(w, h) +
                           ": sides must be powers of two");
                t->resample(w, h);
            }
            const int beforeW = t->width, beforeH = t->height;
            while (t->width > side || t->height > side)
            {
                if (std::min(t->width, t->height) < 2)
                    break;
                t->halve();
            }
            if (beforeW != t->width || beforeH != t->height)
                report.did("texture " + t->name + " " + size(beforeW, beforeH) + " halved to " + size(t->width, t->height) +
                           ": the texture side budget (" + std::to_string(side) + ")");
        }
        auto total = [&textures]()
        {
            size_t n = 0;
            for (const Texture *t : textures)
                n += t->texels();
            return n;
        };
        while (total() > static_cast<size_t>(budgetKb) * 1024)
        {
            Texture *big = textures[0];
            for (Texture *t : textures)
                if (t->texels() > big->texels())
                    big = t;
            if (big->texels() <= 64)
            {
                report.did("the texel budget (" + std::to_string(budgetKb) + " KB) cannot be met");
                break;
            }
            const int beforeW = big->width, beforeH = big->height;
            big->halve();
            report.did("texture " + big->name + " " + size(beforeW, beforeH) + " halved to " + size(big->width, big->height) +
                       ": the level's texels over " + std::to_string(budgetKb) + " KB");
        }
    }

    void palettize(const std::vector<Rgba> &pixels, bool withAlpha, std::vector<Rgba> &palette, std::vector<uint8_t> &indices)
    {
        // The distinct colours in the order they first appear, with their use.
        std::unordered_map<uint32_t, size_t> where;
        std::vector<Rgba> colours;
        std::vector<long long> uses;
        auto seen = [withAlpha](Rgba p)
        {
            if (!withAlpha)
                p[3] = 255;
            return p;
        };
        for (const Rgba &raw : pixels)
        {
            const Rgba p = seen(raw);
            auto it = where.find(key(p));
            if (it == where.end())
            {
                where.emplace(key(p), colours.size());
                colours.push_back(p);
                uses.push_back(1);
            }
            else
                ++uses[it->second];
        }
        auto count = [&](const Rgba &p) { return uses[where.at(key(p))]; };
        palette.clear();
        if (colours.size() <= 256)
            palette = colours;
        else
        {
            std::vector<std::vector<Rgba>> boxes = {colours};
            const int channels = withAlpha ? 4 : 3;
            while (boxes.size() < 256)
            {
                // The box with the widest channel range splits at its weighted median.
                int best = -1, width = -1, axis = 0;
                for (size_t b = 0; b < boxes.size(); ++b)
                {
                    if (boxes[b].size() < 2)
                        continue;
                    for (int c = 0; c < channels; ++c)
                    {
                        int lo = 255, hi = 0;
                        for (const Rgba &p : boxes[b])
                        {
                            lo = std::min<int>(lo, p[c]);
                            hi = std::max<int>(hi, p[c]);
                        }
                        if (hi - lo > width)
                        {
                            best = static_cast<int>(b);
                            width = hi - lo;
                            axis = c;
                        }
                    }
                }
                if (best < 0)
                    break;
                std::vector<Rgba> box = std::move(boxes[static_cast<size_t>(best)]);
                boxes.erase(boxes.begin() + best);
                std::stable_sort(box.begin(), box.end(), [axis](const Rgba &a, const Rgba &b) { return a[axis] < b[axis]; });
                long long total = 0, acc = 0;
                for (const Rgba &p : box)
                    total += count(p);
                size_t cut = 0;
                for (size_t i = 0; i < box.size(); ++i)
                {
                    acc += count(box[i]);
                    if (acc * 2 >= total)
                    {
                        cut = std::max<size_t>(1, std::min(box.size() - 1, i + 1));
                        break;
                    }
                }
                boxes.emplace_back(box.begin(), box.begin() + static_cast<ptrdiff_t>(cut));
                boxes.emplace_back(box.begin() + static_cast<ptrdiff_t>(cut), box.end());
            }
            for (const std::vector<Rgba> &b : boxes)
            {
                long long n = 0;
                for (const Rgba &p : b)
                    n += count(p);
                Rgba mean{};
                for (int c = 0; c < 4; ++c)
                {
                    long long sum = 0;
                    for (const Rgba &p : b)
                        sum += p[c] * count(p);
                    mean[c] = static_cast<uint8_t>(std::nearbyint(static_cast<double>(sum) / static_cast<double>(n)));
                }
                palette.push_back(mean);
            }
        }
        // Each colour's nearest entry (the first of equals).
        std::unordered_map<uint32_t, uint8_t> nearest;
        for (const Rgba &p : colours)
        {
            size_t bestIndex = 0;
            long long bestDistance = -1;
            for (size_t i = 0; i < palette.size(); ++i)
            {
                long long d = 0;
                for (int c = 0; c < 4; ++c)
                {
                    const long long diff = static_cast<long long>(palette[i][c]) - p[c];
                    d += diff * diff;
                }
                if (bestDistance < 0 || d < bestDistance)
                {
                    bestDistance = d;
                    bestIndex = i;
                }
            }
            nearest[key(p)] = static_cast<uint8_t>(bestIndex);
        }
        indices.clear();
        indices.reserve(pixels.size());
        for (const Rgba &raw : pixels)
            indices.push_back(nearest.at(key(seen(raw))));
    }

    Bytes writeTextureFile(const std::vector<const Texture *> &textures)
    {
        std::vector<Chunk> kids;
        for (const Texture *t : textures)
        {
            const bool alpha = t->hasAlpha();
            std::vector<Rgba> palette;
            std::vector<uint8_t> indices;
            palettize(t->pixels, alpha, palette, indices);
            ByteWriter head;
            head.u32(nameHash(t->name)).u16(static_cast<uint16_t>(t->width)).u16(static_cast<uint16_t>(t->height)).u16(0).u16(0).zeros(8);
            head.text(t->name, 32);
            kids.push_back(Chunk::node(tag("TXRC"), {Chunk::leaf(tag("TXRH"), head.take()),
                                                     Chunk::leaf(alpha ? tag("TZMP") : tag("TBMP"),
                                                                 aiBmp8(t->width, t->height, palette, indices, alpha))}));
        }
        return serializeChunks({Chunk::node(tag("FORM"), std::move(kids))});
    }

    Bytes dm2Bmp8(int width, int height, const std::vector<uint8_t> &indices, const std::vector<Rgba> &palette)
    {
        const size_t pad = static_cast<size_t>((4 - width % 4) % 4);
        ByteWriter pixels;
        for (int y = height - 1; y >= 0; --y)
        {
            pixels.bytes(indices.data() + static_cast<size_t>(y) * static_cast<size_t>(width), static_cast<size_t>(width));
            pixels.zeros(pad);
        }
        ByteWriter out;
        out.u8('B').u8('M').u32(static_cast<uint32_t>(14 + 40 + 1024 + pixels.size())).u16(0).u16(0).u32(14 + 40 + 1024);
        out.u32(40).s32(width).s32(height).u16(1).u16(8).u32(0).u32(static_cast<uint32_t>((static_cast<size_t>(width) + pad) * static_cast<size_t>(height)));
        out.s32(0).s32(0).u32(256).u32(0);
        for (const Rgba &c : palette)
            out.u8(c[2]).u8(c[1]).u8(c[0]).u8(0);
        out.zeros(4 * (256 - palette.size()));
        out.bytes(pixels.data());
        return out.take();
    }
}
