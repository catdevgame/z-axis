// Sound banks: the ZSD files loaded, unloaded and looked into (zge/sound/bank.h).

#include "zge/sound/bank.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/sound/driver.h"
#include "zge/sound/sound.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/banks.cpp.h"

namespace zge
{
    namespace
    {
        // How many of each table the loaded banks hold between them. Nothing
        // reads these.
        inline ee::Global<uint32_t, sym::g_bankSampleTotal> g_bankSampleTotal;
        inline ee::Global<uint32_t, sym::g_bankReferenceTotal> g_bankReferenceTotal;
        inline ee::Global<uint32_t, sym::g_bankWavesUploaded> g_bankWavesUploaded;
        inline ee::Global<uint32_t, sym::g_bankWavesStreamed> g_bankWavesStreamed;
        inline ee::Global<uint32_t, sym::g_bankPlaylistTotal> g_bankPlaylistTotal;
        inline ee::Global<uint32_t, sym::g_bankGroupTotal> g_bankGroupTotal;
        inline ee::Global<uint32_t, sym::g_bankSoundSetTotal> g_bankSoundSetTotal;
        inline ee::Global<WaveLocation, sym::g_lastWaveLocation> g_lastWaveLocation;

        constexpr uint16_t kEveryBank = 0xFFFE;

        // A piece of a wave on its way to the driver (zaudio::WaveLoad). Two
        // take turns: the request does not wait, so the last one may still be
        // crossing. (ZGE2's piece is 0x410 bytes, the two in uploadSoundBank's
        // frame; ZGE3's 0x440, a heap block of 64-byte alignment.)
        constexpr uint32_t kWavePieceBytes = zaudio::kWaveLoadBytes;
        struct WavePiece : zaudio::WaveLoad
        {
            uint8_t _408[game::readable::perGame(0x38u, 0x08u, 0x38u)];
        };
        static_assert(sizeof(WavePiece) == game::readable::perGame(0x440u, 0x410u, 0x440u));
        constexpr uint32_t kWavePieceAlignment = 0x40;


        void flushCache(uint32_t which) { ee::game(sym::flushCache, which); }

        ee::Ptr<SoundBankSlot> bankSlot(uint16_t bank) { return ee::Ptr<SoundBankSlot>(sym::g_soundBanks) + bank; }
    }

    // bankTables (ZGE4 only): a slot's bank, its tables in memory. ZGE4's bank accessors call it where ZGE2's and
    // ZGE3's read the slot inline.
    inline ee::Ptr<SoundBank> bankTables(uint16_t bank) { return bankSlot(bank)->bank; }
    READABLE_FUNCTION_ONLY(bmx, bankTables);

    namespace
    {
        ee::Ptr<SoundBank> bankFile(uint16_t bank)
        {
            if constexpr (game::readable::kIsBmx)
                return bankTables(bank);
            else
                return bankSlot(bank)->bank;
        }

        // An offset in the bank's file as an address.
        template <typename T>
        void makeAddress(ee::Ptr<T> &offset, uint32_t file)
        {
            offset = (offset.template cast<uint8_t>() + static_cast<ptrdiff_t>(file)).template cast<T>();
        }

        // A wave's bytes sent to the room the driver gave them, a piece at a time.
        void sendWave(uint32_t handle, ee::Ptr<uint8_t> data, int32_t size)
        {
            const auto send = [&](ee::Ptr<WavePiece> pieces) {
                uint32_t sent = 0;
                uint32_t count = 0;
                for (int32_t left = size; left > 0; left -= kWavePieceBytes)
                {
                    const ee::Ptr<WavePiece> piece = pieces + (count & 1);
                    piece->handle = handle;
                    piece->offset = sent;
                    memoryCopy(ee::Ptr<uint8_t>(piece->data), data + sent, left > static_cast<int32_t>(kWavePieceBytes) ? kWavePieceBytes : left);
                    sent += kWavePieceBytes;
                    flushCache(0);
                    ++count;
                    zaudio::sendWave(piece);
                }
            };
            if constexpr (game::readable::kIsDm2)
            {
                ee::Local<WavePiece[2]> stackPieces;
                send(stackPieces);
            }
            else
            {
                const ee::Ptr<WavePiece> pieces(allocateAligned(kWavePieceAlignment, 2 * sizeof(WavePiece)));
                send(pieces);
                heapFree(pieces);
            }
        }

        // One wave reference's copy (ZGE2, ZGE3): room asked of the driver,
        // then the wave sent.
        void uploadWave(WaveReference &reference, const BankWave &wave, ee::Ptr<uint8_t> data, ee::Local<uint32_t> &size)
        {
            *size = wave.size;
            reference.handle = zaudio::allocateWave(size);
            sendWave(reference.handle, data, static_cast<int32_t>(wave.size));
        }

        // ZGE4's request for the room of all of a bank's waves: how many bytes,
        // then where in sound memory the driver put them.
        struct WaveBlockRequest
        {
            uint32_t bytes;
            uint32_t soundAddress;
        };
        // What the driver answers it: the block's handle (unloadSoundBankAt
        // frees the room with it) and where the block is in sound memory.
        struct WaveBlockRoom
        {
            uint32_t handle;
            uint32_t soundAddress;
        };
        constexpr uint32_t kWaveBlockAlignment = 0x40;
    }

    // A bank read into memory, made ready: a slot taken (its id in *bankId),
    // the tables' offsets made addresses, each record told its bank, and with
    // inSoundMemory the waves sent to sound memory. With sixteen banks loaded
    // no slot is found: bankId stays as it was and `slot` stays null, so the
    // writes to it land at the very bottom of memory, as they always did.
    uint32_t uploadSoundBank(uint32_t fileAddress, uint32_t inSoundMemory, ee::Ptr<uint16_t> bankId)
    {
        const ee::Ptr<SoundBank> bank(fileAddress);

        ee::Ptr<SoundBankSlot> slot;
        for (uint16_t i = 0; i < kSoundBankSlots; ++i)
        {
            const ee::Ptr<SoundBankSlot> candidate = bankSlot(i);
            if ((candidate->flags & kBankLoaded) != 0)
                continue;
            slot = candidate;
            slot->flags = inSoundMemory != 0 ? kBankLoaded | kBankInSoundMemory : kBankLoaded;
            slot->path[0] = 0;
            *bankId = i;
            break;
        }
        SoundBankSlot &loaded = slot[0];
        loaded.bank = bank;

        makeAddress(bank->samples, fileAddress);
        makeAddress(bank->references, fileAddress);
        makeAddress(bank->waves, fileAddress);
        makeAddress(bank->playlists, fileAddress);
        makeAddress(bank->groups, fileAddress);
        makeAddress(bank->soundSets, fileAddress);
        for (ee::Ptr<HashIndex> *names : bankNameTables(*bank))
            makeAddress(*names, fileAddress);

        for (uint32_t i = 0; i < bank->sampleCount; ++i)
            bank->samples[i].bank = *bankId;
        *g_bankSampleTotal += bank->sampleCount;

        for (uint32_t i = 0; i < bank->referenceCount; ++i)
            bank->references[i].bank = *bankId;
        *g_bankReferenceTotal += bank->referenceCount;

        for (uint32_t i = 0; i < bank->playlistCount; ++i)
        {
            makeAddress(bank->playlists[i], fileAddress);
            const ee::Ptr<Playlist> playlist = bank->playlists[i];
            if ((playlist->flags & kPlaylistSections) != 0)
            {
                for (uint32_t section = 0; section < playlist->count; ++section)
                    makeAddress(playlist->sections[section].tracks, fileAddress);
            }
        }
        *g_bankPlaylistTotal += bank->playlistCount;

        for (uint32_t i = 0; i < bank->groupCount; ++i)
        {
            SoundGroup &group = bank->groups[i];
            group.data += fileAddress;
            group.bank = *bankId;
        }
        *g_bankGroupTotal += bank->groupCount;

        for (uint32_t i = 0; i < bank->soundSetCount; ++i)
        {
            makeAddress(bank->soundSets[i], fileAddress);
            bank->soundSets[i]->bank = *bankId;
        }
        *g_bankSoundSetTotal += bank->soundSetCount;

        if (inSoundMemory == 0)
        {
            *g_bankWavesStreamed += bank->waveCount;
            return kBankLoadDone;
        }

        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 sends the waves as one block, from the first wave's start
            // to the last one's end, and each reference keeps where its wave
            // landed in sound memory.
            ee::Local<WaveBlockRequest> request;
            const BankWave &first = bank->waves[0];
            const BankWave &last = bank->waves[bank->waveCount - 1];
            const uint32_t bytes = (last.offset + last.size - first.offset + kWaveBlockAlignment - 1) & ~(kWaveBlockAlignment - 1);
            request->bytes = bytes;
            const ee::Ptr<WaveBlockRoom> room(soundRpc(zaudio::kAllocateWave, request, sizeof(uint32_t), zaudio::kReplyWanted));
            setBankWavesHandle(loaded, room->handle);
            request->soundAddress = room->soundAddress;
            sendWave(bankWavesHandle(loaded), ee::Ptr<uint8_t>(fileAddress + first.offset), static_cast<int32_t>(bytes));
            *g_bankWavesUploaded += bank->waveCount;

            for (uint32_t i = 0; i < bank->referenceCount; ++i)
            {
                WaveReference &reference = bank->references[i];
                if (reference.wave != kNoWave)
                    reference.handle = request->soundAddress + (bank->waves[reference.wave].offset - first.offset);
            }
            return kBankLoadDone;
        }
        else
        {
            ee::Local<uint32_t> waveSize; // the request for a wave's room
            for (uint32_t i = 0; i < bank->referenceCount; ++i)
            {
                WaveReference &reference = bank->references[i];
                if (reference.wave == kNoWave)
                    continue;
                const BankWave &wave = bank->waves[reference.wave];
                uploadWave(reference, wave, ee::Ptr<uint8_t>(fileAddress + wave.offset), waveSize);
            }
            *g_bankWavesUploaded += bank->waveCount;
            return kBankLoadDone;
        }
    }
    READABLE_FUNCTION_WAITS(uploadSoundBank, Verified);

    // A ZSD file loaded: all of it for sound memory, keeping only its tables
    // once the waves have gone; its tables alone otherwise, and its path, for
    // the waves to stream from. *bankId is kNoBank until it has an id.
    // ZGE2 (DM2) keeps all of a file whose waves went to sound memory; ZGE2
    // and ZGE4 do not write the cache back before taking the file's size.
    int32_t loadBank(uint32_t path, uint32_t inSoundMemory, ee::Ptr<uint16_t> bankId)
    {
        struct FileHead
        {
            SoundBank head;
            uint8_t _48[game::readable::perGame(8u, 8u, 0x0Cu)];
            uint32_t bytes; // +0x50 (BMX +0x70)
        };
        ee::Local<FileHead> file;

        *bankId = kNoBank;
        const ee::Ptr<ZFile> opened = openFile(path, kFileModeRead, 0).file();
        if (!opened)
            return kBankNoFile;

        if (inSoundMemory != 0)
        {
            if constexpr (!game::readable::kIsDm2 && !game::readable::kIsBmx) // (only ZGE3 flushes first)
                flushCache(0);
            fileSize(opened, ee::Ptr<uint32_t>(&file->bytes), 0);
        }
        else
        {
            fileRead(opened, file, sizeof(SoundBank), 0);
            file->bytes = file->head.tableBytes;
        }
        fileSeek(opened, 0, 0, 0);

        const uint32_t block = heapAllocate(file->bytes);
        int32_t result = kBankNoMemory;
        if (block != 0)
        {
            result = kBankReadFailed;
            if (static_cast<uint32_t>(fileRead(opened, block, file->bytes, 0)) == file->bytes)
                result = static_cast<int32_t>(uploadSoundBank(block, inSoundMemory, bankId));
            if (result == kBankLoadDone)
            {
                const ee::Ptr<SoundBankSlot> slot = bankSlot(*bankId);
                slot->flags |= kBankOwnsMemory;
                if (inSoundMemory != 0)
                {
                    // ZGE2 keeps the whole file, waves and all.
                    if constexpr (!game::readable::kIsDm2)
                        slot->bank = ee::Ptr<SoundBank>(reallocate(block, ee::Ptr<SoundBank>(block)->tableBytes));
                }
                else
                    stringCopy(ee::Ptr<char>(slot->path), path);
                closeFile(opened, 0);
                return kBankLoadDone;
            }
            heapFree(block);
        }
        closeFile(opened, 0);
        return result;
    }
    READABLE_FUNCTION_WAITS(loadBank, Verified);

    void unloadSoundBankAt(uint16_t bank);

    // A bank unloaded: its sounds stopped, last sample first, its waves'
    // room in sound memory given back, its tables freed if loadBank made them.
    void unloadSoundBankAt(uint16_t bank)
    {
        ee::Local<uint32_t> handle; // the request to free a wave's room
        const ee::Ptr<SoundBankSlot> slot = bankSlot(bank);
        const ee::Ptr<SoundBank> file = slot->bank;

        for (int32_t i = static_cast<int32_t>(file->sampleCount) - 1; i >= 0; --i)
            stopChannelsOfOwner(file->samples + i);
        *g_bankSampleTotal -= file->sampleCount;

        if constexpr (game::readable::kIsBmx) // ZGE4's waves are one block of sound memory, the slot's
        {
            *handle = bankWavesHandle(*slot);
            zaudio::freeWave(handle);
        }
        else
        {
            for (int32_t i = static_cast<int32_t>(file->referenceCount) - 1; i >= 0; --i)
            {
                *handle = file->references[i].handle;
                zaudio::freeWave(handle);
            }
        }
        const uint32_t flags = slot->flags;
        *g_bankReferenceTotal -= file->referenceCount;
        if ((flags & kBankInSoundMemory) != 0)
            *g_bankWavesUploaded -= file->waveCount;
        else
            *g_bankWavesStreamed -= file->waveCount;
        *g_bankPlaylistTotal -= file->playlistCount;
        *g_bankGroupTotal -= file->groupCount;
        *g_bankSoundSetTotal -= file->soundSetCount;
        if ((flags & kBankOwnsMemory) != 0)
            heapFree(file);
        slot->flags = 0;
    }
    READABLE_FUNCTION_WAITS(unloadSoundBankAt, Verified);

    // A bank unloaded, or with kEveryBank all of them.
    void unloadSoundBank(uint16_t bank)
    {
        if (bank != kEveryBank)
        {
            game::tail(unloadSoundBankAt, bank);
            return;
        }
        for (uint16_t i = 0; i < kSoundBankSlots; ++i)
        {
            if ((bankSlot(i)->flags & kBankLoaded) != 0)
                unloadSoundBankAt(i);
        }
    }
    READABLE_FUNCTION_WAITS(unloadSoundBank, Verified);

    // 1 if the bank is loaded. An id past the slots answers 0.
    uint32_t soundBankLoaded(uint16_t bank)
    {
        return bank < kSoundBankSlots ? bankSlot(bank)->flags & kBankLoaded : 0;
    }
    READABLE_FUNCTION(soundBankLoaded, Draft);

    uint32_t sampleCount(uint16_t bank) { return bankFile(bank)->sampleCount; }
    READABLE_FUNCTION(sampleCount, Verified);

    uint32_t bankReferenceCount(uint16_t bank) { return bankFile(bank)->referenceCount; }
    READABLE_FUNCTION(bankReferenceCount, Draft);

    uint32_t bankWaveCount(uint16_t bank) { return bankFile(bank)->waveCount; }
    READABLE_FUNCTION(bankWaveCount, Verified);

    uint32_t bankPlaylistCount(uint16_t bank) { return bankFile(bank)->playlistCount; }
    READABLE_FUNCTION(bankPlaylistCount, Verified);

    uint32_t groupCount(uint16_t bank) { return bankFile(bank)->groupCount; }
    READABLE_FUNCTION(groupCount, Verified);

    uint32_t bankSoundSetCount(uint16_t bank) { return bankFile(bank)->soundSetCount; }
    READABLE_FUNCTION(bankSoundSetCount, Draft);

    ee::Ptr<BankSample> sample(uint16_t bank, uint32_t index) { return bankFile(bank)->samples + index; }
    READABLE_FUNCTION(sample, Verified);

    ee::Ptr<WaveReference> waveRef(uint16_t bank, uint32_t index) { return bankFile(bank)->references + index; }
    READABLE_FUNCTION(waveRef, Verified);

    ee::Ptr<Playlist> bankPlaylist(uint16_t bank, uint32_t index) { return bankFile(bank)->playlists[index]; }
    READABLE_FUNCTION(bankPlaylist, Verified);

    ee::Ptr<SoundGroup> group(uint16_t bank, uint32_t index) { return bankFile(bank)->groups + index; }
    READABLE_FUNCTION(group, Verified);

    ee::Ptr<SoundSet> bankSoundSet(uint16_t bank, uint32_t index) { return bankFile(bank)->soundSets[index]; }
    READABLE_FUNCTION(bankSoundSet, Draft);

    // Where a wave of the bank is, for a stream: its file (the slot's path),
    // offset and size, in the one record kept for the purpose.
    ee::Ptr<WaveLocation> bankSampleInfo(uint16_t bank, uint32_t wave)
    {
        const ee::Ptr<SoundBankSlot> slot = bankSlot(bank);
        const ee::Ptr<SoundBank> file = game::readable::kIsBmx ? bankFile(bank) : slot->bank; // (ZGE4 asks bankTables)
        const BankWave &found = file->waves[wave];
        g_lastWaveLocation->path = ee::Ptr<const char>(slot->path);
        g_lastWaveLocation->offset = found.offset;
        g_lastWaveLocation->size = found.size;
        return ee::Ptr<WaveLocation>(sym::g_lastWaveLocation);
    }
    READABLE_FUNCTION(bankSampleInfo, Verified);

    namespace
    {
        constexpr uint32_t kNoHashIndex = 0xFFFFFFFFu;
    }

    // findHashIndex (ZGE4 only): the index a hash has in a name table (`count` records sorted by hash), by halving;
    // kNoHashIndex when the table has no such hash.
    inline uint32_t findHashIndex(uint32_t hash, ee::Ptr<HashIndex> table, uint32_t count)
    {
        while (count != 0)
        {
            const ee::Ptr<HashIndex> middle = table + (count >> 1);
            if (hash == middle->hash)
                return middle->index;
            if (middle->hash < hash)
            {
                --count;
                table = middle + 1;
            }
            count >>= 1;
        }
        return kNoHashIndex;
    }
    READABLE_FUNCTION_ONLY(bmx, findHashIndex);

    // sampleByHash (ZGE4 only).
    inline ee::Ptr<BankSample> sampleByHash(uint16_t bank, uint32_t hash)
    {
        const ee::Ptr<SoundBank> file = bankTables(bank);
        const uint32_t index = findHashIndex(hash, bankSampleHashes(*file), file->sampleCount);
        if (index == kNoHashIndex)
            return ee::Ptr<BankSample>();
        return file->samples + index;
    }
    READABLE_FUNCTION_ONLY(bmx, sampleByHash);

    // waveLocationByHash (ZGE4 only): bankSampleInfo of the wave of that name.
    inline ee::Ptr<WaveLocation> waveLocationByHash(uint16_t bank, uint32_t hash)
    {
        const ee::Ptr<SoundBank> file = bankTables(bank);
        const uint32_t index = findHashIndex(hash, bankWaveHashes(*file), file->waveCount);
        if (index == kNoHashIndex)
            return ee::Ptr<WaveLocation>();
        return bankSampleInfo(bank, index);
    }
    READABLE_FUNCTION_ONLY(bmx, waveLocationByHash);

    // playlistByHash (ZGE4 only).
    inline ee::Ptr<Playlist> playlistByHash(uint16_t bank, uint32_t hash)
    {
        const ee::Ptr<SoundBank> file = bankTables(bank);
        const uint32_t index = findHashIndex(hash, bankPlaylistHashes(*file), file->playlistCount);
        if (index == kNoHashIndex)
            return ee::Ptr<Playlist>();
        return file->playlists[index];
    }
    READABLE_FUNCTION_ONLY(bmx, playlistByHash);
}
