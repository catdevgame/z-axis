// Chunk trees and little-endian fields (collection/levels/chunks.h).
#include "collection/levels/chunks.h"

#include <algorithm>

namespace zx::app::levels
{
    namespace
    {
        // The tags whose bytes are chunks: every container the three games'
        // level, scene and texture files use (ai_level.py CONTAINERS).
        constexpr Tag kContainers[] = {tag("FORM"), tag("TGEO"), tag("TGPT"), tag("TMTB"), tag("TGIN"),
                                       tag("TXRC"), tag("FSCN"), tag("TERR"), tag("ZPVS"), tag("ZDYN"),
                                       tag("TIVC"), tag("TIVN"), tag("TIVU"), tag("TIFN")};

        bool isContainer(const Tag &t)
        {
            return std::find(std::begin(kContainers), std::end(kContainers), t) != std::end(kContainers);
        }

        void walkInto(const std::vector<Chunk> &chunks, std::vector<const Chunk *> &out)
        {
            for (const Chunk &c : chunks)
            {
                out.push_back(&c);
                if (c.container)
                    walkInto(c.kids, out);
            }
        }
    }

    std::string tagText(const Tag &t) { return std::string(t.data(), t.size()); }

    Chunk Chunk::leaf(Tag t, Bytes bytes)
    {
        Chunk c;
        c.tag = t;
        c.data = std::move(bytes);
        return c;
    }

    Chunk Chunk::node(Tag t, std::vector<Chunk> children)
    {
        Chunk c;
        c.tag = t;
        c.kids = std::move(children);
        c.container = true;
        return c;
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

    const Chunk *Chunk::find(Tag t) const
    {
        for (const Chunk &k : kids)
            if (k.tag == t)
                return &k;
        return nullptr;
    }

    std::vector<Chunk> parseChunks(const Bytes &data, size_t start, size_t end)
    {
        end = std::min(end, data.size());
        std::vector<Chunk> chunks;
        size_t at = start;
        while (at + 8 <= end)
        {
            Tag t;
            std::memcpy(t.data(), data.data() + at, 4);
            const size_t size = readU32(data, at + 4);
            const size_t body = at + 8;
            if (body + size > end)
                throw LevelError("chunk " + tagText(t) + " runs past its parent");
            if (isContainer(t))
                chunks.push_back(Chunk::node(t, parseChunks(data, body, body + size)));
            else
                chunks.push_back(Chunk::leaf(t, Bytes(data.begin() + static_cast<ptrdiff_t>(body),
                                                      data.begin() + static_cast<ptrdiff_t>(body + size))));
            at = body + size;
        }
        if (at != end)
            throw LevelError(std::to_string(end - at) + " stray bytes after the chunks");
        return chunks;
    }

    void appendChunk(Bytes &out, const Chunk &chunk)
    {
        out.insert(out.end(), chunk.tag.begin(), chunk.tag.end());
        const uint32_t size = static_cast<uint32_t>(chunk.size());
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(size >> (8 * i)));
        if (!chunk.container)
            out.insert(out.end(), chunk.data.begin(), chunk.data.end());
        else
            for (const Chunk &k : chunk.kids)
                appendChunk(out, k);
    }

    Bytes serializeChunks(const std::vector<Chunk> &chunks)
    {
        Bytes out;
        for (const Chunk &c : chunks)
            appendChunk(out, c);
        return out;
    }

    std::vector<const Chunk *> walkChunks(const std::vector<Chunk> &chunks)
    {
        std::vector<const Chunk *> out;
        walkInto(chunks, out);
        return out;
    }

    uint32_t nameHash(std::string_view name)
    {
        uint32_t h = 0;
        for (const char ch : name)
        {
            uint32_t c = static_cast<uint8_t>(ch);
            if (c >= 'a' && c <= 'z')
                c -= 32;
            h = (h << 4) + c;
            const uint32_t high = h & 0xF0000000u;
            if (high)
                h ^= high >> 24;
            h &= ~high;
        }
        return h;
    }

    Bytes slice(const Bytes &data, size_t at, size_t count)
    {
        if (at >= data.size())
            return {};
        const size_t end = count > data.size() - at ? data.size() : at + count;
        return Bytes(data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(end));
    }

    std::string cString(const Bytes &data, size_t at, size_t size)
    {
        std::string out;
        for (size_t i = at; i < data.size() && i < at + size && data[i]; ++i)
            out.push_back(static_cast<char>(data[i]));
        return out;
    }

    ByteWriter &ByteWriter::u8(uint8_t v)
    {
        m_bytes.push_back(v);
        return *this;
    }

    ByteWriter &ByteWriter::u16(uint16_t v)
    {
        m_bytes.push_back(static_cast<uint8_t>(v));
        m_bytes.push_back(static_cast<uint8_t>(v >> 8));
        return *this;
    }

    ByteWriter &ByteWriter::s16(int16_t v) { return u16(static_cast<uint16_t>(v)); }

    ByteWriter &ByteWriter::u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            m_bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }

    ByteWriter &ByteWriter::s32(int32_t v) { return u32(static_cast<uint32_t>(v)); }

    ByteWriter &ByteWriter::f32(double v)
    {
        const float f = static_cast<float>(v);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return u32(bits);
    }

    ByteWriter &ByteWriter::bytes(const Bytes &data)
    {
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
        return *this;
    }

    ByteWriter &ByteWriter::bytes(const uint8_t *data, size_t size)
    {
        m_bytes.insert(m_bytes.end(), data, data + size);
        return *this;
    }

    ByteWriter &ByteWriter::zeros(size_t count)
    {
        m_bytes.insert(m_bytes.end(), count, 0);
        return *this;
    }

    ByteWriter &ByteWriter::text(std::string_view value, size_t size)
    {
        if (value.size() > size)
            throw LevelError("name \"" + std::string(value) + "\" longer than " + std::to_string(size));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return zeros(size - value.size());
    }

    ByteWriter &ByteWriter::padTo(size_t n)
    {
        while (m_bytes.size() % n)
            m_bytes.push_back(0);
        return *this;
    }

    Bytes paddedTo(Bytes data, size_t n)
    {
        while (data.size() % n)
            data.push_back(0);
        return data;
    }
}
