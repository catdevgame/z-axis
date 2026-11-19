// The asset libraries (zassetLib.c; zge/assets/asset_library.h lays the record out): the ZDIR
// form's chunk handlers, a library opened and closed, an entry found, selected, read and walked.
//
// Original quirks kept: the GRID, DLEN, OFST and CLEN handlers end with a loop over the entries
// that does nothing (what is left of a per-entry step the PS2 build compiles away); readAsset
// with a callback queues its seek and read but inflates a compressed entry at once, before the
// queued read has filled the buffer (no caller reads a compressed entry with a callback).

#include "asset_library.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/zfile_library.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"

#include <cstdint>
#include "sym/zge/assets/asset_library.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_assetLibrariesReady> g_assetLibrariesReady;
        inline ee::Global<uint32_t, sym::g_assetLibraryPool> g_assetLibraryPool; // AssetLibrary records, 8 a block

        constexpr uint32_t kLibrariesPerBlock = 8;
        constexpr uint32_t kDecompressMethod = 2; // zcompres's method for the libraries

        constexpr uint32_t kNewPool = sym::createPool;        // (record bytes, records a block, free callback) -> a pool
        constexpr uint32_t kDestroyPool = sym::destroyPool;    // (pool)
        // zcompres's (decompressFileData): (method, src, src bytes, dst, state)
        // -> 0: "ZCOMPRES: Decompression failed!" (the Xbox builds' text).
        constexpr uint32_t kDecompress = sym::decompressFileData;
        // This file's.
        constexpr uint32_t kHandleDidfChunk = sym::handleDidfChunk;
        constexpr uint32_t kHandleGridChunk = sym::handleGridChunk;
        constexpr uint32_t kHandleDlenChunk = sym::handleDlenChunk;
        constexpr uint32_t kHandleOfstChunk = sym::handleOfstChunk;
        constexpr uint32_t kHandleClenChunk = sym::handleClenChunk;
        constexpr uint32_t kHandleNameChunk = sym::handleNameChunk;
        constexpr uint32_t kFreeAssetLibraryRecord = sym::freeAssetLibraryRecord;
        constexpr uint32_t kIgnoreFileReport = sym::ignoreFileReport;

        AssetLibrary &libraryAt(uint32_t library) { return *ee::at<AssetLibrary>(library); }
        uint32_t &wordAt(uint32_t address) { return *ee::at<uint32_t>(address); }
        // One entry's word in a table (ids, lengths, offsets).
        uint32_t entryWord(uint32_t table, int32_t entry) { return ee::Ptr<uint32_t>(table)[static_cast<uint32_t>(entry)]; }
        void callAssetVisitor(uint32_t function, uint32_t library, uint32_t id, uint32_t length, uint32_t context) { ee::game(function, library, id, length, context); }

        // ZGE4 answers an entry's length rounded up to its alignment (the code taken as it is, with no default for
        // 0), where ZGE2 and ZGE3 answer it as stored (forEachAsset's visitor, currentAssetLength).
        uint32_t alignedAssetLength(uint32_t packed)
        {
            const uint32_t shift = ((packed >> kAssetAlignmentShift) - 1u) & 31u;
            const uint32_t alignment = 1u << shift;
            return ((packed & kAssetLengthMask) + alignment - 1u) / alignment << shift;
        }

        // The alignment a packed length's code gives, as a shift.
        uint32_t alignmentShift(uint32_t packed)
        {
            const uint32_t code = packed >> kAssetAlignmentShift;
            return (code == 0 ? kAssetDefaultAlignmentCode : code) - 1;
        }

        // A table chunk (GRID, DLEN, OFST, CLEN): a word per entry, read into
        // a heap block put at `field` (0 for none).
        uint32_t readEntryTable(ChunkReader *reader, AssetLibrary &library, uint32_t &field)
        {
            const uint32_t bytes = static_cast<uint32_t>(library.count) * sizeof(uint32_t);
            if (bytes == 0 && !game::readable::kIsDm2) // (DM2 asks the heap for nothing)
            {
                field = 0;
                return 1;
            }
            field = heapAllocate(bytes);
            readChunkData(reader, field);
            return 1;
        }
    }

    // ---- the ZDIR form's chunks ------------------------------------------

    // handleDidfChunk: DIDF: the entry count and the reserved word.
    uint32_t handleDidfChunk(ChunkReader *reader, uint32_t library)
    {
        readChunkData(reader, &ee::Ptr<AssetLibrary>(library)->count);
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleDidfChunk, Verified);

    // handleGridChunk, handleDlenChunk, handleOfstChunk, handleClenChunk: GRID (the ids),
    // DLEN (the packed lengths), OFST (the offsets), CLEN (the compressed
    // lengths).
    uint32_t handleGridChunk(ChunkReader *reader, uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        return readEntryTable(reader, target, target.ids);
    }
    READABLE_FUNCTION_WAITS(handleGridChunk, Verified);

    uint32_t handleDlenChunk(ChunkReader *reader, uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        return readEntryTable(reader, target, target.lengths);
    }
    READABLE_FUNCTION_WAITS(handleDlenChunk, Verified);

    uint32_t handleOfstChunk(ChunkReader *reader, uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        return readEntryTable(reader, target, target.offsets);
    }
    READABLE_FUNCTION_WAITS(handleOfstChunk, Verified);

    uint32_t handleClenChunk(ChunkReader *reader, uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        return readEntryTable(reader, target, target.packedLengths);
    }
    READABLE_FUNCTION_WAITS(handleClenChunk, Draft);

    // handleNameChunk: NAME: the names, read whole into a heap block (0 when
    // the chunk is empty).
    uint32_t handleNameChunk(ChunkReader *reader, uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 takes an empty chunk as any other)
        {
            if (chunkDataSize(reader) == 0)
            {
                target.names = 0;
                return 1;
            }
        }
        const uint32_t names = heapAllocate(chunkDataSize(reader));
        target.names = names;
        readChunkData(reader, names);
        return 1;
    }
    READABLE_FUNCTION_WAITS(handleNameChunk, Draft);

    // ---- libraries -----------------------------------------------------------

    // freeAssetLibraryRecord: the record pool's free callback (nothing to do). 1.
    uint32_t freeAssetLibraryRecord()
    {
        return 1;
    }
    READABLE_FUNCTION(freeAssetLibraryRecord, Verified);

    // initAssetLibraries: the record pool made and the ZDIR form's handlers
    // registered. 1.
    uint32_t initAssetLibraries()
    {
        *g_assetLibrariesReady = 1;
        *g_assetLibraryPool = createPool(sizeof(AssetLibrary), kLibrariesPerBlock, kFreeAssetLibraryRecord);
        addChunkHandler(chunkTag("DIDF"), kHandleDidfChunk);
        addChunkHandler(chunkTag("GRID"), kHandleGridChunk);
        addChunkHandler(chunkTag("DLEN"), kHandleDlenChunk);
        addChunkHandler(chunkTag("OFST"), kHandleOfstChunk);
        addChunkHandler(chunkTag("CLEN"), kHandleClenChunk);
        addChunkHandler(chunkTag("NAME"), kHandleNameChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initAssetLibraries, Verified);

    // shutDownAssetLibraries: the record pool destroyed (once made). 1.
    uint32_t shutDownAssetLibraries()
    {
        if (*g_assetLibrariesReady != 0)
        {
            destroyPool(ee::Ptr<Pool>(*g_assetLibraryPool));
            *g_assetLibrariesReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownAssetLibraries, Verified);

    // openAssetLibrary: a library opened: the file opened for reading, a record
    // over it, its ZDIR form parsed into the record; 0 if the file could not
    // be opened or has no ZDIR form (the file closed again).
    uint32_t openAssetLibrary(ee::Address path)
    {
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
            return 0;
        const uint32_t library = poolAlloc(ee::Ptr<Pool>(*g_assetLibraryPool));
        memoryFill(library, 0, sizeof(AssetLibrary));
        libraryAt(library).file = file;
        const ee::Ptr<ChunkReader> reader = openChunkFile(file, chunkTag("ZDIR"), 0);
        if (!reader)
        {
            poolFree(ee::Ptr<Pool>(*g_assetLibraryPool), library);
            closeFile(file, 0);
            // DM2 (a bug it keeps) goes on as if the directory were there: it
            // reads and closes a chunk file of 0 and returns the record it has
            // just given back.
            if (!game::readable::kIsDm2)
                return 0;
        }
        parseChunks(reader, library);
        closeChunkFile(reader);
        return library;
    }
    READABLE_FUNCTION_WAITS(openAssetLibrary, Verified);

    // closeAssetLibrary: a library closed: its file, its tables and names freed,
    // the record given back. 1.
    uint32_t closeAssetLibrary(uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        closeFile(target.file, 0);
        if constexpr (game::readable::kIsDm2) // ZGE2 leaves the tables and names where they are
        {
            poolFree(ee::Ptr<Pool>(*g_assetLibraryPool), library);
            return 1;
        }
        // Each table freed if there is one (the original's order).
        const auto freeTable = [&](uint32_t table) {
            if (table != 0)
                heapFree(table);
        };
        freeTable(target.lengths);
        freeTable(target.offsets);
        freeTable(target.names);
        freeTable(target.ids);
        if (target.packedLengths != 0)
            heapFree(target.packedLengths);
        poolFree(ee::Ptr<Pool>(*g_assetLibraryPool), library);
        return 1;
    }
    READABLE_FUNCTION_WAITS(closeAssetLibrary, Verified);

    // findAsset: the entry with this id selected; 0 if there is none.
    uint32_t findAsset(uint32_t library, uint32_t id)
    {
        AssetLibrary &target = libraryAt(library);
        const int32_t count = target.count;
        int32_t index = 0;
        if (count <= 0)
            return 0;
        for (uint32_t entry = target.ids;; entry += 4) // the branch back per entry
        {
            if (wordAt(entry) == id)
            {
                target.current = index;
                return 1;
            }
            ++index;
            if (index >= count)
                break;
        }
        return 0;
    }
    READABLE_FUNCTION(findAsset, Verified);

    // ignoreFileReport: the callback readAsset gives its queued seek (nothing to do).
    void ignoreFileReport()
    {
    }
    READABLE_FUNCTION(ignoreFileReport, Verified);

    // readAsset: the selected entry read into dst (its length rounded up
    // to its alignment); a compressed one read into memory and inflated into
    // dst. 0 with no entry selected (current at the count). With a callback
    // the seek and the read are queued (see the top).
    namespace
    {
        // readAsset's inflate state (its first word: the bytes inflated). ZGE2 inflates nothing: no room in its frame.
        struct alignas(16) AssetInflateState
        {
            uint32_t words[0x24];
            static constexpr uint32_t frameBytes() { return game::readable::perGame<uint32_t>(sizeof(words), 0u, sizeof(words)); }
        };
    }

    uint32_t readAsset(uint32_t library, uint32_t dst, ee::Address callback)
    {
        AssetLibrary &target = libraryAt(library);
        ee::Local<FileCallback> ignore;
        ee::Local<AssetInflateState> decompressState;
        const int32_t current = target.current;
        ignore->function = kIgnoreFileReport;
        ignore->context = 0;
        if (current == target.count)
            return 0;
        const uint32_t entryOffset = static_cast<uint32_t>(current) << 2;
        fileSeek(target.file, wordAt(entryOffset + target.offsets), kSeekFromStart, callback != 0 ? ignore.address() : 0);
        const uint32_t packedLengths = target.packedLengths;
        if (packedLengths != 0)
        {
            if constexpr (game::readable::kIsDm2) // (ZGE2 reads no packed entry)
                return 1;
            const uint32_t shift = alignmentShift(wordAt(entryOffset + target.lengths));
            const uint32_t alignment = 1u << shift;
            const uint32_t padded = (wordAt(entryOffset + packedLengths) & kAssetLengthMask) + alignment;
            const uint32_t bytes = (padded - 1) / alignment << shift;
            const uint32_t stored = allocateMemory(bytes);
            fileRead(target.file, stored, bytes, callback);
            decompressFileData(kDecompressMethod, ee::Ptr<uint8_t>(stored), wordAt(entryOffset + target.packedLengths) & kAssetLengthMask, ee::Ptr<uint8_t>(dst), ee::Ptr<uint32_t>(decompressState.address()));
            freeMemory(stored);
            return 1;
        }
        const uint32_t packed = wordAt(entryOffset + target.lengths);
        const uint32_t shift = alignmentShift(packed);
        const uint32_t alignment = 1u << shift;
        const uint32_t padded = (packed & kAssetLengthMask) + alignment;
        fileRead(target.file, dst, (padded - 1) / alignment << shift, callback);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readAsset, Verified);

    // forEachAsset: each entry selected in turn, the file put at it and
    // function(library, id, length, context) called.
    void forEachAsset(uint32_t library, uint32_t function, ee::Address context)
    {
        AssetLibrary &target = libraryAt(library);
        for (int32_t next = 0; next < target.count;)
        {
            target.current = next;
            fileSeek(target.file, entryWord(target.offsets, next), kSeekFromStart, 0);
            ++next;
            const int32_t current = target.current;
            if constexpr (game::readable::kIsBmx)
                callAssetVisitor(function, library, entryWord(target.ids, current), alignedAssetLength(entryWord(target.lengths, current)), context);
            else
                callAssetVisitor(function, library, entryWord(target.ids, current), entryWord(target.lengths, current) & kAssetLengthMask, context);
        }
    }
    READABLE_FUNCTION_WAITS(forEachAsset, Verified);

    // parseCurrentAsset (ZGE4 only): the selected entry read as a FORM, its chunks handed to their handlers with
    // `context`: read into memory first when it fits (under the main region's free bytes less 1 KB), else parsed
    // from the library's file as it is read. 0 with no entry selected, or no form in the file; 1 once read into
    // memory, whether a form was in it or not.
    inline uint32_t parseCurrentAsset(uint32_t library, ee::Address context)
    {
        constexpr uint32_t kFormTag = chunkTag("FORM");
        constexpr uint32_t kMemoryKeptFree = 0x400;
        AssetLibrary &target = libraryAt(library);
        const int32_t current = target.current;
        if (current == target.count)
            return 0;
        const uint32_t entryOffset = static_cast<uint32_t>(current) << 2;
        fileSeek(target.file, wordAt(entryOffset + target.offsets), kSeekFromStart, 0);
        const uint32_t packed = wordAt(entryOffset + target.lengths);
        const uint32_t shift = alignmentShift(packed);
        const uint32_t alignment = 1u << shift;
        const uint32_t bytes = ((packed & kAssetLengthMask) + alignment - 1) / alignment << shift;
        if (bytes < static_cast<uint32_t>(mainRegionFreeBytes()) - kMemoryKeptFree)
        {
            const uint32_t held = allocateMemory(bytes);
            fileRead(target.file, held, bytes, 0);
            const ee::Ptr<ZFile> file = openMemoryFile(held, bytes);
            const ee::Ptr<ChunkReader> reader = openChunkFile(file, kFormTag, 0);
            if (reader)
            {
                parseChunks(reader, context);
                closeChunkFile(reader);
            }
            closeFile(file, 0);
            freeMemory(held);
            return 1;
        }
        const ee::Ptr<ChunkReader> reader = openChunkFile(target.file, kFormTag, 0);
        if (!reader)
            return 0;
        parseChunks(reader, context);
        closeChunkFile(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, parseCurrentAsset);

    // assetCount .. currentAssetAlignment (the first six no caller in this build):
    // the entry count, the selected entry, its id, its name (0 without
    // names), an entry selected by index (0 past the end), the next selected
    // (0 at the end), the selected entry's length and alignment.
    uint32_t assetCount(uint32_t library)
    {
        return static_cast<uint32_t>(libraryAt(library).count);
    }
    READABLE_FUNCTION(assetCount, Verified);

    uint32_t currentAsset(uint32_t library)
    {
        return static_cast<uint32_t>(libraryAt(library).current);
    }
    READABLE_FUNCTION(currentAsset, Verified);

    uint32_t currentAssetId(uint32_t library)
    {
        const AssetLibrary &target = libraryAt(library);
        return wordAt((static_cast<uint32_t>(target.current) << 2) + target.ids);
    }
    READABLE_FUNCTION(currentAssetId, Verified);

    uint32_t currentAssetName(uint32_t library)
    {
        const AssetLibrary &target = libraryAt(library);
        uint32_t name = 0;
        if (target.names != 0)
        {
            name = target.names;
            int32_t index = 0;
            if (target.current > 0)
            {
                for (;;) // the branch back per name passed
                {
                    ++index;
                    name += static_cast<uint32_t>(stringLength(name) + 1);
                    if (index >= target.current)
                        break;
                }
            }
        }
        return name;
    }
    READABLE_FUNCTION(currentAssetName, Verified);

    uint32_t selectAsset(uint32_t library, uint32_t index)
    {
        AssetLibrary &target = libraryAt(library);
        if (static_cast<int32_t>(index) >= target.count)
            return 0;
        target.current = static_cast<int32_t>(index);
        return 1;
    }
    READABLE_FUNCTION(selectAsset, Verified);

    uint32_t nextAsset(uint32_t library)
    {
        AssetLibrary &target = libraryAt(library);
        const int32_t current = target.current;
        if (current >= target.count)
            return 0;
        target.current = current + 1;
        return 1;
    }
    READABLE_FUNCTION(nextAsset, Verified);

    uint32_t currentAssetLength(uint32_t library)
    {
        const AssetLibrary &target = libraryAt(library);
        if constexpr (game::readable::kIsBmx)
            return alignedAssetLength(wordAt((static_cast<uint32_t>(target.current) << 2) + target.lengths));
        return wordAt((static_cast<uint32_t>(target.current) << 2) + target.lengths) & kAssetLengthMask;
    }
    READABLE_FUNCTION(currentAssetLength, Verified);

    uint64_t currentAssetAlignment(uint32_t library)
    {
        const AssetLibrary &target = libraryAt(library);
        const uint32_t shift = alignmentShift(wordAt((static_cast<uint32_t>(target.current) << 2) + target.lengths));
        // (dsllv then the 32-bit sign extension, as the original computes it)
        return static_cast<int64_t>(static_cast<uint64_t>(1) << (shift & 0x3Fu) << 32) >> 32;
    }
    READABLE_FUNCTION(currentAssetAlignment, Verified);

    // unusedAssetLibraryHook (no caller, empty).
    void unusedAssetLibraryHook()
    {
    }
    READABLE_FUNCTION(unusedAssetLibraryHook, Verified);
}
