// The soundtracks (the zsnd block initSoundBankPool .. soundtrackIndex; the
// pool's set-up and freeing are in effects.cpp): a soundtrack is a music
// bank's waves with a name and each track's title and artist, kept in a pool
// in the order they were made. And the music stream's stubs.

#include "zge/memory/pool.h"
#include "zge/sound/bank.h"
#include "zge/sound/streams.h"
#include "zge/sound/soundtrack.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/music.cpp.h"

namespace zge
{
    namespace
    {

        inline ee::Global<ee::Ptr<Pool>, sym::g_soundBankPool> g_soundtracks;

        // The soundtrack after this one. Kept bug: a record's address is its
        // link's + 0x10, so the end of the list (a 0 link) gives 0x10, not 0:
        // the walks below never see it and go on from address 0.
        ee::Ptr<Soundtrack> nextSoundtrack(ee::Ptr<Soundtrack> soundtrack) { return poolRecordAs<Soundtrack>(nextPoolLink(poolLinkOfRecord(soundtrack))); }
        namespace tail
        {
            void stringCopy(ee::Ptr<char> dst, uint32_t src) { ee::tailCall(sym::stringCopy, dst, src); }
        }
    }

    // newSoundtrack (was newSoundBankRecord; initSound): a soundtrack of the
    // music bank's waves, unnamed and its tracks' names as they were.
    ee::Ptr<Soundtrack> newSoundtrack(uint32_t bank, uint32_t newArgument)
    {
        const ee::Ptr<Soundtrack> soundtrack(poolAlloc(*g_soundtracks));
        soundtrack->bank = static_cast<uint16_t>(bank);
        soundtrack->newArgument = newArgument;
        soundtrack->trackCount = bankWaveCount(bank & 0xFFFFu);
        return soundtrack;
    }

    // newSoundtrackWithStrings: ZGE4's newSoundtrack: a soundtrack of the
    // music bank's waves, unnamed, with a table of its tracks' names made
    // from the heap, every name none.
    ee::Ptr<Soundtrack> newSoundtrackWithStrings(uint32_t bank, uint32_t newArgument)
    {
        const ee::Ptr<Soundtrack> soundtrack(poolAlloc(*g_soundtracks));
        soundtrack->bank = static_cast<uint16_t>(bank);
        soundtrack->newArgument = newArgument;
        setSoundtrackNameString(soundtrack, ee::Ptr<char>());
        const uint32_t tracks = bankWaveCount(bank & 0xFFFFu);
        const ee::Ptr<SoundtrackTrackStrings> table(heapAllocate(tracks * static_cast<uint32_t>(sizeof(SoundtrackTrackStrings))));
        soundtrack->trackCount = tracks;
        setSoundtrackTrackStrings(soundtrack, table);
        for (uint32_t track = 0; track < soundtrack->trackCount; ++track)
        {
            SoundtrackTrackStrings &names = soundtrackTrackStrings(soundtrack)[track];
            names.title = ee::Ptr<char>();
            names.artist = ee::Ptr<char>();
        }
        return soundtrack;
    }
    READABLE_FUNCTION_WAITS_OWN(newSoundtrack, Verified, bmx, newSoundtrackWithStrings);

    // soundtrackAt (was loadMusic): the soundtrack at index (0 the first
    // made), the first for an index past the last (as meant: see
    // nextSoundtrack's kept bug).
    ee::Ptr<Soundtrack> soundtrackAt(uint32_t index)
    {
        ee::Ptr<Soundtrack> soundtrack = poolRecordAs<Soundtrack>(firstPoolLink(*g_soundtracks));
        for (int32_t left = static_cast<int32_t>(index) - 1; soundtrack && left != -1; --left)
            soundtrack = nextSoundtrack(soundtrack);
        if (!soundtrack)
            soundtrack = poolRecordAs<Soundtrack>(firstPoolLink(*g_soundtracks));
        return soundtrack;
    }
    READABLE_FUNCTION(soundtrackAt, Verified);

    // setSoundtrackName (was copyStringIfAny; initSound): the soundtrack's
    // name copied from name, if there is one.
    void setSoundtrackName(ee::Ptr<Soundtrack> soundtrack, uint32_t name)
    {
        if (name != 0)
            tail::stringCopy(ee::Ptr<char>(soundtrack->name), name); // the original's tail call
    }

    // copySoundtrackName: ZGE4's setSoundtrackName: the soundtrack's name a
    // copy of name on the heap (the old copy freed), if there is a name.
    void copySoundtrackName(ee::Ptr<Soundtrack> soundtrack, uint32_t name)
    {
        if (name == 0)
            return;
        if (const ee::Ptr<char> old = soundtrackNameString(soundtrack))
            heapFree(old);
        const ee::Ptr<char> copy(heapAllocate(static_cast<uint32_t>(stringLength(name)) + 1));
        setSoundtrackNameString(soundtrack, copy);
        tail::stringCopy(copy, name); // the original's tail call
    }
    READABLE_FUNCTION_OWN(setSoundtrackName, Verified, bmx, copySoundtrackName);


    namespace
    {
        // ZGE4's names: a copy of text made on the heap, the old one freed
        // (text 0: none).
        void replaceHeapString(ee::Ptr<char> &string, uint32_t text)
        {
            if (string)
                heapFree(string);
            if (text == 0)
            {
                string = ee::Ptr<char>();
                return;
            }
            string = ee::Ptr<char>(heapAllocate(static_cast<uint32_t>(stringLength(text)) + 1));
            stringCopy(string, text);
        }
    }

    // setPlaylistEntry: a track's title and artist (0: blank), if the list has
    // the track. A game override wraps it (the custom soundtrack's songs).
    void setPlaylistEntry(ee::Address soundtrack, uint32_t index, uint32_t title, uint32_t artist)
    {
        const ee::Ptr<Soundtrack> list(soundtrack);
        if (index >= list->trackCount)
            return;
        const ee::Ptr<MusicTrackNames> entry = ee::Ptr<MusicTrackNames>(list->names) + index;
        if (title != 0)
            stringCopy(ee::Ptr<char>(entry->title), title);
        else
            entry->title[0] = 0;
        if (artist != 0)
            stringCopy(ee::Ptr<char>(entry->artist), artist);
        else
            entry->artist[0] = 0;
    }

    // setPlaylistStrings: ZGE4's setPlaylistEntry: a track's title and artist
    // copied to the heap (0: none), the old ones freed, if the list has the
    // track.
    void setPlaylistStrings(ee::Address soundtrack, uint32_t index, uint32_t title, uint32_t artist)
    {
        const ee::Ptr<Soundtrack> list(soundtrack);
        if (index >= list->trackCount)
            return;
        replaceHeapString(soundtrackTrackStrings(list)[index].title, title);
        replaceHeapString(soundtrackTrackStrings(list)[index].artist, artist);
    }
    READABLE_FUNCTION_OWN(setPlaylistEntry, Verified, bmx, setPlaylistStrings);

    namespace
    {
        inline ee::Global<int32_t, sym::g_musicTrackBufferSize> g_musicTrackBufferSize;
        inline ee::Global<int32_t, sym::g_musicTrackStream> g_musicTrackStream;
    }

    namespace
    {
        namespace routed
        {
        }
    }

    // (callback): the sound streams set up (initSoundStreams) and the music's callback kept
    // (g_musicTrackBufferSize)
    int32_t initMusicStream(uint32_t musicTrackBufferSize)
    {
        const int32_t result = zge::initSoundStreams();
        if (result == 0)
        {
            *g_musicTrackBufferSize = musicTrackBufferSize;
            return result;
        }
        return result;
    }
    READABLE_FUNCTION(initMusicStream, Draft);

    // The music stream (g_musicTrackStream) closed and every stream closed (closeAllSoundStreams)
    uint32_t shutdownMusicStream()
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        if (musicTrackStream != 0)
        {
            closeSoundStream(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)));
            *g_musicTrackStream = 0;
        }
        zge::closeAllSoundStreams();
        return 0;
    }
    READABLE_FUNCTION_WAITS(shutdownMusicStream, Draft);

    uint32_t musicStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(musicStubZero, Draft);

    // (&status): streamStatus of the music stream.
    uint32_t musicStreamStatus(ee::Ptr<uint32_t> status)
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        int32_t streamStatus = 0;
        if (musicTrackStream != 0)
        {
            streamStatus = zge::streamStatus(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)), status);
            return streamStatus;
        }
        streamStatus = 0;
        return streamStatus;
        return streamStatus;
    }
    READABLE_FUNCTION(musicStreamStatus, Draft);

    // PlayStream of the music stream.
    uint32_t playMusicStream()
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        if (musicTrackStream != 0)
        {
            return playStream(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(playMusicStream, Draft);

    // StopStream of the music stream.
    uint32_t stopMusicStream()
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        if (musicTrackStream != 0)
        {
            return zge::stopStream(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(stopMusicStream, Draft);

    // StopStream of the music stream (a second entry, likely pause)
    uint32_t stopMusicStreamB()
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        if (musicTrackStream != 0)
        {
            return zge::stopStream(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(stopMusicStreamB, Draft);

    // (volume, pan): setStreamVolume of the music stream.
    uint32_t setMusicStreamVolume(uint32_t flags, uint32_t param_2)
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        const int32_t volume = flags & 0xFFu;
        if (musicTrackStream != 0)
        {
            return setStreamVolume(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)), volume);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(setMusicStreamVolume, Draft);

    // (.., command, ..): sendStreamCommand16 to the music stream.
    uint32_t sendMusicStreamCommand(uint32_t milliseconds, uint32_t flags, uint32_t how)
    {
        const int32_t musicTrackStream = *g_musicTrackStream;
        const int32_t volume = flags & 0xFFu;
        int32_t rampStreamVolume = 0;
        if (musicTrackStream != 0)
        {
            rampStreamVolume = zge::rampStreamVolume(ee::Ptr<SoundStream>(static_cast<uint32_t>(musicTrackStream)), milliseconds, volume, how);
            return rampStreamVolume;
        }
        rampStreamVolume = 0;
        return rampStreamVolume;
        return rampStreamVolume;
    }
    READABLE_FUNCTION_WAITS(sendMusicStreamCommand, Draft);


    // SoundStreamStubZeroG.
    int32_t musicStreamStubG()
    {
        return zge::soundStreamStubZeroG();
    }
    READABLE_FUNCTION(musicStreamStubG, Draft);


    // SoundStreamStubZeroH.
    int32_t musicStreamStubH()
    {
        return zge::soundStreamStubZeroH();
    }
    READABLE_FUNCTION(musicStreamStubH, Draft);

    // soundtrackName (was musicTrackId; inlinesoundUIUpdate): its name.
    ee::Ptr<char> soundtrackName(ee::Ptr<Soundtrack> soundtrack)
    {
        return ee::Ptr<char>(soundtrack->name);
    }
    READABLE_FUNCTION(soundtrackName, Draft);

    // soundtrackTrackCount (was musicTrackField48; inlinesoundUIUpdate).
    uint32_t soundtrackTrackCount(ee::Ptr<Soundtrack> soundtrack)
    {
        return soundtrack->trackCount;
    }
    READABLE_FUNCTION(soundtrackTrackCount, Draft);

    // soundtrackTrackTitle (was musicTrackName): the track's title, 0 past
    // the last track.
    ee::Ptr<char> soundtrackTrackTitle(ee::Ptr<Soundtrack> soundtrack, uint32_t track)
    {
        if (track >= soundtrack->trackCount)
            return ee::Ptr<char>();
        return ee::Ptr<char>((ee::Ptr<MusicTrackNames>(soundtrack->names) + track)->title);
    }

    // soundtrackTitleString: ZGE4's soundtrackTrackTitle: the track's title
    // string (0: none), 0 past the last track.
    ee::Ptr<char> soundtrackTitleString(ee::Ptr<Soundtrack> soundtrack, uint32_t track)
    {
        if (track >= soundtrack->trackCount)
            return ee::Ptr<char>();
        return soundtrackTrackStrings(soundtrack)[track].title;
    }
    READABLE_FUNCTION_OWN(soundtrackTrackTitle, Verified, bmx, soundtrackTitleString);

    // soundtrackTrackArtist (was trackArtist; the soundtrack screen): the
    // track's artist, 0 past the last track.
    ee::Ptr<char> soundtrackTrackArtist(ee::Ptr<Soundtrack> soundtrack, uint32_t track)
    {
        if (track >= soundtrack->trackCount)
            return ee::Ptr<char>();
        return ee::Ptr<char>((ee::Ptr<MusicTrackNames>(soundtrack->names) + track)->artist);
    }

    // soundtrackArtistString: ZGE4's soundtrackTrackArtist: the track's
    // artist string (0: none), 0 past the last track.
    ee::Ptr<char> soundtrackArtistString(ee::Ptr<Soundtrack> soundtrack, uint32_t track)
    {
        if (track >= soundtrack->trackCount)
            return ee::Ptr<char>();
        return soundtrackTrackStrings(soundtrack)[track].artist;
    }
    READABLE_FUNCTION_OWN(soundtrackTrackArtist, Verified, bmx, soundtrackArtistString);

    // soundtrackTrackLocation (was trackFileName; the music player): where
    // the track's wave is (its bank's file, offset and size: bankSampleInfo's
    // one WaveLocation), for a stream.
    ee::Ptr<WaveLocation> soundtrackTrackLocation(ee::Ptr<Soundtrack> soundtrack, uint32_t track)
    {
        return bankSampleInfo(soundtrack->bank, track);
    }
    READABLE_FUNCTION_WAITS(soundtrackTrackLocation, Verified);

    namespace
    {
        // The music track's stream (g_musicTrackStream) as openSoundStream
        // fills it.
        inline ee::Global<ee::Ptr<SoundStream>, sym::g_musicTrackStream> g_musicStream;

        // openMusicTrack's flags: the stream's rate and channels.
        constexpr uint32_t kTrackLowRate = 1u << 0; // 11025 Hz, else 44100
        constexpr uint32_t kTrackNoRate = 1u << 1;  // no rate bit at all (before kTrackLowRate)
        constexpr uint32_t kTrackMono = 1u << 2;    // else stereo

        namespace routed
        {
        }
    }

    // openMusicTrack (nothing calls it in AI): the music stream closed, then,
    // for a wave, opened on it (flags: kTrackLowRate, kTrackNoRate,
    // kTrackMono; the buffer initMusicStream kept), its loop count set.
    // 0, or openSoundStream's failure.
    uint32_t openMusicTrack(ee::Ptr<WaveLocation> location, uint32_t flags, uint32_t loops)
    {
        if (const ee::Ptr<SoundStream> open = *g_musicStream)
            closeSoundStream(open);
        *g_musicStream = ee::Ptr<SoundStream>();
        if (!location)
            return 0;
        uint32_t streamFlags = kStream44100;
        if (flags & kTrackNoRate)
            streamFlags = 0;
        else if (flags & kTrackLowRate)
            streamFlags = kStream11025;
        if (!(flags & kTrackMono))
            streamFlags |= kStreamStereo;
        const uint32_t failed = openSoundStream(g_musicStream.get(), location, 0u, *g_musicTrackBufferSize, streamFlags);
        if (failed)
            return failed;
        zge::setSoundStreamLoops(*g_musicStream, loops);
        return 0;
    }
    READABLE_FUNCTION_WAITS(openMusicTrack, Draft);

    // soundtrackIndex (was musicState): the soundtrack's place in the order
    // they were made (soundtrackAt's index). Kept bug: one not among them
    // walks on past the last (see nextSoundtrack).
    int32_t soundtrackIndex(ee::Ptr<Soundtrack> soundtrack)
    {
        int32_t index = 0;
        for (ee::Ptr<Soundtrack> at = poolRecordAs<Soundtrack>(firstPoolLink(*g_soundtracks)); at && at != soundtrack; at = nextSoundtrack(at))
            ++index;
        return index;
    }
    READABLE_FUNCTION(soundtrackIndex, Verified);

}
