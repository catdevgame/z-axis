#pragma once

// The soundtracks (zge/sound/music.cpp): a soundtrack is a music bank's
// waves with a name and each track's title and artist, a record of the
// soundtrack pool (g_soundBankPool) in the order they were made.

#include "ee/ee.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // (The records are music.cpp's; effects.cpp takes only the size.)
    struct MusicTrackNames
    {
        char title[0x40];
        char artist[0x40];
    };
    static_assert(sizeof(MusicTrackNames) == 0x80);

    // ZGE4's (BMX XXX) track names: two strings on the heap, made as they
    // are set (setPlaylistEntry) and the old ones freed.
    struct SoundtrackTrackStrings
    {
        ee::Ptr<char> title;  // +0x00 0: none
        ee::Ptr<char> artist; // +0x04 0: none
    };
    static_assert(sizeof(SoundtrackTrackStrings) == 8);

    // A soundtrack: 0x84C bytes, its name and sixteen tracks' names in the
    // record. ZGE4's is 0x14 bytes: the name a string on the heap and the
    // tracks' names a table of SoundtrackTrackStrings newSoundtrack makes
    // for the bank's waves. ZGE3's fields stand past the end of BMX XXX's
    // record, for code its instance compiles and never runs.
    struct Soundtrack
    {
#if PS2X_GAME_BMX
        ee::Ptr<char> nameString;   // BMX +0x00 setSoundtrackName's copy, 0: none
        uint16_t bank;              // BMX +0x04
        uint16_t _06;
        uint32_t newArgument;       // BMX +0x08
        uint32_t trackCount;        // BMX +0x0C
        ee::Ptr<SoundtrackTrackStrings> trackStrings; // BMX +0x10 trackCount of them
        char name[0x40];
        MusicTrackNames names[0x10];
#else
        char name[0x40];            // +0x00
        uint16_t bank;              // +0x40 the music bank its tracks are the waves of
        uint16_t _42;
        // +0x44 newSoundtrack's second argument (1, AI's one soundtrack);
        // nothing reads it.
        uint32_t newArgument;
        uint32_t trackCount;        // +0x48 the bank's waves
        MusicTrackNames names[0x10]; // +0x4C
#endif
    };
    static_assert(offsetof(Soundtrack, bank) == game::readable::perGame(0x40u, 0x40u, 0x04u) &&
                  offsetof(Soundtrack, newArgument) == game::readable::perGame(0x44u, 0x44u, 0x08u) &&
                  offsetof(Soundtrack, trackCount) == game::readable::perGame(0x48u, 0x48u, 0x0Cu));
#if PS2X_GAME_BMX
    static_assert(offsetof(Soundtrack, trackStrings) == 0x10);
#else
    static_assert(offsetof(Soundtrack, names) == 0x4C && sizeof(Soundtrack) == 0x84C);
#endif
    // The pool's record (createPool's size): ZGE4's ends at its table.
    constexpr uint32_t kSoundtrackBytes = game::readable::perGame(0x84Cu, 0x84Cu, 0x14u);

    // ZGE4's two heap fields (BMX XXX only: ZGE2 and ZGE3 have none, and only
    // ZGE4's own bodies reach these).
    inline ee::Ptr<char> soundtrackNameString(ee::Ptr<Soundtrack> soundtrack)
    {
#if PS2X_GAME_BMX
        return soundtrack->nameString;
#else
        (void)soundtrack;
        return {};
#endif
    }
    inline void setSoundtrackNameString(ee::Ptr<Soundtrack> soundtrack, ee::Ptr<char> name)
    {
#if PS2X_GAME_BMX
        soundtrack->nameString = name;
#else
        (void)soundtrack;
        (void)name;
#endif
    }
    inline ee::Ptr<SoundtrackTrackStrings> soundtrackTrackStrings(ee::Ptr<Soundtrack> soundtrack)
    {
#if PS2X_GAME_BMX
        return soundtrack->trackStrings;
#else
        (void)soundtrack;
        return {};
#endif
    }
    inline void setSoundtrackTrackStrings(ee::Ptr<Soundtrack> soundtrack, ee::Ptr<SoundtrackTrackStrings> table)
    {
#if PS2X_GAME_BMX
        soundtrack->trackStrings = table;
#else
        (void)soundtrack;
        (void)table;
#endif
    }
}

namespace zge
{
    struct WaveLocation; // (zge/sound/bank.h)

    // (music.cpp) The soundtracks, in the order they were made: one made for
    // a music bank and named; one by its index, and its index; its name, its
    // track count, a track's title and artist and where its wave is.
    ee::Ptr<Soundtrack> newSoundtrack(uint32_t bank, uint32_t newArgument);
    void setSoundtrackName(ee::Ptr<Soundtrack> soundtrack, uint32_t name);
    ee::Ptr<Soundtrack> soundtrackAt(uint32_t index);
    int32_t soundtrackIndex(ee::Ptr<Soundtrack> soundtrack);
    ee::Ptr<char> soundtrackName(ee::Ptr<Soundtrack> soundtrack);
    uint32_t soundtrackTrackCount(ee::Ptr<Soundtrack> soundtrack);
    ee::Ptr<char> soundtrackTrackTitle(ee::Ptr<Soundtrack> soundtrack, uint32_t track);
    ee::Ptr<char> soundtrackTrackArtist(ee::Ptr<Soundtrack> soundtrack, uint32_t track);
    ee::Ptr<WaveLocation> soundtrackTrackLocation(ee::Ptr<Soundtrack> soundtrack, uint32_t track);
}
