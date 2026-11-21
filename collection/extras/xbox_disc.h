#pragma once

// An Xbox disc image's files (its XDVDFS volume), as tools/modding/xbox_files.py
// extract reads them: an XISO (the volume descriptor "MICROSOFT*XBOX*MEDIA"
// at 0x10000) or a full image whose game partition starts at one of the
// offsets the dumps use.
//
// XDVDFS, by the volume descriptor at sector 32 of the partition: the root
// directory table's sector (u32) and size (u32) after the 20-byte magic. A
// directory table is a binary tree of entries, each at a 4-byte multiple:
// u16 left and right subtree (in 4-byte units, 0 none, 0xFFFF a padding
// entry), u32 first sector, u32 size, u8 attributes (0x10 a directory, whose
// sector and size are its own table's), u8 name length, then the name.
// Every value little-endian; sectors 2,048 bytes from the partition's start.

#include "collection/extras/disc_image.h"

namespace zx::app::extras
{
    class XboxDisc final : public DiscFiles
    {
    public:
        // False with *error in plain words: not an Xbox disc image, or no
        // default.xbe (not a game disc).
        bool open(const std::filesystem::path &path, std::string *error);
        // The same from an image file already open (ImageFile::adopt).
        bool open(std::unique_ptr<ImageFile> file, std::string *error);
        // Where the volume starts in the image (0 for an XISO).
        uint64_t partitionOffset() const { return m_partition; }

    private:
        bool walk(uint32_t sector, uint32_t size, const std::string &folder, int depth);
        uint64_t m_partition = 0;
    };
}
