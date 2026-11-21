#pragma once

// The compressed disc image formats behind openDiscImageFile
// (zx/disc/disc_image_file.h), each in its own file.

#include "zx/disc/disc_image_file.h"

#include <cstdio>
#include <memory>
#include <string>

namespace zx::disc::formats
{
    // Closes the file it holds (a disc image file's own).
    struct FileCloser
    {
        void operator()(std::FILE *file) const
        {
            if (file)
                std::fclose(file);
        }
    };
    using OwnedFile = std::unique_ptr<std::FILE, FileCloser>;

    bool seekTo(std::FILE *file, uint64_t offset);
    // The file's size, -1 when it cannot be told.
    int64_t fileSize(std::FILE *file);
    bool readAt(std::FILE *file, uint64_t offset, void *destination, size_t size);

    // A CHD ("MComprHD"): disc_image_chd.cpp, through libchdr.
    std::unique_ptr<DiscImageFile> openChd(OwnedFile file, std::string *error);
    // A CSO ("CISO", deflate or LZ4 blocks) or a ZSO ("ZISO", LZ4 blocks):
    // disc_image_cso.cpp.
    std::unique_ptr<DiscImageFile> openCompressedIso(OwnedFile file, DiscImageFile::Format format, std::string *error);
}
