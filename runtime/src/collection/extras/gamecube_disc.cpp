// A GameCube disc image's FST (collection/extras/gamecube_disc.h).
#include "collection/extras/gamecube_disc.h"

namespace ps2x::app::extras
{
    namespace
    {
        constexpr uint32_t kMagic = 0xC2339F3D;
        constexpr size_t kHeaderSize = 0x440;
        constexpr size_t kEntrySize = 12;

        uint32_t be32(const uint8_t *p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        // Latin-1 text up to its first zero, as UTF-8.
        std::string latin1(const uint8_t *p, size_t limit)
        {
            std::string out;
            for (size_t i = 0; i < limit && p[i]; ++i)
            {
                if (p[i] < 0x80)
                    out += char(p[i]);
                else
                {
                    out += char(0xC0 | (p[i] >> 6));
                    out += char(0x80 | (p[i] & 0x3F));
                }
            }
            return out;
        }
    }

    bool GameCubeDisc::open(const std::filesystem::path &path, std::string *error)
    {
        std::unique_ptr<ImageFile> file = ImageFile::open(path, error);
        return file && open(std::move(file), error);
    }

    bool GameCubeDisc::open(std::unique_ptr<ImageFile> file, std::string *error)
    {
        const std::string name = file->name();
        m_image = openDiscImage(std::move(file), error);
        if (!m_image)
            return false;
        uint8_t head[kHeaderSize];
        if (!m_image->read(0, sizeof head, head))
        {
            *error = "The disc image " + name + " cannot be read.";
            return false;
        }
        if (be32(head + 0x1C) != kMagic)
        {
            *error = name + " is not a GameCube disc image.";
            return false;
        }
        m_gameId = std::string(reinterpret_cast<const char *>(head), 6);
        m_title = latin1(head + 0x20, 0x400 - 0x20);
        m_dolOffset = be32(head + 0x420);
        const uint32_t fstOffset = be32(head + 0x424), fstSize = be32(head + 0x428);
        Bytes fst(fstSize);
        auto damaged = [&]
        {
            *error = "The GameCube disc image " + name + " is damaged (its file table cannot be read).";
            return false;
        };
        if (fstSize < kEntrySize || !m_image->read(fstOffset, fstSize, fst.data()))
            return damaged();
        const uint32_t count = be32(fst.data() + 8);
        const size_t names = size_t(count) * kEntrySize;
        if (names > fst.size())
            return damaged();
        struct Folder
        {
            uint32_t end; // the index past its last entry
            std::string path;
        };
        std::vector<Folder> folders = {{count, ""}};
        m_files.clear();
        for (uint32_t i = 1; i < count; ++i)
        {
            while (!folders.empty() && i >= folders.back().end)
                folders.pop_back();
            const uint8_t *entry = fst.data() + size_t(i) * kEntrySize;
            const uint32_t word = be32(entry), a = be32(entry + 4), b = be32(entry + 8);
            const size_t at = names + (word & 0xFFFFFF);
            if (at >= fst.size())
                return damaged();
            const std::string path = (folders.empty() ? "" : folders.back().path) + latin1(fst.data() + at, fst.size() - at);
            if (word >> 24)
                folders.push_back({b, path + "/"});
            else
                m_files.push_back({path, a, b});
        }
        return true;
    }
}
