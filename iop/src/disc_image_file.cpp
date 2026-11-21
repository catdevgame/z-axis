// A disc image's file read as the image it holds (zx/disc/disc_image_file.h).
#include "disc_image_formats.h"

#include <cstring>

namespace zx::disc
{
    namespace formats
    {
        bool seekTo(std::FILE *file, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        int64_t fileSize(std::FILE *file)
        {
            // From the file itself: one opened through a descriptor (a
            // document the system's picker gave) has no path to ask.
#if defined(_WIN32)
            return _fseeki64(file, 0, SEEK_END) == 0 ? static_cast<int64_t>(_ftelli64(file)) : -1;
#else
            return fseeko(file, 0, SEEK_END) == 0 ? static_cast<int64_t>(ftello(file)) : -1;
#endif
        }

        bool readAt(std::FILE *file, uint64_t offset, void *destination, size_t size)
        {
            return seekTo(file, offset) && std::fread(destination, 1, size, file) == size;
        }
    }

    namespace
    {
        using namespace formats;

        // The image's own bytes.
        class PlainImage final : public DiscImageFile
        {
        public:
            PlainImage(OwnedFile file, uint64_t size) : m_file(std::move(file)), m_size(size) {}
            Format format() const override { return Format::Plain; }
            uint64_t size() const override { return m_size; }
            bool read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset > m_size || size > m_size - offset)
                    return false;
                return size == 0u || readAt(m_file.get(), offset, destination, size);
            }

        private:
            OwnedFile m_file;
            uint64_t m_size = 0u;
        };
    }

    std::unique_ptr<DiscImageFile> openDiscImageFile(std::FILE *file, std::string *error)
    {
        auto fail = [&](const char *message) -> std::unique_ptr<DiscImageFile>
        {
            if (error)
                *error = message;
            return nullptr;
        };
        OwnedFile owned(file);
        if (!owned)
            return fail("cannot open the file");
        const int64_t size = fileSize(owned.get());
        if (size < 0)
            return fail("cannot read the file's size");
        char magic[8] = {};
        const bool hasMagic = size >= static_cast<int64_t>(sizeof(magic)) && readAt(owned.get(), 0u, magic, sizeof(magic));
        if (hasMagic && std::memcmp(magic, "MComprHD", 8) == 0)
            return openChd(std::move(owned), error);
        if (hasMagic && std::memcmp(magic, "CISO", 4) == 0)
            return openCompressedIso(std::move(owned), DiscImageFile::Format::Cso, error);
        if (hasMagic && std::memcmp(magic, "ZISO", 4) == 0)
            return openCompressedIso(std::move(owned), DiscImageFile::Format::Zso, error);
        return std::make_unique<PlainImage>(std::move(owned), static_cast<uint64_t>(size));
    }

    const char *discImageFormatName(DiscImageFile::Format format)
    {
        switch (format)
        {
        case DiscImageFile::Format::Chd:
            return "CHD";
        case DiscImageFile::Format::Cso:
            return "CSO";
        case DiscImageFile::Format::Zso:
            return "ZSO";
        case DiscImageFile::Format::Plain:
            break;
        }
        return "";
    }
}
