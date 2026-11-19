// CSO and ZSO disc images: an ISO cut in blocks of equal size, each stored
// compressed or as it is, found through an index of their offsets. CSO as
// maxcso 1.13.0's README_CSO.md documents it; ZSO is CSO v1's layout with LZ4
// blocks (maxcso --format=zso, ziso.py):
//
//   header (little endian): magic "CISO" or "ZISO", header size, the ISO's
//   size (u64), block size (u32), version (u8), index shift (u8), 2 unused;
//   then ceil(size / block size) + 1 index entries (u32): entry i's low 31
//   bits << index shift are where block i starts, entry i + 1's where it ends.
//   CSO v0/v1: the high bit set means stored, else raw deflate.
//   CSO v2: a block taking block size or more bytes is stored; else the high
//   bit set means LZ4, clear deflate.
//   ZSO: the high bit set means stored, else LZ4.
//
// Deflate is libchdr's miniz (ps2x_chdr.cmake), LZ4 the pinned LZ4 1.10.0.
#include "disc_image_formats.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <lz4.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace ps2x::iop::disc_formats
{
    namespace
    {
        constexpr size_t kHeaderBytes = 24u;
        constexpr uint32_t kMaxBlockBytes = 16u * 1024u * 1024u;
        constexpr uint32_t kHighBit = 0x80000000u;

        uint32_t le32(const uint8_t *bytes)
        {
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
                   (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
        }

        class CompressedIso final : public DiscImageFile
        {
        public:
            CompressedIso(OwnedFile file, Format format) : m_file(std::move(file)), m_format(format) {}

            bool init(std::string *error)
            {
                auto fail = [&](const char *message)
                {
                    if (error)
                        *error = message;
                    return false;
                };
                const int64_t fileBytes = fileSize(m_file.get());
                uint8_t header[kHeaderBytes];
                if (fileBytes < static_cast<int64_t>(kHeaderBytes) || !readAt(m_file.get(), 0u, header, sizeof(header)))
                    return fail("a compressed ISO without its header");
                m_size = static_cast<uint64_t>(le32(header + 8)) | (static_cast<uint64_t>(le32(header + 12)) << 32u);
                m_blockBytes = le32(header + 16);
                m_version = header[20];
                m_indexShift = header[21];
                if (m_blockBytes == 0u || m_blockBytes > kMaxBlockBytes || m_indexShift > 31u)
                    return fail("a compressed ISO with a damaged header");
                const uint64_t blocks = (m_size + m_blockBytes - 1u) / m_blockBytes;
                if ((blocks + 1u) * 4u > static_cast<uint64_t>(fileBytes) - kHeaderBytes)
                    return fail("a compressed ISO shorter than its index");
                std::vector<uint8_t> index(static_cast<size_t>(blocks + 1u) * 4u);
                if (!readAt(m_file.get(), kHeaderBytes, index.data(), index.size()))
                    return fail("a compressed ISO whose index cannot be read");
                m_index.resize(static_cast<size_t>(blocks + 1u));
                for (size_t i = 0; i < m_index.size(); ++i)
                    m_index[i] = le32(index.data() + i * 4u);
                m_block.resize(m_blockBytes);
                return true;
            }

            Format format() const override { return m_format; }
            uint64_t size() const override { return m_size; }

            bool read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset > m_size || size > m_size - offset)
                    return false;
                auto *out = static_cast<uint8_t *>(destination);
                while (size != 0u)
                {
                    const uint64_t block = offset / m_blockBytes;
                    const size_t within = static_cast<size_t>(offset % m_blockBytes);
                    const size_t count = std::min<size_t>(size, m_blockBytes - within);
                    if (!loadBlock(block))
                        return false;
                    std::memcpy(out, m_block.data() + within, count);
                    out += count;
                    offset += count;
                    size -= count;
                }
                return true;
            }

        private:
            uint64_t positionOf(uint32_t entry) const { return static_cast<uint64_t>(entry & ~kHighBit) << m_indexShift; }

            bool loadBlock(uint64_t block)
            {
                if (static_cast<int64_t>(block) == m_loadedBlock)
                    return true;
                m_loadedBlock = -1;
                const uint32_t entry = m_index[static_cast<size_t>(block)];
                const uint64_t start = positionOf(entry), end = positionOf(m_index[static_cast<size_t>(block) + 1u]);
                // The block's bytes (the last block may be short).
                const size_t wanted = static_cast<size_t>(std::min<uint64_t>(m_blockBytes, m_size - block * m_blockBytes));
                if (end < start || end - start > kMaxBlockBytes + (static_cast<uint64_t>(1u) << m_indexShift))
                    return false;
                const size_t stored = static_cast<size_t>(end - start);
                enum class Packing { Stored, Deflate, Lz4 } packing;
                if (m_format == Format::Zso)
                    packing = (entry & kHighBit) ? Packing::Stored : Packing::Lz4;
                else if (m_version >= 2u)
                    packing = stored >= m_blockBytes ? Packing::Stored : (entry & kHighBit) ? Packing::Lz4 : Packing::Deflate;
                else
                    packing = (entry & kHighBit) ? Packing::Stored : Packing::Deflate;

                if (packing == Packing::Stored)
                {
                    if (stored < wanted || !readAt(m_file.get(), start, m_block.data(), wanted))
                        return false;
                }
                else
                {
                    m_packed.resize(stored);
                    if (!readAt(m_file.get(), start, m_packed.data(), stored))
                        return false;
                    if (packing == Packing::Deflate)
                    {
                        // Raw deflate (no zlib header); the padding after it is not read.
                        const size_t made = tinfl_decompress_mem_to_mem(m_block.data(), wanted, m_packed.data(), stored, 0);
                        if (made != wanted)
                            return false;
                    }
                    else
                    {
                        // Decoding stops at the block's size: the padding after it is not read.
                        const int made = LZ4_decompress_safe_partial(reinterpret_cast<const char *>(m_packed.data()),
                                                                     reinterpret_cast<char *>(m_block.data()), static_cast<int>(stored),
                                                                     static_cast<int>(wanted), static_cast<int>(m_block.size()));
                        if (made != static_cast<int>(wanted))
                            return false;
                    }
                }
                m_loadedBlock = static_cast<int64_t>(block);
                return true;
            }

            OwnedFile m_file;
            Format m_format;
            uint64_t m_size = 0u;
            uint32_t m_blockBytes = 0u;
            uint8_t m_version = 0u;
            uint8_t m_indexShift = 0u;
            std::vector<uint32_t> m_index;
            std::vector<uint8_t> m_block, m_packed;
            int64_t m_loadedBlock = -1;
        };
    }

    std::unique_ptr<DiscImageFile> openCompressedIso(OwnedFile file, DiscImageFile::Format format, std::string *error)
    {
        auto image = std::make_unique<CompressedIso>(std::move(file), format);
        if (!image->init(error))
            return nullptr;
        return image;
    }
}
