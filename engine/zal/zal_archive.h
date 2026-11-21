#pragma once

// The games' .ZAL archives, read and written (the zal tool, and the game
// file system's mod entries: runtime/game_files.h, docs/MODS.md).
//
// A .ZAL is one of two formats, both little endian:
//
//   - a ZDIR archive (the engine's asset library, zge/assets/asset_library.cpp):
//     "ZDIR" <size>, then the chunks DIDF (the entry count and a reserved
//     word), GRID (each entry's id: the hash of its name, nameHash), DLEN
//     (each entry's length, its alignment code in the top four bits: 5 for
//     16 bytes, 6 for 32), OFST (each entry's offset in the file) and, in some
//     of BMX XXX's, NAME (the entries' names, each ended by a zero, padded with
//     zeros to four bytes). The entries follow in the tables' order, each at
//     its alignment, the file ending at the last one's.
//   - a zfile library (zge/assets/zfile_library.h): the magic 0x2A81511C,
//     twelve header bytes, the asset and group counts at 0x10, then per group
//     its id and an entry per asset {offset from byte 0x10, length (24 bits)};
//     the assets follow in that order, each at four bytes, the file ending at
//     four bytes too.
//
// Every archive on the three games' PS2 discs is laid out that way, so an
// archive read and written again is the same file, byte for byte: the bytes
// between entries (zeros in most, left-over bytes in some of BMX XXX's) are
// kept with the entry before them and written back while the gap is the
// same size. Compressed entries (ZDIR's CLEN, a compressed or external zfile
// library) are not on any disc and are not read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zx::zal
{
    enum class Format
    {
        Directory, // a ZDIR archive
        Library,   // a zfile library
    };

    struct Entry
    {
        uint32_t id = 0;    // Directory: its name's hash (GRID); Library: its group's id
        uint32_t asset = 0; // Library: its place in its group; 0 in a Directory
        // Directory: the alignment code (DLEN's top four bits);
        // Library: the length word's top byte (0 on every disc).
        uint32_t flags = 0;
        std::string name; // a Directory with names (NAME): its name; else ""
        std::vector<uint8_t> data;
        // The bytes after it up to the next entry (or the end of the file), as
        // the archive has them: written back while the gap is this size, zeros
        // otherwise.
        std::vector<uint8_t> padding;
    };

    struct Archive
    {
        Format format = Format::Directory;
        bool names = false;    // Directory: it has a NAME chunk
        uint32_t reserved = 0; // Directory: DIDF's second word (0 on every disc)
        std::array<uint8_t, 12> header{}; // Library: the bytes after the magic (zeros on every disc)
        uint16_t assetsPerGroup = 1;      // Library
        std::vector<uint8_t> leadPadding; // the bytes between the directory and the first entry
        std::vector<Entry> entries;       // in the archive's order
    };

    // The engine's name hash (zge::nameHash): the ELF hash of the name with
    // ASCII letters upper-cased.
    uint32_t nameHash(std::string_view name);

    // An entry's alignment in the file, in bytes.
    uint32_t alignmentOf(const Archive &archive, const Entry &entry);
    // A Directory's alignment code for an alignment in bytes (16 -> 5), 0 if
    // it is not a power of two from 1 byte to 16 KB.
    uint32_t alignmentCode(uint32_t bytes);

    // The bytes at the start of a file its directory takes (the header and
    // the tables), from at least its first 20 bytes; 0 when it is not an
    // archive of either format.
    uint64_t directoryBytes(const uint8_t *start, size_t size);

    // A whole archive: false (with why) when it is not one, or is one this
    // reads not (compressed), or is damaged.
    bool read(const std::vector<uint8_t> &file, Archive &out, std::string *error = nullptr);
    // Its directory alone (directoryBytes of the file): every entry's id,
    // asset, flags and name, no data.
    bool readDirectory(const uint8_t *start, size_t size, Archive &out, std::string *error = nullptr);
    // The archive's file, laid out as the discs' are. False (with why) when
    // it cannot be (a Library whose entries do not fill its groups, an
    // alignment with no code).
    bool write(const Archive &archive, std::vector<uint8_t> &out, std::string *error = nullptr);

    // ---- entries by file name (an unpacked folder's, a mod's ASSETS/<NAME>.ZAL/<entry>) ----

    // "0x1A2B3C4D": the name of an entry known by its id alone.
    std::string idName(uint32_t id);
    // A Library entry's name: its group's id ("12"), with its place in the
    // group when groups hold more than one asset ("12-0").
    std::string libraryName(const Archive &archive, const Entry &entry);
    // The entry a file name means, the first if more have it (as the game
    // finds the first): in a Directory "0x1A2B3C4D" (any case, an extension
    // after it allowed: "0x1A2B3C4D.zsc") is that id, and any other name the
    // id its hash is (any case: "CommonFonts.zif"); in a Library a name as
    // libraryName gives it, an extension allowed ("12.mot"). None when the
    // archive has no such entry.
    std::optional<size_t> findEntry(const Archive &archive, std::string_view fileName);

    // ---- an unpacked archive: a file per entry and zal.txt (zal_folder.cpp) ----
    //
    // zal.txt lists the archive's entries in its order, a line each, with
    // what else the archive needs to be made again byte for byte:
    //
    //   format zdir            (or zfile)
    //   names no               zdir: whether it has a NAME chunk (its names are then the files' names)
    //   align 32               zdir: the entries' alignment in bytes, unless an entry says otherwise
    //   assets 1               zfile: assets a group
    //   lead 00ff...           the bytes before the first entry, when they are not zeros
    //   entry 0x0A1B2C3D CommonFonts.zif
    //     pad 00ff...          the bytes after it, when they are not zeros
    //     align 16             its own alignment (zdir)
    //     name Common Fonts    its name in NAME, when its file is named otherwise (zdir)
    //   entry 12 12            zfile: its group's id and its file
    //
    // An entry's id may be "-": the hash of its file's name (an entry added
    // by hand). Lines starting with # are comments.
    constexpr const char *kListingFile = "zal.txt";

    struct UnpackedFile
    {
        std::string name; // a file name, no folders
        std::vector<uint8_t> bytes;
    };
    struct Unpacked
    {
        std::string listing; // zal.txt's text
        std::vector<UnpackedFile> files;
    };
    // The archive as files (`title`: its name, for zal.txt's first line).
    // A Directory's entries are named by its NAME chunk; else by a name in
    // `nameSources` (any bytes: the game's program, other archives; the
    // archive's own are always read) whose hash is the entry's id; else
    // idName. A Library's by libraryName.
    Unpacked unpack(const Archive &archive, std::string_view title, const std::vector<const std::vector<uint8_t> *> &nameSources = {});
    // An archive from zal.txt's text and its files (`readFile`: a file
    // beside it by name, false when there is none). False (with why) on a
    // line that does not read or a file missing.
    bool repack(std::string_view listing, const std::function<bool(const std::string &name, std::vector<uint8_t> &bytes)> &readFile,
                Archive &out, std::string *error = nullptr);
    // Whether a name can be a file's on every host (Windows' rules too) and
    // is not zal.txt.
    bool fileNameOk(std::string_view name);
}
