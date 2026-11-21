#pragma once

#include "disc/disc_image_file.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zx::disc
{
    // Read-only view of an ISO9660 disc image: 2048-byte sectors (the PS2 DVD
    // and CD images PCSX2 uses), or a raw CD image of 2352-byte sectors (the
    // BIN of a BIN/CUE dump, Mode 1 or Mode 2 Form 1), whose 2048 bytes of
    // user data each sector holds are read as if the image had only those;
    // either one kept in a compressed file (CHD, CSO, ZSO: disc_image_file.h).
    // Offsets and sizes are the 2048-byte view's. Paths are looked up case-insensitively,
    // with "/" or "\" separators, an optional device prefix ("cdrom0:") and an
    // optional ";1" version suffix. Safe to use from several threads.
    struct IsoEntry
    {
        uint32_t lsn = 0u;
        uint32_t size = 0u;
        bool directory = false;
        std::string name; // identifier as on the disc, without ";1"
    };

    class Iso9660Image
    {
    public:
        static constexpr uint32_t kSectorSize = 2048u;
        static constexpr uint32_t kRawSectorSize = 2352u;
        static constexpr uint32_t kMode2SectorSize = 2336u; // Mode 2 without sync and header (a CHD's CD track)

        Iso9660Image() = default;
        ~Iso9660Image();
        Iso9660Image(const Iso9660Image &) = delete;
        Iso9660Image &operator=(const Iso9660Image &) = delete;

        // Opens the image and reads the primary volume descriptor.
        bool open(const std::filesystem::path &path, std::string *error = nullptr);
        // The same for a file already open (a document the system's picker
        // gave, read through its descriptor): the image owns it from here
        // (closed even when it is no image), and `name` stands for its path.
        bool open(std::FILE *file, const std::filesystem::path &name, std::string *error = nullptr);
        void close();
        [[nodiscard]] bool isOpen() const noexcept { return m_image != nullptr; }
        [[nodiscard]] const std::filesystem::path &path() const noexcept { return m_path; }
        // The 2048-byte view's size.
        [[nodiscard]] uint64_t sizeBytes() const noexcept { return m_sizeBytes; }
        // The file the image is kept in: its own bytes, or compressed (CHD, CSO, ZSO).
        [[nodiscard]] DiscImageFile::Format format() const noexcept;
        // How the image holds its sectors: 2048 (user data only), 2352 (raw)
        // or 2336 (Mode 2 without sync and header), and where a sector's user
        // data starts (16: Mode 1, 24: Mode 2 Form 1 raw, 8: Mode 2 Form 1).
        [[nodiscard]] uint32_t fileSectorSize() const noexcept { return m_fileSectorSize; }
        [[nodiscard]] uint32_t userDataOffset() const noexcept { return m_userDataOffset; }
        [[nodiscard]] const std::string &volumeId() const noexcept { return m_volumeId; }

        [[nodiscard]] std::optional<IsoEntry> find(std::string_view path) const;
        [[nodiscard]] std::vector<IsoEntry> list(std::string_view directoryPath) const;

        // Raw byte access; reads past the end of the image fail.
        bool read(uint64_t offset, void *destination, size_t size) const;
        bool readFile(const IsoEntry &entry, std::vector<uint8_t> &bytes, size_t maxBytes = SIZE_MAX) const;

        // Splits a guest or host style path into upper-case disc components.
        [[nodiscard]] static std::vector<std::string> pathComponents(std::string_view path);

        // One image per path for the whole process (the runtime's CD image).
        [[nodiscard]] static std::shared_ptr<const Iso9660Image> shared(const std::filesystem::path &path);

    private:
        const std::vector<IsoEntry> *directory(uint32_t lsn, uint32_t size) const;
        // The 2048-byte view's bytes [offset, offset + size); m_mutex held.
        bool readView(uint64_t offset, void *destination, size_t size) const;

        std::filesystem::path m_path;
        std::unique_ptr<DiscImageFile> m_image;
        uint64_t m_sizeBytes = 0u;
        uint32_t m_fileSectorSize = kSectorSize;
        uint32_t m_userDataOffset = 0u;
        std::string m_volumeId;
        IsoEntry m_root;
        mutable std::mutex m_mutex;
        mutable std::map<uint32_t, std::vector<IsoEntry>> m_directories;
    };
}
