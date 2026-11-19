#pragma once

// The size of a part's VIF packet (zge/render/packet.h), from its strip
// groups alone: for the readable packet builder (the part in RAM) and for
// the packaging step's model converter (zge/native/xbox_models.h, the part
// in a file). No game state; header-only.
//
// The class builders (buildRigid rigid, buildTerrain terrain,
// buildSkinned skinned) write the groups in chunks of at most 60 entries
// (chunkEntries; whole triangles for a list): per chunk a 24-byte unpack
// header, three streams (their VIF codes 16 + 8 + 16 bytes, then per entry
// 24 bytes for rigid and terrain parts, 28 for skinned ones), MSCAL or MSCNT
// (4), padded to a quadword; a material change first pads to a quadword and
// opens the material's DMA tag (RET, 8 bytes: its VIF half is the next
// chunk's). The packet ends padded to a quadword. These are the sizes each
// stream's writer skips when that stream is not rebuilt (a rebuild keeps
// the layout).

#include "zge/memory/alignment.h"

#include <cstdint>

namespace zge
{
    // Bytes per strip entry of a render type's packet (0: no builder).
    constexpr uint32_t packetBytesPerEntry(uint32_t renderType)
    {
        switch (renderType)
        {
        case 0x04: // rigid prelit
        case 0x18: // sky
        case 0x20: // terrain
            return 24u;
        case 0x08: // skinned
        case 0x0C: // skinned prelit
            return 28u;
        default:
            return 0u;
        }
    }

    // A packet's size as its groups are walked: begin(), group() per strip
    // group in order, bytes() at the end.
    class PacketSize
    {
    public:
        static constexpr uint32_t kMaxChunkEntries = 60u;
        static constexpr uint32_t kChunkFixed = 24u + 16u + 8u + 16u + 4u;
        static constexpr uint32_t kMaterialTag = 8u;

        // startAlignment: the packet's address modulo 16 (0, 4, 8, 12).
        PacketSize(uint32_t bytesPerEntry, uint32_t startAlignment)
            : m_perEntry(bytesPerEntry), m_start(startAlignment), m_at(startAlignment)
        {
        }

        // A group: its mode (header byte 0, bits 0-1; 1 a triangle list),
        // its material (byte 1) and its entry count.
        void group(uint32_t mode, uint32_t material, uint32_t count)
        {
            for (uint32_t start = 0; start < count;)
            {
                uint32_t n = count - start;
                if (n > kMaxChunkEntries)
                    n = kMaxChunkEntries;
                if (mode == 1u)
                    n -= n % 3u;
                if (n == 0u)
                    return; // a list's last one or two entries: the builder would never end
                if (material != m_material)
                {
                    m_material = material;
                    m_at = roundUp16(m_at) + kMaterialTag;
                }
                m_at = roundUp16(m_at + kChunkFixed + m_perEntry * n);
                m_entries += n;
                start += n;
            }
        }

        uint32_t bytes() const { return roundUp16(m_at) - m_start; }
        // Strip entries sent (the builders count them in 13 bits of the
        // part's type word).
        uint32_t entries() const { return m_entries; }

    private:

        uint32_t m_perEntry, m_start, m_at;
        uint32_t m_material = 0xFFu;
        uint32_t m_entries = 0;
    };

    // The most padding a word-aligned packet start can add (a start 4 past a
    // quadword pads 12 before the first tag).
    constexpr uint32_t kPacketWorstStartAlignment = 4u;
}
