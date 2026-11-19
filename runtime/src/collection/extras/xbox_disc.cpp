// An Xbox disc image's XDVDFS volume (collection/extras/xbox_disc.h).
#include "collection/extras/xbox_disc.h"

#include <cstring>

namespace ps2x::app::extras
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr uint64_t kVolumeDescriptor = 0x10000; // sector 32 of the partition
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
        constexpr uint8_t kDirectory = 0x10;
        constexpr uint16_t kPadding = 0xFFFF;
        constexpr int kDeepestFolder = 64;
        // Where the game partition starts: an XISO (0), and the full
        // images' layouts (Redump's, and the two older dump tools').
        constexpr uint64_t kPartitionOffsets[] = {0, 0x18300000, 0x0FD90000, 0x02080000};

        uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    }

    bool XboxDisc::open(const std::filesystem::path &path, std::string *error)
    {
        std::unique_ptr<ImageFile> file = ImageFile::open(path, error);
        return file && open(std::move(file), error);
    }

    bool XboxDisc::open(std::unique_ptr<ImageFile> file, std::string *error)
    {
        const std::string name = file->name();
        m_image = openDiscImage(std::move(file), error);
        if (!m_image)
            return false;
        for (uint64_t base : kPartitionOffsets)
        {
            uint8_t head[28];
            if (!m_image->read(base + kVolumeDescriptor, sizeof head, head) || std::memcmp(head, kMagic, 20) != 0)
                continue;
            m_partition = base;
            m_files.clear();
            if (!walk(le32(head + 20), le32(head + 24), "", 0))
            {
                *error = "The Xbox disc image " + name + " is damaged (its folders cannot be read).";
                return false;
            }
            if (!find("default.xbe"))
            {
                *error = "The Xbox disc image " + name + " has no default.xbe: it is not a game disc.";
                return false;
            }
            return true;
        }
        *error = name + " is not an Xbox disc image.";
        return false;
    }

    // Every file below a directory table, depth first as tools/xbox_files.py
    // walks it (each subtree's right side before its left).
    bool XboxDisc::walk(uint32_t sector, uint32_t size, const std::string &folder, int depth)
    {
        if (size == 0)
            return true;
        if (depth > kDeepestFolder)
            return false;
        Bytes table(size);
        if (!m_image->read(m_partition + sector * kSector, size, table.data()))
            return false;
        std::vector<uint32_t> pending = {0};
        std::vector<bool> seen(size / 4 + 1, false);
        while (!pending.empty())
        {
            const uint32_t index = pending.back();
            pending.pop_back();
            const size_t at = size_t(index) * 4;
            if (index < seen.size() && seen[index])
                continue;
            if (at + 14 > table.size())
                continue;
            seen[index] = true;
            const uint8_t *entry = table.data() + at;
            const uint16_t left = le16(entry), right = le16(entry + 2);
            if (left == kPadding)
                continue;
            const uint32_t start = le32(entry + 4), length = le32(entry + 8);
            const uint8_t attributes = entry[12], nameLength = entry[13];
            if (at + 14 + nameLength > table.size())
                return false;
            const std::string path = folder + std::string(reinterpret_cast<const char *>(entry + 14), nameLength);
            if (left)
                pending.push_back(left);
            if (right)
                pending.push_back(right);
            if (attributes & kDirectory)
            {
                if (!walk(start, length, path + "/", depth + 1))
                    return false;
            }
            else
                m_files.push_back({path, m_partition + uint64_t(start) * kSector, length});
        }
        return true;
    }
}
