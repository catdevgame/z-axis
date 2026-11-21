// The Xbox texture pack (collection/extras/xbox_texture_pack.h): Aggressive
// Inline's as tools/xbox_pack.py makes it, DM2's and BMX XXX's from their own
// Xbox libraries' forms.
#include "collection/extras/xbox_texture_pack.h"

#include "lzo1x.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace zx::app::extras
{
    namespace
    {
        constexpr uint8_t kPackFormatBc2 = 1; // ps2_texture_replace.h's DXT3
        constexpr uint32_t kXboxDxt3 = 0x0E;  // the D3D format of a DXT3 texture
        // A DDS file's header (DM2's TDDS): its size, after the "DDS " magic.
        constexpr size_t kDdsHeaderBytes = 124;
        constexpr uint32_t kDdsHasMipCount = 0x20000; // DDSD_MIPMAPCOUNT
        constexpr int kThumbSide = 8;

        // Renamed textures (PS2 file, PS2 name) -> Xbox name, all lower case;
        // paired by picture in the Xbox survey and checked by eye.
        const std::map<std::pair<std::string, std::string>, std::string> kRenamed = {
            {{"levels/hanger/hanger.txr", "grassa01"}, "o_grassa01"},
            {{"skaters/sboek/sb_zit.zit", "svenmid"}, "svenmid_512"},
            {{"skaters/statue/st_zit.zit", "statue_256_id1_v1"}, "statfacea01"},
            {{"skaters/statue/st_zit.zit", "statue_256_id2_v1"}, "statmida01"},
            {{"skaters/statue/st_zit.zit", "statue_256_id3_v1"}, "statbottoma01"},
            {{"skaters/tinman/tin_zit.zit", "tinfacea01"}, "tinman_512_id1_v1"},
            {{"skaters/tinman/tin_zit.zit", "tinmida01"}, "tinman_512_id2_v1"},
            {{"skaters/tinman/tin_zit.zit", "tinbottoma01"}, "tinman_512_id3_v1"},
        };

        uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

        // A view of bytes inside a file's buffer.
        struct Span
        {
            const uint8_t *data = nullptr;
            size_t size = 0;
        };

        // A picture of a TXRC: a PS2 BMP (TBMP, TZMP) or an Xbox picture, an
        // XPR (TXPR: AI, BMX XXX) or a DDS file (TDDS: DM2).
        struct Texture
        {
            std::string name; // the TXRH's, its bytes as they are
            std::string kind; // the chunk's tag
            Span data;
            int width = 0, height = 0; // a BMP's height < 0: rows top down
            int bitsPerPixel = 0;
            uint32_t xboxFormat = 0; // an Xbox picture's D3D format (a DDS's DXT3 as kXboxDxt3)
            int mipLevels = 0;
            size_t pixels = 0; // an Xbox picture's first level, from the chunk's start
            std::vector<Texture> levels; // a PS2 picture's smaller mip levels
        };

        // ---- IFF texture libraries --------------------------------------------

        // Each chunk (tag, body, size) from p to end, as the libraries nest them.
        template <typename Visit>
        void forChunks(const Bytes &d, size_t p, size_t end, Visit visit)
        {
            while (p + 8 <= end)
            {
                const uint32_t size = le32(d.data() + p + 4);
                if (!visit(std::string(reinterpret_cast<const char *>(d.data() + p), 4), p + 8, size_t(size)))
                    return;
                p += 8 + size_t(size);
            }
        }

        // Bytes from..from+size, cut at the buffer's end.
        Span slice(const Bytes &d, size_t from, size_t size)
        {
            if (from >= d.size())
                return {d.data() + d.size(), 0};
            return {d.data() + from, std::min(size, d.size() - from)};
        }

        void readXprHeader(Texture &t)
        {
            const Span x = t.data;
            if (x.size < 16 || std::memcmp(x.data + 4, "XPR0", 4) != 0)
                return;
            const uint32_t headerSize = le32(x.data + 12);
            size_t r = 16;
            while (r + 20 <= 4 + size_t(headerSize) && r + 20 <= x.size)
            {
                const uint32_t common = le32(x.data + r);
                if (common == 0xFFFFFFFF)
                    break;
                const uint32_t type = (common >> 16) & 7;
                if (type == 4) // a texture
                {
                    const uint32_t data = le32(x.data + r + 4), format = le32(x.data + r + 12), linear = le32(x.data + r + 16);
                    if (linear) // a linear texture (never DXT)
                        return;
                    t.xboxFormat = (format >> 8) & 0xFF;
                    t.mipLevels = int((format >> 16) & 15);
                    t.width = 1 << ((format >> 20) & 15);
                    t.height = 1 << ((format >> 24) & 15);
                    t.pixels = 4 + size_t(headerSize) + data;
                    return;
                }
                if (type == 3) // a palette
                    r += 12;
                else
                    return;
            }
        }

        // A TDDS: u32 0, then a DDS file (DXT1, DXT3 or 32-bit; its mips
        // after the first level, as a DDS file keeps them).
        void readDdsHeader(Texture &t)
        {
            const Span x = t.data;
            if (x.size < 8 + kDdsHeaderBytes || std::memcmp(x.data + 4, "DDS ", 4) != 0)
                return;
            const uint8_t *header = x.data + 8;
            const uint32_t flags = le32(header + 4);
            t.height = int(le32(header + 8));
            t.width = int(le32(header + 12));
            const uint32_t mips = le32(header + 24);
            t.mipLevels = (flags & kDdsHasMipCount) && mips > 0 ? int(mips) : 1;
            const uint32_t pixelFlags = le32(header + 76);
            if ((pixelFlags & 4) && std::memcmp(header + 80, "DXT3", 4) == 0) // DDPF_FOURCC
                t.xboxFormat = kXboxDxt3;
            t.pixels = 8 + kDdsHeaderBytes;
        }

        // A TXRC: the TXRH, then the picture; a PS2 picture may be followed by
        // its smaller mip levels (TBMPs of half the size).
        std::optional<Texture> readTxrc(const Bytes &d, size_t body, size_t size)
        {
            std::string name;
            std::vector<Texture> pictures;
            forChunks(d, body, body + size, [&](const std::string &tag, size_t at, size_t length)
            {
                if (tag == "TXRH")
                {
                    const Span s = slice(d, at + 0x14, length >= 0x14 ? length - 0x14 : 0);
                    name.assign(reinterpret_cast<const char *>(s.data), strnlen(reinterpret_cast<const char *>(s.data), s.size));
                }
                else if (tag == "TBMP" || tag == "TZMP" || tag == "TXPR" || tag == "TDDS" || tag == "TTGA")
                {
                    Texture t;
                    t.kind = tag;
                    t.data = slice(d, at, length);
                    pictures.push_back(std::move(t));
                }
                return true;
            });
            if (pictures.empty())
                return std::nullopt;
            for (Texture &t : pictures)
            {
                t.name = name;
                if ((t.kind == "TBMP" || t.kind == "TZMP") && t.data.size >= 54)
                {
                    t.width = int32_t(le32(t.data.data + 18));
                    t.height = int32_t(le32(t.data.data + 22));
                    t.bitsPerPixel = le16(t.data.data + 28);
                }
                else if (t.kind == "TXPR")
                    readXprHeader(t);
                else if (t.kind == "TDDS")
                    readDdsHeader(t);
            }
            Texture base = pictures[0];
            for (size_t i = 1; i < pictures.size(); ++i)
                if (pictures[i].kind == base.kind && pictures[i].bitsPerPixel == base.bitsPerPixel)
                    base.levels.push_back(pictures[i]);
            return base;
        }

        bool printableTag(const std::string &tag)
        {
            return std::all_of(tag.begin(), tag.end(), [](char c) { return c >= 32 && c < 127; });
        }

        // Every TXRC in a library, in order (the libraries' container chunks
        // looked into, a FORM's type skipped unless it is a chunk of its own).
        // A ZLZO chunk (BMX XXX's Xbox libraries: u32 the unpacked size, then
        // an LZO1X stream of one chunk, a TXRC) is unpacked into `unpacked`,
        // which keeps the bytes the textures point into.
        void collectTxrc(const Bytes &d, size_t p, size_t end, std::vector<Texture> &out, std::deque<Bytes> &unpacked)
        {
            forChunks(d, p, end, [&](const std::string &tag, size_t body, size_t size)
            {
                if (body + size > end || !printableTag(tag))
                    return false;
                if (tag == "TXRC")
                {
                    if (std::optional<Texture> t = readTxrc(d, body, size))
                        out.push_back(std::move(*t));
                }
                else if (tag == "FORM")
                {
                    const bool untyped = body + 4 <= d.size() && (std::memcmp(d.data() + body, "TXRC", 4) == 0 ||
                                                                  std::memcmp(d.data() + body, "ZLZO", 4) == 0);
                    collectTxrc(d, untyped ? body : body + 4, body + size, out, unpacked);
                }
                else if (tag == "ZLZO" && size >= 4)
                {
                    Bytes &chunk = unpacked.emplace_back();
                    if (lzo1xDecompress(d.data() + body + 4, size - 4, le32(d.data() + body), chunk))
                        collectTxrc(chunk, 0, chunk.size(), out, unpacked);
                }
                else if (tag == "ULIB" || tag == "UAST" || tag == "UMTL" || tag == "UFNT" || tag == "LIST" ||
                         tag == "CAT " || tag == "FMDL" || tag == "MODL")
                    collectTxrc(d, body, body + size, out, unpacked);
                return true;
            });
        }

        // Every TXRC of a file: a library, or a ZDIR archive's (a .ZAL:
        // "DIDF" with the entry count at 0x10, then the GRID of name hashes,
        // the DLEN of sizes (the top four bits flags) and the OFST of
        // offsets) entries, each a library.
        void collectFile(const Bytes &d, std::vector<Texture> &out, std::deque<Bytes> &unpacked)
        {
            if (d.size() < 0x18 || std::memcmp(d.data(), "ZDIR", 4) != 0 || std::memcmp(d.data() + 8, "DIDF", 4) != 0)
            {
                collectTxrc(d, 0, d.size(), out, unpacked);
                return;
            }
            const size_t count = le32(d.data() + 0x10);
            const size_t grid = 0x18, lengths = grid + 8 + 4 * count, offsets = lengths + 8 + 4 * count;
            if (offsets + 8 + 4 * count > d.size() || std::memcmp(d.data() + grid, "GRID", 4) != 0 ||
                std::memcmp(d.data() + lengths, "DLEN", 4) != 0 || std::memcmp(d.data() + offsets, "OFST", 4) != 0)
                return;
            for (size_t i = 0; i < count; ++i)
            {
                const size_t length = le32(d.data() + lengths + 8 + 4 * i) & 0x0FFFFFFFu;
                const size_t at = le32(d.data() + offsets + 8 + 4 * i);
                if (at <= d.size() && length <= d.size() - at)
                    collectTxrc(d, at, at + length, out, unpacked);
            }
        }

        std::vector<size_t> dxtLevelSizes(int w, int h, int mips)
        {
            std::vector<size_t> out;
            for (int i = 0; i < mips; ++i)
            {
                out.push_back(size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * 16);
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
            }
            return out;
        }

        // ---- pictures ---------------------------------------------------------

        using Rgb = std::array<int, 3>;

        // A BMP's indices, one byte a texel, rows top down (4-bit indices
        // unpacked, the first texel in a byte's high nibble), and its palette.
        struct Indexed
        {
            std::vector<Bytes> rows;
            std::vector<Rgb> palette;
        };

        Indexed bmpRows(const Texture &t)
        {
            const uint8_t *d = t.data.data;
            const uint32_t offset = le32(d + 10), header = le32(d + 14);
            const int w = t.width, h = std::abs(t.height), bpp = t.bitsPerPixel;
            const size_t stride = size_t((w * bpp + 31) / 32) * 4;
            uint32_t colours = le32(d + 14 + 32);
            if (colours == 0)
                colours = 1u << bpp;
            Indexed out;
            for (uint32_t i = 0; i < colours; ++i)
            {
                const uint8_t *c = d + 14 + header + 4 * i;
                out.palette.push_back({c[2], c[1], c[0]});
            }
            while (out.palette.size() < 256)
                out.palette.push_back({0, 0, 0});
            const size_t rowBytes = size_t((w * bpp + 7) / 8);
            for (int y = 0; y < h; ++y)
            {
                const size_t at = offset + size_t(y) * stride;
                const size_t n = at >= t.data.size ? 0 : std::min(rowBytes, t.data.size - at);
                Bytes bytes(d + at, d + at + n);
                if (bpp == 4)
                {
                    Bytes unpacked(size_t(w), 0);
                    for (int x = 0; x < w; ++x)
                    {
                        const size_t byte = size_t(x / 2);
                        if (byte < bytes.size())
                            unpacked[size_t(x)] = (x % 2) == 0 ? bytes[byte] >> 4 : bytes[byte] & 15;
                    }
                    bytes = std::move(unpacked);
                }
                out.rows.push_back(std::move(bytes));
            }
            if (t.height > 0)
                std::reverse(out.rows.begin(), out.rows.end());
            return out;
        }

        uint64_t fnv1a64(const std::vector<Bytes> &rows)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const Bytes &row : rows)
                for (uint8_t c : row)
                    h = (h ^ c) * 0x100000001B3ull;
            return h;
        }

        bool oneIndex(const std::vector<Bytes> &rows)
        {
            std::optional<uint8_t> first;
            for (const Bytes &row : rows)
                for (uint8_t c : row)
                {
                    if (!first)
                        first = c;
                    else if (c != *first)
                        return false;
                }
            return true;
        }

        // The PS2 textures' colour key (magenta): transparent once loaded.
        bool isColourKey(const Rgb &c) { return c[0] >= 248 && c[1] <= 8 && c[2] >= 248; }

        // A thumbnail cell: the mean RGB of its opaque samples (none: no
        // opaque one) and the share of opaque samples.
        struct Cell
        {
            std::optional<std::array<double, 3>> colour;
            double opaqueShare = 0.0;
        };
        using Thumb = std::vector<Cell>;

        Cell cellOf(const std::vector<std::pair<Rgb, bool>> &samples)
        {
            long sum[3] = {0, 0, 0};
            long opaque = 0;
            for (const auto &[c, isOpaque] : samples)
                if (isOpaque)
                {
                    for (int i = 0; i < 3; ++i)
                        sum[i] += c[i];
                    ++opaque;
                }
            if (opaque == 0)
                return {};
            Cell cell;
            cell.colour = std::array<double, 3>{double(sum[0]) / double(opaque), double(sum[1]) / double(opaque),
                                                double(sum[2]) / double(opaque)};
            cell.opaqueShare = double(opaque) / double(samples.size());
            return cell;
        }

        Thumb ps2Thumb(const Indexed &picture)
        {
            const int h = int(picture.rows.size()), w = int(picture.rows[0].size());
            Thumb out;
            for (int gy = 0; gy < kThumbSide; ++gy)
                for (int gx = 0; gx < kThumbSide; ++gx)
                {
                    std::vector<std::pair<Rgb, bool>> samples;
                    for (int sy = 0; sy < 4; ++sy)
                    {
                        const Bytes &row = picture.rows[size_t(std::min(h - 1, (gy * 4 + sy) * h / (kThumbSide * 4)))];
                        for (int sx = 0; sx < 4; ++sx)
                        {
                            const Rgb &c = picture.palette[row[size_t(std::min(w - 1, (gx * 4 + sx) * w / (kThumbSide * 4)))]];
                            samples.push_back({c, !isColourKey(c)});
                        }
                    }
                    out.push_back(cellOf(samples));
                }
            return out;
        }

        Rgb rgb565(uint16_t c)
        {
            const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
        }

        // A DXT3 block's four colours.
        std::array<Rgb, 4> dxtColours(const uint8_t *block)
        {
            const Rgb p0 = rgb565(le16(block + 8)), p1 = rgb565(le16(block + 10));
            std::array<Rgb, 4> out = {p0, p1, Rgb{}, Rgb{}};
            for (int i = 0; i < 3; ++i)
            {
                out[2][i] = (2 * p0[i] + p1[i]) / 3;
                out[3][i] = (p0[i] + 2 * p1[i]) / 3;
            }
            return out;
        }

        // (RGB, opaque) rows of a DXT3 level (opaque: alpha at least 8/15).
        std::vector<std::vector<std::pair<Rgb, bool>>> dxt3Decode(const uint8_t *raw, int w, int h)
        {
            const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
            std::vector<std::vector<std::pair<Rgb, bool>>> image(size_t(bh * 4),
                                                                 std::vector<std::pair<Rgb, bool>>(size_t(bw * 4)));
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx)
                {
                    const uint8_t *block = raw + size_t(by * bw + bx) * 16;
                    const uint64_t alpha = le64(block);
                    const uint32_t bits = le32(block + 12);
                    const std::array<Rgb, 4> colours = dxtColours(block);
                    for (int i = 0; i < 16; ++i)
                        image[size_t(by * 4 + i / 4)][size_t(bx * 4 + i % 4)] = {colours[(bits >> (2 * i)) & 3],
                                                                                ((alpha >> (4 * i)) & 15) >= 8};
                }
            image.resize(size_t(h));
            for (auto &row : image)
                row.resize(size_t(w));
            return image;
        }

        Thumb xboxThumb(const Texture &t)
        {
            const std::vector<size_t> sizes = dxtLevelSizes(t.width, t.height, t.mipLevels);
            int level = 0;
            size_t at = t.pixels;
            int w = t.width, h = t.height;
            // The smallest level at least 8 x 8.
            while (level + 1 < t.mipLevels && std::min(w, h) / 2 >= kThumbSide)
            {
                at += sizes[size_t(level)];
                ++level;
                w /= 2;
                h /= 2;
            }
            const auto image = dxt3Decode(t.data.data + at, w, h);
            Thumb out;
            for (int gy = 0; gy < kThumbSide; ++gy)
                for (int gx = 0; gx < kThumbSide; ++gx)
                {
                    std::vector<std::pair<Rgb, bool>> samples;
                    for (int y = gy * h / kThumbSide; y < std::max(gy * h / kThumbSide + 1, (gy + 1) * h / kThumbSide); ++y)
                        for (int x = gx * w / kThumbSide; x < std::max(gx * w / kThumbSide + 1, (gx + 1) * w / kThumbSide); ++x)
                            samples.push_back(image[size_t(y)][size_t(x)]);
                    out.push_back(cellOf(samples));
                }
            return out;
        }

        // Texels of a DXT3 level that are opaque and magenta (the PS2 colour
        // key left in some Xbox pictures: pink where the PS2 is transparent).
        int opaqueMagenta(const uint8_t *raw, int w, int h)
        {
            int count = 0;
            const size_t bytes = size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * 16;
            for (size_t o = 0; o < bytes; o += 16)
            {
                const std::array<Rgb, 4> colours = dxtColours(raw + o);
                bool keyed[4];
                bool any = false;
                for (int i = 0; i < 4; ++i)
                {
                    // magenta after DXT rounding
                    keyed[i] = colours[i][0] >= 192 && colours[i][1] <= 64 && colours[i][2] >= 192;
                    any = any || keyed[i];
                }
                if (!any)
                    continue;
                const uint64_t alpha = le64(raw + o);
                const uint32_t bits = le32(raw + o + 12);
                for (int i = 0; i < 16; ++i)
                    if (keyed[(bits >> (2 * i)) & 3] && ((alpha >> (4 * i)) & 15) >= 8)
                        ++count;
            }
            return count;
        }

        // ---- pictures made small, to pair by picture -------------------------

        // (RGB, opaque) rows of a picture.
        using Image = std::vector<std::vector<std::pair<Rgb, bool>>>;

        // A PS2 picture as (RGB, opaque) rows: its colour key transparent.
        Image ps2Image(const Indexed &picture)
        {
            Image out;
            for (const Bytes &row : picture.rows)
            {
                std::vector<std::pair<Rgb, bool>> texels;
                for (uint8_t index : row)
                {
                    const Rgb &c = picture.palette[index];
                    texels.push_back({c, !isColourKey(c)});
                }
                out.push_back(std::move(texels));
            }
            return out;
        }

        // A picture box-averaged to w x h cells (each cell the mean of the
        // texels it covers, as Cell holds it).
        Thumb boxAverage(const Image &image, int w, int h)
        {
            const int sh = int(image.size()), sw = sh ? int(image[0].size()) : 0;
            Thumb out;
            for (int gy = 0; gy < h; ++gy)
                for (int gx = 0; gx < w; ++gx)
                {
                    std::vector<std::pair<Rgb, bool>> samples;
                    for (int y = gy * sh / h; y < std::max(gy * sh / h + 1, (gy + 1) * sh / h); ++y)
                        for (int x = gx * sw / w; x < std::max(gx * sw / w + 1, (gx + 1) * sw / w); ++x)
                            samples.push_back(image[size_t(y)][size_t(x)]);
                    out.push_back(cellOf(samples));
                }
            return out;
        }

        // (mean RGB difference where both are opaque, mean difference of the opaque share)
        std::pair<double, double> thumbDiff(const Thumb &a, const Thumb &b)
        {
            double colourSum = 0.0;
            int colourCells = 0;
            double cover = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].colour && b[i].colour)
                {
                    double channels = 0.0;
                    for (int c = 0; c < 3; ++c)
                        channels += std::fabs((*a[i].colour)[size_t(c)] - (*b[i].colour)[size_t(c)]);
                    colourSum += channels / 3.0;
                    ++colourCells;
                }
                cover += std::fabs(a[i].opaqueShare - b[i].opaqueShare);
            }
            return {colourCells ? colourSum / double(colourCells) : 0.0, cover / double(a.size())};
        }

        // Latin-1 bytes as UTF-8 (the report's names).
        std::string utf8OfLatin1(const std::string &text)
        {
            std::string out;
            for (unsigned char c : text)
            {
                if (c < 0x80)
                    out += char(c);
                else
                {
                    out += char(0xC0 | (c >> 6));
                    out += char(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        template <typename... Args>
        std::string format(const char *pattern, Args... args)
        {
            char buffer[512];
            std::snprintf(buffer, sizeof buffer, pattern, args...);
            return buffer;
        }

        // ---- matching ---------------------------------------------------------

        bool isDigit(char c) { return c >= '0' && c <= '9'; }

        // The name a texture is paired by: a skater's IDn slot ("slot1"), else
        // its lower-case name without size parts ("_256") and with single "_".
        std::string pairingKey(const std::string &name, bool slots)
        {
            const std::string n = lowerAscii(name);
            if (slots)
                for (size_t i = 0; i + 2 < n.size(); ++i)
                    if (n.compare(i, 2, "id") == 0)
                    {
                        if (isDigit(n[i + 2]))
                            return std::string("slot") + n[i + 2];
                        if (n[i + 2] == '_' && i + 3 < n.size() && isDigit(n[i + 3]))
                            return std::string("slot") + n[i + 3];
                    }
            std::string cut;
            static const char *const kSizes[] = {"64", "128", "256", "512", "1024"};
            for (size_t i = 0; i < n.size();)
            {
                bool removed = false;
                if (n[i] == '_')
                    for (const char *size : kSizes)
                    {
                        const size_t len = std::strlen(size);
                        const size_t end = i + 1 + len;
                        if (n.compare(i + 1, len, size) == 0 && end <= n.size() && (end == n.size() || n[end] == '_'))
                        {
                            i = end;
                            removed = true;
                            break;
                        }
                    }
                if (!removed)
                    cut += n[i++];
            }
            std::string out;
            for (char c : cut)
                if (!(c == '_' && !out.empty() && out.back() == '_'))
                    out += c;
            return out;
        }

        // An Xbox-only crop of a PS2 texture drawn with REGION_CLAMP (a name
        // ending in _ and 6 to 8 hex digits): never a pair.
        bool isCrop(const std::string &name)
        {
            const size_t underscore = name.rfind('_');
            if (underscore == std::string::npos)
                return false;
            const size_t digits = name.size() - underscore - 1;
            if (digits < 6 || digits > 8)
                return false;
            return std::all_of(name.begin() + long(underscore) + 1, name.end(),
                               [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        }

        struct Pair
        {
            const Texture *ps2;
            const Texture *xbox;
            std::string byPicture = {}; // paired by picture: the report's line (empty: by name)
        };

        // The pairs of one file: each PS2 texture with the Xbox one of its
        // name (or its renamed one), each Xbox texture used once.
        std::vector<Pair> pairsIn(const std::string &relative, const std::vector<Texture> &ps2,
                                  const std::vector<Texture> &xbox)
        {
            const bool slots = relative.compare(0, 8, "skaters/") == 0;
            std::map<std::string, const Texture *> byName, byKey;
            for (const Texture &x : xbox)
                byName[lowerAscii(x.name)] = &x;
            for (const Texture &x : xbox)
                if (!isCrop(x.name))
                    byKey.emplace(pairingKey(x.name, slots), &x);
            std::vector<Pair> out;
            std::set<const Texture *> used;
            for (const Texture &p : ps2)
            {
                const Texture *x = nullptr;
                const auto renamed = kRenamed.find({relative, lowerAscii(p.name)});
                if (renamed != kRenamed.end())
                {
                    const auto it = byName.find(renamed->second);
                    x = it == byName.end() ? nullptr : it->second;
                }
                else
                {
                    const auto it = byKey.find(pairingKey(p.name, slots));
                    x = it == byKey.end() ? nullptr : it->second;
                }
                if (x && used.insert(x).second)
                    out.push_back({&p, x});
            }
            return out;
        }

        // An Xbox picture the pack can put in for a PS2 texture: DXT3 with
        // its mips, at least as many texels.
        bool canReplace(const Texture &ps2, const Texture &xbox)
        {
            return xbox.xboxFormat == kXboxDxt3 && xbox.mipLevels >= 1 &&
                   xbox.width * xbox.height >= ps2.width * std::abs(ps2.height);
        }

        // The side pictures are compared at when paired by picture: small
        // enough that DXT3's blocks and the PS2 palettes' steps average out.
        constexpr int kPictureSide = 32;

        // The PS2 textures of an archive with no Xbox picture by name, paired
        // by picture (BMX XXX's level props: their Xbox names drop the size,
        // "NDMBURNB_ID1128" -> "NDMBURNB_ID1", and a few are renamed): each
        // with the most alike of the archive's Xbox pictures that can replace
        // it and are at least its width and height, both box-averaged to at
        // most kPictureSide a side, when the mean RGB difference where both
        // are opaque is at most options.maxPictureDiff and the mean
        // difference of the opaque share at most options.maxPictureCover.
        // `rejected` gets a line for each texture left as it is, with the
        // nearest picture.
        void addPicturePairs(std::vector<Pair> &pairs, const std::vector<Texture> &ps2, const std::vector<Texture> &xbox,
                             const XboxTexturePackOptions &options, std::vector<std::string> &rejected)
        {
            // A pair by name with an Xbox picture the pack cannot put in (not
            // DXT3, or smaller) is tried by picture too.
            std::map<const Texture *, const Texture *> namedUnusable;
            for (const Pair &pair : pairs)
                if (!canReplace(*pair.ps2, *pair.xbox))
                    namedUnusable[pair.ps2] = pair.xbox;
            std::erase_if(pairs, [](const Pair &pair) { return !canReplace(*pair.ps2, *pair.xbox); });
            // The keys (size, index hash) paired by name or tried by picture:
            // a texture kept in several of the archive's entries is tried once.
            std::set<std::tuple<int, int, uint64_t>> paired;
            for (const Pair &pair : pairs)
                paired.insert({pair.ps2->width, std::abs(pair.ps2->height), fnv1a64(bmpRows(*pair.ps2).rows)});
            std::map<std::tuple<const Texture *, int, int>, Thumb> small; // each Xbox picture made small once
            for (const Texture &p : ps2)
            {
                const int pw = p.width, ph = std::abs(p.height);
                const Indexed indexed = bmpRows(p);
                // One index everywhere: no key of its own (as below).
                if (oneIndex(indexed.rows) || !paired.insert({pw, ph, fnv1a64(indexed.rows)}).second)
                    continue;
                const int w = std::min(kPictureSide, pw), h = std::min(kPictureSide, ph);
                const Thumb mine = boxAverage(ps2Image(indexed), w, h);
                const Texture *best = nullptr;
                double bestDiff = 0.0, bestCover = 0.0;
                for (const Texture &x : xbox)
                {
                    if (!canReplace(p, x) || x.width < pw || x.height < ph ||
                        x.pixels + dxtLevelSizes(x.width, x.height, 1)[0] > x.data.size)
                        continue;
                    auto it = small.find({&x, w, h});
                    if (it == small.end())
                        it = small.emplace(std::make_tuple(&x, w, h),
                                           boxAverage(dxt3Decode(x.data.data + x.pixels, x.width, x.height), w, h)).first;
                    const auto [diff, cover] = thumbDiff(mine, it->second);
                    if (!best || diff + 255.0 * cover < bestDiff + 255.0 * bestCover)
                    {
                        best = &x;
                        bestDiff = diff;
                        bestCover = cover;
                    }
                }
                const std::string pName = utf8OfLatin1(p.name);
                std::string why = "no Xbox picture alike";
                if (const auto it = namedUnusable.find(&p); it != namedUnusable.end())
                    why = format("its Xbox picture by name is %dx%d in D3D format 0x%02X, none other alike", it->second->width,
                                 it->second->height, unsigned(it->second->xboxFormat));
                if (!best)
                {
                    rejected.push_back(format("%s %dx%d: %s", pName.c_str(), pw, ph, why.c_str()));
                    continue;
                }
                const std::string xbox = format("%s %dx%d (colour %.1f, cover %.2f)", utf8OfLatin1(best->name).c_str(),
                                                best->width, best->height, bestDiff, bestCover);
                if (bestDiff > options.maxPictureDiff || bestCover > options.maxPictureCover)
                {
                    rejected.push_back(format("%s %dx%d: %s, nearest %s", pName.c_str(), pw, ph, why.c_str(), xbox.c_str()));
                    continue;
                }
                pairs.push_back({&p, best, format("%s %dx%d -> %s", pName.c_str(), pw, ph, xbox.c_str())});
            }
        }

        // ---- the pack ---------------------------------------------------------

        struct PackEntry
        {
            int ps2Width, ps2Height, ps2Bits;
            uint64_t indexHash;
            int width, height, mipLevels;
            size_t payload; // index into the pictures
            std::string name;
        };

        void put16(Bytes &out, uint16_t v)
        {
            out.push_back(uint8_t(v));
            out.push_back(uint8_t(v >> 8));
        }
        void put32(Bytes &out, uint32_t v)
        {
            put16(out, uint16_t(v));
            put16(out, uint16_t(v >> 16));
        }

        // runtime/ps2_texture_replace.h's pack: "PXTP", version 1, the entry
        // count; 64-byte entries; each picture once, at a 16-byte boundary.
        Bytes packBytes(const std::vector<PackEntry> &entries, const std::vector<Span> &pictures)
        {
            Bytes out;
            out.insert(out.end(), {'P', 'X', 'T', 'P'});
            put32(out, 1);
            put32(out, uint32_t(entries.size()));
            put32(out, 0);
            std::map<size_t, size_t> placed;
            std::vector<std::pair<size_t, size_t>> blobs; // (offset, picture)
            size_t at = 16 + 64 * entries.size();
            for (const PackEntry &e : entries)
            {
                auto it = placed.find(e.payload);
                if (it == placed.end())
                {
                    at = (at + 15) & ~size_t(15);
                    it = placed.emplace(e.payload, at).first;
                    blobs.push_back({at, e.payload});
                    at += pictures[e.payload].size;
                }
                put16(out, uint16_t(e.ps2Width));
                put16(out, uint16_t(e.ps2Height));
                out.push_back(uint8_t(e.ps2Bits));
                out.push_back(0);
                put16(out, 0);
                put32(out, uint32_t(e.indexHash));
                put32(out, uint32_t(e.indexHash >> 32));
                put16(out, uint16_t(e.width));
                put16(out, uint16_t(e.height));
                out.push_back(uint8_t(e.mipLevels));
                out.push_back(kPackFormatBc2);
                put16(out, 0);
                put32(out, uint32_t(it->second));
                put32(out, uint32_t(pictures[e.payload].size));
                char name[32] = {};
                std::memcpy(name, e.name.data(), std::min<size_t>(e.name.size(), 31));
                out.insert(out.end(), name, name + 32);
            }
            for (const auto &[offset, picture] : blobs)
            {
                out.resize(offset, 0);
                out.insert(out.end(), pictures[picture].data, pictures[picture].data + pictures[picture].size);
            }
            return out;
        }

        bool endsWithAny(const std::string &path, std::initializer_list<const char *> endings)
        {
            const std::string low = lowerAscii(path);
            for (const char *ending : endings)
            {
                const size_t n = std::strlen(ending);
                if (low.size() >= n && low.compare(low.size() - n, n, ending) == 0)
                    return true;
            }
            return false;
        }

        // The Xbox Bink file of each PS2 movie (MOVIES/<path>.PSS|PSM ->
        // movies/ASSETS/MOVIES/<path>.PSS.bik, any case).
        bool copyMovies(const SourceFiles &ps2, const SourceFiles &xbox, const std::filesystem::path &out,
                        std::vector<std::string> &log, std::string *error)
        {
            std::vector<std::string> missing;
            size_t copied = 0;
            uint64_t bytes = 0;
            for (const std::string &file : ps2.files())
            {
                if (lowerAscii(file).compare(0, 7, "movies/") != 0 || !endsWithAny(file, {".pss", ".psm"}))
                    continue;
                const std::string ps2Path = "ASSETS/" + upperAscii(file);
                const std::optional<std::string> bink = xbox.find(file.substr(0, file.size() - 4) + ".bik");
                if (!bink)
                {
                    missing.push_back(ps2Path);
                    continue;
                }
                Bytes data;
                if (!xbox.read(*bink, data))
                {
                    *error = "Could not read the Xbox movie " + *bink + ".";
                    return false;
                }
                if (!writeFile(out / "movies" / (ps2Path + ".bik"), data, error))
                    return false;
                ++copied;
                bytes += data.size();
            }
            std::string line = "xbox_pack: " + std::to_string(copied) + " Xbox movies (" + std::to_string(bytes >> 20) + " MB)";
            if (!missing.empty())
            {
                line += "; none for ";
                for (size_t i = 0; i < missing.size(); ++i)
                    line += (i ? ", " : "") + missing[i];
                line += " (PS2 movie kept)";
            }
            log.push_back(line);
            return true;
        }
    }

    bool makeXboxTexturePack(const SourceFiles &ps2, const SourceFiles &xbox, const std::filesystem::path &out,
                             const XboxTexturePackOptions &options, std::vector<std::string> &log, std::string *error)
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::string> report;
        long files = 0, ps2Count = 0, packed = 0, keys = 0, rejectedCount = 0, packBytesTotal = 0, byPicture = 0;
        long long ps2Texels = 0, xboxTexels = 0;
        for (const std::string &xboxPath : xbox.files())
        {
            if (!endsWithAny(xboxPath, {".txr", ".zit"}) && !(options.archives && endsWithAny(xboxPath, {".zal"})))
                continue;
            const std::string relative = lowerAscii(xboxPath);
            const std::optional<std::string> ps2Path = ps2.find(relative);
            if (!ps2Path)
                continue;
            Bytes ps2Data, xboxData;
            if (!ps2.read(*ps2Path, ps2Data) || !xbox.read(xboxPath, xboxData))
            {
                *error = "Could not read " + relative + ".";
                return false;
            }
            std::vector<Texture> all, ps2Textures, xboxTextures;
            std::deque<Bytes> unpacked;
            collectFile(ps2Data, all, unpacked);
            for (Texture &t : all)
                if ((t.kind == "TBMP" || t.kind == "TZMP") && (t.bitsPerPixel == 4 || t.bitsPerPixel == 8))
                    ps2Textures.push_back(std::move(t));
            all.clear();
            collectFile(xboxData, all, unpacked);
            for (Texture &t : all)
                if (t.kind == "TXPR" || t.kind == "TDDS")
                    xboxTextures.push_back(std::move(t));

            std::vector<PackEntry> entries;
            std::vector<Span> pictures;
            std::vector<std::string> rejected, byPictureLines;
            std::vector<Pair> pairs = pairsIn(relative, ps2Textures, xboxTextures);
            if (endsWithAny(relative, {".zal"}))
                addPicturePairs(pairs, ps2Textures, xboxTextures, options, rejected);
            std::set<std::tuple<int, int, uint64_t>> seen;
            std::map<const Texture *, size_t> pictureOf; // an Xbox picture paired by picture with several PS2 ones: once
            long long texels[2] = {0, 0};
            for (const Pair &pair : pairs)
            {
                const Texture &p = *pair.ps2, &x = *pair.xbox;
                const int pw = p.width, ph = std::abs(p.height);
                if (!canReplace(p, x))
                    continue;
                const std::vector<size_t> sizes = dxtLevelSizes(x.width, x.height, x.mipLevels);
                size_t chain = 0;
                for (size_t s : sizes)
                    chain += s;
                const Span payload = x.pixels <= x.data.size
                                         ? Span{x.data.data + x.pixels, std::min(chain, x.data.size - x.pixels)}
                                         : Span{x.data.data + x.data.size, 0};
                const std::string pName = utf8OfLatin1(p.name), xName = utf8OfLatin1(x.name);
                if (payload.size != chain)
                {
                    rejected.push_back(pName + " -> " + xName + ": short mip chain");
                    continue;
                }
                const Indexed indexed = bmpRows(p);
                if (oneIndex(indexed.rows))
                {
                    // One index everywhere: its key (size and index hash) is
                    // any flat texture's of that size, the game's run-time
                    // ones too ("Procedural:<n>", the juice bar).
                    rejected.push_back(format("%s %dx%d: one colour, no key of its own", pName.c_str(), pw, ph));
                    continue;
                }
                const auto [diff, cover] = thumbDiff(ps2Thumb(indexed), xboxThumb(x));
                const int pink = opaqueMagenta(payload.data, x.width, x.height);
                if (pink > std::max(16, x.width * x.height / 2048))
                {
                    rejected.push_back(format("%s %dx%d -> %s %dx%d: %d opaque magenta texels", pName.c_str(), pw, ph,
                                              xName.c_str(), x.width, x.height, pink));
                    continue;
                }
                if (diff > options.maxDiff || cover > options.maxCover)
                {
                    rejected.push_back(format("%s %dx%d -> %s %dx%d: pictures differ (colour %.1f, cover %.2f)",
                                              pName.c_str(), pw, ph, xName.c_str(), x.width, x.height, diff, cover));
                    continue;
                }
                const uint64_t hash = fnv1a64(indexed.rows);
                if (!seen.insert({pw, ph, hash}).second)
                    continue;
                const auto placed = pictureOf.emplace(&x, pictures.size());
                const size_t picture = placed.first->second;
                if (placed.second)
                    pictures.push_back(payload);
                if (!pair.byPicture.empty())
                    byPictureLines.push_back(pair.byPicture);
                entries.push_back({pw, ph, p.bitsPerPixel, hash, x.width, x.height, x.mipLevels, picture, p.name});
                // The PS2 mip levels show the same picture (the GS samples
                // them with the same normalized coordinates).
                for (const Texture &level : p.levels)
                {
                    const Indexed levelRows = bmpRows(level);
                    const std::tuple<int, int, uint64_t> key = {level.width, std::abs(level.height), fnv1a64(levelRows.rows)};
                    if (!seen.count(key) && !oneIndex(levelRows.rows))
                    {
                        seen.insert(key);
                        entries.push_back({std::get<0>(key), std::get<1>(key), level.bitsPerPixel, std::get<2>(key),
                                           x.width, x.height, x.mipLevels, picture, p.name});
                    }
                }
                texels[0] += (long long)pw * ph;
                texels[1] += (long long)x.width * x.height;
            }
            if (!entries.empty())
            {
                const std::string ps2Relative = "ASSETS/" + upperAscii(*ps2Path);
                const Bytes data = packBytes(entries, pictures);
                if (!writeFile(out / "textures" / (ps2Relative + ".xtp"), data, error))
                    return false;
                ++files;
                packBytesTotal += long(data.size());
                std::string padded = ps2Relative;
                if (padded.size() < 40)
                    padded.resize(40, ' ');
                report.push_back(format("%s %5zu %5zu %5zu %4zu %6.2f %8zu", padded.c_str(), ps2Textures.size(),
                                        pictures.size(), entries.size(), rejected.size(),
                                        double(texels[1]) / double(std::max(1ll, texels[0])), data.size() >> 10));
            }
            for (const std::string &line : byPictureLines)
                report.push_back("    by picture: " + line);
            for (const std::string &r : rejected)
                report.push_back("    rejected: " + r);
            ps2Count += long(ps2Textures.size());
            packed += long(pictures.size());
            keys += long(entries.size());
            rejectedCount += long(rejected.size());
            byPicture += long(byPictureLines.size());
            ps2Texels += texels[0];
            xboxTexels += texels[1];
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // (AI's, with no archives, as tools/xbox_pack.py's)
        const std::string byPictureNote = options.archives ? format(", %ld of them paired by picture", byPicture) : "";
        const std::string summary = format(
            "%ld Xbox textures (%ld keys) for %ld PS2 files (%ld indexed PS2 textures in the paired files%s, %ld pairs "
            "rejected), %.2fx the texels, %ld MB, %.1f s",
            packed, keys, files, ps2Count, byPictureNote.c_str(), rejectedCount,
            double(xboxTexels) / double(std::max(1ll, ps2Texels)), packBytesTotal >> 20, seconds);
        std::string text = format("%-40s %5s %5s %5s %4s %6s %8s", "PS2 file", "ps2#", "xbox", "keys", "rej", "texels", "KB");
        text += "\n(ps2#: indexed PS2 textures in the file; xbox: replaced by an Xbox picture; keys: with the PS2 mip "
                "levels; texels: Xbox / PS2 texels of the replaced ones)\n";
        for (const std::string &line : report)
            text += line + "\n";
        text += "\n" + summary + "\n";
        if (!writeFile(out / "textures" / "report.txt", text, error))
            return false;
        log.push_back("xbox_pack: " + summary);
        return !options.movies || copyMovies(ps2, xbox, out, log, error);
    }
}
