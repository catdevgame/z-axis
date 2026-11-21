// BMX XXX's soundtrack (soundtrack.h): BMX XXX's own code (bmx.c).

#include "soundtrack.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/sound/soundtrack.h"
#include "zge/sound/streams.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/sound/soundtrack.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The soundtrack's tracks (0 until the soundtrack is loaded).
        inline ee::Global<ee::Ptr<SoundtrackTrack>, sym::g_soundtrackTracks> g_soundtrackTracks;
        // How many tracks the table has: every soundtrack's.
        inline ee::Global<uint32_t, sym::g_soundtrackTrackCount> g_soundtrackTrackCount;
        // The game's own soundtrack (the engine's record), made from the
        // music bank.
        inline ee::Global<ee::Ptr<zge::Soundtrack>, sym::g_gameSoundtrack> g_gameSoundtrack;
        // The banks: the common effects and the game's (both in sound
        // memory), and the music.
        inline ee::Global<uint16_t, sym::g_commonBank> g_commonBank;
        inline ee::Global<uint16_t, sym::g_gameBank> g_gameBank;
        inline ee::Global<uint16_t, sym::g_musicBank> g_musicBank;
        // The game's sound mode (sound events' setGameMode): 0, no sound yet.
        inline ee::Global<int32_t, sym::g_gameMode> g_gameMode;
        // The sound updates a second (the engine's emitters count by it).
        inline ee::Global<int32_t, sym::g_soundRate> g_soundRate;
        constexpr int32_t kSoundUpdatesASecond = 60;
        // The emitters' range and fade time (milliseconds).
        constexpr uint32_t kEmitterRange = 80, kEmitterMilliseconds = 100;

        constexpr ee::Ptr<const char> kAudioFolder{sym::g_textAudioFolder};         // "assets\audio\"
        constexpr ee::Ptr<const char> kCommonBankFile{sym::g_textCommonBank};       // "SFXCOM.ZSD"
        constexpr ee::Ptr<const char> kGameBankFile{sym::g_textGameBank};           // "SFXGAME.ZSD"
        constexpr ee::Ptr<const char> kMusicBankFile{sym::g_textMusicBank};         // "MUSIC.ZSD"
        constexpr uint32_t kSoundtrackName = sym::g_textSoundtrackName;             // "DMF-BMX3 Soundtrack"
        constexpr uint32_t kSongNameKey = sym::bmx::g_textSongNameKey;               // "$KSTR_SONGINFO_%02u_SONGNAME"
        constexpr uint32_t kSongArtistKey = sym::bmx::g_textSongArtistKey;           // "$KSTR_SONGINFO_%02u_ARTIST"

    }

    int32_t initGameSound()
    {
        // A three-byte record (127, 0, 0) the original fills and hands the
        // channels' set-up, which takes nothing.
        ee::Local<uint8_t[3]> channelSettings;
        ee::Local<char[0x40]> title;
        ee::Local<char[0x40]> artist;

        zge::memoryFill(channelSettings, 0, 3);
        channelSettings[1] = 0;
        channelSettings[0] = 0x7F;
        channelSettings[2] = 0;
        zge::initSoundChannels();
        *g_soundRate = kSoundUpdatesASecond;
        zge::initSoundSources();
        zge::initSoundEmitters(kEmitterRange, kEmitterMilliseconds);
        zge::initSoundStreams();
        zge::loadBank(zge::joinPath(kAudioFolder, kCommonBankFile), 1, ee::Ptr<uint16_t>(sym::g_commonBank));
        zge::loadBank(zge::joinPath(kAudioFolder, kGameBankFile), 1, ee::Ptr<uint16_t>(sym::g_gameBank));
        zge::loadBank(zge::joinPath(kAudioFolder, kMusicBankFile), 0, ee::Ptr<uint16_t>(sym::g_musicBank));
        *g_gameMode = 0;
        zge::initSoundBankPool();

        const ee::Ptr<zge::Soundtrack> soundtrack = zge::newSoundtrack(*g_musicBank, 1);
        *g_gameSoundtrack = soundtrack;
        zge::setSoundtrackName(soundtrack, kSoundtrackName);
        for (uint32_t track = 0; track < zge::soundtrackTrackCount(*g_gameSoundtrack); ++track)
        {
            zge::formatString(title, kSongNameKey, track);
            zge::formatString(artist, kSongArtistKey, track);
            zge::setPlaylistEntry(*g_gameSoundtrack, track, title.address(), artist.address());
        }

        // A row for every track of every soundtrack; the game's own start on.
        const uint32_t soundtracks = static_cast<uint32_t>(zge::soundRecordsInUse());
        *g_soundtrackTrackCount = 0;
        for (uint32_t i = 0; i < soundtracks; ++i)
            *g_soundtrackTrackCount += zge::soundtrackTrackCount(zge::soundtrackAt(i));
        *g_soundtrackTracks = ee::Ptr<SoundtrackTrack>(zge::heapAllocate(*g_soundtrackTrackCount * sizeof(SoundtrackTrack)));
        uint32_t row = 0;
        for (uint32_t i = 0; i < soundtracks; ++i)
        {
            const ee::Ptr<zge::Soundtrack> each = zge::soundtrackAt(i);
            const uint32_t own = each == *g_gameSoundtrack;
            const uint32_t tracks = zge::soundtrackTrackCount(each);
            for (uint32_t track = 0; track < tracks; ++track, ++row)
            {
                SoundtrackTrack &entry = (*g_soundtrackTracks)[row];
                entry.index = track;
                entry.enabled = own;
                entry.soundtrack = each;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(initGameSound, Verified);

    uint32_t trackEnabled(int32_t track)
    {
        const ee::Ptr<SoundtrackTrack> tracks = *g_soundtrackTracks;
        if (!tracks)
            return 0;
        return (tracks + static_cast<ptrdiff_t>(track))->enabled;
    }
    READABLE_FUNCTION(trackEnabled, Verified);

    void setTrackEnabled(int32_t track, uint32_t enabled)
    {
        const ee::Ptr<SoundtrackTrack> tracks = *g_soundtrackTracks;
        if (tracks)
            (tracks + static_cast<ptrdiff_t>(track))->enabled = enabled;
    }
    READABLE_FUNCTION(setTrackEnabled, Verified);
}
