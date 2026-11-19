#pragma once

// The chunk trees the three games' level files are made of (the engine's
// chunk reader, zge/assets/chunk_reader.cpp): a four-letter tag, a 32-bit
// little-endian size, then either the chunk's bytes or, for a container
// tag, the chunks inside it. The collection's map converter reads and
// writes levels through these (collection/levels/records.h); the tooling's
// ai_level.py is the same reader in Python, the reference the converter
// is checked against byte for byte.

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::app::levels
{
    using Bytes = std::vector<uint8_t>;

    // The three games' versions of the engine, whose level files differ in a
    // few records (collection/levels/geometry.h, scene.h list them).
    enum class Engine
    {
        Zge2, // Dave Mirra Freestyle BMX 2
        Zge3, // Aggressive Inline
        Zge4, // BMX XXX
    };

    // A level file that does not read as one (a size past its parent, a
    // record of the wrong size), or a model the converter cannot make into
    // a level (over a budget it cannot fix): what the menu shows when a map
    // does not convert.
    class LevelError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // A chunk tag ("TGEO") as the four bytes the file holds.
    using Tag = std::array<char, 4>;
    constexpr Tag tag(const char (&text)[5]) { return {text[0], text[1], text[2], text[3]}; }
    std::string tagText(const Tag &t);

    struct Chunk
    {
        Tag tag{};
        Bytes data;               // a leaf's bytes
        std::vector<Chunk> kids;  // a container's chunks
        bool container = false;

        static Chunk leaf(Tag t, Bytes bytes);
        static Chunk node(Tag t, std::vector<Chunk> children);
        // The bytes it takes after its own 8-byte head.
        size_t size() const;
        // The first child with that tag, or null.
        const Chunk *find(Tag t) const;
    };

    // The chunks of `data` (all of it, or the [start, end) range), a
    // container's read inside it. Throws LevelError on a chunk that runs
    // past its parent or stray bytes at the end.
    std::vector<Chunk> parseChunks(const Bytes &data, size_t start = 0, size_t end = SIZE_MAX);
    Bytes serializeChunks(const std::vector<Chunk> &chunks);
    void appendChunk(Bytes &out, const Chunk &chunk);
    // Every chunk of the trees, each before its children.
    std::vector<const Chunk *> walkChunks(const std::vector<Chunk> &chunks);

    // The engine's name hash (a case-insensitive ELF hash): every object's,
    // instance's, texture's and scene point's id is its name's.
    uint32_t nameHash(std::string_view name);

    // ---- little-endian fields -------------------------------------------------

    template <typename T>
    T readField(const Bytes &data, size_t at)
    {
        if (at + sizeof(T) > data.size())
            throw LevelError("a field past the end of a record");
        T value;
        std::memcpy(&value, data.data() + at, sizeof(T));
        return value;
    }
    inline uint8_t readU8(const Bytes &data, size_t at) { return readField<uint8_t>(data, at); }
    inline uint16_t readU16(const Bytes &data, size_t at) { return readField<uint16_t>(data, at); }
    inline int16_t readS16(const Bytes &data, size_t at) { return readField<int16_t>(data, at); }
    inline uint32_t readU32(const Bytes &data, size_t at) { return readField<uint32_t>(data, at); }
    inline int32_t readS32(const Bytes &data, size_t at) { return readField<int32_t>(data, at); }
    inline float readF32(const Bytes &data, size_t at) { return readField<float>(data, at); }
    // `count` floats from `at`, each widened to a double (the converters
    // compute in doubles, as the Python reference does).
    template <size_t N>
    std::array<double, N> readFloats(const Bytes &data, size_t at)
    {
        std::array<double, N> out{};
        for (size_t i = 0; i < N; ++i)
            out[i] = readF32(data, at + 4 * i);
        return out;
    }
    // The bytes [at, at + count) (clipped to the end).
    Bytes slice(const Bytes &data, size_t at, size_t count = SIZE_MAX);
    // A fixed char[] field: the text before the first zero.
    std::string cString(const Bytes &data, size_t at, size_t size);

    // Bytes appended as the files lay them out.
    class ByteWriter
    {
    public:
        ByteWriter &u8(uint8_t v);
        ByteWriter &u16(uint16_t v);
        ByteWriter &s16(int16_t v);
        ByteWriter &u32(uint32_t v);
        ByteWriter &s32(int32_t v);
        // A double narrowed to a float, as a file's float field holds it.
        ByteWriter &f32(double v);
        template <size_t N>
        ByteWriter &f32s(const std::array<double, N> &values)
        {
            for (double v : values)
                f32(v);
            return *this;
        }
        ByteWriter &bytes(const Bytes &data);
        ByteWriter &bytes(const uint8_t *data, size_t size);
        ByteWriter &zeros(size_t count);
        // A fixed char[size] field: the text and zeros after it. Throws on a
        // text longer than the field.
        ByteWriter &text(std::string_view value, size_t size);
        // Zeros to a multiple of `n` bytes.
        ByteWriter &padTo(size_t n);
        size_t size() const { return m_bytes.size(); }
        Bytes &data() { return m_bytes; }
        Bytes take() { return std::move(m_bytes); }

    private:
        Bytes m_bytes;
    };

    // Zeros after `data` to a multiple of `n` bytes.
    Bytes paddedTo(Bytes data, size_t n);
}
