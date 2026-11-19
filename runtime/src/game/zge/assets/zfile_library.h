#pragma once

// Zfile libraries (the GameCube and Xbox builds call the record a
// ZfileLibraryHandle): a packed file of assets in groups,
// every group with the same number of assets. The file starts with a header,
// then the directory: per group its id (a word) and an entry per asset; the
// assets follow. An open library keeps a cursor on one asset's entry. The
// scene database reads its packed animation libraries this way
// (loadAnimationLibrary: one motion file per group). The library is game code
// in AI and DM2 (the same in both): the engine calls it through zge/game.h,
// AI's is game/aggressive/zfile_library.cpp.
//
// A library's assets are stored as they are, compressed (the entries are
// then 24 bytes), or kept in files of their own whose names the library
// stores in their place.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/assets/file.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // The header the library is read from: a word that, when it is this
    // magic, says the counts sit at +0x10 (else at +8); the directory
    // starts after them.
    constexpr uint32_t kZfileLibraryMagic = 0x2A81511Cu;
    constexpr uint32_t kZfileLibraryHeaderBytes = 28;
    constexpr uint32_t kZfileLibraryCountsAt = 8, kZfileLibraryMagicCountsAt = 0x10;
    // An entry's offset counts from the file's 16th byte.
    constexpr uint32_t kZfileLibraryDataStart = 0x10;

    // The header's counts and flags: two halfwords.
    struct ZfileLibraryCounts
    {
        uint16_t assets; // the low 13 bits: assets a group; 1 << 13: external; 1 << 15: compressed (2)
        uint16_t groups; // the low 14 bits: groups; 1 << 14: LZO; 1 << 15: compressed (1)
    };
    static_assert(sizeof(ZfileLibraryCounts) == 4);
    constexpr uint16_t kZfileAssetsMask = 0x1FFF, kZfileExternalBit = 1u << 13;
    constexpr uint16_t kZfileGroupsMask = 0x3FFF, kZfileLzoBit = 1u << 14;
    constexpr uint16_t kZfileCompressedBit = 1u << 15;

    // A directory entry: 8 bytes, 24 in a compressed library.
    struct ZfileLibraryEntry
    {
        uint32_t offset;      // +0x00 from kZfileLibraryDataStart
        uint32_t length : 24; // +0x04 the asset's bytes (decompressed; an external asset's name's)
        uint32_t : 8;         // nothing in the PS2 builds reads these
    };
    struct ZfileLibraryCompressedEntry
    {
        ZfileLibraryEntry entry;  // +0x00
        uint8_t _08[8];           // +0x08 nothing in the PS2 builds reads these
        uint32_t storedBytes;     // +0x10 the compressed bytes in the file
        uint8_t _14[4];           // +0x14 nothing in the PS2 builds reads these
    };
    static_assert(sizeof(ZfileLibraryEntry) == 8 && sizeof(ZfileLibraryCompressedEntry) == 24);

    // An open library (a record of g_zfileLibraryPool, 0x20 a block).
    struct ZfileLibrary
    {
        ee::Ptr<ZFile> file;        // +0x00
        uint8_t _04[4];             // +0x04 nothing reads or writes it
        uint16_t groupCount;        // +0x08
        uint16_t assetsPerGroup;    // +0x0A
        uint32_t cursor;            // +0x0C the current asset's entry
        uint32_t directory;         // +0x10 heap: the groups, each its id then its entries
        uint32_t directoryBytes;    // +0x14
        uint16_t entryBytes;        // +0x18 8, or 24 when compressed
        uint16_t compressed;        // +0x1A the header's two compressed bits (0: stored)
        uint16_t lzo;               // +0x1C 0: LZ, 1: LZO (zcompres's methods 1 and 2)
        uint8_t external;           // +0x1E the assets are files of their own
        char externalName[0x101];   // +0x1F the last external asset's file name
        uint32_t externalEntry;     // +0x120 the entry whose name externalName holds
        uint32_t externalBytes;     // +0x124 that file's size
    };
    static_assert(offsetof(ZfileLibrary, cursor) == 0x0C && offsetof(ZfileLibrary, entryBytes) == 0x18 &&
                  offsetof(ZfileLibrary, external) == 0x1E && offsetof(ZfileLibrary, externalName) == 0x1F &&
                  offsetof(ZfileLibrary, externalEntry) == 0x120 && sizeof(ZfileLibrary) == 0x128);

}

namespace zge
{
    // (decompress.cpp)
    uint32_t decompressFileData(uint32_t method, ee::Ptr<uint8_t> source, uint32_t bytes, ee::Ptr<uint8_t> output, ee::Ptr<uint32_t> bytesOut);
}
