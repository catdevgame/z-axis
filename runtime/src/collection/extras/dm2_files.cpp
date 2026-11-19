// Dave Mirra BMX 2's files for the DM2 extras' converters (dm2_files.h).
#include "dm2_files.h"

#include <algorithm>
#include <numeric>

namespace ps2x::app::extras::dm2
{
    namespace
    {
        bool isLevelContainer(std::string_view tag)
        {
            static const char *const kContainers[] = {"FORM", "TGEO", "TGPT", "TMTB", "TGIN",
                                                      "TXRC", "FSCN", "TERR", "ZPVS", "ZDYN"};
            for (const char *c : kContainers)
                if (tag == c)
                    return true;
            return false;
        }

        bool isUiContainer(std::string_view tag)
        {
            return tag == "ULIB" || tag == "UAST" || tag == "UMTL" || tag == "TXRC";
        }

        void appendChunk(Bytes &out, const Chunk &c)
        {
            appendText(out, c.tag);
            appendU32(out, uint32_t(c.size()));
            if (!c.container)
                appendBytes(out, c.data);
            else
                for (const Chunk &k : c.kids)
                    appendChunk(out, k);
        }
    }

    uint32_t nameHash(std::string_view name)
    {
        uint32_t h = 0;
        for (unsigned char c : name)
        {
            if (c >= 'a' && c <= 'z')
                c = static_cast<unsigned char>(c - 32);
            h = (h << 4) + c;
            const uint32_t high = h & 0xF0000000u;
            if (high)
                h ^= high >> 24;
            h &= ~high;
        }
        return h;
    }

    size_t Chunk::size() const
    {
        if (!container)
            return data.size();
        size_t total = 0;
        for (const Chunk &k : kids)
            total += 8 + k.size();
        return total;
    }

    Chunk *Chunk::find(std::string_view t)
    {
        for (Chunk &k : kids)
            if (k.tag == t)
                return &k;
        return nullptr;
    }

    const Chunk *Chunk::find(std::string_view t) const
    {
        for (const Chunk &k : kids)
            if (k.tag == t)
                return &k;
        return nullptr;
    }

    Chunks parse(const Bytes &data, size_t start, size_t end)
    {
        end = std::min(end, data.size());
        Chunks chunks;
        size_t at = start;
        while (at + 8 <= end)
        {
            const std::string tag(reinterpret_cast<const char *>(data.data() + at), 4);
            const size_t size = u32At(data, at + 4);
            const size_t body = at + 8;
            if (body + size > end)
                throw Dm2Error("chunk " + tag + " runs past its parent");
            if (isLevelContainer(tag))
                chunks.push_back(Chunk::node(tag, parse(data, body, body + size)));
            else
                chunks.push_back(Chunk::leaf(tag, sliceOf(data, body, body + size)));
            at = body + size;
        }
        if (at != end)
            throw Dm2Error(std::to_string(end - at) + " stray bytes in a chunk tree");
        return chunks;
    }

    Chunks parseUi(const Bytes &data, size_t start, size_t end)
    {
        end = std::min(end, data.size());
        Chunks chunks;
        size_t at = start;
        while (at + 8 <= end)
        {
            const std::string tag(reinterpret_cast<const char *>(data.data() + at), 4);
            const size_t size = u32At(data, at + 4);
            const size_t body = at + 8;
            if (body + size > end)
                throw Dm2Error("chunk " + tag + " runs past its parent");
            if (isUiContainer(tag))
                chunks.push_back(Chunk::node(tag, parseUi(data, body, body + size)));
            else
                chunks.push_back(Chunk::leaf(tag, sliceOf(data, body, body + size)));
            at = body + size;
        }
        return chunks;
    }

    Bytes serialize(const Chunks &chunks)
    {
        Bytes out;
        for (const Chunk &c : chunks)
            appendChunk(out, c);
        return out;
    }

    void walk(Chunks &chunks, const std::function<void(Chunk &)> &visit)
    {
        for (Chunk &c : chunks)
        {
            visit(c);
            if (c.container)
                walk(c.kids, visit);
        }
    }

    void walk(const Chunks &chunks, const std::function<void(const Chunk &)> &visit)
    {
        for (const Chunk &c : chunks)
        {
            visit(c);
            if (c.container)
                walk(c.kids, visit);
        }
    }

    std::vector<Bytes> zalEntries(const Bytes &data)
    {
        if (data.size() < 0x14 || std::memcmp(data.data(), "ZDIR", 4) != 0 || std::memcmp(data.data() + 8, "DIDF", 4) != 0)
            throw Dm2Error("not a ZDIR archive");
        const uint32_t count = u32At(data, 0x10);
        std::vector<Bytes> out;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t length = u32At(data, 0x20 + 4 * size_t(count) + 4 * i);
            const uint32_t offset = u32At(data, 0x28 + 8 * size_t(count) + 4 * i);
            out.push_back(sliceOf(data, offset, size_t(offset) + (length & 0x0FFFFFFF)));
        }
        return out;
    }

    Picture decodeBmp(const Bytes &data, bool opaque)
    {
        if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
            throw Dm2Error("not a BMP");
        const uint32_t pixels = u32At(data, 10), head = u32At(data, 14);
        const int32_t w = s32At(data, 18), h = s32At(data, 22);
        const int bpp = u16At(data, 28);
        if (bpp != 4 && bpp != 8)
            throw Dm2Error(std::to_string(bpp) + "-bit BMP");
        uint32_t colours = u32At(data, 46);
        if (colours == 0)
            colours = 1u << bpp;
        std::vector<Rgba> palette;
        for (uint32_t i = 0; i < colours; ++i)
        {
            const size_t at = 14 + head + 4 * size_t(i);
            if (at + 4 > data.size())
                throw Dm2Error("a BMP's palette past its end");
            palette.push_back({data[at + 2], data[at + 1], data[at], opaque ? 255 : data[at + 3]});
        }
        const size_t stride = size_t((w * bpp + 31) / 32) * 4;
        const int rowsCount = std::abs(h);
        Picture out;
        out.width = w;
        out.height = rowsCount;
        for (int y = 0; y < rowsCount; ++y)
        {
            const size_t at = pixels + size_t(h > 0 ? rowsCount - 1 - y : y) * stride;
            std::vector<Rgba> row;
            for (int x = 0; x < w; ++x)
            {
                const size_t byte = at + size_t(bpp == 8 ? x : x >> 1);
                if (byte >= data.size())
                    throw Dm2Error("a BMP's pixels past its end");
                const int index = bpp == 8 ? data[byte] : (x % 2 == 0 ? data[byte] >> 4 : data[byte] & 15);
                if (size_t(index) >= palette.size())
                    throw Dm2Error("a BMP pixel past its palette");
                row.push_back(palette[size_t(index)]);
            }
            out.rows.push_back(std::move(row));
        }
        return out;
    }

    Palette medianCut(const std::map<Rgba, long long> &counts, size_t boxes)
    {
        using Box = std::vector<Rgba>;
        std::vector<Box> work;
        {
            Box all;
            for (const auto &[colour, n] : counts)
                all.push_back(colour); // sorted: the map's order is the colours'
            work.push_back(std::move(all));
        }
        auto weight = [&](const Box &box)
        {
            long long n = 0;
            for (const Rgba &c : box)
                n += counts.at(c);
            return n;
        };
        while (work.size() < boxes)
        {
            int best = -1, bestChannel = 0;
            long long bestScore = 0;
            for (size_t i = 0; i < work.size(); ++i)
            {
                const Box &box = work[i];
                if (box.size() < 2)
                    continue;
                int ranges[4];
                for (int k = 0; k < 4; ++k)
                {
                    int lo = box[0][size_t(k)], hi = lo;
                    for (const Rgba &c : box)
                    {
                        lo = std::min(lo, c[size_t(k)]);
                        hi = std::max(hi, c[size_t(k)]);
                    }
                    ranges[k] = hi - lo;
                }
                int channel = 0; // the widest, the first of equal ones
                for (int k = 1; k < 4; ++k)
                    if (ranges[k] > ranges[channel])
                        channel = k;
                const long long score = ranges[channel] * weight(box);
                if (score > bestScore)
                {
                    best = int(i);
                    bestScore = score;
                    bestChannel = channel;
                }
            }
            if (best < 0)
                break;
            Box box = work[size_t(best)];
            std::sort(box.begin(), box.end(), [bestChannel](const Rgba &a, const Rgba &b)
            {
                if (a[size_t(bestChannel)] != b[size_t(bestChannel)])
                    return a[size_t(bestChannel)] < b[size_t(bestChannel)];
                return a < b;
            });
            const long long total = weight(box);
            long long run = 0;
            size_t cut = 1;
            for (size_t k = 0; k + 1 < box.size(); ++k)
            {
                run += counts.at(box[k]);
                if (run * 2 >= total)
                {
                    cut = k + 1;
                    break;
                }
            }
            Box low(box.begin(), box.begin() + long(cut)), high(box.begin() + long(cut), box.end());
            work[size_t(best)] = std::move(low);
            work.insert(work.begin() + best + 1, std::move(high));
        }
        Palette out;
        for (const Box &box : work)
        {
            const long long n = weight(box);
            Rgba mean{};
            for (int k = 0; k < 4; ++k)
            {
                long long sum = 0;
                for (const Rgba &c : box)
                    sum += c[size_t(k)] * counts.at(c);
                mean[size_t(k)] = int(double(sum) / double(n) + 0.5);
            }
            out.colours.push_back(mean);
            for (const Rgba &c : box)
                out.index[c] = int(out.colours.size()) - 1;
        }
        return out;
    }

    Bytes bmp8(const std::vector<std::vector<uint8_t>> &rows, const std::vector<Rgba> &palette)
    {
        const size_t w = rows.empty() ? 0 : rows[0].size(), h = rows.size();
        const size_t pad = (4 - w % 4) % 4;
        Bytes pixels;
        for (size_t y = h; y-- > 0;)
        {
            pixels.insert(pixels.end(), rows[y].begin(), rows[y].end());
            pixels.insert(pixels.end(), pad, 0);
        }
        Bytes out;
        appendText(out, "BM");
        appendU32(out, uint32_t(14 + 40 + 1024 + pixels.size()));
        appendU16(out, 0);
        appendU16(out, 0);
        appendU32(out, 14 + 40 + 1024);
        appendU32(out, 40);
        appendU32(out, uint32_t(w));
        appendU32(out, uint32_t(h));
        appendU16(out, 1);
        appendU16(out, 8);
        appendU32(out, 0);
        appendU32(out, uint32_t((w + pad) * h));
        appendU32(out, 0);
        appendU32(out, 0);
        appendU32(out, 256);
        appendU32(out, 0);
        for (const Rgba &c : palette)
        {
            out.push_back(uint8_t(c[2]));
            out.push_back(uint8_t(c[1]));
            out.push_back(uint8_t(c[0]));
            out.push_back(uint8_t(c[3]));
        }
        out.insert(out.end(), 4 * (256 - std::min<size_t>(256, palette.size())), 0);
        appendBytes(out, pixels);
        return out;
    }

    Vec3 quatRotate(const Quat &q, const Vec3 &v)
    {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
        return {v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)};
    }
}
