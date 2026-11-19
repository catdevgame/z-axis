// Dolphin's RVZ disc images, read as tools/gc_disc.py reads them (the
// format: Dolphin's docs/WiaAndRvz.md). Only what the players' GameCube
// dumps use is read: GameCube discs (no Wii partitions) whose groups are
// Zstandard-compressed (zstd 1.5.7, runtime/cmake/ps2x_zstd.cmake) or
// stored, with RVZ's packing (runs of a disc's junk data kept as the seed
// that regenerates them).
//
// The file: header 1 (0x48 bytes, its own SHA-1 at 0x34) names header 2's
// size and SHA-1; header 2 holds the disc's first 0x80 bytes, the
// compression, the chunk size and where the raw data table and the group
// table are (each compressed as the groups are). A raw data entry covers a
// run of the disc in chunk-size groups; a group is u32 file offset / 4, u32
// size (bit 31: compressed) and u32 packed size (0: not packed). A packed
// group is records of a u32 length (bit 31: junk, followed by the 68-byte
// seed) and that many bytes of data.
#include "rvz_image.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace ps2x::app::extras
{
    namespace
    {
        constexpr uint32_t kZstandard = 5;   // header 2's compression
        constexpr uint32_t kGameCubeDisc = 1; // header 2's disc type
        constexpr uint64_t kJunkBlock = 0x8000; // the junk generator restarts every 32 KiB of the disc
        constexpr size_t kGroupsKept = 16;

        uint32_t be32(const uint8_t *p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

        // ---- SHA-1 (FIPS 180-4), for the headers' own hashes ------------------

        std::array<uint8_t, 20> sha1(const uint8_t *data, size_t size)
        {
            uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
            auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
            std::vector<uint8_t> message(data, data + size);
            const uint64_t bits = uint64_t(size) * 8u;
            message.push_back(0x80);
            while (message.size() % 64 != 56)
                message.push_back(0);
            for (int i = 7; i >= 0; --i)
                message.push_back(uint8_t(bits >> (i * 8)));
            for (size_t chunk = 0; chunk < message.size(); chunk += 64)
            {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = be32(message.data() + chunk + size_t(i) * 4);
                for (int i = 16; i < 80; ++i)
                    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i)
                {
                    uint32_t f, k;
                    if (i < 20)
                        f = (b & c) | (~b & d), k = 0x5A827999;
                    else if (i < 40)
                        f = b ^ c ^ d, k = 0x6ED9EBA1;
                    else if (i < 60)
                        f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
                    else
                        f = b ^ c ^ d, k = 0xCA62C1D6;
                    const uint32_t next = rotl(a, 5) + f + e + k + w[i];
                    e = d, d = c, c = rotl(b, 30), b = a, a = next;
                }
                h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
            }
            std::array<uint8_t, 20> out{};
            for (int i = 0; i < 20; ++i)
                out[i] = uint8_t(h[i / 4] >> (24 - (i % 4) * 8));
            return out;
        }

        // ---- junk data: Dolphin's lagged Fibonacci generator ------------------

        // What a GameCube disc holds between its files: pseudo-random bytes
        // from a 17-word seed, which RVZ keeps in place of the bytes.
        class JunkGenerator
        {
        public:
            static constexpr int kLag = 521;   // words of state
            static constexpr int kShortLag = 32;
            static constexpr int kSeedWords = 17;
            static constexpr size_t kStateBytes = kLag * 4;

            explicit JunkGenerator(const uint8_t *seed)
            {
                for (int i = 0; i < kSeedWords; ++i)
                    m_state[i] = be32(seed + i * 4);
                for (int i = kSeedWords; i < kLag; ++i)
                    m_state[i] = (m_state[i - 17] << 23) ^ (m_state[i - 16] >> 9) ^ m_state[i - 1];
                for (uint32_t &word : m_state)
                    word = (word & 0xFF00FFFF) | ((word >> 2) & 0x00FF0000);
                for (int i = 0; i < 4; ++i)
                    forward();
            }

            void skip(uint64_t count)
            {
                m_position += count;
                while (m_position >= kStateBytes)
                {
                    forward();
                    m_position -= kStateBytes;
                }
            }

            void take(size_t count, uint8_t *out)
            {
                while (count)
                {
                    const size_t n = std::min<size_t>(count, kStateBytes - m_position);
                    for (size_t i = 0; i < n; ++i)
                    {
                        const size_t at = m_position + i;
                        out[i] = uint8_t(m_state[at / 4] >> (24 - (at % 4) * 8));
                    }
                    out += n;
                    m_position += n;
                    count -= n;
                    if (m_position == kStateBytes)
                    {
                        forward();
                        m_position = 0;
                    }
                }
            }

        private:
            void forward()
            {
                for (int i = 0; i < kShortLag; ++i)
                    m_state[i] ^= m_state[i + kLag - kShortLag];
                for (int i = kShortLag; i < kLag; ++i)
                    m_state[i] ^= m_state[i - kShortLag];
            }

            std::array<uint32_t, kLag> m_state{};
            size_t m_position = 0;
        };

        // A packed group expanded to `size` disc bytes starting at `discOffset`.
        bool unpack(const Bytes &packed, uint64_t discOffset, size_t size, Bytes &out)
        {
            out.assign(size, 0);
            size_t at = 0, done = 0;
            while (at + 4 <= packed.size() && done < size)
            {
                const uint32_t word = be32(packed.data() + at);
                at += 4;
                const size_t length = word & 0x7FFFFFFF;
                const size_t n = std::min(length, size - done);
                if (word & 0x80000000)
                {
                    if (at + JunkGenerator::kSeedWords * 4 > packed.size())
                        return false;
                    JunkGenerator junk(packed.data() + at);
                    at += JunkGenerator::kSeedWords * 4;
                    junk.skip(discOffset % kJunkBlock);
                    junk.take(n, out.data() + done);
                }
                else
                {
                    if (at + length > packed.size())
                        return false;
                    std::memcpy(out.data() + done, packed.data() + at, n);
                    at += length;
                }
                discOffset += length;
                done += n;
            }
            return done == size;
        }

        struct RawEntry
        {
            uint64_t discOffset; // where its run of the disc starts
            uint64_t size;
            uint32_t firstGroup;
            uint32_t groups;
        };

        struct Group
        {
            uint64_t fileOffset;
            uint32_t storedSize; // 0: the group is zeros
            bool compressed;
            uint32_t packedSize; // 0: not packed
        };

        class RvzImage final : public DiscImage
        {
        public:
            ~RvzImage() override
            {
                if (m_zstd)
                    ZSTD_freeDCtx(m_zstd);
            }

            bool open(std::unique_ptr<ImageFile> file, std::string *error)
            {
                m_file = std::move(file);
                const std::string name = m_file->name();
                auto fail = [&](const std::string &why)
                {
                    *error = "The GameCube disc image " + name + " " + why;
                    return false;
                };
                uint8_t head1[0x48];
                if (!readAt(0, sizeof head1, head1))
                    return fail("cannot be read.");
                if (std::memcmp(head1, "WIA\x01", 4) == 0)
                    return fail("is a WIA image: convert it to RVZ or ISO in Dolphin.");
                if (sha1(head1, 0x34) != toHash(head1 + 0x34))
                    return fail("is damaged (its first header's hash is wrong).");
                const uint32_t head2Size = be32(head1 + 0x0C);
                m_discSize = be64(head1 + 0x24);
                Bytes head2(head2Size);
                if (head2Size < 0xDC || !readAt(0x48, head2Size, head2.data()))
                    return fail("cannot be read.");
                if (sha1(head2.data(), head2.size()) != toHash(head1 + 0x10))
                    return fail("is damaged (its second header's hash is wrong).");
                const uint8_t *h = head2.data();
                const uint32_t discType = be32(h);
                m_compression = be32(h + 4);
                m_level = int32_t(be32(h + 8));
                m_chunkSize = be32(h + 0x0C);
                std::memcpy(m_discHeader.data(), h + 0x10, m_discHeader.size());
                const uint32_t partitions = be32(h + 0x90);
                if (discType != kGameCubeDisc || partitions != 0)
                    return fail("is not of a GameCube disc.");
                if (m_compression != kZstandard)
                    return fail("is compressed in a way the app does not read: convert it in Dolphin to RVZ with "
                                "Zstandard, or to ISO.");
                if (m_chunkSize == 0)
                    return fail("is damaged (no chunk size).");
                m_zstd = ZSTD_createDCtx();

                const uint32_t rawCount = be32(h + 0xB4);
                Bytes rawTable;
                if (!readTable(be64(h + 0xB8), be32(h + 0xC0), size_t(rawCount) * 24, rawTable))
                    return fail("is damaged (its table of the disc's runs).");
                for (uint32_t i = 0; i < rawCount; ++i)
                {
                    const uint8_t *e = rawTable.data() + size_t(i) * 24;
                    m_raw.push_back({be64(e), be64(e + 8), be32(e + 16), be32(e + 20)});
                }
                const uint32_t groupCount = be32(h + 0xC4);
                Bytes groupTable;
                if (!readTable(be64(h + 0xC8), be32(h + 0xD0), size_t(groupCount) * 12, groupTable))
                    return fail("is damaged (its table of groups).");
                for (uint32_t i = 0; i < groupCount; ++i)
                {
                    const uint8_t *g = groupTable.data() + size_t(i) * 12;
                    const uint32_t stored = be32(g + 4);
                    m_groups.push_back({uint64_t(be32(g)) << 2, stored & 0x7FFFFFFF, (stored & 0x80000000) != 0,
                                        be32(g + 8)});
                }
                for (const RawEntry &entry : m_raw)
                    if (uint64_t(entry.firstGroup) + entry.groups > m_groups.size())
                        return fail("is damaged (a run names groups it does not have).");
                return true;
            }

            uint64_t size() const override { return m_discSize; }

            std::string describe() const override
            {
                return "RVZ, zstd level " + std::to_string(m_level) + ", " + std::to_string(m_chunkSize / 1024) +
                       " KiB chunks, " + std::to_string(m_groups.size()) + " groups, disc " +
                       std::to_string(m_discSize) + " bytes";
            }

            bool read(uint64_t offset, size_t count, uint8_t *out) override
            {
                if (offset > m_discSize || count > m_discSize - offset)
                    return false;
                std::lock_guard<std::mutex> lock(m_mutex);
                const uint64_t end = offset + count;
                if (offset < m_discHeader.size())
                {
                    const size_t n = size_t(std::min<uint64_t>(end, m_discHeader.size()) - offset);
                    std::memcpy(out, m_discHeader.data() + offset, n);
                    out += n;
                    offset += n;
                }
                while (offset < end)
                {
                    const RawEntry *entry = nullptr;
                    for (const RawEntry &e : m_raw)
                        if (e.discOffset - e.discOffset % kJunkBlock <= offset && offset < e.discOffset + e.size)
                        {
                            entry = &e;
                            break;
                        }
                    if (!entry)
                    {
                        // Not in any run: zeros (none on a GameCube disc's dump).
                        uint64_t next = end;
                        for (const RawEntry &e : m_raw)
                            if (e.discOffset > offset)
                                next = std::min(next, e.discOffset);
                        std::memset(out, 0, size_t(next - offset));
                        out += next - offset;
                        offset = next;
                        continue;
                    }
                    // A run's groups start at the junk block holding its start.
                    const uint64_t base = entry->discOffset - entry->discOffset % kJunkBlock;
                    const uint64_t limit = entry->discOffset + entry->size;
                    const uint64_t index = (offset - base) / m_chunkSize;
                    if (index >= entry->groups)
                        return false;
                    const uint64_t groupStart = base + index * m_chunkSize;
                    const size_t groupSize = size_t(std::min<uint64_t>(m_chunkSize, limit - groupStart));
                    const Bytes *data = group(entry->firstGroup + uint32_t(index), groupStart, groupSize);
                    if (!data)
                        return false;
                    const size_t n = size_t(std::min<uint64_t>(end, groupStart + groupSize) - offset);
                    std::memcpy(out, data->data() + (offset - groupStart), n);
                    out += n;
                    offset += n;
                }
                return true;
            }

        private:
            static std::array<uint8_t, 20> toHash(const uint8_t *p)
            {
                std::array<uint8_t, 20> out{};
                std::memcpy(out.data(), p, out.size());
                return out;
            }

            bool readAt(uint64_t offset, size_t count, uint8_t *out)
            {
                return m_file->readAt(offset, count, out);
            }

            // A zstd frame's bytes; `expected` is how many the format says.
            bool decompress(const Bytes &stored, size_t expected, Bytes &out)
            {
                const unsigned long long known = ZSTD_getFrameContentSize(stored.data(), stored.size());
                const size_t capacity = (known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR)
                                            ? std::max<size_t>(size_t(known), expected)
                                            : expected;
                out.resize(capacity);
                const size_t got = ZSTD_decompressDCtx(m_zstd, out.data(), out.size(), stored.data(), stored.size());
                if (ZSTD_isError(got))
                    return false;
                out.resize(got);
                return true;
            }

            bool readTable(uint64_t offset, uint32_t storedSize, size_t size, Bytes &out)
            {
                Bytes stored(storedSize);
                return readAt(offset, storedSize, stored.data()) && decompress(stored, size, out) && out.size() >= size;
            }

            // A group's disc bytes (kept for the next reads).
            const Bytes *group(uint32_t index, uint64_t discOffset, size_t size)
            {
                for (auto it = m_kept.begin(); it != m_kept.end(); ++it)
                    if (it->first == index)
                    {
                        std::rotate(m_kept.begin(), it, it + 1);
                        return &m_kept.front().second;
                    }
                const Group &g = m_groups[index];
                Bytes data;
                if (g.storedSize == 0)
                    data.assign(size, 0);
                else
                {
                    Bytes stored(g.storedSize);
                    if (!readAt(g.fileOffset, g.storedSize, stored.data()))
                        return nullptr;
                    if (g.compressed)
                    {
                        if (!decompress(stored, g.packedSize ? g.packedSize : size, data))
                            return nullptr;
                    }
                    else
                        data = std::move(stored);
                    if (g.packedSize)
                    {
                        data.resize(std::min<size_t>(data.size(), g.packedSize));
                        Bytes unpacked;
                        if (!unpack(data, discOffset, size, unpacked))
                            return nullptr;
                        data = std::move(unpacked);
                    }
                }
                if (data.size() < size)
                    return nullptr;
                data.resize(size);
                if (m_kept.size() == kGroupsKept)
                    m_kept.pop_back();
                m_kept.insert(m_kept.begin(), {index, std::move(data)});
                return &m_kept.front().second;
            }

            std::unique_ptr<ImageFile> m_file;
            std::mutex m_mutex;
            ZSTD_DCtx *m_zstd = nullptr;
            uint64_t m_discSize = 0;
            uint32_t m_compression = 0;
            int32_t m_level = 0;
            uint32_t m_chunkSize = 0;
            std::array<uint8_t, 0x80> m_discHeader{};
            std::vector<RawEntry> m_raw;
            std::vector<Group> m_groups;
            std::vector<std::pair<uint32_t, Bytes>> m_kept; // most recent first
        };
    }

    std::unique_ptr<DiscImage> openRvzImage(std::unique_ptr<ImageFile> file, std::string *error)
    {
        auto image = std::make_unique<RvzImage>();
        if (!image->open(std::move(file), error))
            return nullptr;
        return image;
    }
}
