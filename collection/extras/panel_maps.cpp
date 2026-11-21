// The bottom panel's files (collection/extras/panel_maps.h): the entry
// point, and the readers and writers the three games' makers share.
#include "panel_maps_parts.h"

#include <cstdarg>
#include <cstdio>

namespace zx::app::extras
{
    bool makePanelMaps(PanelGame game, const SourceFiles &disc, const PanelExtras &extras,
                       const std::filesystem::path &out, std::vector<std::string> &log, std::string *error)
    {
        try
        {
            switch (game)
            {
            case PanelGame::AggressiveInline:
                panel_maps::makeAggressiveInlineMaps(disc, extras, out, log);
                break;
            case PanelGame::DaveMirra2:
                panel_maps::makeDaveMirraMaps(disc, extras, out, log);
                break;
            case PanelGame::BmxXxx:
                panel_maps::makeBmxXxxMaps(disc, extras, out, log);
                break;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            // panel_maps::PanelError, dm2::Dm2Error and an allocation failure alike.
            if (error)
                *error = std::string("The panel's maps could not be made: ") + e.what() + ".";
            return false;
        }
    }

    bool makeMapPanelFile(PanelGame game, const SourceFiles &files, const std::string &folder, const std::string &title, Bytes &out,
                          std::vector<std::string> &log, std::string *error)
    {
        try
        {
            switch (game)
            {
            case PanelGame::AggressiveInline:
                out = panel_maps::aggressiveInlineMapFile(files, folder, title, log);
                break;
            case PanelGame::DaveMirra2:
                out = panel_maps::daveMirraMapFile(files, folder, title, log);
                break;
            case PanelGame::BmxXxx:
                out = panel_maps::bmxXxxMapFile(files, folder, title, log);
                break;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = std::string("the map's panel file could not be made: ") + e.what();
            return false;
        }
    }
}

namespace zx::app::extras::panel_maps
{
    double pythonSum(const double *values, size_t count)
    {
        if (count == 0)
            return 0.0;
        // The integer start plus the first float: a float (-0.0 becomes 0.0).
        double total = 0.0 + values[0];
        double compensation = 0.0;
        for (size_t i = 1; i < count; ++i)
        {
            const double x = values[i];
            const double t = total + x;
            if (std::fabs(total) >= std::fabs(x))
                compensation += (total - t) + x;
            else
                compensation += (x - t) + total;
            total = t;
        }
        if (compensation != 0.0 && std::isfinite(compensation))
            total += compensation;
        return total;
    }

    // ---- bytes ----------------------------------------------------------------

    uint16_t ByteView::u16(size_t at) const
    {
        if (at + 2 > size || at + 2 < at)
            throw PanelError("a field past the end of a record");
        return uint16_t(data[at] | (data[at + 1] << 8));
    }

    uint32_t ByteView::u32(size_t at) const
    {
        if (at + 4 > size || at + 4 < at)
            throw PanelError("a field past the end of a record");
        return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
               (uint32_t(data[at + 3]) << 24);
    }

    double ByteView::f32(size_t at) const
    {
        const uint32_t bits = u32(at);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    ByteView ByteView::slice(size_t from, size_t to) const
    {
        from = std::min(from, size);
        to = std::max(from, std::min(to, size));
        return ByteView(data + from, to - from);
    }

    std::string ByteView::text(size_t from, size_t to) const
    {
        const ByteView s = slice(from, to);
        const uint8_t *end = static_cast<const uint8_t *>(std::memchr(s.data, 0, s.size));
        return std::string(reinterpret_cast<const char *>(s.data), end ? size_t(end - s.data) : s.size);
    }

    std::string ByteView::zeroEnded(size_t at) const
    {
        const uint8_t *end = at < size ? static_cast<const uint8_t *>(std::memchr(data + at, 0, size - at)) : nullptr;
        if (!end)
            throw PanelError("a text without its end");
        return std::string(reinterpret_cast<const char *>(data + at), size_t(end - (data + at)));
    }

    namespace
    {
        void putChecked(Bytes &out, int64_t v, int64_t lowest, int64_t highest, int bytes)
        {
            if (v < lowest || v > highest)
                throw PanelError("a value too large for its field (" + std::to_string(v) + ")");
            const uint64_t u = uint64_t(v);
            for (int i = 0; i < bytes; ++i)
                out.push_back(uint8_t(u >> (8 * i)));
        }
    }

    void putU8(Bytes &out, int64_t v) { putChecked(out, v, 0, 0xFF, 1); }
    void putU16(Bytes &out, int64_t v) { putChecked(out, v, 0, 0xFFFF, 2); }
    void putU32(Bytes &out, int64_t v) { putChecked(out, v, 0, 0xFFFFFFFFll, 4); }
    void putS32(Bytes &out, int64_t v) { putChecked(out, v, -0x80000000ll, 0x7FFFFFFF, 4); }

    void putF32(Bytes &out, double v)
    {
        const float f = float(v);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        putU32(out, bits);
    }

    void putBytes(Bytes &out, const Bytes &more) { out.insert(out.end(), more.begin(), more.end()); }
    void putText(Bytes &out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

    void putPadded(Bytes &out, std::string_view text, size_t keep, size_t size)
    {
        text = text.substr(0, keep);
        putText(out, text);
        for (size_t i = text.size(); i < size; ++i)
            out.push_back(0);
    }

    Bytes chunk(std::string_view tag, const Bytes &body)
    {
        Bytes out;
        out.reserve(body.size() + 12);
        putText(out, tag);
        putU32(out, int64_t(body.size()));
        putBytes(out, body);
        while (out.size() % 4)
            out.push_back(0);
        return out;
    }

    // ---- chunk trees ------------------------------------------------------------

    namespace
    {
        bool isPanelContainer(const uint8_t *tag)
        {
            static const char *const kContainers[] = {"FORM", "TGEO", "TGPT", "TMTB", "TGIN", "TXRC", "FSCN",
                                                      "TERR", "ZPVS", "ZDYN", "TIVC", "TIVN", "TIVU", "TIFN",
                                                      "ULIB", "UAST", "UMTL", "UFNT"};
            for (const char *c : kContainers)
                if (std::memcmp(tag, c, 4) == 0)
                    return true;
            return false;
        }

        Chunks parseRange(ByteView data, size_t start, size_t end)
        {
            Chunks chunks;
            size_t at = start;
            while (at + 8 <= end)
            {
                Chunk c;
                c.tag.assign(reinterpret_cast<const char *>(data.data + at), 4);
                const size_t size = data.u32(at + 4);
                const size_t body = at + 8;
                if (body + size > end)
                {
                    std::string shown = c.tag; // (a broken file's tag may be any bytes)
                    for (char &ch : shown)
                        ch = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
                    throw PanelError("a chunk " + shown + " runs past its parent");
                }
                c.data = ByteView(data.data + body, size);
                if (isPanelContainer(data.data + at))
                {
                    c.container = true;
                    c.kids = parseRange(data, body, body + size);
                    c.data = ByteView();
                }
                chunks.push_back(std::move(c));
                at = body + size;
            }
            if (at != end)
                throw PanelError(std::to_string(end - at) + " stray bytes in a chunk tree");
            return chunks;
        }
    }

    const Chunk *Chunk::first(std::string_view t) const
    {
        for (const Chunk &k : kids)
            if (k.tag == t)
                return &k;
        return nullptr;
    }

    Chunks parseChunks(ByteView data) { return parseRange(data, 0, data.size); }

    void walkChunks(const Chunks &chunks, const std::function<void(const Chunk &)> &visit)
    {
        for (const Chunk &c : chunks)
        {
            visit(c);
            if (c.container)
                walkChunks(c.kids, visit);
        }
    }

    // ---- executables, archives, string tables -------------------------------------

    Elf::Elf(Bytes data) : m_data(std::move(data))
    {
        const ByteView d(m_data);
        const uint32_t count = d.u16(44), table = d.u32(28);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t at = size_t(table) + 32 * size_t(i);
            if (d.u32(at) == 1) // a loadable segment
                m_segments.push_back({d.u32(at + 8), d.u32(at + 4), d.u32(at + 16)});
        }
    }

    size_t Elf::fileOffset(uint32_t address) const
    {
        for (const Segment &s : m_segments)
            if (s.address <= address && uint64_t(address) < uint64_t(s.address) + s.size)
                return size_t(s.offset) + (address - s.address);
        throw PanelError(formatted("the executable has nothing at 0x%x", address));
    }

    uint32_t Elf::word(uint32_t address) const { return ByteView(m_data).u32(fileOffset(address)); }

    std::string Elf::text(uint32_t address) const { return ByteView(m_data).zeroEnded(fileOffset(address)); }

    namespace
    {
        uint32_t zdirCount(ByteView archive)
        {
            if (!archive.startsWith("ZDIR") || archive.slice(8, 12).str() != "DIDF")
                throw PanelError("not a ZDIR archive");
            return archive.u32(0x10);
        }
    }

    std::vector<ByteView> zdirEntries(ByteView archive)
    {
        const size_t count = zdirCount(archive);
        const size_t lengths = 0x18 + 8 + 4 * count;
        const size_t offsets = lengths + 8 + 4 * count;
        std::vector<ByteView> out;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t length = archive.u32(lengths + 8 + 4 * i);
            const uint32_t offset = archive.u32(offsets + 8 + 4 * i);
            out.push_back(archive.slice(offset, size_t(offset) + (length & 0x0FFFFFFF)));
        }
        return out;
    }

    std::unordered_map<uint32_t, ByteView> zdirNamed(ByteView archive)
    {
        const size_t count = zdirCount(archive);
        const std::vector<ByteView> entries = zdirEntries(archive);
        std::unordered_map<uint32_t, ByteView> out;
        for (size_t i = 0; i < count; ++i)
            out[archive.u32(0x18 + 8 + 4 * i)] = entries[i];
        return out;
    }

    std::unordered_map<uint32_t, std::string> dm2Strings(ByteView table)
    {
        // u32 text bytes, u32 count, {u32 key hash, u32 offset}[count]
        std::unordered_map<uint32_t, std::string> out;
        const uint32_t count = table.u32(4);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t key = table.u32(8 + 8 * size_t(i)), at = table.u32(12 + 8 * size_t(i));
            if (at >= table.size)
                continue;
            const void *end = std::memchr(table.data + at, 0, table.size - at);
            if (end && end != table.data + at)
                out[key] = std::string(reinterpret_cast<const char *>(table.data + at),
                                       size_t(static_cast<const uint8_t *>(end) - (table.data + at)));
        }
        return out;
    }

    Bytes readFile(const SourceFiles &files, std::string_view path)
    {
        Bytes out;
        if (!files.readAny(path, out))
            out.clear();
        return out;
    }

    Bytes needFile(const SourceFiles &files, std::string_view path)
    {
        Bytes out;
        if (!files.readAny(path, out))
            throw PanelError("the disc has no " + std::string(path));
        return out;
    }

    std::string pythonUpper(std::string_view text)
    {
        std::string out;
        for (const char ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c == 0xDF)
                out += "SS";
            else if (c == 0xB5 || c == 0xFF)
                throw PanelError("a name with a letter whose capital is not Latin-1");
            else if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
                out += char(c - 0x20);
            else
                out += ch;
        }
        return out;
    }

    void writeMade(const std::filesystem::path &path, const Bytes &data)
    {
        std::string error;
        if (!writeFile(path, data, &error))
            throw PanelError(error);
    }

    std::string formatted(const char *pattern, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, pattern);
        std::vsnprintf(buffer, sizeof buffer, pattern, args);
        va_end(args);
        return buffer;
    }

    bool wanted(const PanelExtras &extras, const std::string &level)
    {
        return extras.levels.empty() ||
               std::find(extras.levels.begin(), extras.levels.end(), level) != extras.levels.end();
    }

    // ---- goals ------------------------------------------------------------------

    uint32_t TextTable::add(std::string_view text)
    {
        const uint32_t at = uint32_t(bytes.size());
        putText(bytes, text);
        bytes.push_back(0);
        return at;
    }

    void putGoal(Bytes &out, const GoalFields &g)
    {
        // "<IIIiiIIBBBBffBBHiIiI"
        putU32(out, g.title);
        putU32(out, g.text);
        putU32(out, g.owner);
        putS32(out, g.counterWord);
        putS32(out, g.counterTarget);
        putU32(out, g.firstTarget);
        putU32(out, g.targetCount);
        putU8(out, g.type);
        putU8(out, g.tier);
        putU8(out, g.locked);
        putU8(out, g.points);
        putF32(out, g.anchorX);
        putF32(out, g.anchorZ);
        putU8(out, g.scoreKind);
        putU8(out, 0);
        putU16(out, 0);
        putS32(out, g.scoreTarget);
        putU32(out, g.status);
        putS32(out, g.targetWord);
        putU32(out, g.activate);
    }

    void putTarget(Bytes &out, uint32_t name, double x, double z, int64_t doneWord, int64_t doneValue, uint32_t role)
    {
        putU32(out, name);
        putF32(out, x);
        putF32(out, z);
        putS32(out, doneWord);
        putU32(out, doneValue);
        putU32(out, role);
    }

    void putBubble(Bytes &out, uint32_t name, double x, double z, int64_t stateWord, int64_t stateValue)
    {
        putU32(out, name);
        putF32(out, x);
        putF32(out, z);
        putS32(out, stateWord);
        putU32(out, stateValue);
    }

    std::vector<std::pair<double, double>> joinPlaces(const std::vector<Vec3> &points)
    {
        constexpr double kReach = 12.0; // world units: the faces of one ramp, the ends of one rail's edges
        std::vector<std::vector<Vec3>> groups;
        for (const Vec3 &p : points)
        {
            std::vector<Vec3> joined{p};
            std::vector<std::vector<Vec3>> kept;
            for (std::vector<Vec3> &g : groups)
            {
                bool near = false;
                for (const Vec3 &q : g)
                {
                    const double dx = p[0] - q[0], dz = p[2] - q[2];
                    if (dx * dx + dz * dz <= kReach * kReach)
                    {
                        near = true;
                        break;
                    }
                }
                if (near)
                    joined.insert(joined.end(), g.begin(), g.end());
                else
                    kept.push_back(std::move(g));
            }
            kept.push_back(std::move(joined));
            groups = std::move(kept);
        }
        std::vector<std::pair<double, double>> out;
        for (const std::vector<Vec3> &g : groups)
        {
            std::vector<double> xs, zs;
            for (const Vec3 &q : g)
            {
                xs.push_back(q[0]);
                zs.push_back(q[2]);
            }
            out.emplace_back(pythonMean(xs), pythonMean(zs));
        }
        return out;
    }
}
