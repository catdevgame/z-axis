// Zfile libraries: a packed file of assets in groups (zge/assets/zfile_library.h).
// DM2's own code (its game block, after physics.c), word for word Aggressive
// Inline's (game/aggressive/zfile_library.cpp; the work database's pairs are
// identical, all eleven). The engine reads its packed animation libraries
// through six of these (zge/game.h), so the file keeps the engine's
// namespace: in DM2's instance these define the instance's names.

#include "zge/assets/zfile_library.h"
#include "zge/game.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/assets/file.h"

#include <cstdint>
#include "sym/dm2/zfile_library.cpp.h"

namespace zge
{
    void nextZfileLibraryAsset(ee::Ptr<ZfileLibrary> library); // (below)

    namespace
    {
        // ZfileLibrary records, made at the first open and destroyed when the
        // last library closes.
        inline ee::Global<uint32_t, sym::g_zfileLibraryPool> g_zfileLibraryPool;
        constexpr uint32_t kLibrariesPerBlock = 0x20;
        // zcompres's methods (decompress.cpp).
        constexpr uint32_t kMethodLz = 1, kMethodLzo = 2;

        ee::Ptr<Pool> libraryPool() { return ee::Ptr<Pool>(*g_zfileLibraryPool); }
        uint32_t groupBytes(const ZfileLibrary &library) { return library.entryBytes * library.assetsPerGroup + 4u; }
        // A group in the directory starts with its id.
        uint32_t groupId(uint32_t group) { return *ee::Ptr<uint32_t>(group); }
    }

    // A walk over the groups that does nothing (its body is compiled out:
    // the original counts the groups down). Nothing calls it.
    void walkZfileLibraryGroups(ee::Ptr<ZfileLibrary> library)
    {
        for (uint32_t left = library->groupCount; left != 0; --left)
        {
        }
    }
    READABLE_FUNCTION(walkZfileLibraryGroups, Draft);

    // openZfileLibrary: the library at `path` opened, its directory read and
    // the cursor on the first group's first asset; 0 when the file is not
    // there or not whole.
    // Kept from the original: a failed header or directory read closes the
    // file but keeps the record (and the directory) from the pool and heap,
    // and a failed directory allocation keeps the file open as well.
    ee::Ptr<ZfileLibrary> openZfileLibrary(ee::Address path)
    {
        ee::Local<ZfileLibraryCounts> counts;
        ee::Local<uint32_t[kZfileLibraryHeaderBytes / 4]> header;
        if (*g_zfileLibraryPool == 0)
            *g_zfileLibraryPool = createPool(sizeof(ZfileLibrary), kLibrariesPerBlock, 0);
        const ee::Ptr<ZfileLibrary> library(poolAlloc(libraryPool()));
        if (!library)
            return ee::Ptr<ZfileLibrary>(0u);

        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
        {
            poolFree(libraryPool(), library);
            return ee::Ptr<ZfileLibrary>(0u);
        }
        fileSeek(file, 0, kSeekFromStart, 0);
        if (fileRead(file, header, kZfileLibraryHeaderBytes, 0) != static_cast<int32_t>(kZfileLibraryHeaderBytes))
        {
            closeFile(file, 0);
            return ee::Ptr<ZfileLibrary>(0u);
        }
        const uint32_t countsAt = header[0] == kZfileLibraryMagic ? kZfileLibraryMagicCountsAt : kZfileLibraryCountsAt;
        memoryCopy(counts.address(), header.address() + countsAt, sizeof(ZfileLibraryCounts));
        const uint32_t directoryAt = countsAt + sizeof(ZfileLibraryCounts);

        library->file = file;
        library->compressed = ((counts->assets & kZfileCompressedBit) >> 14) + (counts->groups >> 15);
        library->lzo = (counts->groups & kZfileLzoBit) >> 14;
        library->entryBytes = library->compressed != 0 ? sizeof(ZfileLibraryCompressedEntry) : sizeof(ZfileLibraryEntry);
        library->groupCount = counts->groups & kZfileGroupsMask;
        library->assetsPerGroup = counts->assets & kZfileAssetsMask;
        library->directoryBytes = library->groupCount * groupBytes(*library);
        library->external = (counts->assets & kZfileExternalBit) != 0;
        library->directory = heapAllocate(library->directoryBytes);
        if (library->directory == 0)
            return ee::Ptr<ZfileLibrary>(0u);

        fileSeek(file, directoryAt, kSeekFromStart, 0);
        if (fileRead(file, library->directory, library->directoryBytes, 0) != static_cast<int32_t>(library->directoryBytes))
        {
            closeFile(file, 0);
            return ee::Ptr<ZfileLibrary>(0u);
        }
        library->cursor = library->directory + 4; // past the first group's id
        return library;
    }
    READABLE_FUNCTION_WAITS(openZfileLibrary, Verified);

    // closeZfileLibrary: the file closed, the directory freed, the record
    // given back; the pool goes with the last library.
    void closeZfileLibrary(ee::Ptr<ZfileLibrary> library)
    {
        closeFile(library->file, 0);
        heapFree(library->directory);
        poolFree(libraryPool(), library);
        if (zge::poolCount(libraryPool()) == 0)
        {
            destroyPool(libraryPool());
            *g_zfileLibraryPool = 0;
        }
    }
    READABLE_FUNCTION_WAITS(closeZfileLibrary, Verified);

    uint32_t zfileLibraryGroupCount(ee::Ptr<ZfileLibrary> library)
    {
        return library->groupCount;
    }
    READABLE_FUNCTION(zfileLibraryGroupCount, Verified);

    // nextZfileLibraryGroup: the cursor on the next group's first asset; 0
    // (the cursor where it was) after the last group.
    uint32_t nextZfileLibraryGroup(ee::Ptr<ZfileLibrary> library)
    {
        const uint32_t bytes = groupBytes(*library);
        const int32_t next = static_cast<int32_t>((library->cursor - library->directory - 4) / bytes + 1);
        if (next >= static_cast<int32_t>(library->groupCount))
            return 0;
        library->cursor = next * bytes + library->directory + 4;
        return 1;
    }
    READABLE_FUNCTION(nextZfileLibraryGroup, Verified);

    // findZfileLibraryAsset: the cursor on asset `asset` of the group whose
    // id is `group` (looked for at its own index first, then from the
    // start); 0 when no group has that id. The asset is not checked against
    // the group's count.
    uint32_t findZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, uint32_t group, uint32_t asset)
    {
        const uint32_t bytes = groupBytes(*library);
        const auto select = [&](uint32_t groupEntry) {
            library->cursor = groupEntry + library->entryBytes * (asset & 0xFFFFu) + 4;
            return 1u;
        };
        if (group < library->groupCount)
        {
            const uint32_t atIndex = library->directory + group * bytes;
            if (groupId(atIndex) == group)
                return select(atIndex);
        }
        uint32_t groupEntry = library->directory;
        for (int32_t left = library->groupCount; left > 0; --left, groupEntry += bytes)
            if (groupId(groupEntry) == group)
                return select(groupEntry);
        return 0;
    }
    READABLE_FUNCTION(findZfileLibraryAsset, Verified);

    // zfileLibraryAssetBytes: the current asset's bytes. An external asset's
    // name is read into the library first and its file's size taken.
    uint32_t zfileLibraryAssetBytes(ee::Ptr<ZfileLibrary> library)
    {
        ee::Local<uint32_t> bytes;
        const ee::Ptr<ZfileLibraryEntry> entry(library->cursor);
        *bytes = entry->length;
        if (library->external == 0)
            return *bytes;

        const ee::Ptr<char> name(library->externalName);
        fileSeek(library->file, entry->offset + kZfileLibraryDataStart, kSeekFromStart, 0);
        fileRead(library->file, name, *bytes, 0);
        const ee::Ptr<ZFile> asset = openFile(name, kFileModeRead, 0).file();
        fileSize(asset, bytes, 0);
        closeFile(asset, 0);
        library->externalBytes = *bytes;
        library->externalEntry = library->cursor;
        return *bytes;
    }
    READABLE_FUNCTION_WAITS(zfileLibraryAssetBytes, Verified);

    uint32_t zfileLibraryAssetsPerGroup(ee::Ptr<ZfileLibrary> library)
    {
        return library->assetsPerGroup;
    }
    READABLE_FUNCTION(zfileLibraryAssetsPerGroup, Draft);

    // readZfileLibraryAsset: the current asset read into dst (decompressed,
    // or from its own file), then the cursor on the next asset. 0 when the
    // library's read came up short.
    // Kept from the original: an external asset whose name was read before
    // (zfileLibraryAssetBytes) is read without a check, the compressed
    // data's buffer is not checked, and a compressed library of another
    // method than LZ or LZO reads nothing into dst.
    uint32_t readZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, ee::Address dst)
    {
        ee::Local<uint32_t> bytes;
        ee::Local<uint32_t> decompressedBytes;
        uint32_t read = 1;
        const ee::Ptr<ZfileLibraryEntry> entry(library->cursor);
        if (library->compressed == 0)
            *bytes = entry->length;
        else
            *bytes = ee::Ptr<ZfileLibraryCompressedEntry>(library->cursor)->storedBytes;
        fileSeek(library->file, entry->offset + kZfileLibraryDataStart, kSeekFromStart, 0);

        if (library->compressed != 0)
        {
            const uint32_t stored = heapAllocate(*bytes);
            if (fileRead(library->file, stored, *bytes, 0) != static_cast<int32_t>(*bytes))
                read = 0;
            if (library->lzo == 0)
                zge::decompressFileData(kMethodLz, ee::Ptr<uint8_t>(stored), *bytes, ee::Ptr<uint8_t>(dst), ee::Ptr<uint32_t>(decompressedBytes));
            else if (library->lzo == 1)
                zge::decompressFileData(kMethodLzo, ee::Ptr<uint8_t>(stored), *bytes, ee::Ptr<uint8_t>(dst), ee::Ptr<uint32_t>(decompressedBytes));
            heapFree(stored);
        }
        else if (library->external == 0)
        {
            if (fileRead(library->file, dst, *bytes, 0) != static_cast<int32_t>(*bytes))
                read = 0;
        }
        else
        {
            const ee::Ptr<char> name(library->externalName);
            if (library->externalEntry == library->cursor)
            {
                const ee::Ptr<ZFile> asset = openFile(name, kFileModeRead, 0).file();
                fileRead(asset, dst, library->externalBytes, 0);
                closeFile(asset, 0);
            }
            else
            {
                if (fileRead(library->file, name, *bytes, 0) != static_cast<int32_t>(*bytes))
                    read = 0;
                const ee::Ptr<ZFile> asset = openFile(name, kFileModeRead, 0).file();
                fileSize(asset, bytes, 0);
                fileRead(asset, dst, *bytes, 0);
                closeFile(asset, 0);
                library->externalBytes = *bytes;
                library->externalEntry = library->cursor;
            }
        }
        nextZfileLibraryAsset(library);
        return read;
    }
    READABLE_FUNCTION_WAITS(readZfileLibraryAsset, Verified);

    // nextZfileLibraryAsset: the cursor on the next entry (past the group's
    // last asset it is on the next group's id).
    void nextZfileLibraryAsset(ee::Ptr<ZfileLibrary> library)
    {
        library->cursor = library->cursor + library->entryBytes;
    }
    READABLE_FUNCTION(nextZfileLibraryAsset, Verified);

    // loadZfileLibraryAsset: asset `asset` of group `group` read into a new
    // heap block (its bytes into *bytesOut, when given); 0 when there is no
    // such group or no memory. Nothing calls it.
    uint32_t loadZfileLibraryAsset(ee::Ptr<ZfileLibrary> library, uint32_t group, uint32_t asset, ee::Ptr<uint32_t> bytesOut)
    {
        if (findZfileLibraryAsset(library, group, asset & 0xFFFFu) == 0)
            return 0;
        const uint32_t bytes = zfileLibraryAssetBytes(library);
        const uint32_t data = heapAllocate(bytes);
        if (data == 0)
            return 0;
        readZfileLibraryAsset(library, data);
        if (bytesOut)
            *bytesOut = bytes;
        return data;
    }
    READABLE_FUNCTION_WAITS(loadZfileLibraryAsset, Draft);
}
