// PCSX2 memory card images (collection/card_import.h): reading a save out of
// one and importing it into the app's card folder or a native save file.
#include "collection/card_import.h"
#include "strings.h"

#include "runtime/ps2_save_store.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace zx::app
{
    // ---- PCSX2 memory card images -----------------------------------------------
    // Layout as in tools/ps2mc.py (mymc/PCSX2): 512-byte pages (+16 spare bytes
    // when the image has ECC), clusters of pagesPerCluster pages, a two-level
    // FAT, 512-byte directory entries.

    namespace
    {
        constexpr uint32_t kFatEnd = 0xFFFFFFFFu;
        constexpr uint16_t kModeExists = 0x8000u;
        constexpr uint16_t kModeDir = 0x0020u;
        constexpr uint16_t kModeFile = 0x0010u;

        uint32_t u32At(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        struct CardEntry
        {
            uint16_t mode = 0;
            uint32_t length = 0;
            uint32_t cluster = 0;
            std::string name;
        };

        class CardImage
        {
        public:
            bool open(const std::filesystem::path &path, std::string &error)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    error = "cannot open the card image";
                    return false;
                }
                m_raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (m_raw.size() >= 528u && m_raw.size() % 528u == 0u)
                    m_rawPage = 528u;
                else if (m_raw.size() >= 512u && m_raw.size() % 512u == 0u)
                    m_rawPage = 512u;
                else
                {
                    error = "not a memory card image";
                    return false;
                }
                const uint8_t *sb = m_raw.data();
                if (std::memcmp(sb, "Sony PS2 Memory Card Format ", 28) != 0)
                {
                    error = "the card image is not formatted";
                    return false;
                }
                m_pageLength = static_cast<uint32_t>(sb[0x28] | (sb[0x29] << 8));
                m_pagesPerCluster = static_cast<uint32_t>(sb[0x2A] | (sb[0x2B] << 8));
                m_allocOffset = u32At(sb + 0x34);
                m_root = u32At(sb + 0x3C);
                for (int i = 0; i < 32; ++i)
                    if (const uint32_t ifc = u32At(sb + 0x50 + i * 4))
                        m_ifc.push_back(ifc);
                if (m_pageLength != 512u || m_pagesPerCluster == 0u || m_pagesPerCluster > 16u || m_ifc.empty())
                {
                    error = "unsupported card geometry";
                    return false;
                }
                m_clusterSize = m_pageLength * m_pagesPerCluster;
                return true;
            }

            bool cluster(uint32_t n, std::vector<uint8_t> &out) const
            {
                out.resize(m_clusterSize);
                for (uint32_t i = 0; i < m_pagesPerCluster; ++i)
                {
                    const uint64_t offset = (static_cast<uint64_t>(n) * m_pagesPerCluster + i) * m_rawPage;
                    if (offset + 512u > m_raw.size())
                        return false;
                    std::memcpy(out.data() + i * 512u, m_raw.data() + offset, 512u);
                }
                return true;
            }

            bool fat(uint32_t n, uint32_t &value) const
            {
                const uint32_t perCluster = m_clusterSize / 4u;
                const uint32_t indirect = n / (perCluster * perCluster);
                if (indirect >= m_ifc.size())
                    return false;
                std::vector<uint8_t> data;
                if (!cluster(m_ifc[indirect], data))
                    return false;
                const uint32_t fatCluster = u32At(data.data() + ((n / perCluster) % perCluster) * 4u);
                if (!cluster(fatCluster, data))
                    return false;
                value = u32At(data.data() + (n % perCluster) * 4u);
                return true;
            }

            bool readChain(uint32_t first, uint32_t size, std::vector<uint8_t> &out) const
            {
                out.clear();
                uint32_t n = first;
                std::vector<uint8_t> data;
                for (uint32_t guard = 0; guard < 16384u && out.size() < size; ++guard)
                {
                    if (!cluster(m_allocOffset + n, data))
                        return false;
                    out.insert(out.end(), data.begin(), data.end());
                    uint32_t next = 0;
                    if (!fat(n, next))
                        return false;
                    if (next == kFatEnd)
                        break;
                    if ((next & 0x80000000u) == 0u)
                        return false;
                    n = next & 0x7FFFFFFFu;
                }
                if (out.size() < size)
                    return false;
                out.resize(size);
                return true;
            }

            bool entries(uint32_t first, uint32_t count, std::vector<CardEntry> &out) const
            {
                std::vector<uint8_t> data;
                if (count > 4096u || !readChain(first, count * 512u, data))
                    return false;
                out.clear();
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint8_t *e = data.data() + i * 512u;
                    CardEntry entry;
                    entry.mode = static_cast<uint16_t>(e[0] | (e[1] << 8));
                    entry.length = u32At(e + 4);
                    entry.cluster = u32At(e + 0x10);
                    const char *name = reinterpret_cast<const char *>(e + 0x40);
                    entry.name.assign(name, strnlen(name, 32));
                    out.push_back(std::move(entry));
                }
                return true;
            }

            bool rootEntries(std::vector<CardEntry> &out) const
            {
                std::vector<CardEntry> self;
                if (!entries(m_root, 1u, self) || self.empty())
                    return false;
                return entries(m_root, self[0].length, out);
            }

        private:
            std::vector<uint8_t> m_raw;
            uint32_t m_rawPage = 528u;
            uint32_t m_pageLength = 512u;
            uint32_t m_pagesPerCluster = 2u;
            uint32_t m_clusterSize = 1024u;
            uint32_t m_allocOffset = 0u;
            uint32_t m_root = 0u;
            std::vector<uint32_t> m_ifc;
        };

        bool safeCardName(const std::string &name)
        {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
                   name.find('\\') == std::string::npos;
        }
    }

    bool readCardSave(const std::filesystem::path &cardImage, const std::string &saveDir, CardSave &save, std::string *error)
    {
        std::string message;
        auto fail = [&](const std::string &text)
        {
            if (error)
                *error = text;
            return false;
        };
        CardImage card;
        if (!card.open(cardImage, message))
            return fail(message);
        std::vector<CardEntry> root;
        if (!card.rootEntries(root))
            return fail("the card's root directory cannot be read");
        const auto dir = std::find_if(root.begin(), root.end(), [&](const CardEntry &entry)
                                      { return (entry.mode & kModeExists) && (entry.mode & kModeDir) && entry.name == saveDir; });
        if (dir == root.end())
            return fail("no " + saveDir + " save on this card");
        std::vector<CardEntry> files;
        if (!card.entries(dir->cluster, dir->length, files))
            return fail("the save's directory cannot be read");

        save = {};
        save.name = saveDir;
        for (const CardEntry &file : files)
        {
            if (!(file.mode & kModeExists) || !(file.mode & kModeFile) || !safeCardName(file.name))
                continue;
            std::vector<uint8_t> bytes;
            if (!card.readChain(file.cluster, file.length, bytes))
                return fail("cannot read " + file.name + " from the card");
            save.files.emplace_back(file.name, std::move(bytes));
        }
        if (save.files.empty())
            return fail("the " + saveDir + " save on this card is empty");
        return true;
    }

    bool importCardSave(const std::filesystem::path &cardImage, const std::string &saveDir,
                        const std::filesystem::path &slotFolder, bool overwrite, std::string *error)
    {
        CardSave save;
        if (!readCardSave(cardImage, saveDir, save, error))
            return false;
        const std::filesystem::path target = slotFolder / saveDir;
        std::error_code ec;
        if (std::filesystem::exists(target, ec) && !overwrite)
        {
            if (error)
                *error = "slot already has a " + saveDir + " save";
            return false;
        }
        // Write next to the target, then swap it in.
        const std::filesystem::path staging = slotFolder / (saveDir + ".import");
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(staging, ec);
        if (ec)
        {
            if (error)
                *error = "cannot create " + staging.string();
            return false;
        }
        for (const auto &[name, bytes] : save.files)
        {
            std::ofstream out(staging / name, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out.good())
            {
                if (error)
                    *error = "cannot write " + (staging / name).string();
                std::filesystem::remove_all(staging, ec);
                return false;
            }
        }
        std::filesystem::remove_all(target, ec);
        std::filesystem::rename(staging, target, ec);
        if (ec && error)
            *error = "cannot move the save into place: " + ec.message();
        return !ec;
    }

    bool importCardSaveToFile(const std::filesystem::path &cardImage, const std::string &saveDir,
                              const std::filesystem::path &file, bool overwrite, std::string *error,
                              const std::string &dataFile)
    {
        CardSave save;
        if (!readCardSave(cardImage, saveDir, save, error))
            return false;
        const std::string name = dataFile.empty() ? saveDir : dataFile;
        const auto data = std::find_if(save.files.begin(), save.files.end(), [&](const auto &entry)
                                       { return entry.first == name; });
        if (data == save.files.end())
        {
            if (error)
                *error = "the " + saveDir + " save on this card has no " + name + " file";
            return false;
        }
        std::error_code ec;
        if (std::filesystem::exists(file, ec) && !overwrite)
        {
            if (error)
                *error = file.string() + " already exists";
            return false;
        }
        return zx::saves::writeFileAtomic(file, data->second.data(), data->second.size(), error);
    }

    std::vector<std::filesystem::path> findPcsx2CardsWithSave(const std::string &saveDir, const GetEnv &getEnv)
    {
        std::vector<std::filesystem::path> folders;
        const std::string home = envString(getEnv, "HOME");
#if defined(__APPLE__)
        if (!home.empty())
            folders.push_back(std::filesystem::path(home) / "Library" / "Application Support" / "PCSX2" / "memcards");
#elif defined(_WIN32)
        if (const std::string profile = envString(getEnv, "USERPROFILE"); !profile.empty())
            folders.push_back(std::filesystem::path(profile) / "Documents" / "PCSX2" / "memcards");
#else
        const std::string xdg = envString(getEnv, "XDG_CONFIG_HOME");
        folders.push_back((xdg.empty() ? std::filesystem::path(home) / ".config" : std::filesystem::path(xdg)) / "PCSX2" / "memcards");
#endif
        std::vector<std::filesystem::path> cards;
        for (const auto &folder : folders)
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (lower(entry.path().extension().string()) != ".ps2" || !entry.is_regular_file(ec))
                    continue;
                CardSave save;
                if (readCardSave(entry.path(), saveDir, save))
                    cards.push_back(entry.path());
            }
        }
        std::sort(cards.begin(), cards.end());
        return cards;
    }

}
