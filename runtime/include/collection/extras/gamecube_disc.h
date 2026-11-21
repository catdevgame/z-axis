#pragma once

// A GameCube disc image's files (its FST), as tools/gc_disc.py reads them,
// from a plain GCM/ISO or an RVZ (collection/extras/disc_image.h).
//
// The disc header: the game ID (6 bytes) at 0, the magic 0xC2339F3D at
// 0x1C, the title at 0x20 (to 0x400), and at 0x420 main.dol's offset, the
// FST's offset and its size (u32 each). The FST: entries of 12 bytes, the
// root's first (its third word the entry count), then the names. An entry:
// u8 kind (1 a directory) and a u24 offset into the names, then a file's
// disc offset and size, or a directory's parent and the index past its
// last entry. Every value big-endian.

#include "collection/extras/disc_image.h"

namespace zx::app::extras
{
    class GameCubeDisc final : public DiscFiles
    {
    public:
        // False with *error in plain words: not a GameCube disc (no magic),
        // an image that cannot be read, a damaged FST.
        bool open(const std::filesystem::path &path, std::string *error);
        // The same from an image file already open (ImageFile::adopt).
        bool open(std::unique_ptr<ImageFile> file, std::string *error);
        const std::string &gameId() const { return m_gameId; } // "GILE51"
        const std::string &title() const { return m_title; }  // "Aggressive Inline"
        // main.dol's offset in the disc's bytes.
        uint64_t dolOffset() const { return m_dolOffset; }

    private:
        std::string m_gameId;
        std::string m_title;
        uint64_t m_dolOffset = 0;
    };
}
