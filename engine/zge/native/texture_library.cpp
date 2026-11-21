#include "texture_library.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <regex>
#include <set>

namespace zge::native
{
    namespace
    {
        uint32_t u32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        int32_t s32(const uint8_t *p) { return static_cast<int32_t>(u32(p)); }
        uint16_t u16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool printableTag(const uint8_t *tag)
        {
            for (int i = 0; i < 4; ++i)
                if (tag[i] < 32 || tag[i] >= 127)
                    return false;
            return true;
        }

        bool tagIs(const uint8_t *tag, const char *name) { return std::memcmp(tag, name, 4) == 0; }

        // A Windows BMP of 4 or 8 bits per texel: indices top row first, the
        // palette's RGB.
        bool readBmp(const uint8_t *d, size_t size, Ps2Picture &out)
        {
            if (size < 54 || d[0] != 'B' || d[1] != 'M')
                return false;
            const uint32_t pixels = u32(d + 10);
            const uint32_t header = u32(d + 14);
            const int32_t width = s32(d + 18);
            const int32_t height = s32(d + 22);
            const int bits = u16(d + 28);
            if ((bits != 4 && bits != 8) || width <= 0 || height == 0)
                return false;
            const int h = std::abs(height);
            const size_t stride = ((static_cast<size_t>(width) * bits + 31) / 32) * 4;
            if (pixels + stride * h > size)
                return false;
            uint32_t colours = u32(d + 14 + 32);
            if (colours == 0 || colours > 256)
                colours = 1u << bits;
            if (14 + header + 4 * colours > size)
                return false;
            out = Ps2Picture{};
            out.width = width;
            out.height = h;
            out.bits = bits;
            for (uint32_t i = 0; i < colours; ++i)
            {
                const uint8_t *c = d + 14 + header + 4 * i; // B G R A
                out.palette[i] = c[2] | (c[1] << 8) | (c[0] << 16);
            }
            out.indices.resize(static_cast<size_t>(width) * h);
            for (int y = 0; y < h; ++y)
            {
                // Rows are stored bottom-up unless the height is negative.
                const uint8_t *row = d + pixels + stride * (height > 0 ? h - 1 - y : y);
                uint8_t *dst = out.indices.data() + static_cast<size_t>(y) * width;
                for (int x = 0; x < width; ++x)
                    dst[x] = bits == 8 ? row[x] : ((x & 1) ? row[x >> 1] & 15 : row[x >> 1] >> 4);
            }
            return true;
        }

        // The texture in a TXPR chunk: its format, size and the mip chain.
        bool readTxpr(const uint8_t *d, size_t size, XboxPicture &out)
        {
            if (size < 16 || std::memcmp(d + 4, "XPR0", 4) != 0)
                return false;
            const uint32_t headerSize = u32(d + 12);
            size_t r = 16;
            while (r + 20 <= 4 + headerSize && r + 20 <= size)
            {
                const uint32_t common = u32(d + r);
                if (common == 0xFFFFFFFFu)
                    return false;
                const uint32_t type = (common >> 16) & 7;
                if (type == 4) // a texture
                {
                    const uint32_t data = u32(d + r + 4);
                    const uint32_t format = u32(d + r + 12);
                    const uint32_t linearSize = u32(d + r + 16);
                    if (linearSize) // linear textures are never DXT
                        return false;
                    out.format = static_cast<int>((format >> 8) & 0xFF);
                    out.mips = static_cast<int>((format >> 16) & 15);
                    out.width = 1 << ((format >> 20) & 15);
                    out.height = 1 << ((format >> 24) & 15);
                    size_t bytes = 0;
                    for (int level = 0, w = out.width, h = out.height; level < out.mips; ++level)
                    {
                        bytes += dxt3LevelBytes(w, h);
                        w = std::max(1, w / 2);
                        h = std::max(1, h / 2);
                    }
                    const size_t at = 4 + headerSize + data;
                    if (at + bytes > size)
                        return false;
                    out.levels.assign(d + at, d + at + bytes);
                    return true;
                }
                if (type != 3) // only palettes come before
                    return false;
                r += 12;
            }
            return false;
        }

        void readTxrc(const uint8_t *d, size_t begin, size_t end, std::vector<LibraryTexture> &out)
        {
            std::string name;
            uint32_t id = 0;
            std::vector<LibraryTexture> pictures;
            for (size_t p = begin; p + 8 <= end;)
            {
                const uint8_t *tag = d + p;
                const uint32_t size = u32(d + p + 4);
                const size_t body = p + 8;
                if (body + size > end)
                    break;
                if (tagIs(tag, "TXRH"))
                {
                    if (size >= 4)
                        id = u32(d + body);
                    const char *text = reinterpret_cast<const char *>(d + body + 0x14);
                    const size_t room = size > 0x14 ? size - 0x14 : 0;
                    name.assign(text, strnlen(text, room));
                }
                else if (tagIs(tag, "TBMP") || tagIs(tag, "TZMP") || tagIs(tag, "TXPR") || tagIs(tag, "TTGA"))
                {
                    LibraryTexture t;
                    t.kind.assign(reinterpret_cast<const char *>(tag), 4);
                    if (t.kind == "TXPR")
                        t.readable = readTxpr(d + body, size, t.xbox);
                    else if (t.kind != "TTGA")
                        t.readable = readBmp(d + body, size, t.ps2);
                    pictures.push_back(std::move(t));
                }
                p = body + size;
            }
            if (pictures.empty())
                return;
            LibraryTexture base = std::move(pictures[0]);
            base.name = name;
            base.id = id;
            for (size_t i = 1; i < pictures.size(); ++i)
                if (pictures[i].kind == base.kind && pictures[i].readable && pictures[i].ps2.bits == base.ps2.bits)
                    base.ps2Mips.push_back(std::move(pictures[i].ps2));
            out.push_back(std::move(base));
        }

        void walk(const uint8_t *d, size_t begin, size_t end, std::vector<LibraryTexture> &out, int depth)
        {
            if (depth > 16)
                return;
            for (size_t p = begin; p + 8 <= end;)
            {
                const uint8_t *tag = d + p;
                const uint32_t size = u32(d + p + 4);
                const size_t body = p + 8;
                if (body + size > end || !printableTag(tag))
                    return;
                if (tagIs(tag, "TXRC"))
                    readTxrc(d, body, body + size, out);
                else if (tagIs(tag, "FORM"))
                    // A FORM's body starts with its type, unless it is a TXRC.
                    walk(d, (size >= 4 && tagIs(d + body, "TXRC")) ? body : body + 4, body + size, out, depth + 1);
                else
                {
                    static const char *const kContainers[] = {"ULIB", "UAST", "UMTL", "UFNT", "LIST", "CAT ", "FMDL", "MODL"};
                    for (const char *container : kContainers)
                        if (tagIs(tag, container))
                        {
                            walk(d, body, body + size, out, depth + 1);
                            break;
                        }
                }
                p = body + size;
            }
        }

        // ---- pairing (tools/modding/xbox_pack.py) ---------------------------------

        // Renamed textures (PS2 file, PS2 name) -> Xbox name, all lower
        // case; found by picture and checked by eye.
        const std::map<std::pair<std::string, std::string>, std::string> &renamed()
        {
            static const std::map<std::pair<std::string, std::string>, std::string> table = {
                {{"levels/hanger/hanger.txr", "grassa01"}, "o_grassa01"},
                {{"skaters/sboek/sb_zit.zit", "svenmid"}, "svenmid_512"},
                {{"skaters/statue/st_zit.zit", "statue_256_id1_v1"}, "statfacea01"},
                {{"skaters/statue/st_zit.zit", "statue_256_id2_v1"}, "statmida01"},
                {{"skaters/statue/st_zit.zit", "statue_256_id3_v1"}, "statbottoma01"},
                {{"skaters/tinman/tin_zit.zit", "tinfacea01"}, "tinman_512_id1_v1"},
                {{"skaters/tinman/tin_zit.zit", "tinmida01"}, "tinman_512_id2_v1"},
                {{"skaters/tinman/tin_zit.zit", "tinbottoma01"}, "tinman_512_id3_v1"},
            };
            return table;
        }

        // Xbox-only crops of PS2 textures drawn with REGION_CLAMP: never a pair.
        bool isCrop(const std::string &name)
        {
            static const std::regex crop("_[0-9a-f]{6,8}$", std::regex::icase);
            return std::regex_search(name, crop);
        }

        // The name both releases agree on: sizes dropped ("_256"), a skater's
        // texture slots (id1, id_2 ...) as such.
        std::string pairKey(const std::string &name, bool slots)
        {
            std::string n = lower(name);
            if (slots)
            {
                static const std::regex slot("id_?([0-9])");
                std::smatch m;
                if (std::regex_search(n, m, slot))
                    return "slot" + m[1].str();
            }
            static const std::regex sizes("_(64|128|256|512|1024)(?=_|$)");
            n = std::regex_replace(n, sizes, "");
            static const std::regex underscores("_+");
            return std::regex_replace(n, underscores, "_");
        }

        // An 8x8 thumbnail: per cell the mean RGB of its opaque samples (if
        // any) and the share of opaque samples.
        struct Cell
        {
            bool any = false;
            double r = 0, g = 0, b = 0;
            double cover = 0;
        };
        using Thumb = std::array<Cell, 64>;

        Cell cell(const std::vector<std::pair<uint32_t, bool>> &samples)
        {
            Cell c;
            int k = 0;
            for (const auto &[rgb, opaque] : samples)
                if (opaque)
                {
                    c.r += rgb & 0xFF;
                    c.g += (rgb >> 8) & 0xFF;
                    c.b += (rgb >> 16) & 0xFF;
                    ++k;
                }
            if (k)
            {
                c.any = true;
                c.r /= k;
                c.g /= k;
                c.b /= k;
                c.cover = static_cast<double>(k) / static_cast<double>(samples.size());
            }
            return c;
        }

        // The PS2 colour key (magenta): transparent once loaded.
        bool isColourKey(uint32_t rgb)
        {
            return (rgb & 0xFF) >= 248 && ((rgb >> 8) & 0xFF) <= 8 && ((rgb >> 16) & 0xFF) >= 248;
        }

        Thumb ps2Thumb(const Ps2Picture &p)
        {
            Thumb out;
            constexpr int n = 8;
            for (int gy = 0; gy < n; ++gy)
                for (int gx = 0; gx < n; ++gx)
                {
                    std::vector<std::pair<uint32_t, bool>> samples;
                    for (int sy = 0; sy < 4; ++sy)
                    {
                        const int y = std::min(p.height - 1, (gy * 4 + sy) * p.height / (n * 4));
                        for (int sx = 0; sx < 4; ++sx)
                        {
                            const int x = std::min(p.width - 1, (gx * 4 + sx) * p.width / (n * 4));
                            const uint32_t c = p.palette[p.indices[static_cast<size_t>(y) * p.width + x]];
                            samples.emplace_back(c, !isColourKey(c));
                        }
                    }
                    out[gy * n + gx] = cell(samples);
                }
            return out;
        }

        uint32_t rgb565(uint16_t c)
        {
            const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
        }

        uint32_t mix(uint32_t a, uint32_t b, int wa, int wb)
        {
            uint32_t out = 0;
            for (int s = 0; s < 24; s += 8)
                out |= ((((a >> s) & 0xFF) * wa + ((b >> s) & 0xFF) * wb) / (wa + wb)) << s;
            return out;
        }

        // The four colours of a DXT3 block.
        std::array<uint32_t, 4> blockColours(const uint8_t *block)
        {
            const uint32_t c0 = rgb565(u16(block + 8)), c1 = rgb565(u16(block + 10));
            return {c0, c1, mix(c0, c1, 2, 1), mix(c0, c1, 1, 2)};
        }

        // (RGB, opaque: alpha >= 8/15) of each texel of a DXT3 level.
        std::vector<std::pair<uint32_t, bool>> decodeDxt3(const uint8_t *raw, int w, int h)
        {
            const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
            std::vector<std::pair<uint32_t, bool>> image(static_cast<size_t>(bw) * 4 * bh * 4);
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                {
                    const uint8_t *block = raw + (static_cast<size_t>(by) * bw + bx) * 16;
                    uint64_t alpha;
                    std::memcpy(&alpha, block, 8);
                    const auto colours = blockColours(block);
                    const uint32_t bits = u32(block + 12);
                    for (int i = 0; i < 16; ++i)
                        image[static_cast<size_t>(by * 4 + i / 4) * (bw * 4) + bx * 4 + i % 4] = {
                            colours[(bits >> (2 * i)) & 3], ((alpha >> (4 * i)) & 15) >= 8};
                }
            std::vector<std::pair<uint32_t, bool>> out(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    out[static_cast<size_t>(y) * w + x] = image[static_cast<size_t>(y) * (bw * 4) + x];
            return out;
        }

        Thumb xboxThumb(const XboxPicture &x)
        {
            constexpr int n = 8;
            // The smallest level at least n x n.
            int level = 0;
            int w = x.width;
            int h = x.height;
            size_t at = 0;
            while (level + 1 < x.mips && std::min(w, h) / 2 >= n)
            {
                at += dxt3LevelBytes(w, h);
                ++level;
                w /= 2;
                h /= 2;
            }
            const auto image = decodeDxt3(x.levels.data() + at, w, h);
            Thumb out;
            for (int gy = 0; gy < n; ++gy)
                for (int gx = 0; gx < n; ++gx)
                {
                    std::vector<std::pair<uint32_t, bool>> samples;
                    const int y0 = gy * h / n, y1 = std::max(y0 + 1, (gy + 1) * h / n);
                    const int x0 = gx * w / n, x1 = std::max(x0 + 1, (gx + 1) * w / n);
                    for (int y = y0; y < y1; ++y)
                        for (int xx = x0; xx < x1; ++xx)
                            samples.push_back(image[static_cast<size_t>(y) * w + xx]);
                    out[gy * n + gx] = cell(samples);
                }
            return out;
        }

        // (mean RGB difference where both are opaque, mean difference of the
        // opaque share).
        std::pair<double, double> thumbDiff(const Thumb &a, const Thumb &b)
        {
            double colour = 0;
            double cover = 0;
            int both = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].any && b[i].any)
                {
                    colour += (std::abs(a[i].r - b[i].r) + std::abs(a[i].g - b[i].g) + std::abs(a[i].b - b[i].b)) / 3.0;
                    ++both;
                }
                cover += std::abs(a[i].cover - b[i].cover);
            }
            return {both ? colour / both : 0.0, cover / a.size()};
        }

        // Texels of the first DXT3 level that are opaque and magenta: the PS2
        // colour key left in some Xbox pictures (pink where the PS2 is
        // transparent).
        int opaqueMagenta(const XboxPicture &x)
        {
            int count = 0;
            const int blocks = std::max(1, (x.width + 3) / 4) * std::max(1, (x.height + 3) / 4);
            for (int b = 0; b < blocks; ++b)
            {
                const uint8_t *block = x.levels.data() + static_cast<size_t>(b) * 16;
                const auto colours = blockColours(block);
                bool keyed[4];
                bool any = false;
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t c = colours[i];
                    keyed[i] = (c & 0xFF) >= 192 && ((c >> 8) & 0xFF) <= 64 && ((c >> 16) & 0xFF) >= 192;
                    any = any || keyed[i];
                }
                if (!any)
                    continue;
                uint64_t alpha;
                std::memcpy(&alpha, block, 8);
                const uint32_t bits = u32(block + 12);
                for (int i = 0; i < 16; ++i)
                    if (keyed[(bits >> (2 * i)) & 3] && ((alpha >> (4 * i)) & 15) >= 8)
                        ++count;
            }
            return count;
        }

        constexpr int kXboxDxt3 = 0x0E;
        constexpr double kMaxThumbDiff = 48.0;
        constexpr double kMaxCoverDiff = 0.5;
    }

    size_t dxt3LevelBytes(int width, int height)
    {
        return static_cast<size_t>(std::max(1, (width + 3) / 4)) * std::max(1, (height + 3) / 4) * 16u;
    }

    std::vector<LibraryTexture> readTextureLibrary(const std::vector<uint8_t> &file)
    {
        std::vector<LibraryTexture> out;
        walk(file.data(), 0, file.size(), out, 0);
        return out;
    }

    uint64_t indexHash(const Ps2Picture &picture)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint8_t c : picture.indices)
            h = (h ^ c) * 0x100000001B3ull;
        return h;
    }

    std::vector<TexturePair> pairTextures(const std::string &relativePath, const std::vector<LibraryTexture> &ps2,
                                          const std::vector<LibraryTexture> &xbox, std::vector<Rejected> *rejected)
    {
        const std::string rel = lower(relativePath);
        const bool slots = rel.rfind("skaters/", 0) == 0;
        std::map<std::string, const LibraryTexture *> byName, byKey;
        for (const LibraryTexture &x : xbox)
        {
            if (x.kind != "TXPR")
                continue;
            byName[lower(x.name)] = &x; // the last of a name, as a dict would

            if (!isCrop(x.name))
                byKey.emplace(pairKey(x.name, slots), &x);
        }
        auto reject = [&](const LibraryTexture &p, const LibraryTexture *x, std::string why)
        {
            if (rejected)
                rejected->push_back({p.name, x ? x->name : std::string(), std::move(why)});
        };

        std::vector<TexturePair> out;
        std::set<const LibraryTexture *> used;
        for (const LibraryTexture &p : ps2)
        {
            if ((p.kind != "TBMP" && p.kind != "TZMP") || !p.readable)
                continue;
            const LibraryTexture *x = nullptr;
            std::string how = "name";
            const auto r = renamed().find({rel, lower(p.name)});
            if (r != renamed().end())
            {
                const auto it = byName.find(r->second);
                x = it == byName.end() ? nullptr : it->second;
                how = "renamed";
            }
            else
            {
                const auto it = byKey.find(pairKey(p.name, slots));
                x = it == byKey.end() ? nullptr : it->second;
            }
            if (!x || !used.insert(x).second)
                continue;
            // The Xbox picture is the default wherever it is DXT3 and at least
            // as large (redrawn, sharper, or its own DXT3 of the same size).
            if (!x->readable || x->xbox.format != kXboxDxt3 || x->xbox.mips < 1 ||
                x->xbox.width * x->xbox.height < p.ps2.width * p.ps2.height)
                continue;
            if (std::all_of(p.ps2.indices.begin(), p.ps2.indices.end(),
                            [&](uint8_t i) { return i == p.ps2.indices[0]; }))
            {
                // One index everywhere: its key is any flat texture's of that
                // size, the game's run-time ones too.
                reject(p, x, "one colour, no key of its own");
                continue;
            }
            const int pink = opaqueMagenta(x->xbox);
            if (pink > std::max(16, x->xbox.width * x->xbox.height / 2048))
            {
                reject(p, x, std::to_string(pink) + " opaque magenta texels");
                continue;
            }
            const auto [colour, cover] = thumbDiff(ps2Thumb(p.ps2), xboxThumb(x->xbox));
            if (colour > kMaxThumbDiff || cover > kMaxCoverDiff)
            {
                reject(p, x, "pictures differ");
                continue;
            }
            out.push_back({&p, x, how});
        }
        return out;
    }
}
