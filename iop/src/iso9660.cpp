#include "ps2x/iop/iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>
#include <unordered_map>

namespace ps2x::iop
{
    namespace
    {
        constexpr uint32_t kPrimaryVolumeDescriptorLsn = 16u;
        constexpr uint32_t kMaxDirectoryBytes = 4u * 1024u * 1024u;

        uint32_t le32(const uint8_t *bytes)
        {
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
                   (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
        }

        std::string upperAscii(std::string_view text)
        {
            std::string out(text);
            for (char &c : out)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        }

        std::string stripVersion(std::string_view identifier)
        {
            const size_t semicolon = identifier.find(';');
            std::string_view name = semicolon == std::string_view::npos ? identifier : identifier.substr(0, semicolon);
            // "NAME." is how ISO9660 spells a file without an extension.
            if (!name.empty() && name.back() == '.')
                name.remove_suffix(1);
            return std::string(name);
        }

        bool parseRecord(const uint8_t *record, size_t available, IsoEntry &entry, bool &special)
        {
            const uint8_t length = record[0];
            if (length < 34u || length > available)
                return false;
            const uint8_t nameLength = record[32];
            if (33u + nameLength > length)
                return false;
            entry.lsn = le32(record + 2);
            entry.size = le32(record + 10);
            entry.directory = (record[25] & 0x02u) != 0u;
            special = nameLength == 1u && (record[33] == 0u || record[33] == 1u);
            entry.name = special ? std::string() : stripVersion(std::string_view(reinterpret_cast<const char *>(record + 33), nameLength));
            return true;
        }
    }

    Iso9660Image::~Iso9660Image() = default;

    bool Iso9660Image::open(const std::filesystem::path &path, std::string *error)
    {
        std::error_code ec;
        std::filesystem::file_size(path, ec);
        if (ec)
        {
            close();
            if (error)
                *error = "cannot read the file (" + ec.message() + ")";
            return false;
        }
#if defined(_WIN32)
        std::FILE *file = ::_wfopen(path.c_str(), L"rb");
#else
        std::FILE *file = std::fopen(path.string().c_str(), "rb");
#endif
        if (!file)
        {
            close();
            if (error)
                *error = "cannot open the file";
            return false;
        }
        return open(file, path, error);
    }

    void Iso9660Image::close()
    {
        std::lock_guard lock(m_mutex);
        m_image.reset();
        m_directories.clear();
    }

    bool Iso9660Image::open(std::FILE *file, const std::filesystem::path &name, std::string *error)
    {
        auto fail = [&](const std::string &message)
        {
            if (error)
                *error = message;
            m_image.reset();
            return false;
        };

        std::lock_guard lock(m_mutex);
        m_directories.clear();
        m_path = name;
        m_fileSectorSize = kSectorSize;
        m_userDataOffset = 0u;
        // The file as the image it holds: its own bytes, or a compressed
        // image's (CHD, CSO, ZSO), by its first bytes.
        std::string why;
        m_image = openDiscImageFile(file, &why);
        if (!m_image)
            return fail(why);
        const uint64_t fileBytes = m_image->size();

        // The layout: the sectors the format says (a CHD's CD track), else
        // 2048-byte sectors when the primary volume descriptor is where they
        // put it, else a raw CD image (2352-byte sectors: the sync pattern,
        // then the header, whose mode byte says where the user data starts).
        uint8_t pvd[kSectorSize];
        auto readPvd = [&](uint32_t sectorSize, uint32_t dataOffset)
        {
            m_fileSectorSize = sectorSize;
            m_userDataOffset = dataOffset;
            m_sizeBytes = fileBytes / sectorSize * kSectorSize;
            return m_sizeBytes >= (kPrimaryVolumeDescriptorLsn + 1u) * static_cast<uint64_t>(kSectorSize) &&
                   readView(kPrimaryVolumeDescriptorLsn * static_cast<uint64_t>(kSectorSize), pvd, sizeof(pvd)) &&
                   pvd[0] == 1u && std::memcmp(pvd + 1, "CD001", 5) == 0;
        };
        auto readRawPvd = [&]()
        {
            static constexpr uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
            uint8_t header[16];
            return fileBytes >= (kPrimaryVolumeDescriptorLsn + 1u) * static_cast<uint64_t>(kRawSectorSize) &&
                   m_image->read(kPrimaryVolumeDescriptorLsn * static_cast<uint64_t>(kRawSectorSize), header, sizeof(header)) &&
                   std::memcmp(header, kSync, sizeof(kSync)) == 0 && (header[15] == 1u || header[15] == 2u) &&
                   readPvd(kRawSectorSize, header[15] == 1u ? 16u : 24u);
        };
        bool found = false;
        switch (m_image->sectorSize())
        {
        case kSectorSize:
            found = readPvd(kSectorSize, 0u);
            break;
        case kRawSectorSize:
            found = readRawPvd();
            break;
        case kMode2SectorSize:
            // Mode 2 sectors without sync and header: the 8-byte subheader, then Form 1's data.
            found = readPvd(kMode2SectorSize, 8u);
            break;
        default:
            found = readPvd(kSectorSize, 0u) || (fileBytes % kRawSectorSize == 0u && readRawPvd());
            break;
        }
        if (!found)
        {
            const bool small = fileBytes < (kPrimaryVolumeDescriptorLsn + 1u) * static_cast<uint64_t>(kSectorSize);
            m_fileSectorSize = kSectorSize;
            m_userDataOffset = 0u;
            return fail(small ? "the file is too small to be a disc image"
                              : "not an ISO9660 disc image (no primary volume descriptor)");
        }
        if ((pvd[128] | (pvd[129] << 8)) != kSectorSize)
            return fail("unsupported sector size");

        std::string volumeId(reinterpret_cast<const char *>(pvd + 40), 32);
        volumeId.erase(volumeId.find_last_not_of(' ') + 1);
        m_volumeId = volumeId;

        bool special = false;
        if (!parseRecord(pvd + 156, 34, m_root, special) || !m_root.directory)
            return fail("bad root directory record");
        m_root.name.clear();
        return true;
    }

    DiscImageFile::Format Iso9660Image::format() const noexcept
    {
        return m_image ? m_image->format() : DiscImageFile::Format::Plain;
    }

    const std::vector<IsoEntry> *Iso9660Image::directory(uint32_t lsn, uint32_t size) const
    {
        // m_mutex held by the caller.
        auto cached = m_directories.find(lsn);
        if (cached != m_directories.end())
            return &cached->second;
        if (!m_image || size == 0u || size > kMaxDirectoryBytes)
            return nullptr;
        const uint64_t offset = static_cast<uint64_t>(lsn) * kSectorSize;
        if (offset + size > m_sizeBytes)
            return nullptr;

        std::vector<uint8_t> bytes(size);
        if (!readView(offset, bytes.data(), bytes.size()))
            return nullptr;

        std::vector<IsoEntry> entries;
        size_t pos = 0u;
        while (pos < bytes.size())
        {
            const size_t inSector = kSectorSize - (pos % kSectorSize);
            if (bytes[pos] == 0u)
            {
                // Records never cross a sector; zero padding up to the next one.
                pos += inSector;
                continue;
            }
            IsoEntry entry;
            bool special = false;
            if (!parseRecord(bytes.data() + pos, std::min(inSector, bytes.size() - pos), entry, special))
                break;
            pos += bytes[pos];
            if (!special)
                entries.push_back(std::move(entry));
        }
        return &m_directories.emplace(lsn, std::move(entries)).first->second;
    }

    std::vector<std::string> Iso9660Image::pathComponents(std::string_view path)
    {
        const size_t colon = path.find(':');
        if (colon != std::string_view::npos)
            path.remove_prefix(colon + 1);
        std::vector<std::string> components;
        size_t begin = 0u;
        while (begin <= path.size())
        {
            size_t end = path.find_first_of("/\\", begin);
            if (end == std::string_view::npos)
                end = path.size();
            const std::string_view part = path.substr(begin, end - begin);
            begin = end + 1u;
            if (part.empty() || part == ".")
                continue;
            components.push_back(upperAscii(stripVersion(part)));
        }
        return components;
    }

    std::optional<IsoEntry> Iso9660Image::find(std::string_view path) const
    {
        std::lock_guard lock(m_mutex);
        if (!m_image)
            return std::nullopt;
        IsoEntry current = m_root;
        for (const std::string &component : pathComponents(path))
        {
            if (component == ".." || !current.directory)
                return std::nullopt;
            const std::vector<IsoEntry> *entries = directory(current.lsn, current.size);
            if (!entries)
                return std::nullopt;
            const auto match = std::find_if(entries->begin(), entries->end(), [&](const IsoEntry &entry)
                                            { return upperAscii(entry.name) == component; });
            if (match == entries->end())
                return std::nullopt;
            current = *match;
        }
        return current;
    }

    std::vector<IsoEntry> Iso9660Image::list(std::string_view directoryPath) const
    {
        const std::optional<IsoEntry> dir = find(directoryPath);
        if (!dir || !dir->directory)
            return {};
        std::lock_guard lock(m_mutex);
        const std::vector<IsoEntry> *entries = directory(dir->lsn, dir->size);
        return entries ? *entries : std::vector<IsoEntry>{};
    }

    bool Iso9660Image::read(uint64_t offset, void *destination, size_t size) const
    {
        if (size == 0u)
            return true;
        std::lock_guard lock(m_mutex);
        if (!m_image || offset > m_sizeBytes || size > m_sizeBytes - offset)
            return false;
        return readView(offset, destination, size);
    }

    bool Iso9660Image::readView(uint64_t offset, void *destination, size_t size) const
    {
        if (m_fileSectorSize == kSectorSize)
            return m_image->read(offset, destination, size);
        // A raw image: each sector's user data in turn.
        auto *out = static_cast<uint8_t *>(destination);
        while (size != 0u)
        {
            const uint64_t sector = offset / kSectorSize;
            const uint32_t within = static_cast<uint32_t>(offset % kSectorSize);
            const size_t count = std::min<size_t>(size, kSectorSize - within);
            if (!m_image->read(sector * m_fileSectorSize + m_userDataOffset + within, out, count))
                return false;
            out += count;
            offset += count;
            size -= count;
        }
        return true;
    }

    bool Iso9660Image::readFile(const IsoEntry &entry, std::vector<uint8_t> &bytes, size_t maxBytes) const
    {
        if (entry.directory || entry.size > maxBytes)
            return false;
        bytes.resize(entry.size);
        return read(static_cast<uint64_t>(entry.lsn) * kSectorSize, bytes.data(), bytes.size());
    }

    std::shared_ptr<const Iso9660Image> Iso9660Image::shared(const std::filesystem::path &path)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::weak_ptr<const Iso9660Image>> images;
        if (path.empty())
            return nullptr;
        std::lock_guard lock(mutex);
        const std::string key = path.lexically_normal().string();
        if (auto existing = images[key].lock())
            return existing;
        auto image = std::make_shared<Iso9660Image>();
        if (!image->open(path))
            return nullptr;
        images[key] = image;
        return image;
    }
}
