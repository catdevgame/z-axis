#pragma once

// The soundtrack's controls, one body for the games built on the Z-Axis
// engine (zge_soundtrack.cpp): the next and the previous song, shuffle, and
// the songs a mod adds after the game's own (zge_sound.h, addedSongs).
//
// Each game picks its songs itself, in its sound frame: when the song playing
// has ended and none is asked for, the next one of its playlist (the one after,
// round the list; or the same again when its playlist repeats), else the one
// asked for. Its menus ask for a song through its sound event (AI's pause
// page's Next Track, DM2's music menu), which sets the song to start next.
// The controls do the same, from the game's sound frame, before it runs:
//  - Next Track, Previous Track (controls.ini [hotkeys] next_track,
//    previous_track; ZX_CONTROL `set hotkey.next_track 1`): the song after
//    or before the one playing in the game's playlist (the songs it has on),
//    asked for as the game's event asks; with shuffle on, Previous Track is
//    the song played before;
//  - Shuffle (Settings > Sound, audio.shuffle; the shuffle key turns it on
//    and off; ZX_MUSIC_SHUFFLE=1 without the app's settings): when a song
//    ends and the game would go on to the next of its playlist, a song of
//    the playlist at random is asked for instead, none twice until each has
//    played. Off by default: then the game picks as it always has;
//  - the added songs: Dave Mirra BMX 2 and BMX XXX count MUSIC.ZSD's waves,
//    so theirs come in their turn on their own; Aggressive Inline's playlist
//    is its 13 songs, so here they are asked for after its last, in turn.
// With none of them in use nothing is written: the game's choice stands.
// Every song started is logged ("[soundtrack] song N"), with what asked
// for it.
//
// Each game describes its music in a Game record:
// aggressive_inline_soundtrack.cpp, dave_mirra_soundtrack.cpp,
// bmx_xxx_soundtrack.cpp.

#include <cstdint>
#include <vector>

class PS2Machine;

namespace zge_host::soundtrack
{
    constexpr int kNoSong = -1;

    // The game's music as its sound frame is about to see it.
    struct Music
    {
        // A level's music runs (its sound mode and play phase), so a song
        // can be asked for.
        bool running = false;
        int playing = kNoSong; // the song playing (or last started)
        int next = kNoSong;    // the song asked for, to start next
        // The game picks a song at this frame or as soon as it may: the one
        // playing has ended (or was turned off) and none is asked for.
        bool picking = false;
        // Its playlist goes on to the next song (else the song repeats).
        bool goesOn = true;
    };

    struct Game
    {
        const char *tag;    // the log's ("[aggressive-inline]")
        uint32_t frame;     // the game's sound frame (where it picks its songs): wrapped
        Music (*read)(const uint8_t *rdram);
        // The songs of its playlist it would play (those it has on), in its
        // order; Aggressive Inline's 13 (the added songs are not its).
        std::vector<int> (*playlist)(const uint8_t *rdram);
        // A song asked for as the game's own event asks (its conditions: the
        // play phase; false when the game would not take it).
        bool (*ask)(uint8_t *rdram, int song);
        // The game counts MUSIC.ZSD's waves, so the added songs are in its
        // playlist already (DM2, BMX XXX).
        bool countsAddedSongs = false;
    };

    // Guest memory as the games' records hold it.
    uint32_t guestWord(const uint8_t *rdram, uint32_t address);
    uint8_t guestByte(const uint8_t *rdram, uint32_t address);
    void setGuestByte(uint8_t *rdram, uint32_t address, uint8_t value);
    // A stream (a SoundStream's address, engine/zge/sound/streams.h) with no
    // song playing, as each game's streamStatus reads its driver state:
    // ended, stopped or never started (states 0, 2, 5, 7), which every game's
    // sound frame takes as the song over.
    bool streamEnded(const uint8_t *rdram, uint32_t stream);

    // Wraps game.frame. False if the hook could not be installed.
    bool install(PS2Machine &runtime, const Game &game);
}
