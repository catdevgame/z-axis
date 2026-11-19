// The globals.dat saves: named entries of data in one file on the memory
// card (mc00:\ZAxis\globals.dat), with a directory: the file's first word
// is where the entry table is (after the data), and there the count and
// the entries (GlobalsEntry). A save opened for writing replaces the entry
// of its name with a new one at the end of the data; closing it writes the
// table after the new data. Nothing in the game calls these (cut).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "aggressive/zone_memory.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/globals_file.cpp.h"

namespace aggressive
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_globalsSavePool> g_globalsSavePool;
    }

    void globalsNoopA()
    {
    }
    READABLE_FUNCTION(globalsNoopA, Draft);

    uint32_t globalsStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(globalsStubZero, Draft);

    void globalsNoopB()
    {
    }
    READABLE_FUNCTION(globalsNoopB, Draft);

    namespace
    {
        int poolTake(uint32_t pool) { return aggressive::poolTake(pool); }
    }

    // An entry of the table: where its data starts in the file, its bytes
    // and its name.
    struct GlobalsEntry
    {
        uint32_t offset;   // +0x00
        uint32_t bytes;    // +0x04
        char name[0x48];   // +0x08
    };
    static_assert(sizeof(GlobalsEntry) == 0x50);

    // A card's globals file (g_globalsFiles, by the card's slot): its path,
    // then its directory as read.
    struct GlobalsFile
    {
        char path[0x100];                // +0x000 "mc00:\ZAxis\globals.dat"
        uint32_t tableOffset;            // +0x100 the file's first word
        uint32_t count;                  // +0x104 the entries
        ee::Ptr<GlobalsEntry> entries;   // +0x108 heap
    };
    static_assert(offsetof(GlobalsFile, tableOffset) == 0x100 && sizeof(GlobalsFile) == 0x10C);

    // An open save (a record of g_globalsSavePool).
    struct GlobalsSave
    {
        ee::Ptr<GlobalsEntry> entry; // +0x00
        ee::Ptr<zge::ZFile> file;    // +0x04
        uint8_t mode;                // +0x08 kGlobalsRead or kGlobalsWrite
        uint8_t slot;                // +0x09 the card (written for a write only)
    };
    static_assert(offsetof(GlobalsSave, mode) == 8 && offsetof(GlobalsSave, slot) == 9);

    namespace
    {
        inline ee::Global<GlobalsFile, sym::g_globalsFiles> g_globalsFiles;
        constexpr uint32_t kGlobalsRead = 1, kGlobalsWrite = 2; // openFile's modes too
        constexpr uint32_t kCopyChunkBytes = 5000;              // removeGlobalsEntry's buffer
        constexpr uint32_t kMountTries = 60;
        constexpr uint32_t kFileFound = 1;                      // findFile's answer
        constexpr uint32_t kCardRoot = sym::g_textMc00;         // "mc00:"
        constexpr uint32_t kCardPortSlot = sym::g_globalsCardPortSlot; // "0:0"
        constexpr uint32_t kGlobalsFolder = sym::g_textMc00ZAxis; // "mc00:\ZAxis"

        ee::Ptr<GlobalsFile> globalsFile(uint32_t slot) { return ee::Ptr<GlobalsFile>(g_globalsFiles.get()) + slot; }

        // The calls with what this file hands them.
        ee::Ptr<GlobalsSave> takeSave() { return ee::Ptr<GlobalsSave>(static_cast<uint32_t>(poolTake(*g_globalsSavePool))); }
        namespace tail
        {
            int32_t giveSave(ee::Ptr<GlobalsSave> save) { return game::tail(aggressive::poolGive, *g_globalsSavePool, save); }
        }
    }

    // globalsFileWrite: data written to an open save; its entry grows by it.
    // Returns the entry's bytes.
    uint32_t globalsFileWrite(ee::Ptr<GlobalsSave> save, uint32_t src, uint32_t bytes)
    {
        zge::fileWrite(save->file, src, bytes, 0);
        save->entry->bytes = save->entry->bytes + bytes;
        return save->entry->bytes;
    }
    READABLE_FUNCTION_WAITS(globalsFileWrite, Draft);

    // globalsFileRead: data read from an open save.
    int32_t globalsFileRead(ee::Ptr<GlobalsSave> save, uint32_t dst, uint32_t bytes)
    {
        return game::tail(zge::fileRead, save->file, dst, bytes, 0);
    }
    READABLE_FUNCTION_WAITS(globalsFileRead, Draft);

    // createGlobalsFile: an empty file: the table right after the first
    // word, no entries.
    void createGlobalsFile(ee::Ptr<GlobalsFile> globals)
    {
        ee::Local<uint32_t[2]> header; // the table's offset, the count
        header[0] = sizeof(uint32_t);
        header[1] = 0;
        const ee::Ptr<zge::ZFile> file = zge::openFile(globals, kGlobalsWrite, 0).file();
        if (!file)
            return;
        zge::fileWrite(file, header, sizeof(uint32_t), 0);
        globals->tableOffset = header[0];
        zge::fileWrite(file, ee::Ptr<uint32_t>(header) + 1, sizeof(uint32_t), 0);
        globals->count = 0;
        zge::closeFile(file, 0);
    }
    READABLE_FUNCTION_WAITS(createGlobalsFile, Draft);

    // readGlobalsDirectory: the table read from the file (the old one freed).
    // Kept from the original: with no entries the freed table stays the
    // record's, to be freed again on the next read.
    void readGlobalsDirectory(ee::Ptr<GlobalsFile> globals)
    {
        const ee::Ptr<zge::ZFile> file = zge::openFile(globals, kGlobalsRead, 0).file();
        if (globals->entries)
            zge::heapFree(globals->entries);
        zge::fileRead(file, ee::Ptr<uint32_t>(&globals->tableOffset), sizeof(uint32_t), 0);
        zge::fileSeek(file, globals->tableOffset, zge::kSeekFromStart, 0);
        zge::fileRead(file, ee::Ptr<uint32_t>(&globals->count), sizeof(uint32_t), 0);
        if (globals->count != 0)
        {
            globals->entries = ee::Ptr<GlobalsEntry>(zge::heapAllocate(globals->count * sizeof(GlobalsEntry)));
            zge::fileRead(file, globals->entries, globals->count * sizeof(GlobalsEntry), 0);
        }
        game::tail(zge::closeFile, file, 0);
    }
    READABLE_FUNCTION_WAITS(readGlobalsDirectory, Draft);

    // findGlobalsEntry: the entry called `name`, or 0.
    // Kept from the original: the index is a byte, so past 255 entries the
    // search starts over (and never ends when the name is not there).
    ee::Ptr<GlobalsEntry> findGlobalsEntry(ee::Ptr<GlobalsFile> globals, uint32_t name)
    {
        if (!globals->entries)
            return ee::Ptr<GlobalsEntry>(0u);
        for (uint8_t i = 0; i < globals->count; ++i)
        {
            const ee::Ptr<GlobalsEntry> entry(&globals->entries[i]);
            if (zge::compareStrings(name, ee::Ptr<char>(entry->name)) == 0)
                return entry;
        }
        return ee::Ptr<GlobalsEntry>(0u);
    }
    READABLE_FUNCTION(findGlobalsEntry, Draft);

    // addGlobalsEntry: a new, empty entry called `name` at the end of the
    // table; its data starts where the table was.
    ee::Ptr<GlobalsEntry> addGlobalsEntry(ee::Ptr<GlobalsFile> globals, uint32_t name)
    {
        const ee::Ptr<GlobalsEntry> table(zge::heapAllocate(globals->count * sizeof(GlobalsEntry) + sizeof(GlobalsEntry)));
        if (globals->entries)
        {
            zge::memoryCopy(table, globals->entries, globals->count * sizeof(GlobalsEntry));
            zge::heapFree(globals->entries);
        }
        globals->entries = table;
        const ee::Ptr<GlobalsEntry> entry(&table[globals->count]);
        entry->bytes = 0;
        entry->offset = globals->tableOffset;
        zge::stringCopy(ee::Ptr<char>(entry->name), name);
        globals->count = globals->count + 1;
        return entry;
    }
    READABLE_FUNCTION_WAITS(addGlobalsEntry, Draft);

    // removeGlobalsEntry: the entry called `name` taken out: the data of the
    // entries after it moved down over its own (through a 5000-byte buffer,
    // the file opened again for each read and write), the table shifted,
    // and the table written after the data. 0 when there is no such entry
    // (or no table), else closeFile's answer.
    // (The original keeps the addresses of the offset and the count in its
    // frame across the copy; they are the record's fields.)
    int32_t removeGlobalsEntry(ee::Ptr<GlobalsFile> globals, uint32_t name)
    {
        ee::Local<uint8_t[kCopyChunkBytes]> buffer;
        if (!globals->entries)
            return 0;
        uint32_t found = 0;
        for (; found < globals->count; ++found)
            if (zge::compareStrings(name, ee::Ptr<char>(globals->entries[found].name)) == 0)
                break;
        if (found == globals->count)
            return 0;

        // `to` is where the next byte goes, `from` where it comes from.
        uint32_t to = globals->entries[found].offset;
        if (found < globals->count - 1)
        {
            uint32_t from = globals->entries[found + 1].offset;
            for (uint32_t next = found + 1; next < globals->count; ++next)
            {
                const uint32_t movedTo = to;
                for (uint32_t left = globals->entries[next].bytes; left != 0;)
                {
                    const uint32_t bytes = left < kCopyChunkBytes + 1 ? left : kCopyChunkBytes;
                    const ee::Ptr<zge::ZFile> source = zge::openFile(globals, kGlobalsRead, 0).file();
                    left -= bytes;
                    zge::fileSeek(source, from, zge::kSeekFromStart, 0);
                    from += bytes;
                    zge::fileRead(source, buffer, bytes, 0);
                    zge::closeFile(source, 0);
                    const ee::Ptr<zge::ZFile> target = zge::openFile(globals, kGlobalsWrite, 0).file();
                    zge::fileSeek(target, to, zge::kSeekFromStart, 0);
                    to += bytes;
                    zge::fileWrite(target, buffer, bytes, 0);
                    zge::closeFile(target, 0);
                }
                zge::memoryCopy(ee::Ptr<GlobalsEntry>(&globals->entries[next - 1]), ee::Ptr<GlobalsEntry>(&globals->entries[next]), sizeof(GlobalsEntry));
                globals->entries[next - 1].offset = movedTo;
            }
        }

        const ee::Ptr<zge::ZFile> file = zge::openFile(globals, kGlobalsWrite, 0).file();
        globals->tableOffset = to;
        globals->count = globals->count - 1;
        zge::fileSeek(file, 0, zge::kSeekFromStart, 0);
        zge::fileWrite(file, ee::Ptr<uint32_t>(&globals->tableOffset), sizeof(uint32_t), 0);
        zge::fileSeek(file, globals->tableOffset, zge::kSeekFromStart, 0);
        zge::fileWrite(file, ee::Ptr<uint32_t>(&globals->count), sizeof(uint32_t), 0);
        if (globals->count != 0)
            zge::fileWrite(file, globals->entries, globals->count * sizeof(GlobalsEntry), 0);
        return zge::closeFile(file, 0);
    }
    READABLE_FUNCTION_WAITS(removeGlobalsEntry, Draft);

    // openGlobalsSave: the entry called `name` of the card in `slot` opened
    // to read (kGlobalsRead) or written anew (kGlobalsWrite): the card
    // mounted (up to 60 tries), formatted if it is not, the file made if it
    // is not there and its table read. The save, positioned at the entry's
    // data; 0 when the card does not mount, the entry is not there to read,
    // or the mode is neither.
    // Kept from the original: a mount on the 60th try counts as none, and a
    // missing ZAxis folder is looked for twice, never made.
    ee::Ptr<GlobalsSave> openGlobalsSave(uint32_t name, uint32_t mode, uint32_t slot)
    {
        ee::Local<uint32_t> answer;
        mode &= 0xFFu;
        slot &= 0xFFu;
        const ee::Ptr<GlobalsFile> globals = globalsFile(slot);
        const uint32_t card = zge::findDevice(ee::Ptr<const char>(kCardRoot));
        *answer = 0;
        uint32_t tries = 0;
        do
        {
            zge::mountDevice(card, kCardPortSlot, answer, 0);
            ++tries;
        } while (*answer == 0 && tries < kMountTries);
        if (tries >= kMountTries)
            return ee::Ptr<GlobalsSave>(0u);

        zge::checkDeviceFormatted(card, answer, 0);
        if (*answer == 0)
            zge::formatDevice(card, 0);
        zge::findDirectory(ee::Ptr<const char>(kGlobalsFolder), answer, 0);
        if (*answer == 0)
            zge::findDirectory(ee::Ptr<const char>(kGlobalsFolder), answer, 0);
        zge::findFile(globals, answer, 0);
        if (*answer != kFileFound)
            createGlobalsFile(globals);
        readGlobalsDirectory(globals);

        ee::Ptr<GlobalsSave> save;
        if (mode == kGlobalsRead)
        {
            const ee::Ptr<GlobalsEntry> entry = findGlobalsEntry(globals, name);
            if (!entry)
                return ee::Ptr<GlobalsSave>(0u);
            save = takeSave();
            save->mode = mode;
            save->entry = entry;
        }
        else if (mode == kGlobalsWrite)
        {
            save = takeSave();
            save->mode = mode;
            save->slot = slot;
            save->entry = findGlobalsEntry(globals, name);
            if (save->entry)
                removeGlobalsEntry(globals, name);
            save->entry = addGlobalsEntry(globals, name);
        }
        else
        {
            return ee::Ptr<GlobalsSave>(0u);
        }
        save->file = zge::openFile(globals, mode, 0).file();
        zge::fileSeek(save->file, save->entry->offset, zge::kSeekFromStart, 0);
        return save;
    }
    READABLE_FUNCTION_WAITS(openGlobalsSave, Draft);

    // closeGlobalsSave: an open save closed and given back; a written one
    // first gets the table written after its data and the file's first word
    // pointed at it. 2 for a save of neither mode (nothing done).
    int32_t closeGlobalsSave(ee::Ptr<GlobalsSave> save)
    {
        const ee::Ptr<GlobalsFile> globals = globalsFile(save->slot);
        if (save->mode == kGlobalsRead)
        {
            zge::closeFile(save->file, 0);
            return tail::giveSave(save);
        }
        if (save->mode != kGlobalsWrite)
            return 2;
        globals->tableOffset = save->entry->offset + save->entry->bytes;
        zge::fileWrite(save->file, ee::Ptr<uint32_t>(&globals->count), sizeof(uint32_t), 0);
        zge::fileWrite(save->file, globals->entries, globals->count * sizeof(GlobalsEntry), 0);
        zge::fileSeek(save->file, 0, zge::kSeekFromStart, 0);
        zge::fileWrite(save->file, ee::Ptr<uint32_t>(&globals->tableOffset), sizeof(uint32_t), 0);
        zge::closeFile(save->file, 0);
        return tail::giveSave(save);
    }
    READABLE_FUNCTION_WAITS(closeGlobalsSave, Draft);
}
