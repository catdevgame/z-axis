#include "runtime/texture_replace.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace zx::texture_replace
{
    bool isBlockFormat(Format format)
    {
        return format == Format::BC1 || format == Format::BC2 || format == Format::BC3 || format == Format::BC7;
    }

    size_t Picture::levelBytes(Format format, uint32_t width, uint32_t height)
    {
        width = std::max<uint32_t>(width, 1u);
        height = std::max<uint32_t>(height, 1u);
        if (isBlockFormat(format))
            return static_cast<size_t>((width + 3u) / 4u) * ((height + 3u) / 4u) * (format == Format::BC1 ? 8u : 16u);
        return static_cast<size_t>(width) * height * 4u;
    }

    uint32_t Picture::levelWidth(uint32_t level) const { return std::max<uint32_t>(width >> level, 1u); }
    uint32_t Picture::levelHeight(uint32_t level) const { return std::max<uint32_t>(height >> level, 1u); }

    size_t Picture::levelOffset(uint32_t level) const
    {
        size_t offset = 0;
        for (uint32_t i = 0; i < level; ++i)
            offset += levelSize(i);
        return offset;
    }

    size_t Picture::levelSize(uint32_t level) const { return levelBytes(format, levelWidth(level), levelHeight(level)); }

    bool Picture::valid() const
    {
        if (width == 0u || height == 0u || width > 4096u || height > 4096u || mips == 0u || mips > 13u)
            return false;
        if (format != Format::RGBA8 && !isBlockFormat(format))
            return false;
        // A level below 1 x 1 does not exist.
        if ((std::max(width, height) >> (mips - 1u)) == 0u)
            return false;
        return levelOffset(mips) <= data.size();
    }

    uint64_t indexHash(const uint8_t *indices, size_t count)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < count; ++i)
        {
            hash ^= indices[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    uint64_t rgbHash(const uint32_t *texels, uint32_t width, uint32_t height)
    {
        const size_t count = height > 1u ? static_cast<size_t>(width) * (height - 1u) : 0u;
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t t = texels[i];
            hash = (hash ^ (t & 0xFFu)) * 0x100000001B3ull;
            hash = (hash ^ ((t >> 8) & 0xFFu)) * 0x100000001B3ull;
            hash = (hash ^ ((t >> 16) & 0xFFu)) * 0x100000001B3ull;
        }
        return hash;
    }

    namespace
    {
        struct Key
        {
            uint32_t width, height;
            uint64_t hash;
            bool rgb;
            bool operator==(const Key &o) const
            {
                return width == o.width && height == o.height && hash == o.hash && rgb == o.rgb;
            }
        };
        struct KeyHash
        {
            size_t operator()(const Key &k) const
            {
                return static_cast<size_t>(k.hash ^ (static_cast<uint64_t>(k.width) << 40) ^ (static_cast<uint64_t>(k.height) << 20));
            }
        };

        struct Source
        {
            std::string name;
            uint64_t serial = 0;
            std::vector<Entry> entries;
            size_t bytes = 0;
        };

        struct Registry
        {
            std::mutex mutex;
            std::vector<Source> sources; // oldest first
            std::unordered_map<Key, PicturePtr, KeyHash> index;
            uint64_t serial = 0;
            size_t budget = size_t(512) << 20;
            bool on = true;
            std::atomic<uint64_t> generation{1};
            std::atomic<bool> rgbEntries{false};
            std::atomic<bool> anySource{false};

            void rebuild()
            {
                index.clear();
                bool rgb = false;
                for (const Source &s : sources)
                    for (const Entry &e : s.entries)
                    {
                        index[Key{e.width, e.height, e.indexHash, e.rgb}] = e.picture;
                        rgb = rgb || e.rgb;
                    }
                rgbEntries = rgb;
                anySource = !sources.empty();
                generation.fetch_add(1);
            }

            size_t totalBytes() const
            {
                size_t total = 0;
                for (const Source &s : sources)
                    total += s.bytes;
                return total;
            }
        };

        Registry &registry()
        {
            static Registry r;
            return r;
        }

        size_t pictureBytes(const std::vector<Entry> &entries)
        {
            std::unordered_set<const Picture *> seen;
            size_t bytes = 0;
            for (const Entry &e : entries)
                if (e.picture && seen.insert(e.picture.get()).second)
                    bytes += e.picture->data.size();
            return bytes;
        }
    }

    void setSource(const std::string &source, std::vector<Entry> entries)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.sources.erase(std::remove_if(r.sources.begin(), r.sources.end(), [&](const Source &s)
                                       { return s.name == source; }),
                        r.sources.end());
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry &e)
                                     { return !e.picture; }),
                      entries.end());
        Source s;
        s.name = source;
        s.serial = ++r.serial;
        s.bytes = pictureBytes(entries);
        s.entries = std::move(entries);
        r.sources.push_back(std::move(s));
        // Over budget: drop the oldest sources (never the one just set).
        while (r.sources.size() > 1u && r.totalBytes() > r.budget)
            r.sources.erase(r.sources.begin());
        r.rebuild();
    }

    void removeSource(const std::string &source)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const size_t before = r.sources.size();
        r.sources.erase(std::remove_if(r.sources.begin(), r.sources.end(), [&](const Source &s)
                                       { return s.name == source; }),
                        r.sources.end());
        if (r.sources.size() != before)
            r.rebuild();
    }

    bool hasSource(const std::string &source)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const Source &s : r.sources)
            if (s.name == source)
                return true;
        return false;
    }

    void clear()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.sources.clear();
        r.rebuild();
    }

    bool hasRgbEntries() { return registry().rgbEntries.load(); }

    bool hasSources() { return registry().anySource.load(); }

    uint64_t indexHashOfUpload(const uint8_t *texels, uint32_t width, uint32_t height, uint32_t psm)
    {
        const size_t count = static_cast<size_t>(width) * height;
        if (psm == 0x13u)
            return indexHash(texels, count);
        if (psm != 0x14u)
            return 0;
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t b = texels[i >> 1];
            hash ^= (i & 1u) ? (b >> 4) : (b & 0x0Fu);
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    PicturePtr find(uint32_t width, uint32_t height, uint64_t hash, bool rgb)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.on && !rgb)
            return nullptr; // movie frames follow their own setting
        const auto it = r.index.find(Key{width, height, hash, rgb});
        return it == r.index.end() ? nullptr : it->second;
    }

    uint64_t generation() { return registry().generation.load(); }

    void setEnabled(bool on)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.on == on)
            return;
        r.on = on;
        r.generation.fetch_add(1);
    }

    bool enabled()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        return r.on;
    }

    void setBudget(size_t bytes)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.budget = bytes;
    }

    Stats stats()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        Stats s;
        s.sources = r.sources.size();
        s.entries = r.index.size();
        s.bytes = r.totalBytes();
        return s;
    }

    namespace
    {
        template <typename T>
        T readLe(const std::vector<uint8_t> &b, size_t at)
        {
            T v{};
            std::memcpy(&v, b.data() + at, sizeof(T));
            return v; // every supported host is little-endian
        }
    }

    bool parsePack(const std::vector<uint8_t> &bytes, std::vector<Entry> &out, std::string *error, size_t *skipped)
    {
        constexpr size_t kHeader = 16u, kEntry = 64u;
        auto fail = [&](const char *what)
        {
            if (error)
                *error = what;
            return false;
        };
        if (skipped)
            *skipped = 0;
        if (bytes.size() < kHeader || std::memcmp(bytes.data(), "PXTP", 4) != 0)
            return fail("not a texture pack");
        if (readLe<uint32_t>(bytes, 4) != 1u)
            return fail("unknown texture pack version");
        const uint32_t count = readLe<uint32_t>(bytes, 8);
        if (count > (bytes.size() - kHeader) / kEntry)
            return fail("texture pack entry table past the end");
        out.reserve(out.size() + count);
        // Entries with the same data (a PS2 texture and its mip levels) share one picture.
        std::unordered_map<uint64_t, PicturePtr> shared;
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t at = kHeader + i * kEntry;
            Entry e;
            e.width = readLe<uint16_t>(bytes, at);
            e.height = readLe<uint16_t>(bytes, at + 2);
            e.indexHash = readLe<uint64_t>(bytes, at + 8);
            auto p = std::make_shared<Picture>();
            p->width = readLe<uint16_t>(bytes, at + 16);
            p->height = readLe<uint16_t>(bytes, at + 18);
            p->mips = bytes[at + 20];
            p->format = static_cast<Format>(bytes[at + 21]);
            const uint32_t offset = readLe<uint32_t>(bytes, at + 24);
            const uint32_t size = readLe<uint32_t>(bytes, at + 28);
            const char *name = reinterpret_cast<const char *>(bytes.data() + at + 32);
            p->name.assign(name, strnlen(name, 32));
            if (static_cast<uint64_t>(offset) + size > bytes.size() || e.width == 0u || e.height == 0u)
            {
                if (skipped)
                    ++*skipped;
                continue;
            }
            const uint64_t where = (static_cast<uint64_t>(offset) << 32) | size;
            if (const auto it = shared.find(where); it != shared.end() && it->second->width == p->width &&
                                                    it->second->height == p->height && it->second->mips == p->mips)
            {
                e.picture = it->second;
                out.push_back(std::move(e));
                continue;
            }
            p->data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
            if (!p->valid())
            {
                if (skipped)
                    ++*skipped;
                continue;
            }
            e.picture = std::move(p);
            shared[where] = e.picture;
            out.push_back(std::move(e));
        }
        return true;
    }

    std::vector<uint8_t> writePack(const std::vector<Entry> &entries, uint8_t indexBits)
    {
        constexpr size_t kHeader = 16u, kEntry = 64u;
        std::vector<const Entry *> written;
        for (const Entry &e : entries)
            if (!e.rgb && e.picture)
                written.push_back(&e);
        std::vector<uint8_t> out(kHeader + kEntry * written.size(), 0u);
        auto put = [&](size_t at, uint64_t value, size_t bytes)
        {
            for (size_t i = 0; i < bytes; ++i)
                out[at + i] = static_cast<uint8_t>(value >> (8u * i));
        };
        std::memcpy(out.data(), "PXTP", 4);
        put(4, 1u, 4);
        put(8, written.size(), 4);
        // Data already written: the picture, or the same bytes.
        std::unordered_map<const Picture *, size_t> byPicture;
        // FNV-1a of the data -> offset and the picture written there (shared
        // only by pictures of the same name, size and levels, as parsePack
        // gives them back as one).
        std::unordered_multimap<uint64_t, std::pair<size_t, const Picture *>> byHash;
        for (size_t i = 0; i < written.size(); ++i)
        {
            const Entry &e = *written[i];
            const Picture &p = *e.picture;
            size_t offset = 0;
            if (const auto it = byPicture.find(&p); it != byPicture.end())
                offset = it->second;
            else
            {
                const uint64_t hash = indexHash(p.data.data(), p.data.size());
                bool found = false;
                for (auto [it2, end] = byHash.equal_range(hash); it2 != end && !found; ++it2)
                {
                    const Picture &q = *it2->second.second;
                    if (q.name == p.name && q.width == p.width && q.height == p.height && q.mips == p.mips &&
                        q.format == p.format && q.data == p.data)
                    {
                        offset = it2->second.first;
                        found = true;
                    }
                }
                if (!found)
                {
                    offset = out.size();
                    out.insert(out.end(), p.data.begin(), p.data.end());
                    byHash.emplace(hash, std::make_pair(offset, &p));
                }
                byPicture[&p] = offset;
            }
            const size_t at = kHeader + i * kEntry;
            put(at, e.width, 2);
            put(at + 2, e.height, 2);
            out[at + 4] = indexBits;
            put(at + 8, e.indexHash, 8);
            put(at + 16, p.width, 2);
            put(at + 18, p.height, 2);
            out[at + 20] = static_cast<uint8_t>(p.mips);
            out[at + 21] = static_cast<uint8_t>(p.format);
            put(at + 24, offset, 4);
            put(at + 28, p.data.size(), 4);
            std::memcpy(&out[at + 32], p.name.data(), std::min<size_t>(p.name.size(), 32u));
        }
        return out;
    }
}
