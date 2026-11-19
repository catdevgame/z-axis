#pragma once

// The file a disc image is kept in, read as the image it holds: the image's
// own bytes (an ISO's 2048-byte sectors, the raw 2352-byte sectors of a BIN),
// or one of the compressed formats players keep their PS2 dumps in: CHD
// (MAME's, which most emulators take: a CD's or a DVD's), CSO and ZSO
// (compressed ISOs). The format is told by the file's first bytes, never by
// its name. Iso9660Image reads every disc image through one of these.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace ps2x::iop
{
    class DiscImageFile
    {
    public:
        enum class Format
        {
            Plain, // the image's own bytes (ISO, BIN)
            Chd,
            Cso,
            Zso,
        };

        virtual ~DiscImageFile() = default;
        [[nodiscard]] virtual Format format() const = 0;
        // The image's size in bytes (uncompressed).
        [[nodiscard]] virtual uint64_t size() const = 0;
        // The size of the sectors the format says the image holds (a CHD's
        // CD track: 2048, 2336 for Mode 2 sectors without their sync and
        // header, or 2352), 0 when it says none: the bytes tell.
        [[nodiscard]] virtual uint32_t sectorSize() const { return 0u; }
        // The image's bytes [offset, offset + size); false past its end or
        // when the file cannot be read. One caller at a time.
        virtual bool read(uint64_t offset, void *destination, size_t size) = 0;
    };

    // The disc image `file` holds (open for reading, its position anywhere);
    // the result owns the file, which is closed when there is none. Null with
    // *error when the file is a compressed image that cannot be read (a CHD
    // that needs its parent, a damaged header).
    std::unique_ptr<DiscImageFile> openDiscImageFile(std::FILE *file, std::string *error = nullptr);

    // "CHD", "CSO", "ZSO"; "" for a plain image.
    const char *discImageFormatName(DiscImageFile::Format format);
}
