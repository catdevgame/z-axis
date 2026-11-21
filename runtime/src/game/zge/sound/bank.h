#pragma once

// Sound banks (zge/sound/banks.cpp): a ZSD file ("ZSND" "PS2 ") holds its
// tables first and its PS-ADPCM waves after them. loadBank either reads the
// whole file, sends the waves to sound memory and keeps the tables, or reads
// the tables alone and plays the waves off the disc as streams. Every offset
// in the tables is an offset in the file until uploadSoundBank makes it an
// address.

#include "ee/ee.h"
#include "ee/readable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/bank.h.h"

namespace zge
{
    constexpr uint16_t kNoBank = 0xFFFF;
    constexpr uint16_t kNoWave = 0xFFFF;

    // A sound: which wave reference, how loud, where, and its envelope.
    // ZGE2's (DM2) sample is 0x14 bytes: no distance byte, so its envelope
    // starts at +0x06 and its bank is at +0x12 (allocateVoice, setVoicePitch,
    // unloadSoundBankAt and sample read them there).
    struct BankSample
    {
        uint16_t reference;     // +0x00 in the bank's references, kNoWave: none
        uint16_t pitch;         // +0x02 0x1000: the wave's own
        uint8_t volume;         // +0x04
        int8_t pan;             // +0x05
#if !ZX_GAME_DM2
        uint8_t distanceScale;  // +0x06 the listener hears it within 80 * sqrt(this entry of the distance table)
#endif
        uint8_t attackRate;     // +0x07 (DM2 +0x06) the ADSR's, 7 bits
        uint8_t decayRate;      // +0x08 (DM2 +0x07) 4 bits
        uint8_t sustainRate;    // +0x09 (DM2 +0x08) 7 bits
        uint8_t releaseRate;    // +0x0A (DM2 +0x09) 5 bits
        uint8_t sustainLevel;   // +0x0B (DM2 +0x0A) 4 bits
#if ZX_GAME_DM2
        uint8_t _0b[7];         // DM2 +0x0B
#else
        uint8_t _0c[6];
        uint8_t oneShot;        // +0x12 0 or 1 [inferred]
        uint8_t _13[3];
#endif
        uint16_t bank;          // +0x16 (DM2 +0x12) the id the bank was loaded as
    };
    static_assert(sizeof(BankSample) == game::readable::perGame(0x18u, 0x14u, 0x18u) &&
                  offsetof(BankSample, bank) == game::readable::perGame(0x16u, 0x12u, 0x16u) &&
                  offsetof(BankSample, attackRate) == game::readable::perGame(0x07u, 0x06u, 0x07u) &&
                  offsetof(BankSample, sustainLevel) == game::readable::perGame(0x0Bu, 0x0Au, 0x0Bu));

    // What sound memory holds: a copy of a wave at a pitch base.
    struct WaveReference
    {
        uint16_t wave;      // +0x00 in the bank's waves, kNoWave: none
        uint16_t pitchBase; // +0x02
        uint8_t _04[2];
        uint16_t bank;      // +0x06
        uint8_t _08[4];
        // +0x0C what the IOP driver gave back for it; ZGE4's: where its wave
        // starts in sound memory, in the bank's one block of waves
        uint32_t handle;
    };
    static_assert(sizeof(WaveReference) == 0x10 && offsetof(WaveReference, bank) == 6 && offsetof(WaveReference, handle) == 0xC);

    // A wave in the file.
    struct BankWave
    {
        uint32_t offset;
        uint32_t size;
    };

    // A track of a playlist section.
    struct PlaylistTrack
    {
        uint8_t kind;       // +0x00 kTrack*
        uint8_t count;      // +0x01 waves; with kTrackChosen and kTrackBlended, which music parameter chooses
        uint16_t firstWave; // +0x02
        uint8_t played[8];  // +0x04 a bit per wave
    };
    static_assert(sizeof(PlaylistTrack) == 0xC);
    enum : uint8_t
    {
        kTrackRandom = 0,  // a wave picked at random, none twice until all have played
        kTrackChosen = 1,  // the wave the parameter says
        kTrackBlended = 2, // the same in steps of a hundred, some with a second wave
    };

    struct PlaylistSection
    {
        uint8_t trackCount;           // +0x00
        uint8_t _01[3];
        ee::Ptr<PlaylistTrack> tracks; // +0x04
    };
    static_assert(sizeof(PlaylistSection) == 8);

    // A piece of music the game asks for: one wave picked from a run of
    // them, or sections of tracks played together.
    struct Playlist
    {
        uint8_t flags;       // +0x00 kPlaylist*
        uint8_t count;       // +0x01 waves, or sections
        uint16_t firstWave;  // +0x02
        uint8_t played[8];   // +0x04 a bit for each one played, so none repeats until all have
        PlaylistSection sections[64]; // +0x0C count of them, with kPlaylistSections
    };
    static_assert(offsetof(Playlist, played) == 4 && offsetof(Playlist, sections) == 0xC);
    enum : uint8_t
    {
        kPlaylistSections = 1u << 0,
        kPlaylistNoRepeat = 1u << 5,    // once all have played it does not start again
        kPlaylistStartMask = 0xC0,      // what happens to what is playing
    };

    // A group of the sound data: what a sound source starts from.
    struct SoundGroup
    {
        uint8_t volume;      // +0x00
        int8_t pan;          // +0x01
        uint8_t sample;      // +0x02 of the bank, 255: none
        uint8_t sampleSet;   // +0x03 of the bank, 255: none
        uint8_t _04[4];      // +0x04
        uint32_t data;       // +0x08 the events, after a word
        uint8_t _0c[2];      // +0x0C
        uint16_t bank;       // +0x0E
    };
    static_assert(sizeof(SoundGroup) == 0x10 && offsetof(SoundGroup, bank) == 0x0E);

    // A run of notes, each a sample and how far from its own pitch.
    struct SoundSet
    {
        struct Note
        {
            uint8_t sample;
            int8_t notesAway; // up or down from the sample's own pitch
        };
        uint8_t firstNote;
        uint8_t noteCount;
        uint16_t bank;
        Note notes[1]; // noteCount of them
    };

    // ZGE4's name tables (BMX XXX only): a bank's samples, waves and playlists
    // each have one, sorted by hash, for the lookups by a name's hash
    // (findHashIndex).
    struct HashIndex
    {
        uint32_t hash;  // +0x00 nameHash of the name
        uint32_t index; // +0x04 in the table it names
    };
    static_assert(sizeof(HashIndex) == 0x08);

    // The head of the file, in memory. ZGE4's (BMX XXX) is 0x64 bytes: each
    // table's count has a second word after it, its name table, made an
    // address with the table (BMX's uploadSoundBank). The samples', waves' and
    // playlists' are looked up (sampleByHash, waveLocationByHash,
    // playlistByHash); no engine function read reads the other three, each
    // count entries of 8 bytes in the file just before the table it names.
    struct SoundBank
    {
        char magic[8];                      // +0x00 "ZSNDPS2 "
        uint32_t fileBytes;                 // +0x08
        uint32_t tableBytes;                // +0x0C all of the file up to the waves
#if ZX_GAME_BMX
        uint32_t sampleCount;               // BMX +0x10
        ee::Ptr<HashIndex> sampleHashes;    // BMX +0x14 sampleCount of them
        ee::Ptr<BankSample> samples;        // BMX +0x18
        uint32_t referenceCount;            // BMX +0x1C
        ee::Ptr<HashIndex> referenceHashes; // BMX +0x20 referenceCount of them
        ee::Ptr<WaveReference> references;  // BMX +0x24
        uint32_t waveCount;                 // BMX +0x28
        ee::Ptr<HashIndex> waveHashes;      // BMX +0x2C waveCount of them
        ee::Ptr<BankWave> waves;            // BMX +0x30
        uint32_t playlistCount;             // BMX +0x34
        ee::Ptr<HashIndex> playlistHashes;  // BMX +0x38 playlistCount of them
        ee::Ptr<ee::Ptr<Playlist>> playlists; // BMX +0x3C
        uint32_t groupCount;                // BMX +0x40
        ee::Ptr<HashIndex> groupHashes;     // BMX +0x44 groupCount of them
        ee::Ptr<SoundGroup> groups;         // BMX +0x48
        uint8_t _4c[0x0C];                  // BMX +0x4C a seventh count, name table and table: uploadSoundBank leaves
                                            // them offsets and no engine function reads them
        uint32_t soundSetCount;             // BMX +0x58
        ee::Ptr<HashIndex> soundSetHashes;  // BMX +0x5C soundSetCount of them
        ee::Ptr<ee::Ptr<SoundSet>> soundSets; // BMX +0x60
    };
    static_assert(sizeof(SoundBank) == 0x64 && offsetof(SoundBank, samples) == 0x18 && offsetof(SoundBank, playlists) == 0x3C);
    static_assert(offsetof(SoundBank, waveCount) == 0x28 && offsetof(SoundBank, groups) == 0x48 && offsetof(SoundBank, soundSets) == 0x60);
#else
        uint32_t sampleCount;               // +0x10
        ee::Ptr<BankSample> samples;        // +0x14
        uint32_t referenceCount;            // +0x18
        ee::Ptr<WaveReference> references;  // +0x1C
        uint32_t waveCount;                 // +0x20
        ee::Ptr<BankWave> waves;            // +0x24
        uint32_t playlistCount;             // +0x28
        ee::Ptr<ee::Ptr<Playlist>> playlists; // +0x2C
        uint32_t groupCount;                // +0x30
        ee::Ptr<SoundGroup> groups;         // +0x34
        uint8_t _38[8];
        uint32_t soundSetCount;             // +0x40
        ee::Ptr<ee::Ptr<SoundSet>> soundSets; // +0x44
    };
    static_assert(sizeof(SoundBank) == 0x48 && offsetof(SoundBank, samples) == 0x14 && offsetof(SoundBank, playlists) == 0x2C);
    static_assert(offsetof(SoundBank, groups) == 0x34 && offsetof(SoundBank, soundSets) == 0x44);
#endif

    // One of the sixteen banks that can be loaded. ZGE4 (BMX XXX) sends all
    // of a bank's waves to sound memory as one block and keeps the driver's
    // handle for it in the slot (its uploadSoundBank), so its path is a word on.
    struct SoundBankSlot
    {
        uint32_t flags;          // +0x00 kBank*
        ee::Ptr<SoundBank> bank; // +0x04
#if ZX_GAME_BMX
        uint32_t wavesHandle;    // BMX +0x08 what the IOP driver gave back for the bank's waves
#endif
        char path[0x40];         // +0x08 (BMX +0x0C) the file its waves stream from; empty when they went to sound memory
    };
    static_assert(sizeof(SoundBankSlot) == game::readable::perGame(0x48u, 0x48u, 0x4Cu) &&
                  offsetof(SoundBankSlot, path) == game::readable::perGame(0x08u, 0x08u, 0x0Cu));
    // ZGE4's own: its banks' name tables (SoundBank::sampleHashes, waveHashes, playlistHashes; ZGE2's and ZGE3's
    // banks have none), all six as one table for uploadSoundBank to make addresses, and the handle of the bank's
    // waves in sound memory (one block, freed with one request when the bank is unloaded; ZGE2 and ZGE3 keep a
    // handle a wave reference, WaveReference::handle, so it is 0 there).
#if ZX_GAME_BMX
    inline ee::Ptr<HashIndex> bankSampleHashes(const SoundBank &bank) { return bank.sampleHashes; }
    inline ee::Ptr<HashIndex> bankWaveHashes(const SoundBank &bank) { return bank.waveHashes; }
    inline ee::Ptr<HashIndex> bankPlaylistHashes(const SoundBank &bank) { return bank.playlistHashes; }
    inline std::array<ee::Ptr<HashIndex> *, 6> bankNameTables(SoundBank &bank)
    {
        return {&bank.sampleHashes, &bank.referenceHashes, &bank.waveHashes,
                &bank.playlistHashes, &bank.groupHashes, &bank.soundSetHashes};
    }
    inline uint32_t bankWavesHandle(const SoundBankSlot &slot) { return slot.wavesHandle; }
    inline void setBankWavesHandle(SoundBankSlot &slot, uint32_t handle) { slot.wavesHandle = handle; }
#else
    inline ee::Ptr<HashIndex> bankSampleHashes(const SoundBank &) { return ee::Ptr<HashIndex>(); }
    inline ee::Ptr<HashIndex> bankWaveHashes(const SoundBank &) { return ee::Ptr<HashIndex>(); }
    inline ee::Ptr<HashIndex> bankPlaylistHashes(const SoundBank &) { return ee::Ptr<HashIndex>(); }
    inline std::array<ee::Ptr<HashIndex> *, 0> bankNameTables(SoundBank &) { return {}; }
    inline uint32_t bankWavesHandle(const SoundBankSlot &) { return 0; }
    inline void setBankWavesHandle(SoundBankSlot &, uint32_t) {}
#endif
    constexpr uint32_t kSoundBankSlots = 16;
    enum : uint32_t
    {
        kBankLoaded = 1u << 0,
        kBankInSoundMemory = 1u << 1,
        kBankOwnsMemory = 1u << 2, // loadBank's: unloading frees the tables
    };

    // Where a wave is: what bankWaveLocation fills.
    struct WaveLocation
    {
        ee::Ptr<const char> path;
        uint32_t offset;
        uint32_t size;
    };
    static_assert(sizeof(WaveLocation) == 12);

    // What loadBank answers.
    enum BankLoadResult : int32_t
    {
        kBankLoadDone = 0,
        kBankNoFile = 1,
        kBankReadFailed = 2,
        kBankNoMemory = 3,
    };

    ee::Ptr<WaveReference> waveRef(uint16_t bank, uint32_t index);
    ee::Ptr<WaveLocation> bankSampleInfo(uint16_t bank, uint32_t wave); // a WaveLocation's address
    void stopChannelsOfOwner(ee::Ptr<BankSample> sample);
    uint32_t uploadSoundBank(uint32_t fileAddress, uint32_t inSoundMemory, ee::Ptr<uint16_t> bankId);
}

namespace zge
{
    // (banks.cpp)
    int32_t loadBank(uint32_t path, uint32_t inSoundMemory, ee::Ptr<uint16_t> bankId);
    void unloadSoundBank(uint16_t bank);
    uint32_t sampleCount(uint16_t bank);
    uint32_t bankWaveCount(uint16_t bank);
    uint32_t groupCount(uint16_t bank);
    ee::Ptr<BankSample> sample(uint16_t bank, uint32_t index);
    ee::Ptr<SoundGroup> group(uint16_t bank, uint32_t index);
}

namespace zge
{
    // (banks.cpp)
    uint32_t bankPlaylistCount(uint16_t bank);
    ee::Ptr<Playlist> bankPlaylist(uint16_t bank, uint32_t index);
    ee::Ptr<SoundSet> bankSoundSet(uint16_t bank, uint32_t index);
    // ZGE4 only: a bank's sample, wave (as bankSampleInfo gives it) and playlist by the hash of its name (nameHash);
    // 0 when the bank has none of that name.
    uint32_t findHashIndex(uint32_t hash, ee::Ptr<HashIndex> table, uint32_t count);
    ee::Ptr<BankSample> sampleByHash(uint16_t bank, uint32_t hash);
    ee::Ptr<WaveLocation> waveLocationByHash(uint16_t bank, uint32_t hash);
    ee::Ptr<Playlist> playlistByHash(uint16_t bank, uint32_t hash);
}
