#include "runtime/ps2_zaxis_ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace zx::zaxis
{
    namespace
    {
        uint32_t readU32(const std::vector<uint8_t> &data, size_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        void putU32(std::vector<uint8_t> &data, uint32_t value)
        {
            const size_t at = data.size();
            data.resize(at + 4u);
            std::memcpy(data.data() + at, &value, sizeof(value));
        }

        void putTag(std::vector<uint8_t> &data, const char *tag, uint32_t size)
        {
            data.insert(data.end(), tag, tag + 4);
            putU32(data, size);
        }

        bool fail(std::string *error, const char *message)
        {
            if (error)
                *error = message;
            return false;
        }

        size_t lineStart(const std::string &text, size_t pos)
        {
            const size_t nl = pos == 0 ? std::string::npos : text.rfind('\n', pos - 1);
            return nl == std::string::npos ? 0 : nl + 1;
        }

        size_t nextLine(const std::string &text, size_t pos)
        {
            const size_t nl = text.find('\n', pos);
            return nl == std::string::npos ? text.size() : nl + 1;
        }

        // The line's text without the line end.
        std::string_view lineAt(const std::string &text, size_t start)
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            if (end > start && text[end - 1] == '\r')
                --end;
            return std::string_view(text).substr(start, end - start);
        }

        // From an OBJECT/ACTION line to after its closing brace line, by
        // indentation (UIBuilder writes the braces at the line's depth).
        Span blockFrom(const std::string &text, size_t headerLine, size_t limit)
        {
            const int depth = indentAt(text, headerLine);
            const std::string open = std::string(static_cast<size_t>(depth), '\t') + "{";
            const std::string close = std::string(static_cast<size_t>(depth), '\t') + "}";
            size_t pos = nextLine(text, headerLine);
            if (pos >= limit || lineAt(text, pos) != open)
                return {};
            for (pos = nextLine(text, pos); pos < limit; pos = nextLine(text, pos))
                if (lineAt(text, pos) == close)
                    return Span{headerLine, nextLine(text, pos)};
            return {};
        }
    }

    bool readArchive(const std::vector<uint8_t> &file, std::vector<ArchiveEntry> &entries, std::string *error)
    {
        entries.clear();
        if (file.size() < 0x20 || std::memcmp(file.data(), "ZDIR", 4) != 0 || std::memcmp(file.data() + 8, "DIDF", 4) != 0)
            return fail(error, "not a ZDIR archive");
        const uint32_t count = readU32(file, 0x10);
        const size_t tableBytes = static_cast<size_t>(count) * 4u;
        const size_t grid = 0x18;
        const size_t dlen = grid + 8 + tableBytes;
        const size_t ofst = dlen + 8 + tableBytes;
        if (count == 0 || count > 0x10000 || ofst + 8 + tableBytes > file.size() ||
            std::memcmp(file.data() + grid, "GRID", 4) != 0 || std::memcmp(file.data() + dlen, "DLEN", 4) != 0 ||
            std::memcmp(file.data() + ofst, "OFST", 4) != 0)
            return fail(error, "bad ZDIR tables");
        for (uint32_t i = 0; i < count; ++i)
        {
            ArchiveEntry entry;
            entry.nameHash = readU32(file, grid + 8 + i * 4u);
            const uint32_t word = readU32(file, dlen + 8 + i * 4u);
            entry.flags = word >> 28;
            const uint32_t length = word & 0x0FFFFFFFu;
            const uint32_t offset = readU32(file, ofst + 8 + i * 4u);
            if (static_cast<uint64_t>(offset) + length > file.size())
                return fail(error, "entry outside the file");
            entry.data.assign(file.begin() + offset, file.begin() + offset + length);
            entries.push_back(std::move(entry));
        }
        return true;
    }

    std::vector<uint8_t> writeArchive(const std::vector<ArchiveEntry> &entries)
    {
        const uint32_t count = static_cast<uint32_t>(entries.size());
        std::vector<uint8_t> out;
        putTag(out, "ZDIR", 40u + 12u * count);
        putTag(out, "DIDF", 8u);
        putU32(out, count);
        putU32(out, 0u);
        putTag(out, "GRID", 4u * count);
        for (const ArchiveEntry &entry : entries)
            putU32(out, entry.nameHash);
        putTag(out, "DLEN", 4u * count);
        for (const ArchiveEntry &entry : entries)
            putU32(out, (entry.flags << 28) | static_cast<uint32_t>(entry.data.size()));
        putTag(out, "OFST", 4u * count);
        const size_t offsetsAt = out.size();
        out.resize(out.size() + 4u * count);
        for (uint32_t i = 0; i < count; ++i)
        {
            out.resize((out.size() + 15u) & ~size_t{15u}, 0u);
            const uint32_t offset = static_cast<uint32_t>(out.size());
            std::memcpy(out.data() + offsetsAt + i * 4u, &offset, sizeof(offset));
            out.insert(out.end(), entries[i].data.begin(), entries[i].data.end());
        }
        out.resize((out.size() + 15u) & ~size_t{15u}, 0u);
        return out;
    }

    std::string scriptName(const ArchiveEntry &entry)
    {
        const std::string head(entry.data.begin(), entry.data.begin() + std::min<size_t>(entry.data.size(), 512u));
        if (head.rfind("//", 0) != 0)
            return {};
        const size_t object = head.find("OBJECT \"");
        if (object == std::string::npos)
            return {};
        const size_t typeEnd = head.find('"', object + 8);
        const size_t nameStart = typeEnd == std::string::npos ? typeEnd : head.find('"', typeEnd + 1);
        const size_t nameEnd = nameStart == std::string::npos ? nameStart : head.find('"', nameStart + 1);
        if (nameEnd == std::string::npos)
            return {};
        return head.substr(nameStart + 1, nameEnd - nameStart - 1);
    }

    int indentAt(const std::string &text, size_t pos)
    {
        size_t at = lineStart(text, std::min(pos, text.size()));
        int depth = 0;
        while (at < text.size() && text[at] == '\t')
        {
            ++depth;
            ++at;
        }
        return depth;
    }

    Span findObject(const std::string &text, std::string_view path, Span within)
    {
        Span span = within.found() ? within : Span{0, text.size()};
        while (!path.empty())
        {
            const size_t slash = path.find('/');
            const std::string_view name = path.substr(0, slash);
            path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
            const std::string quoted = "\" \"" + std::string(name) + "\"";
            Span next;
            // Direct children only (one tab deeper than the parent's
            // header); the first step from the whole text: top level.
            const bool topLevel = span.begin == 0 && !within.found() && span.end == text.size();
            const int childDepth = topLevel ? 0 : indentAt(text, span.begin) + 1;
            size_t pos = topLevel ? 0 : nextLine(text, span.begin);
            while (pos < span.end)
            {
                const size_t hit = text.find(quoted, pos);
                if (hit == std::string::npos || hit >= span.end)
                    break;
                const size_t start = lineStart(text, hit);
                const std::string_view line = lineAt(text, start);
                const size_t first = line.find_first_not_of('\t');
                const std::string_view body = first == std::string_view::npos ? std::string_view{} : line.substr(first);
                if (static_cast<int>(first) == childDepth &&
                    (body.rfind("OBJECT \"", 0) == 0 || body.rfind("ACTION \"", 0) == 0) &&
                    body.size() >= quoted.size() && body.substr(body.size() - quoted.size()) == quoted)
                {
                    next = blockFrom(text, start, span.end);
                    if (next.found())
                        break;
                }
                pos = hit + quoted.size();
            }
            if (!next.found())
                return {};
            span = next;
        }
        return span;
    }

    size_t objectEnd(const std::string &text, Span object)
    {
        if (!object.found() || object.end == 0)
            return std::string::npos;
        return lineStart(text, object.end - 1);
    }

    std::string indentBlock(std::string_view block, int depth)
    {
        const std::string tabs(static_cast<size_t>(std::max(depth, 0)), '\t');
        std::string out;
        size_t pos = 0;
        while (pos < block.size())
        {
            size_t end = block.find('\n', pos);
            if (end == std::string_view::npos)
                end = block.size();
            std::string_view line = block.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (!line.empty())
                out += tabs;
            out.append(line);
            out += "\r\n";
            pos = end + 1;
        }
        return out;
    }

    bool replaceIn(std::string &text, Span &span, std::string_view from, std::string_view to)
    {
        if (!span.found() || from.empty())
            return false;
        const size_t hit = text.find(from, span.begin);
        if (hit == std::string::npos || hit + from.size() > span.end)
            return false;
        text.replace(hit, from.size(), to);
        span.end = span.end - from.size() + to.size();
        return true;
    }

    std::vector<ObjectInfo> listObjects(const std::string &text)
    {
        struct Open
        {
            int depth;
            std::string name;
            size_t end;
        };
        std::vector<ObjectInfo> out;
        std::vector<Open> stack;
        std::set<std::string> seen;
        for (size_t pos = 0; pos < text.size(); pos = nextLine(text, pos))
        {
            const std::string_view line = lineAt(text, pos);
            const size_t first = line.find_first_not_of('\t');
            if (first == std::string_view::npos)
                continue;
            const std::string_view body = line.substr(first);
            const bool object = body.rfind("OBJECT \"", 0) == 0;
            if (!object && body.rfind("ACTION \"", 0) != 0)
                continue;
            // OBJECT "<type>" "<name>"
            const size_t typeEnd = body.find('"', 8);
            const size_t nameStart = typeEnd == std::string_view::npos ? typeEnd : body.find('"', typeEnd + 1);
            const size_t nameEnd = nameStart == std::string_view::npos ? nameStart : body.find('"', nameStart + 1);
            if (nameEnd == std::string_view::npos)
                continue;
            const Span block = blockFrom(text, pos, text.size());
            if (!block.found())
                continue;
            const int depth = static_cast<int>(first);
            while (!stack.empty() && (stack.back().depth >= depth || stack.back().end <= pos))
                stack.pop_back();
            std::string path;
            for (const Open &open : stack)
                path += open.name + "/";
            const std::string name(body.substr(nameStart + 1, nameEnd - nameStart - 1));
            path += name;
            stack.push_back(Open{depth, name, block.end});
            if (!object || !seen.insert(path).second)
                continue;
            out.push_back(ObjectInfo{path, std::string(body.substr(8, typeEnd - 8)), block});
        }
        return out;
    }

    Span findProperty(const std::string &text, Span object, std::string_view name)
    {
        if (!object.found() || name.empty())
            return {};
        const int depth = indentAt(text, object.begin) + 1;
        for (size_t pos = nextLine(text, object.begin); pos < object.end; pos = nextLine(text, pos))
        {
            const std::string_view line = lineAt(text, pos);
            const size_t first = line.find_first_not_of('\t');
            if (first != static_cast<size_t>(depth))
                continue;
            const std::string_view body = line.substr(first);
            // (A child's properties are deeper.)
            if (body.size() > name.size() && body.substr(0, name.size()) == name && body[name.size()] == '\t')
                return Span{pos + first + name.size() + 1u, pos + line.size()};
        }
        return {};
    }

    std::vector<std::string> narrowLike(std::string &text, const std::string &model)
    {
        static const char *const kTextTypes[] = {"guiLabel", "guiMenu", "guiTextBox"};
        auto scaleOf = [](const std::string &script, Span value, float out[3])
        {
            if (!value.found())
                return false;
            const std::string digits = script.substr(value.begin, value.end - value.begin);
            return std::sscanf(digits.c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3;
        };
        std::map<std::string, ObjectInfo> theirs;
        for (ObjectInfo &object : listObjects(model))
            theirs.emplace(object.path, std::move(object));
        struct Change
        {
            Span value;
            std::string scale;
            std::string path;
        };
        std::vector<Change> changes;
        for (const ObjectInfo &object : listObjects(text))
        {
            if (std::find(std::begin(kTextTypes), std::end(kTextTypes), object.type) == std::end(kTextTypes))
                continue;
            const auto other = theirs.find(object.path);
            if (other == theirs.end() || other->second.type != object.type)
                continue;
            const Span mine = findProperty(text, object.span, "Scale");
            float a[3], b[3];
            if (!scaleOf(text, mine, a) || !scaleOf(model, findProperty(model, other->second.span, "Scale"), b) ||
                !(b[0] > 0.0f && b[0] < a[0] - 0.001f))
                continue;
            char scale[96];
            std::snprintf(scale, sizeof(scale), "%f %f %f", static_cast<double>(b[0]), static_cast<double>(a[1]),
                          static_cast<double>(a[2]));
            changes.push_back(Change{mine, scale, object.path});
        }
        // From the end, so the earlier spans stay valid.
        std::vector<std::string> paths;
        for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        {
            text.replace(it->value.begin, it->value.end - it->value.begin, it->scale);
            paths.push_back(it->path);
        }
        std::reverse(paths.begin(), paths.end());
        return paths;
    }
}

namespace zx::zaxis
{
    namespace
    {
        struct Chunk
        {
            char tag[5] = {};
            size_t begin = 0; // data
            size_t size = 0;
        };

        bool chunkAt(const std::vector<uint8_t> &data, size_t pos, size_t limit, Chunk &out)
        {
            if (pos + 8u > limit)
                return false;
            std::memcpy(out.tag, data.data() + pos, 4);
            out.begin = pos + 8u;
            out.size = readU32(data, pos + 4u);
            return out.size <= limit - out.begin;
        }

        uint16_t readU16(const std::vector<uint8_t> &data, size_t offset)
        {
            uint16_t value = 0;
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        std::string cString(const std::vector<uint8_t> &data, size_t begin, size_t limit)
        {
            std::string out;
            for (size_t i = begin; i < limit && data[i] != 0u; ++i)
                out.push_back(static_cast<char>(data[i]));
            return out;
        }

        // TZMP: a Windows BMP with 4 or 8 bits per pixel.
        bool decodeBmp(const std::vector<uint8_t> &data, size_t begin, size_t size, LibraryTexture &out)
        {
            if (size < 54u || data[begin] != 'B' || data[begin + 1] != 'M')
                return false;
            const uint32_t pixels = readU32(data, begin + 10u);
            int32_t width = 0, height = 0;
            std::memcpy(&width, data.data() + begin + 18u, 4);
            std::memcpy(&height, data.data() + begin + 22u, 4);
            const uint16_t bits = readU16(data, begin + 28u);
            const bool bottomUp = height > 0;
            height = height < 0 ? -height : height;
            if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || (bits != 4 && bits != 8))
                return false;
            const size_t rowBytes = ((static_cast<size_t>(width) * bits + 31u) / 32u) * 4u;
            const size_t paletteAt = begin + 14u + readU32(data, begin + 14u);
            if (pixels + rowBytes * static_cast<size_t>(height) > size || paletteAt + (4u << bits) > begin + size)
                return false;
            out.width = width;
            out.height = height;
            out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
            for (int y = 0; y < height; ++y)
            {
                const size_t row = begin + pixels + rowBytes * static_cast<size_t>(bottomUp ? height - 1 - y : y);
                for (int x = 0; x < width; ++x)
                {
                    const uint8_t byte = data[row + (bits == 8 ? static_cast<size_t>(x) : static_cast<size_t>(x) / 2u)];
                    const uint8_t index = bits == 8 ? byte : static_cast<uint8_t>((x & 1) ? byte & 0xFu : byte >> 4);
                    const uint8_t *bgra = data.data() + paletteAt + 4u * index;
                    out.rgba[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                        static_cast<uint32_t>(bgra[2]) | static_cast<uint32_t>(bgra[1]) << 8 |
                        static_cast<uint32_t>(bgra[0]) << 16 | static_cast<uint32_t>(bgra[3]) << 24;
                }
            }
            return true;
        }

        // TTGA: an uncompressed 32-bit TGA.
        bool decodeTga(const std::vector<uint8_t> &data, size_t begin, size_t size, LibraryTexture &out)
        {
            if (size < 18u || data[begin + 2] != 2u || data[begin + 16] != 32u)
                return false;
            const int width = readU16(data, begin + 12u);
            const int height = readU16(data, begin + 14u);
            const bool topDown = (data[begin + 17] & 0x20u) != 0u;
            const size_t pixels = begin + 18u + data[begin];
            if (width <= 0 || height <= 0 || pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u > begin + size)
                return false;
            out.width = width;
            out.height = height;
            out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const uint8_t *bgra = data.data() + pixels + 4u * (static_cast<size_t>(topDown ? y : height - 1 - y) * static_cast<size_t>(width) + static_cast<size_t>(x));
                    out.rgba[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                        static_cast<uint32_t>(bgra[2]) | static_cast<uint32_t>(bgra[1]) << 8 |
                        static_cast<uint32_t>(bgra[0]) << 16 | static_cast<uint32_t>(bgra[3]) << 24;
                }
            return true;
        }

        // TXRC: TXRH and the picture.
        bool readTextureChunk(const std::vector<uint8_t> &data, const Chunk &txrc, LibraryTexture &out)
        {
            bool decoded = false;
            Chunk c;
            for (size_t pos = txrc.begin; chunkAt(data, pos, txrc.begin + txrc.size, c); pos = c.begin + c.size)
            {
                if (std::memcmp(c.tag, "TXRH", 4) == 0 && c.size >= 0x16u)
                    out.name = cString(data, c.begin + 0x14u, c.begin + c.size);
                else if (std::memcmp(c.tag, "TZMP", 4) == 0)
                    decoded = decodeBmp(data, c.begin, c.size, out);
                else if (std::memcmp(c.tag, "TTGA", 4) == 0)
                    decoded = decodeTga(data, c.begin, c.size, out);
            }
            return decoded;
        }

        bool readFontChunk(const std::vector<uint8_t> &data, const Chunk &ufnt, LibraryFont &out)
        {
            bool haveHeader = false, haveAtlas = false;
            Chunk c;
            for (size_t pos = ufnt.begin; chunkAt(data, pos, ufnt.begin + ufnt.size, c); pos = c.begin + c.size)
            {
                if (std::memcmp(c.tag, "HFNT", 4) == 0 && c.size >= 0x2Eu)
                {
                    out.lineHeight = data[c.begin];
                    const size_t count = readU16(data, c.begin + 2u);
                    for (size_t i = 0; i < count && 0x2Eu + 12u * (i + 1u) <= c.size; ++i)
                    {
                        const size_t at = c.begin + 0x2Eu + 12u * i;
                        FontGlyph g;
                        g.character = readU16(data, at);
                        g.offset = readU16(data, at + 2u);
                        g.x = readU16(data, at + 4u);
                        g.y = readU16(data, at + 6u);
                        g.width = readU16(data, at + 8u);
                        g.height = readU16(data, at + 10u);
                        out.glyphs.push_back(g);
                    }
                    haveHeader = true;
                }
                else if (std::memcmp(c.tag, "TXRC", 4) == 0)
                {
                    haveAtlas = readTextureChunk(data, c, out.atlas);
                }
            }
            return haveHeader && haveAtlas;
        }

        void walkLibrary(const std::vector<uint8_t> &data, size_t begin, size_t end, const std::string &source,
                         std::vector<LibraryTexture> *textures, std::vector<LibraryFont> *fonts)
        {
            std::string current = source;
            Chunk c;
            for (size_t pos = begin; chunkAt(data, pos, end, c); pos = c.begin + c.size)
            {
                if (std::memcmp(c.tag, "HAST", 4) == 0)
                {
                    current = cString(data, c.begin, c.begin + c.size);
                }
                else if (std::memcmp(c.tag, "UAST", 4) == 0 || std::memcmp(c.tag, "UMTL", 4) == 0)
                {
                    walkLibrary(data, c.begin, c.begin + c.size, current, textures, fonts);
                }
                else if (std::memcmp(c.tag, "TXRC", 4) == 0 && textures)
                {
                    LibraryTexture texture;
                    texture.source = current;
                    if (readTextureChunk(data, c, texture))
                        textures->push_back(std::move(texture));
                }
                else if (std::memcmp(c.tag, "UFNT", 4) == 0 && fonts)
                {
                    LibraryFont font;
                    font.source = current;
                    font.atlas.source = current;
                    if (readFontChunk(data, c, font))
                        fonts->push_back(std::move(font));
                }
            }
        }
    }

    const FontGlyph *LibraryFont::glyph(char c) const
    {
        for (const FontGlyph &g : glyphs)
            if (g.character == static_cast<uint8_t>(c))
                return &g;
        return nullptr;
    }

    bool readLibrary(const std::vector<uint8_t> &entry, std::vector<LibraryTexture> *textures,
                     std::vector<LibraryFont> *fonts)
    {
        Chunk root;
        if (!chunkAt(entry, 0, entry.size(), root) || std::memcmp(root.tag, "ULIB", 4) != 0)
            return false;
        walkLibrary(entry, root.begin, root.begin + root.size, {}, textures, fonts);
        return true;
    }

    std::string sourceFileName(std::string_view source)
    {
        const size_t slash = source.find_last_of("\\/");
        std::string name(slash == std::string_view::npos ? source : source.substr(slash + 1));
        for (char &c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name;
    }

    std::vector<std::string> replaceFonts(std::vector<uint8_t> &library, const std::vector<uint8_t> &donor)
    {
        // The UASTs of a ULIB: (file name, is a font, chunk start, chunk end).
        struct Asset
        {
            std::string file;
            bool font = false;
            size_t begin = 0;
            size_t end = 0;
        };
        auto assets = [](const std::vector<uint8_t> &data, std::vector<Asset> &out, Chunk &root)
        {
            if (!chunkAt(data, 0, data.size(), root) || std::memcmp(root.tag, "ULIB", 4) != 0)
                return false;
            Chunk c;
            for (size_t pos = root.begin; chunkAt(data, pos, root.begin + root.size, c); pos = c.begin + c.size)
            {
                Asset a;
                a.begin = pos;
                a.end = c.begin + c.size;
                if (std::memcmp(c.tag, "UAST", 4) == 0)
                {
                    Chunk d;
                    for (size_t at = c.begin; chunkAt(data, at, c.begin + c.size, d); at = d.begin + d.size)
                    {
                        if (std::memcmp(d.tag, "HAST", 4) == 0)
                            a.file = sourceFileName(cString(data, d.begin, d.begin + d.size));
                        else if (std::memcmp(d.tag, "UFNT", 4) == 0)
                            a.font = true;
                    }
                }
                out.push_back(std::move(a));
            }
            return true;
        };
        std::vector<std::string> replaced;
        std::vector<Asset> mine, theirs;
        Chunk root, donorRoot;
        if (!assets(library, mine, root) || !assets(donor, theirs, donorRoot))
            return replaced;
        std::vector<uint8_t> out(library.begin(), library.begin() + static_cast<std::ptrdiff_t>(root.begin));
        for (const Asset &a : mine)
        {
            const Asset *swap = nullptr;
            if (a.font)
                for (const Asset &b : theirs)
                    if (b.font && b.file == a.file)
                        swap = &b;
            if (swap)
            {
                out.insert(out.end(), donor.begin() + static_cast<std::ptrdiff_t>(swap->begin),
                           donor.begin() + static_cast<std::ptrdiff_t>(swap->end));
                replaced.push_back(a.file);
            }
            else
            {
                out.insert(out.end(), library.begin() + static_cast<std::ptrdiff_t>(a.begin),
                           library.begin() + static_cast<std::ptrdiff_t>(a.end));
            }
        }
        if (replaced.empty())
            return replaced;
        // Anything after the ULIB chunk (padding) is kept.
        const size_t rootEnd = root.begin + root.size;
        const uint32_t size = static_cast<uint32_t>(out.size() - 8u);
        std::memcpy(out.data() + 4, &size, sizeof(size));
        out.insert(out.end(), library.begin() + static_cast<std::ptrdiff_t>(rootEnd), library.end());
        library = std::move(out);
        return replaced;
    }

    std::vector<std::string> addAssets(std::vector<uint8_t> &library, const std::vector<uint8_t> &donor,
                                       const std::vector<std::string> &names)
    {
        // The UASTs of a ULIB: (texture name, chunk start, chunk end).
        struct Asset
        {
            std::string name;
            size_t begin = 0;
            size_t end = 0;
        };
        auto assets = [](const std::vector<uint8_t> &data, std::vector<Asset> &out, Chunk &root)
        {
            if (!chunkAt(data, 0, data.size(), root) || std::memcmp(root.tag, "ULIB", 4) != 0)
                return false;
            Chunk c;
            for (size_t pos = root.begin; chunkAt(data, pos, root.begin + root.size, c); pos = c.begin + c.size)
            {
                if (std::memcmp(c.tag, "UAST", 4) != 0)
                    continue;
                Asset a;
                a.begin = pos;
                a.end = c.begin + c.size;
                Chunk m, t, h;
                for (size_t at = c.begin; a.name.empty() && chunkAt(data, at, c.begin + c.size, m); at = m.begin + m.size)
                {
                    if (std::memcmp(m.tag, "UMTL", 4) != 0)
                        continue;
                    for (size_t in = m.begin; a.name.empty() && chunkAt(data, in, m.begin + m.size, t); in = t.begin + t.size)
                    {
                        if (std::memcmp(t.tag, "TXRC", 4) != 0)
                            continue;
                        if (chunkAt(data, t.begin, t.begin + t.size, h) && std::memcmp(h.tag, "TXRH", 4) == 0 &&
                            h.size >= 0x34u)
                            a.name = cString(data, h.begin + 0x14u, h.begin + 0x34u);
                    }
                }
                out.push_back(std::move(a));
            }
            return true;
        };
        std::vector<std::string> added;
        std::vector<Asset> mine, theirs;
        Chunk root, donorRoot;
        if (!assets(library, mine, root) || !assets(donor, theirs, donorRoot))
            return added;
        std::vector<uint8_t> extra;
        for (const std::string &name : names)
        {
            bool have = false;
            for (const Asset &a : mine)
                have = have || a.name == name;
            if (have)
                continue;
            for (const Asset &b : theirs)
                if (b.name == name)
                {
                    extra.insert(extra.end(), donor.begin() + static_cast<std::ptrdiff_t>(b.begin),
                                 donor.begin() + static_cast<std::ptrdiff_t>(b.end));
                    added.push_back(name);
                    break;
                }
        }
        if (added.empty())
            return added;
        // After the last asset; anything after the ULIB chunk (padding) is kept.
        const size_t rootEnd = root.begin + root.size;
        library.insert(library.begin() + static_cast<std::ptrdiff_t>(rootEnd), extra.begin(), extra.end());
        const uint32_t size = static_cast<uint32_t>(root.size + extra.size());
        std::memcpy(library.data() + 4, &size, sizeof(size));
        return added;
    }

    bool appendFontPages(std::vector<uint8_t> &library, std::string_view font, const std::vector<uint8_t> &pages,
                         std::string *error)
    {
        // An HFNT's parts: header counts, then page records and glyphs.
        struct Hfnt
        {
            size_t begin = 0;
            uint16_t glyphs = 0;
            uint16_t pages = 0;
            size_t pageRecords() const { return begin + 6u; }
            size_t glyphRecords() const { return begin + 6u + 40u * pages; }
        };
        // The UFNT's HFNT (the first child) and where its page chunks run.
        auto readFont = [](const std::vector<uint8_t> &data, const Chunk &ufnt, Hfnt &h, size_t &restBegin) -> bool
        {
            Chunk c;
            if (!chunkAt(data, ufnt.begin, ufnt.begin + ufnt.size, c) || std::memcmp(c.tag, "HFNT", 4) != 0 || c.size < 6u)
                return false;
            h.begin = c.begin;
            h.glyphs = readU16(data, c.begin + 2u);
            h.pages = readU16(data, c.begin + 4u);
            restBegin = c.begin + c.size;
            return h.pages > 0u && 6u + 40u * h.pages + 12u * h.glyphs <= c.size;
        };
        Chunk root, donorFont;
        if (!chunkAt(library, 0, library.size(), root) || std::memcmp(root.tag, "ULIB", 4) != 0)
            return fail(error, "not a ULIB");
        if (!chunkAt(pages, 0, pages.size(), donorFont) || std::memcmp(donorFont.tag, "UFNT", 4) != 0)
            return fail(error, "the pages are not a UFNT");
        Hfnt theirs;
        size_t theirRest = 0;
        if (!readFont(pages, donorFont, theirs, theirRest))
            return fail(error, "the pages' HFNT is not a multi-page font");
        Chunk asset;
        for (size_t pos = root.begin; chunkAt(library, pos, root.begin + root.size, asset); pos = asset.begin + asset.size)
        {
            if (std::memcmp(asset.tag, "UAST", 4) != 0)
                continue;
            std::string file;
            Chunk c, ufnt;
            bool isFont = false;
            for (size_t at = asset.begin; chunkAt(library, at, asset.begin + asset.size, c); at = c.begin + c.size)
            {
                if (std::memcmp(c.tag, "HAST", 4) == 0)
                    file = sourceFileName(cString(library, c.begin, c.begin + c.size));
                else if (std::memcmp(c.tag, "UFNT", 4) == 0)
                {
                    ufnt = c;
                    isFont = true;
                }
            }
            if (!isFont || file != font)
                continue;
            Hfnt mine;
            size_t myRest = 0;
            if (!readFont(library, ufnt, mine, myRest))
                return fail(error, "the font's HFNT is not a multi-page font");
            for (uint16_t i = 0; i < theirs.glyphs; ++i)
                for (uint16_t j = 0; j < mine.glyphs; ++j)
                    if (library[mine.glyphRecords() + 12u * j] == pages[theirs.glyphRecords() + 12u * i])
                        return fail(error, "a character is in both fonts");
            std::vector<uint8_t> hfnt(library.begin() + static_cast<std::ptrdiff_t>(mine.begin),
                                      library.begin() + static_cast<std::ptrdiff_t>(mine.glyphRecords()));
            const uint16_t glyphs = static_cast<uint16_t>(mine.glyphs + theirs.glyphs);
            const uint16_t pageCount = static_cast<uint16_t>(mine.pages + theirs.pages);
            std::memcpy(hfnt.data() + 2, &glyphs, 2);
            std::memcpy(hfnt.data() + 4, &pageCount, 2);
            hfnt.insert(hfnt.end(), pages.begin() + static_cast<std::ptrdiff_t>(theirs.pageRecords()),
                        pages.begin() + static_cast<std::ptrdiff_t>(theirs.glyphRecords()));
            hfnt.insert(hfnt.end(), library.begin() + static_cast<std::ptrdiff_t>(mine.glyphRecords()),
                        library.begin() + static_cast<std::ptrdiff_t>(mine.glyphRecords() + 12u * mine.glyphs));
            for (uint16_t i = 0; i < theirs.glyphs; ++i)
            {
                const size_t at = hfnt.size();
                hfnt.insert(hfnt.end(), pages.begin() + static_cast<std::ptrdiff_t>(theirs.glyphRecords() + 12u * i),
                            pages.begin() + static_cast<std::ptrdiff_t>(theirs.glyphRecords() + 12u * i + 12u));
                hfnt[at + 1u] = static_cast<uint8_t>(hfnt[at + 1u] + mine.pages);
            }
            hfnt.resize((hfnt.size() + 3u) & ~size_t{3});
            // UFNT: the new HFNT, the font's page chunks, the added ones.
            std::vector<uint8_t> body;
            putTag(body, "HFNT", static_cast<uint32_t>(hfnt.size()));
            body.insert(body.end(), hfnt.begin(), hfnt.end());
            body.insert(body.end(), library.begin() + static_cast<std::ptrdiff_t>(myRest),
                        library.begin() + static_cast<std::ptrdiff_t>(ufnt.begin + ufnt.size));
            body.insert(body.end(), pages.begin() + static_cast<std::ptrdiff_t>(theirRest),
                        pages.begin() + static_cast<std::ptrdiff_t>(donorFont.begin + donorFont.size));
            const size_t ufntHead = ufnt.begin - 8u;
            const ptrdiff_t growth = static_cast<ptrdiff_t>(body.size()) - static_cast<ptrdiff_t>(ufnt.size);
            std::vector<uint8_t> out(library.begin(), library.begin() + static_cast<std::ptrdiff_t>(ufntHead));
            putTag(out, "UFNT", static_cast<uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
            out.insert(out.end(), library.begin() + static_cast<std::ptrdiff_t>(ufnt.begin + ufnt.size), library.end());
            const uint32_t assetSize = static_cast<uint32_t>(static_cast<ptrdiff_t>(asset.size) + growth);
            const uint32_t rootSize = static_cast<uint32_t>(static_cast<ptrdiff_t>(root.size) + growth);
            std::memcpy(out.data() + (asset.begin - 4u), &assetSize, 4);
            std::memcpy(out.data() + 4, &rootSize, 4);
            library = std::move(out);
            return true;
        }
        return fail(error, "the font is not in the library");
    }
}
