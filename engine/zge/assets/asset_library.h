#pragma once

// The asset libraries (zassetLib.c, handleDidfChunk-freeAnimationPools; zge/assets/asset_library.cpp): a .ZAL
// file (UILIB.ZAL, FX.ZAL, OVERLAY.ZAL, SKATERS\MOTIONS.ZAL) starts with a ZDIR form, its
// directory: DIDF (the entry count and a word nothing reads), GRID (an id per entry), OFST (each
// entry's offset in the file), DLEN (each entry's length, packed with its alignment), and in a
// compressed library CLEN (the compressed lengths, packed the same way) and NAME (the entries'
// names, one after another). The entries follow the directory. An open library keeps a current
// entry: findAsset selects one by id, readAsset reads it (inflating it when compressed).

#include "ee/ee.h"
#include "zge/assets/file.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // A packed length: the length in the low 28 bits, the alignment above
    // (log2 of it plus 1; 0: 16 bytes). The game's libraries align to 16.
    constexpr uint32_t kAssetLengthMask = 0x0FFFFFFFu;
    constexpr uint32_t kAssetAlignmentShift = 28;
    constexpr uint32_t kAssetDefaultAlignmentCode = 5;

    struct AssetLibrary
    {
        ee::Ptr<ZFile> file;    // +0x00
        int32_t count;          // +0x04 DIDF: the entries
        uint32_t reserved;      // +0x08 DIDF's second word (0 in the game's libraries; nothing reads it)
        int32_t current;        // +0x0C the selected entry
        uint32_t ids;           // +0x10 GRID: uint32_t[count]
        uint32_t offsets;       // +0x14 OFST: uint32_t[count]
        uint32_t lengths;       // +0x18 DLEN: packed lengths, uint32_t[count]
        uint32_t packedLengths; // +0x1C CLEN: the compressed (stored) lengths, or 0
        uint32_t names;         // +0x20 NAME: the names, each after the last's 0, or 0
    };
    static_assert(offsetof(AssetLibrary, current) == 0x0C && offsetof(AssetLibrary, names) == 0x20 &&
                  sizeof(AssetLibrary) == 0x24);
}

namespace zge
{
    // (asset_library.cpp)
    uint32_t initAssetLibraries();
    uint32_t shutDownAssetLibraries();
    uint32_t openAssetLibrary(ee::Address path);
    uint32_t closeAssetLibrary(uint32_t library);
    uint32_t findAsset(uint32_t library, uint32_t id);
    uint32_t readAsset(uint32_t library, uint32_t dst, ee::Address callback);
    void forEachAsset(uint32_t library, uint32_t function, ee::Address context);
    uint32_t currentAsset(uint32_t library);
    uint32_t currentAssetId(uint32_t library);
    uint32_t selectAsset(uint32_t library, uint32_t index);
    uint32_t currentAssetLength(uint32_t library);
    // (ZGE4 only) the selected entry's FORM parsed, its chunks handed to their handlers with the context.
    uint32_t parseCurrentAsset(uint32_t library, ee::Address context);
    uint64_t currentAssetAlignment(uint32_t library);
}
