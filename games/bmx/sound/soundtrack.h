#pragma once
// BMX XXX's soundtrack (bmx.c, soundtrack.cpp): the music's tracks and
// which of them the playlist plays. BMX XXX's own code.

#include "ee/ee.h"
#include "zge/sound/soundtrack.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A track of the music (0xC bytes), one for every track of every
    // soundtrack the engine holds: its soundtrack and its number in it (the
    // engine finds its stream by the two, soundtrackTrackLocation), and
    // whether the playlist plays it (the music's frame skips a track that is
    // off; the game's own soundtrack's tracks start on, any other's off).
    struct SoundtrackTrack
    {
        ee::Ptr<zge::Soundtrack> soundtrack; // +0x00 the engine's soundtrack record
        uint32_t index;                      // +0x04
        uint32_t enabled;                    // +0x08
    };
    static_assert(offsetof(SoundtrackTrack, enabled) == 0x08 && sizeof(SoundtrackTrack) == 0x0C);

    // The sound's set-up (the intro's loading runs it once): the engine's
    // sound systems started, the three sound banks loaded, the game's
    // soundtrack made and its tracks' titles and artists named, and the
    // track table made. Always 1.
    int32_t initGameSound();

    // Whether the playlist plays a track (0 with no soundtrack loaded), and
    // the setting changed (nothing with no soundtrack loaded).
    uint32_t trackEnabled(int32_t track);
    void setTrackEnabled(int32_t track, uint32_t enabled);
}
