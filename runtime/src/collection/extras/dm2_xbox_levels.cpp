// DM2's Xbox-only levels, Venice and Greenville, made into the PS2's formats
// (collection/extras/dm2_xbox.h), as tools/dm2_xbox.py convert_level makes
// them. The Xbox's level files are DM2's own engine version's (ZGE2), so the
// chunks are the PS2's; what differs is the platforms':
//
// - TXR: TXRC {TXRH, TDDS: u32 0 + a DDS file} with DXT1/DXT3 (and one
//   A8R8G8B8 reflection texture a level). The PS2's: TBMP, an 8-bit BMP with
//   a 256-colour palette whose alpha is 0, then its smaller mip levels as
//   more TBMPs with the same palette; transparency is a colour key (magenta).
//   Made here: the largest DDS level at most maxSide a side, the DDS's next
//   levels as the mips (4 levels at most, down to 8 a side), one palette for
//   all levels (median cut over colours binned to 6 bits a channel), alpha
//   under 128 as the key when the texture's alpha cuts it out (3% of its
//   texels under 64), else opaque. The Xbox picture itself goes into the
//   .XTP keyed by the converted texture's size and indices.
// - GEO, TGFM (strip groups): the Xbox exporter writes one strip a group;
//   the PS2's packs strips into groups of up to 60 entries (its VU1 buffer).
//   Packed here, whole strips in their order, per submesh; TGPH +8 (the
//   group count) updated.
// - GEO, TGPT: the streams put in the PS2's order.
// - GEO, MATI +0 (flags): bit 1 kept; the Xbox material types (bits 0, 2+)
//   whose VU1 effects the PS2's microcode lacks cleared. Clamp regions
//   follow the texture's new size.
// - GEO, levels of detail: a prop's second LOD dropped (DM2 drawing a level
//   object's second LOD overwrote VU1 micro memory), the first kept with the
//   longest range; materials no strip draws dropped (DM2 CALLed whatever
//   their packet pointer held).
// - GEO, TGTD (object class): the Xbox-only AnimPathFollower and riderMod
//   are made Terrain.
// - GEO: objects past the collision tree's range recentred (their INS
//   instances moved to match).
// - DAT: the CubeMap point (no PS2 class) dropped. ZIT: names "<LEVEL>.<EXT>".
#include "dm2_xbox_parts.h"

#include <cmath>
#include <optional>
#include <set>
#include <tuple>

namespace ps2x::app::extras::dm2
{
    namespace
    {
        struct LevelInfo
        {
            const char *folder;     // the output folder ("VENICE")
            const char *xboxFolder; // on the Xbox disc ("Venice")
            const char *name;       // the level select's ("Venice, Italy")
            int xboxValue;          // the Xbox level select's (KSTR_LEVELINFOMENU_<v>, its UILIB's "level<v>")
        };
        const LevelInfo kLevels[] = {
            {"VENICE", "Venice", "Venice, Italy", 8},
            {"GREENVIL", "GREENVIL", "Greenville, NC", 9},
        };

        constexpr int kMaxLevels = 4;
        constexpr int kMinMipSide = 8;
        constexpr Rgba kColourKey = {255, 0, 255, 0}; // the PS2 textures' (palette alpha 0, as theirs)
        constexpr size_t kGroupEntries = 60;          // the PS2's strip group (its VU1 buffer)
        const char *const kPs2PartOrder[] = {"TGPH", "TGVP", "TGVN", "TGVC", "TGVU",
                                             "TGFP", "TGFN", "TGFM", "TGFE", "TGFT"};
        constexpr double kSplitLimit = 327.67; // the collision tree's split values: s16 hundredths

        // A level's counts, for the log.
        using Report = std::map<std::string, long long>;

        // ---- DDS ----------------------------------------------------------------

        Rgba rgb565(uint16_t c)
        {
            const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
        }

        using Rows = std::vector<std::vector<Rgba>>;

        // RGBA rows (top first) of a DXT1 / DXT3 level.
        Rows dxtDecode(const Bytes &d, size_t at, int w, int h, bool dxt3)
        {
            const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
            const size_t step = dxt3 ? 16 : 8;
            Rows rows(size_t(bh * 4), std::vector<Rgba>(size_t(bw * 4)));
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                {
                    size_t o = at + size_t(by * bw + bx) * step;
                    uint64_t alpha = 0;
                    if (dxt3)
                    {
                        alpha = uint64_t(u32At(d, o)) | (uint64_t(u32At(d, o + 4)) << 32);
                        o += 8;
                    }
                    const uint16_t c0 = u16At(d, o), c1 = u16At(d, o + 2);
                    const uint32_t bits = u32At(d, o + 4);
                    const Rgba p0 = rgb565(c0), p1 = rgb565(c1);
                    std::array<Rgba, 4> palette{p0, p1, p0, p0};
                    if (!dxt3 && c0 <= c1)
                    {
                        for (int k = 0; k < 3; ++k)
                            palette[2][size_t(k)] = (p0[size_t(k)] + p1[size_t(k)]) / 2;
                        palette[3] = {0, 0, 0, 0};
                    }
                    else
                        for (int k = 0; k < 3; ++k)
                        {
                            palette[2][size_t(k)] = (2 * p0[size_t(k)] + p1[size_t(k)]) / 3;
                            palette[3][size_t(k)] = (p0[size_t(k)] + 2 * p1[size_t(k)]) / 3;
                        }
                    for (int i = 0; i < 16; ++i)
                    {
                        Rgba c = palette[(bits >> (2 * i)) & 3];
                        if (dxt3)
                            c[3] = int((alpha >> (4 * i)) & 15) * 17;
                        rows[size_t(by * 4 + i / 4)][size_t(bx * 4 + i % 4)] = c;
                    }
                }
            rows.resize(size_t(h));
            for (auto &row : rows)
                row.resize(size_t(w));
            return rows;
        }

        // A DDS file's levels (DXT1, DXT3, A8R8G8B8).
        struct Dds
        {
            struct Level
            {
                size_t at, size;
                int width, height;
            };
            const Bytes *data = nullptr;
            int width = 0, height = 0;
            std::string kind; // "DXT1", "DXT3", "ARGB"
            std::vector<Level> levels;

            explicit Dds(const Bytes &bytes) : data(&bytes)
            {
                if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0)
                    throw Dm2Error("a texture's DDS is not a DDS file");
                height = int(u32At(bytes, 12));
                width = int(u32At(bytes, 16));
                const int mips = int(std::max<uint32_t>(1, u32At(bytes, 28)));
                const std::string fourcc(reinterpret_cast<const char *>(bytes.data() + 84), 4);
                const uint32_t bits = u32At(bytes, 88);
                if (fourcc == "DXT1" || fourcc == "DXT3")
                    kind = fourcc;
                else if (fourcc == std::string(4, '\0') && bits == 32 && u32At(bytes, 92) == 0xFF0000 &&
                         u32At(bytes, 96) == 0xFF00 && u32At(bytes, 100) == 0xFF && u32At(bytes, 104) == 0xFF000000)
                    kind = "ARGB";
                else
                    throw Dm2Error("a DDS of a format the converter does not read (" + std::to_string(bits) + " bits)");
                size_t at = 128;
                int w = width, h = height;
                for (int i = 0; i < mips; ++i)
                {
                    const size_t size = levelBytes(w, h);
                    if (at + size > bytes.size())
                        break;
                    levels.push_back({at, size, w, h});
                    at += size;
                    w = std::max(1, w / 2);
                    h = std::max(1, h / 2);
                }
            }

            size_t levelBytes(int w, int h) const
            {
                if (kind == "ARGB")
                    return size_t(w) * size_t(h) * 4;
                return size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * (kind == "DXT1" ? 8 : 16);
            }

            int mips() const { return int(levels.size()); }

            Rows decode(int k) const
            {
                const Level &l = levels[size_t(k)];
                if (kind != "ARGB")
                    return dxtDecode(*data, l.at, l.width, l.height, kind == "DXT3");
                Rows rows(size_t(l.height), std::vector<Rgba>(size_t(l.width)));
                for (int y = 0; y < l.height; ++y)
                    for (int x = 0; x < l.width; ++x)
                    {
                        const size_t p = l.at + (size_t(y) * size_t(l.width) + size_t(x)) * 4;
                        rows[size_t(y)][size_t(x)] = {(*data)[p + 2], (*data)[p + 1], (*data)[p], (*data)[p + 3]};
                    }
                return rows;
            }
        };

        // A 2x2 box filter (the colour weighted by alpha), in integers.
        Rows halve(const Rows &rows)
        {
            const size_t h = rows.size(), w = rows[0].size();
            Rows out;
            for (size_t y = 0; y + 1 < h; y += 2)
            {
                std::vector<Rgba> line;
                for (size_t x = 0; x + 1 < w; x += 2)
                {
                    const Rgba px[4] = {rows[y][x], rows[y][x + 1], rows[y + 1][x], rows[y + 1][x + 1]};
                    int a = 0;
                    for (const Rgba &p : px)
                        a += p[3];
                    Rgba c{};
                    for (size_t k = 0; k < 3; ++k)
                    {
                        int sum = 0;
                        for (const Rgba &p : px)
                            sum += a ? p[k] * p[3] : p[k];
                        c[k] = a ? (sum + a / 2) / a : (sum + 2) / 4;
                    }
                    c[3] = (a + 2) / 4;
                    line.push_back(c);
                }
                out.push_back(std::move(line));
            }
            return out;
        }

        bool isColourKey(const Rgba &c) { return c[0] >= 248 && c[1] <= 8 && c[2] >= 248; }

        // A colour binned to 6 bits a channel (the median cut works on these),
        // moved off the key if it would read as one.
        Rgba bin6(const Rgba &c)
        {
            Rgba out = {(c[0] & 0xFC) | 2, (c[1] & 0xFC) | 2, (c[2] & 0xFC) | 2, 0};
            if (isColourKey(out))
                out[1] = 10;
            return out;
        }

        uint64_t fnv1a64(const Bytes &data)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint8_t c : data)
                h = (h ^ c) * 0x100000001B3ull;
            return h;
        }

        // An Xbox picture for the GPU renderer's pack.
        struct PackPicture
        {
            int width, height, mips;
            uint8_t format; // 1: BC2 (DXT3) levels as they are; 2: RGBA8
            Bytes data;
        };
        struct PackEntry
        {
            int width, height; // the converted texture's
            uint64_t indexHash;
            PackPicture picture;
            std::string name;
        };

        struct ConvertedTexture
        {
            Bytes txrc;
            uint32_t id;
            std::pair<int, int> xboxSize, ps2Size;
            int levels;
            bool keyed;
            bool opaque;
            std::string kind;
            PackEntry entry;
        };

        // The first DDS level at most maxSide a side, and whether it must
        // still be halved to get there.
        int firstLevel(const Dds &dds, int maxSide)
        {
            int first = 0;
            while (first + 1 < dds.mips() &&
                   std::max(dds.levels[size_t(first)].width, dds.levels[size_t(first)].height) > maxSide)
                ++first;
            return first;
        }

        // One texture: the PS2's TXRC, what it was made from, its pack entry.
        // `opaque`: its alpha is no transparency (the Xbox's grass).
        ConvertedTexture convertTexture(const Bytes &txrh, const Bytes &ddsBytes, int maxSide, bool opaque)
        {
            const Dds dds(ddsBytes);
            if (dds.levels.empty())
                throw Dm2Error("a texture's DDS has no level");
            const int first = firstLevel(dds, maxSide);
            Rows base = dds.decode(first);
            int w = dds.levels[size_t(first)].width, h = dds.levels[size_t(first)].height;
            int shrunk = 0;
            while (std::max(w, h) > maxSide && w > 1 && h > 1)
            {
                base = halve(base);
                w = int(base[0].size());
                h = int(base.size());
                ++shrunk;
            }
            std::vector<Rows> levels = {base};
            for (int k = first + 1; int(levels.size()) < kMaxLevels && !shrunk && k < dds.mips(); ++k)
            {
                const Dds::Level &l = dds.levels[size_t(k)];
                if (std::min(l.width, l.height) < kMinMipSide)
                    break;
                levels.push_back(dds.decode(k));
            }
            long long low = 0;
            for (const auto &row : base)
                for (const Rgba &c : row)
                    low += c[3] < 64;
            const bool cutout = !opaque && low * 100 >= (long long)w * h * 3;
            std::map<Rgba, long long> counts;
            std::vector<Rows> mapped;
            for (const Rows &rows : levels)
            {
                Rows out;
                for (const auto &row : rows)
                {
                    std::vector<Rgba> line;
                    for (const Rgba &c : row)
                    {
                        const Rgba b = cutout && c[3] < 128 ? kColourKey : bin6(c);
                        ++counts[b];
                        line.push_back(b);
                    }
                    out.push_back(std::move(line));
                }
                mapped.push_back(std::move(out));
            }
            const bool keyed = counts.erase(kColourKey) > 0;
            Palette palette;
            if (!counts.empty())
                palette = medianCut(counts, keyed ? 255 : 256);
            if (keyed)
            {
                palette.colours.insert(palette.colours.begin(), kColourKey);
                for (auto &[c, i] : palette.index)
                    ++i;
                palette.index[kColourKey] = 0;
            }
            Bytes head(txrh.begin(), txrh.begin() + 4);
            appendU16(head, uint16_t(w));
            appendU16(head, uint16_t(h));
            head.insert(head.end(), txrh.begin() + 8, txrh.end());
            Chunks kids = {Chunk::leaf("TXRH", std::move(head))};
            Bytes indices;
            for (const Rows &rows : mapped)
            {
                std::vector<std::vector<uint8_t>> index;
                for (const auto &row : rows)
                {
                    std::vector<uint8_t> line;
                    for (const Rgba &c : row)
                        line.push_back(uint8_t(palette.index.at(c)));
                    index.push_back(std::move(line));
                }
                if (indices.empty())
                    for (const auto &line : index)
                        indices.insert(indices.end(), line.begin(), line.end());
                Bytes bmp = bmp8(index, palette.colours);
                bmp.resize((bmp.size() + 3) & ~size_t(3), 0);
                kids.push_back(Chunk::leaf("TBMP", std::move(bmp)));
            }
            ConvertedTexture out;
            out.txrc = serialize({Chunk::node("TXRC", std::move(kids))});
            out.id = u32At(txrh, 0);
            out.xboxSize = {dds.width, dds.height};
            out.ps2Size = {w, h};
            out.levels = int(levels.size());
            out.keyed = keyed;
            out.opaque = opaque;
            out.kind = dds.kind;
            // The Xbox picture for the GPU renderer: DXT3 levels as they are
            // (BC2), else its first level as RGBA8.
            PackPicture picture{dds.width, dds.height, 1, 2, {}};
            if (dds.kind == "DXT3")
            {
                picture.mips = dds.mips();
                picture.format = 1;
                picture.data = sliceOf(ddsBytes, dds.levels.front().at, dds.levels.back().at + dds.levels.back().size);
            }
            else
                for (const auto &row : dds.decode(0))
                    for (const Rgba &c : row)
                        for (int v : c)
                            picture.data.push_back(uint8_t(v));
            out.entry = {w, h, fnv1a64(indices), std::move(picture), cText(txrh, 0x14)};
            return out;
        }

        // runtime/ps2_texture_replace.h's pack (.xtp): "PXTP", version 1, the
        // entries (64 bytes each), then the pictures at 16-byte boundaries.
        Bytes writePack(const std::vector<PackEntry> &entries)
        {
            Bytes out;
            appendText(out, "PXTP");
            appendU32(out, 1);
            appendU32(out, uint32_t(entries.size()));
            appendU32(out, 0);
            Bytes blob;
            const size_t base = 16 + 64 * entries.size();
            for (const PackEntry &e : entries)
            {
                appendU16(out, uint16_t(e.width));
                appendU16(out, uint16_t(e.height));
                out.push_back(8);
                out.push_back(0);
                appendU16(out, 0);
                appendU32(out, uint32_t(e.indexHash));
                appendU32(out, uint32_t(e.indexHash >> 32));
                appendU16(out, uint16_t(e.picture.width));
                appendU16(out, uint16_t(e.picture.height));
                out.push_back(uint8_t(e.picture.mips));
                out.push_back(e.picture.format);
                appendU16(out, 0);
                appendU32(out, uint32_t(base + blob.size()));
                appendU32(out, uint32_t(e.picture.data.size()));
                appendPadded(out, e.name.substr(0, 32), 32);
                appendBytes(blob, e.picture.data);
                blob.resize((blob.size() + 15) & ~size_t(15), 0);
            }
            appendBytes(out, blob);
            return out;
        }

        // The texel bytes convertTexture makes of a DDS at a max side: its
        // first level and the mips it keeps.
        long long ps2Texels(const Dds &dds, int maxSide)
        {
            const int first = firstLevel(dds, maxSide);
            int w = dds.levels[size_t(first)].width, h = dds.levels[size_t(first)].height;
            int shrunk = 0;
            while (std::max(w, h) > maxSide && w > 1 && h > 1)
            {
                w /= 2;
                h /= 2;
                ++shrunk;
            }
            long long total = (long long)w * h;
            int levels = 1;
            for (int k = first + 1; levels < kMaxLevels && !shrunk && k < dds.mips(); ++k)
            {
                const Dds::Level &l = dds.levels[size_t(k)];
                if (std::min(l.width, l.height) < kMinMipSide)
                    break;
                total += (long long)l.width * l.height;
                ++levels;
            }
            return total;
        }

        // Each texture's max side so that all of them come to at most
        // `budget` texel bytes: the largest made one level smaller first
        // (ties by order), none below 16 a side.
        std::vector<int> budgetSides(const std::vector<Dds> &ddss, int maxSide, long long budget, long long &total)
        {
            std::vector<int> sides(ddss.size(), maxSide);
            std::vector<long long> sizes;
            total = 0;
            for (const Dds &d : ddss)
            {
                sizes.push_back(ps2Texels(d, maxSide));
                total += sizes.back();
            }
            while (total > budget)
            {
                int best = -1;
                for (size_t i = 0; i < ddss.size(); ++i)
                    if (sides[i] > 16 && (best < 0 || sizes[i] > sizes[size_t(best)]))
                        best = int(i);
                if (best < 0)
                    break;
                sides[size_t(best)] /= 2;
                const long long made = ps2Texels(ddss[size_t(best)], sides[size_t(best)]);
                total += made - sizes[size_t(best)];
                sizes[size_t(best)] = made;
            }
            return sides;
        }

        using TextureSizes = std::map<uint32_t, std::pair<std::pair<int, int>, std::pair<int, int>>>; // id: (xbox, ps2)

        // The level's TXR as the PS2's, the textures' sizes and the pack's entries.
        Bytes convertTxr(const Bytes &data, int maxSide, const std::set<uint32_t> &opaque, long long budget,
                         TextureSizes &sizes, std::vector<PackEntry> &entries, Report &report)
        {
            const Chunks tree = parse(data);
            if (tree.empty())
                throw Dm2Error("an empty texture library");
            struct Work
            {
                Bytes head, dds;
                int side;
                bool opaque;
            };
            std::vector<Work> work;
            for (const Chunk &c : tree[0].kids)
            {
                if (c.tag != "TXRC")
                    throw Dm2Error("a " + c.tag + " chunk in a texture library");
                const Chunk *head = c.find("TXRH");
                const Chunk *dds = c.find("TDDS");
                if (!head)
                    throw Dm2Error("a texture with no TXRH");
                if (!dds)
                    throw Dm2Error("texture " + cText(head->data, 0x14) + " has no DDS");
                work.push_back({head->data, sliceOf(dds->data, 4), maxSide, opaque.count(u32At(head->data, 0)) > 0});
            }
            if (budget)
            {
                std::vector<Dds> ddss;
                for (const Work &w : work)
                    ddss.emplace_back(w.dds);
                long long total = 0;
                const std::vector<int> sides = budgetSides(ddss, maxSide, budget, total);
                for (size_t i = 0; i < work.size(); ++i)
                {
                    work[i].side = sides[i];
                    report["textures smaller for the budget"] += sides[i] < maxSide;
                }
                report["texel bytes"] = total;
            }
            Bytes out;
            for (const Work &w : work)
            {
                ConvertedTexture t = convertTexture(w.head, w.dds, w.side, w.opaque);
                appendBytes(out, t.txrc);
                sizes[t.id] = {t.xboxSize, t.ps2Size};
                entries.push_back(std::move(t.entry));
                ++report["textures"];
                report["textures smaller"] += t.xboxSize != t.ps2Size;
                report["textures keyed (cut out)"] += t.keyed;
                report["textures opaque (grass)"] += t.opaque;
                ++report["textures " + t.kind];
            }
            Bytes library;
            appendText(library, "FORM");
            appendU32(library, uint32_t(out.size()));
            appendBytes(library, out);
            return library;
        }

        // {texture id: the MATI flags of the materials using it}.
        std::map<uint32_t, std::set<uint32_t>> materialTextures(const Bytes &geo)
        {
            std::map<uint32_t, std::set<uint32_t>> out;
            walk(parse(geo), [&](const Chunk &c)
            {
                if (c.tag != "MATI")
                    return;
                const uint32_t flags = u32At(c.data, 0);
                for (uint32_t i = 0; i < u32At(c.data, 0x28); ++i)
                    out[u32At(c.data, 0x2C + 28 * i + 0x10)].insert(flags);
            });
            return out;
        }

        // ---- geometry -------------------------------------------------------------

        using Group = std::pair<uint16_t, std::vector<uint16_t>>; // (submesh word, indices)

        std::vector<Group> readGroups(const Bytes &data)
        {
            std::vector<Group> groups;
            size_t at = 0;
            while (at + 4 <= data.size())
            {
                const uint16_t head = u16At(data, at), count = u16At(data, at + 2);
                std::vector<uint16_t> indices;
                for (uint16_t i = 0; i < count; ++i)
                    indices.push_back(u16At(data, at + 4 + 2 * size_t(i)));
                groups.push_back({head, std::move(indices)});
                at += (size_t(count) * 2 + 7) & ~size_t(3);
            }
            if (at != data.size())
                throw Dm2Error("a TGFM that ends past its strips");
            return groups;
        }

        Bytes writeGroups(const std::vector<Group> &groups)
        {
            Bytes out;
            for (const auto &[head, indices] : groups)
            {
                appendU16(out, head);
                appendU16(out, uint16_t(indices.size()));
                for (uint16_t i : indices)
                    appendU16(out, i);
                out.resize((out.size() + 3) & ~size_t(3), 0);
            }
            return out;
        }

        // A group's strips: each starts at an index with 0x8000 that follows
        // one without it (a strip starts with two 0x8000 indices).
        std::vector<std::vector<uint16_t>> stripsOf(const std::vector<uint16_t> &indices)
        {
            std::vector<std::vector<uint16_t>> strips;
            std::vector<uint16_t> current;
            for (uint16_t i : indices)
            {
                if ((i & 0x8000) && !current.empty() && !(current.back() & 0x8000))
                {
                    strips.push_back(std::move(current));
                    current.clear();
                }
                current.push_back(i);
            }
            if (!current.empty())
                strips.push_back(std::move(current));
            return strips;
        }

        // The Xbox's one-strip groups packed into PS2 groups of up to 60
        // entries (whole strips, in order, per submesh word).
        std::vector<Group> packGroups(const std::vector<Group> &groups)
        {
            std::vector<Group> out;
            for (const auto &[head, indices] : groups)
                for (const std::vector<uint16_t> &strip : stripsOf(indices))
                {
                    if (strip.size() > kGroupEntries)
                        throw Dm2Error("a strip of " + std::to_string(strip.size()) + " entries");
                    if (!out.empty() && out.back().first == head && out.back().second.size() + strip.size() <= kGroupEntries)
                        out.back().second.insert(out.back().second.end(), strip.begin(), strip.end());
                    else
                        out.push_back({head, strip});
                }
            return out;
        }

        // The triangles strips draw (degenerate ones left out).
        std::vector<std::array<int, 3>> triangles(const std::vector<Group> &groups)
        {
            std::vector<std::array<int, 3>> out;
            for (const auto &[head, indices] : groups)
                for (size_t k = 2; k < indices.size(); ++k)
                {
                    if (indices[k] & 0x8000)
                        continue;
                    const int a = indices[k - 2] & 0x7FFF, b = indices[k - 1] & 0x7FFF, c = indices[k] & 0x7FFF;
                    if (a != b && b != c && a != c)
                        out.push_back({a, b, c});
                }
            return out;
        }

        void convertPart(Chunk &part, Report &report)
        {
            std::map<std::string, Chunk> streams;
            for (Chunk &k : part.kids)
            {
                if (std::find(std::begin(kPs2PartOrder), std::end(kPs2PartOrder), k.tag) == std::end(kPs2PartOrder))
                    throw Dm2Error("a part with a " + k.tag + " stream");
                streams[k.tag] = k; // (a later one of the same tag wins)
            }
            if (!streams.count("TGFM") || !streams.count("TGPH"))
                throw Dm2Error("a part with no TGFM or TGPH");
            const std::vector<Group> groups = readGroups(streams["TGFM"].data);
            const std::vector<Group> packed = packGroups(groups);
            if (triangles(packed) != triangles(groups))
                throw Dm2Error("packing changed the triangles");
            streams["TGFM"].data = writeGroups(packed);
            Bytes &head = streams["TGPH"].data;
            if (u32At(head, 8) != groups.size())
                throw Dm2Error("TGPH's group count is not TGFM's");
            putU32(head, 8, uint32_t(packed.size()));
            report["strip groups in"] += long(groups.size());
            report["strip groups out"] += long(packed.size());
            ++report["parts"];
            report["faces"] += u16At(head, 6);
            part.kids.clear();
            for (const char *t : kPs2PartOrder)
                if (streams.count(t))
                    part.kids.push_back(std::move(streams[t]));
        }

        // A layer's GS CLAMP fields for a texture scaled by sx, sy (powers of
        // two): region clamp (2) keeps its region, region repeat (3) its mask
        // and fix, in the new texels.
        void scaleClamp(uint32_t &w0, uint32_t &w1, double sx, double sy)
        {
            const uint32_t wms = (w0 >> 10) & 3, wmt = (w0 >> 12) & 3;
            long minu = (w0 >> 14) & 0x3FF, maxu = w1 & 0x3FF;
            long minv = (w1 >> 10) & 0x3FF, maxv = (w1 >> 20) & 0x3FF;
            auto region = [](uint32_t mode, long &lo, long &hi, double s)
            {
                if (mode == 2)
                {
                    const long newLo = long(lo * s);
                    hi = std::max(long((hi + 1) * s) - 1, newLo);
                    lo = newLo;
                }
                else if (mode == 3)
                {
                    const long newLo = std::max(long((lo + 1) * s) - 1, 0L);
                    hi = long(hi * s);
                    lo = newLo;
                }
            };
            region(wms, minu, maxu, sx);
            region(wmt, minv, maxv, sy);
            w0 = (w0 & ~(0x3FFu << 14)) | uint32_t(minu << 14);
            w1 = (w1 & ~0x3FFFFFFFu) | uint32_t(maxu) | uint32_t(minv << 10) | uint32_t(maxv << 20);
        }

        void convertMati(Bytes &m, const TextureSizes &sizes, Report &report)
        {
            if (m.size() != kMaterialSize)
                throw Dm2Error("a MATI of " + std::to_string(m.size()) + " bytes");
            const uint32_t flags = u32At(m, 0);
            if (flags & ~2u)
            {
                char text[64];
                std::snprintf(text, sizeof text, "material flags cleared (0x%x)", flags & ~2u);
                ++report[text];
            }
            putU32(m, 0, flags & 2);
            for (uint32_t i = 0; i < u32At(m, 0x28); ++i)
            {
                const size_t at = 0x2C + 28 * size_t(i);
                uint32_t w0 = u32At(m, at), w1 = u32At(m, at + 4);
                const uint32_t texture = u32At(m, at + 0x10);
                if (((w0 >> 10) & 3) >= 2 || ((w0 >> 12) & 3) >= 2)
                {
                    const auto it = sizes.find(texture);
                    const auto [xbox, ps2] = it == sizes.end() ? std::make_pair(std::make_pair(1, 1), std::make_pair(1, 1))
                                                               : it->second;
                    if (xbox != ps2)
                    {
                        scaleClamp(w0, w1, double(ps2.first) / xbox.first, double(ps2.second) / xbox.second);
                        putU32(m, at, w0);
                        putU32(m, at + 4, w1);
                        ++report["clamp regions scaled"];
                    }
                }
            }
        }

        uint32_t partId(const Chunk &part)
        {
            const Chunk *head = part.find("TGPH");
            if (!head)
                throw Dm2Error("a part with no TGPH");
            return u32At(head->data, 0x10);
        }

        // Props with two levels of detail: the first kept with the longest
        // range, the other's TGLO, part and materials dropped.
        void convertLods(Chunk &object, Report &report)
        {
            std::vector<size_t> lods;
            for (size_t i = 0; i < object.kids.size(); ++i)
                if (object.kids[i].tag == "TGLO")
                    lods.push_back(i);
            if (lods.size() < 2)
                return;
            double reach = f32At(object.kids[lods[0]].data, 0);
            for (size_t i : lods)
                reach = std::max(reach, f32At(object.kids[i].data, 0));
            putF32(object.kids[lods[0]].data, 0, reach);
            std::vector<uint32_t> gone;
            for (size_t n = 1; n < lods.size(); ++n)
                gone.push_back(u32At(object.kids[lods[n]].data, 8));
            for (uint32_t id : gone)
            {
                std::vector<Chunk> &kids = object.kids;
                size_t at = 0;
                while (at < kids.size() && !(kids[at].tag == "TGPT" && partId(kids[at]) == id))
                    ++at;
                if (at == kids.size())
                    throw Dm2Error("a level of detail names a part the object does not have");
                size_t end = at + 1;
                while (end < kids.size() && (kids[end].tag == "TGLO" || kids[end].tag == "TMTB"))
                    ++end;
                kids.erase(kids.begin() + long(at), kids.begin() + long(end));
                ++report["second LODs dropped"];
            }
            for (Chunk &k : object.kids)
                if (k.tag == "TGFR" && k.data.size() > 2)
                    k.data[2] = 1;
        }

        // A part's materials no strip group draws, dropped from its TMTB (DM2
        // CALLs each material's strip packet, which such a material never
        // gets); the strips' and faces' material numbers renumbered (a face of
        // a dropped one takes the part's first).
        void dropUnusedMaterials(Chunk &object, Report &report)
        {
            std::vector<Chunk> &kids = object.kids;
            for (size_t i = 0; i < kids.size(); ++i)
            {
                Chunk &part = kids[i];
                if (part.tag != "TGPT")
                    continue;
                Chunk *table = nullptr;
                for (size_t k = i + 1; k < kids.size(); ++k)
                {
                    if (kids[k].tag == "TGPT")
                        break;
                    if (kids[k].tag == "TMTB")
                    {
                        table = &kids[k];
                        break;
                    }
                }
                Chunk *strips = part.find("TGFM"), *faces = part.find("TGFP"), *head = part.find("TGPH");
                if (!table || !strips || !faces)
                    continue;
                std::set<int> used;
                for (size_t at = 0; at + 4 <= strips->data.size();
                     at += (size_t(u16At(strips->data, at + 2)) * 2 + 7) & ~size_t(3))
                    used.insert(u16At(strips->data, at) >> 8);
                const int materials = int(table->kids.size());
                if (used.empty() || *used.rbegin() >= materials || int(used.size()) == materials)
                    continue;
                std::map<int, int> renumber;
                for (int old : used)
                    renumber.emplace(old, int(renumber.size()));
                for (size_t at = 0; at + 4 <= strips->data.size();
                     at += (size_t(u16At(strips->data, at + 2)) * 2 + 7) & ~size_t(3))
                {
                    const uint16_t word = u16At(strips->data, at);
                    putU16(strips->data, at, uint16_t((renumber.at(word >> 8) << 8) | (word & 0xFF)));
                }
                for (size_t f = 0; f < faces->data.size() / 8; ++f)
                {
                    const uint16_t flags = u16At(faces->data, 8 * f);
                    const auto it = renumber.find(flags & 0xFF);
                    putU16(faces->data, 8 * f, uint16_t((flags & 0xFF00) | (it == renumber.end() ? 0 : it->second)));
                }
                std::vector<Chunk> keptMaterials;
                for (int n = 0; n < materials; ++n)
                    if (renumber.count(n))
                        keptMaterials.push_back(std::move(table->kids[size_t(n)]));
                table->kids = std::move(keptMaterials);
                if (head)
                    putU16(head->data, 0xE, uint16_t(table->kids.size()));
                report["unused materials dropped"] += materials - long(table->kids.size());
            }
        }

        // ---- recentring ---------------------------------------------------------------

        // A DM2 TGCI tree with every split value moved by -offset, the wrapped
        // ones first put back into the object's bounds.
        Bytes unwrapSplits(const Bytes &blob, const Vec3 &lo, const Vec3 &hi, const Vec3 &offset)
        {
            Bytes out = blob;
            std::function<void(size_t)> visit = [&](size_t at)
            {
                if (at >= blob.size())
                    throw Dm2Error("a collision tree node past its end");
                const int kind = blob[at];
                if (kind == 0)
                    return;
                const size_t axis = size_t(kind - 1);
                if (axis > 2)
                    throw Dm2Error("a collision tree node of kind " + std::to_string(kind));
                double value = s16At(blob, at + 2) / 100.0;
                while (value < lo[axis] - 1.0)
                    value += 655.36;
                while (value > hi[axis] + 1.0)
                    value -= 655.36;
                const double moved = std::nearbyint((value - offset[axis]) * 100.0);
                if (moved < -32768 || moved > 32767)
                    throw Dm2Error("a collision split out of range after recentring");
                putU16(out, at + 2, uint16_t(int16_t(moved)));
                visit(size_t(u16At(blob, at + 4)) * 4);
                visit(size_t(u16At(blob, at + 6)) * 4);
            };
            visit(0);
            return out;
        }

        // Moves an object whose local bounds pass the tree's range to around
        // its own centre; the offset its instances move by, or none.
        std::optional<Vec3> recentreObject(Chunk &object)
        {
            Chunk *head = object.find("TGHD");
            if (!head)
                throw Dm2Error("an object with no TGHD");
            double bounds[6];
            for (int i = 0; i < 6; ++i)
                bounds[i] = f32At(head->data, 4 * size_t(i));
            double largest = 0;
            for (double v : bounds)
                largest = std::max(largest, std::fabs(v));
            if (largest <= kSplitLimit)
                return std::nullopt;
            const Vec3 lo = {bounds[0], bounds[1], bounds[2]}, hi = {bounds[3], bounds[4], bounds[5]};
            Vec3 offset;
            for (size_t i = 0; i < 3; ++i)
                offset[i] = std::nearbyint((lo[i] + hi[i]) / 2.0); // Python's round(): half to even
            for (int i = 0; i < 6; ++i)
                putF32(head->data, 4 * size_t(i), bounds[i] - offset[size_t(i % 3)]);
            for (Chunk &part : object.kids)
                if (part.tag == "TGPT")
                    for (Chunk &k : part.kids)
                        if (k.tag == "TGVP")
                            for (size_t i = 0; i < k.data.size() / 4 / 3 * 3; ++i)
                                putF32(k.data, 4 * i, f32At(k.data, 4 * i) - offset[i % 3]);
            for (Chunk &k : object.kids)
                if (k.tag == "TGCI")
                {
                    const uint32_t size = u32At(k.data, 4);
                    const Bytes tree = unwrapSplits(sliceOf(k.data, 8, 8 + size_t(size)), lo, hi, offset);
                    std::copy(tree.begin(), tree.end(), k.data.begin() + 8);
                }
            return offset;
        }

        // The classes DM2's PS2 executable makes objects of (its class
        // registry) plus Terrain, and "Prop"'s hash every level uses.
        bool isPs2Class(uint32_t id)
        {
            static const char *const kClasses[] = {"Terrain", "Car",      "TrafficLight", "Glass",        "Exploder",
                                                   "Mover",   "Fountain", "WaterSurface", "Boat",         "DynamicsObject"};
            constexpr uint32_t kPropClass = 0x03BE8154;
            if (id == kPropClass)
                return true;
            for (const char *c : kClasses)
                if (nameHash(c) == id)
                    return true;
            return false;
        }

        std::string xboxClassName(uint32_t id)
        {
            if (id == nameHash("AnimPathFollower"))
                return "AnimPathFollower";
            if (id == nameHash("riderMod"))
                return "riderMod";
            char text[16];
            std::snprintf(text, sizeof text, "0x%08x", id);
            return text;
        }

        // The GEO as the PS2's; `offsets` gets the recentred objects' (by id).
        Bytes convertGeo(const Bytes &data, const TextureSizes &sizes, Report &report, std::map<uint32_t, Vec3> &offsets)
        {
            Chunks tree = parse(data);
            if (tree.empty())
                throw Dm2Error("an empty GEO");
            for (Chunk &object : tree[0].kids)
            {
                if (object.tag != "TGEO")
                    continue;
                ++report["objects"];
                if (std::optional<Vec3> offset = recentreObject(object))
                {
                    ++report["objects recentred"];
                    offsets[u32At(object.find("TGHD")->data, 0x18)] = *offset;
                }
                convertLods(object, report);
                for (Chunk &k : object.kids)
                    if (k.tag == "TGTD" && k.data.size() >= 4)
                    {
                        const uint32_t id = u32At(k.data, 0);
                        if (!isPs2Class(id))
                        {
                            ++report["type " + xboxClassName(id) + " as Terrain"];
                            putU32(k.data, 0, nameHash("Terrain"));
                        }
                    }
                for (Chunk &k : object.kids)
                    if (k.tag == "TGPT")
                        convertPart(k, report);
                dropUnusedMaterials(object, report);
            }
            walk(tree, [&](Chunk &c)
            {
                if (c.tag == "MATI")
                {
                    convertMati(c.data, sizes, report);
                    ++report["materials"];
                }
            });
            return serialize(tree);
        }

        // The INS as it is but for the instances of recentred objects, moved
        // so that the object stays where it was.
        Bytes convertIns(const Bytes &data, const std::map<uint32_t, Vec3> &offsets, Report &report)
        {
            if (offsets.empty())
                return data;
            Chunks tree = parse(data);
            walk(tree, [&](Chunk &c)
            {
                if (c.tag != "TGIH")
                    return;
                const auto it = offsets.find(u32At(c.data, 0x40));
                if (it == offsets.end())
                    return;
                if (u32At(c.data, 0) & 0x400)
                    throw Dm2Error("a recentred object's instance is mirrored");
                const Quat q = {f32At(c.data, 8), f32At(c.data, 12), f32At(c.data, 16), f32At(c.data, 20)};
                const Vec3 turned = quatRotate(q, it->second);
                for (size_t i = 0; i < 3; ++i)
                    putF32(c.data, 0x18 + 4 * i, f32At(c.data, 0x18 + 4 * i) + turned[i]);
                ++report["instances moved"];
            });
            return serialize(tree);
        }

        Bytes convertDat(const Bytes &data, Report &report, Vec3 &start)
        {
            Chunks tree = parse(data);
            if (tree.empty())
                throw Dm2Error("an empty DAT");
            const uint32_t cubeMap = nameHash("CubeMap"), playerOne = nameHash("playerone");
            std::vector<Chunk> kept;
            bool started = false;
            for (Chunk &c : tree[0].kids)
            {
                if (c.tag == "SDPT" && u32At(c.data, 4) == cubeMap)
                {
                    ++report["dropped CubeMap"];
                    continue;
                }
                if (c.tag == "SDPT" && u32At(c.data, 4) == kSpawnClass)
                {
                    ++report["spawns"];
                    if (u32At(c.data, 8) == playerOne)
                    {
                        start = {f32At(c.data, 0x10), f32At(c.data, 0x14), f32At(c.data, 0x18)};
                        started = true;
                    }
                }
                kept.push_back(std::move(c));
            }
            if (!started)
                throw Dm2Error("no playerone start point");
            tree[0].kids = std::move(kept);
            return serialize(tree);
        }

        // ---- the level select -------------------------------------------------------

        // {level: its info text} from the Xbox string table (KSTR_LEVELINFOMENU_<v>).
        std::map<std::string, Bytes> levelTexts(const SourceFiles &xbox)
        {
            std::map<std::string, Bytes> out;
            Bytes data;
            if (!xbox.readAny("string.eus", data))
                return out;
            std::map<uint32_t, Bytes> table;
            const uint32_t count = u32At(data, 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t key = u32At(data, 8 + 8 * size_t(i)), at = u32At(data, 12 + 8 * size_t(i));
                const auto end = std::find(data.begin() + std::min<long>(at, long(data.size())), data.end(), 0);
                if (end == data.end())
                    throw Dm2Error("the Xbox string table has a text with no end");
                table[key] = Bytes(data.begin() + at, end);
            }
            for (const LevelInfo &level : kLevels)
            {
                const auto it = table.find(nameHash("KSTR_LEVELINFOMENU_" + std::to_string(level.xboxValue)));
                if (it != table.end() && !it->second.empty())
                    out[level.folder] = it->second;
            }
            return out;
        }

        // The Xbox UILIB's level pictures (LEVELS.ZIF's UAST "level<v>": HAST
        // {path[128], name[32], u32}, UMTL {HMTL, TXRC {TXRH, TBMP}}, the PS2's
        // format), the asset and its texture named "xb_<level>" for the runtime.
        Chunks pictureAssets(const SourceFiles &xbox)
        {
            Bytes uilib;
            if (!xbox.readAny("uilib.zal", uilib))
                return {};
            std::map<std::string, std::string> want; // "level8" -> "VENICE"
            for (const LevelInfo &level : kLevels)
                want["level" + std::to_string(level.xboxValue)] = level.folder;
            std::map<std::string, Chunk> found;
            for (const Bytes &entry : zalEntries(uilib))
            {
                if (entry.size() < 4 || std::memcmp(entry.data(), "ULIB", 4) != 0)
                    continue;
                Chunks library;
                try
                {
                    library = parseUi(entry);
                }
                catch (const Dm2Error &)
                {
                    continue;
                }
                for (Chunk &c : library)
                {
                    if (c.tag != "ULIB")
                        continue;
                    for (Chunk &asset : c.kids)
                    {
                        if (asset.tag != "UAST" || asset.kids.empty() || asset.kids[0].tag != "HAST")
                            continue;
                        const Bytes &hast = asset.kids[0].data;
                        const std::string name = lowerAscii(cText(hast, 0x80, 0xA0));
                        const auto wanted = want.find(name);
                        if (wanted == want.end() || found.count(wanted->second))
                            continue;
                        const std::string renamed = "xb_" + lowerAscii(wanted->second);
                        Bytes newHast = sliceOf(hast, 0, 0x80);
                        appendPadded(newHast, renamed, 32);
                        appendBytes(newHast, sliceOf(hast, 0xA0));
                        asset.kids[0].data = std::move(newHast);
                        walk(asset.kids, [&](Chunk &k)
                        {
                            if (k.tag != "TXRH")
                                return;
                            Bytes head;
                            appendU32(head, nameHash(renamed));
                            appendBytes(head, sliceOf(k.data, 4, 0x14));
                            appendPadded(head, renamed, 32);
                            appendBytes(head, sliceOf(k.data, 0x34));
                            k.data = std::move(head);
                        });
                        found[wanted->second] = asset;
                    }
                }
            }
            Chunks out;
            for (const LevelInfo &level : kLevels)
                if (found.count(level.folder))
                    out.push_back(found[level.folder]);
            return out;
        }

        std::string megabytes(size_t bytes)
        {
            char text[32];
            std::snprintf(text, sizeof text, "%.1f", double(bytes) / 1e6);
            return text;
        }

        void convertLevel(const SourceFiles &xbox, const LevelInfo &level, const Dm2XboxOptions &options, Made &made,
                          std::vector<std::string> &log)
        {
            std::map<std::string, Bytes> files;
            for (const char *ext : {"ZIT", "GEO", "INS", "TXR", "DAT"})
                files[ext] = need(xbox, std::string("levels/") + level.xboxFolder + "/" + level.xboxFolder + "." + ext,
                                  "the Xbox files");
            // Textures of materials with an Xbox material type (MATI flags
            // 0x4, 0x8: grass) are drawn opaque, as the PS2's grass base layers are.
            std::set<uint32_t> grass;
            for (const auto &[texture, flags] : materialTextures(files["GEO"]))
                for (uint32_t f : flags)
                    if (f & 0x3FC)
                        grass.insert(texture);
            Report report;
            TextureSizes sizes;
            std::vector<PackEntry> entries;
            const Bytes txr = convertTxr(files["TXR"], options.maxSide, grass, (long long)options.textureBudgetKb * 1024,
                                         sizes, entries, report);
            std::map<uint32_t, Vec3> offsets;
            Vec3 start{};
            const std::string folder = std::string(level.folder) + "/" + level.folder + ".";
            const Bytes zit = convertZit(files["ZIT"], level.folder);
            const Bytes geo = convertGeo(files["GEO"], sizes, report, offsets);
            const Bytes ins = convertIns(files["INS"], offsets, report);
            const Bytes dat = convertDat(files["DAT"], report, start);
            const Bytes xtp = writePack(entries);
            char line[512];
            std::snprintf(line, sizeof line,
                          "[dm2xbox] %s: %lld objects, %lld parts, %lld faces, %lld materials, %lld textures (%lld made "
                          "smaller, %lld keyed), strip groups %lld -> %lld, start (%.1f, %.1f, %.1f); GEO %s -> %s MB, "
                          "TXR %s -> %s MB, XTP %s MB",
                          level.folder, report["objects"], report["parts"], report["faces"], report["materials"],
                          report["textures"], report["textures smaller"], report["textures keyed (cut out)"],
                          report["strip groups in"], report["strip groups out"], start[0], start[1], start[2],
                          megabytes(files["GEO"].size()).c_str(), megabytes(geo.size()).c_str(),
                          megabytes(files["TXR"].size()).c_str(), megabytes(txr.size()).c_str(),
                          megabytes(xtp.size()).c_str());
            log.push_back(line);
            made.push_back({folder + "ZIT", zit});
            made.push_back({folder + "GEO", geo});
            made.push_back({folder + "INS", ins});
            made.push_back({folder + "TXR", txr});
            made.push_back({folder + "DAT", dat});
            made.push_back({folder + "XTP", xtp});
        }
    }

    Bytes need(const SourceFiles &files, const std::string &path, const char *what)
    {
        Bytes data;
        if (!files.readAny(path, data))
            throw Dm2Error(path + " is not in " + what);
        return data;
    }

    Bytes convertZit(const Bytes &data, const std::string &name)
    {
        Chunks tree = parse(data);
        walk(tree, [&](Chunk &c)
        {
            if (c.tag != "ZFTX" && c.tag != "ZFGE" && c.tag != "ZFIS" && c.tag != "ZFSD")
                return;
            const std::string old = cText(c.data, 0);
            const size_t dot = old.rfind('.');
            const std::string ext = upperAscii(dot == std::string::npos ? old : old.substr(dot + 1));
            const std::string renamed = name + "." + ext;
            Bytes out;
            appendText(out, renamed);
            out.insert(out.end(), 4 - renamed.size() % 4, 0);
            c.data = std::move(out);
        });
        return serialize(tree);
    }

    void convertLevels(const SourceFiles &xbox, const Dm2XboxOptions &options, Made &made, std::vector<std::string> &log)
    {
        const std::map<std::string, Bytes> texts = levelTexts(xbox);
        std::string lines;
        for (const LevelInfo &level : kLevels)
        {
            convertLevel(xbox, level, options, made, log);
            if (const auto it = texts.find(level.folder); it != texts.end())
                made.push_back({std::string(level.folder) + "/INFO.TXT", it->second});
            lines += std::string(level.folder) + " " + level.name + "\n";
        }
        const Chunks assets = pictureAssets(xbox);
        if (!assets.empty())
        {
            Bytes data = serialize({Chunk::node("ULIB", assets)});
            log.push_back("[dm2xbox] level pictures: " + std::to_string(assets.size()) + " in PICTURES.ZIF (" +
                          std::to_string(data.size()) + " bytes)");
            made.push_back({"PICTURES.ZIF", std::move(data)});
        }
        made.push_back({"levels.txt", Bytes(lines.begin(), lines.end())});
    }
}
