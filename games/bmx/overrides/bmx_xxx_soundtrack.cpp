// BMX XXX (SLUS-20415): the soundtrack's controls (next, previous, shuffle;
// engine/overrides/zge_soundtrack.h) on its music.
//
// The game picks its songs in its sound frame (updateGameFrame's playlist
// step, games/bmx/sound/events.cpp): while a level is ridden (play phases 1
// and 3), when the stream has no song and the wait after the last start is
// over, the song asked for, else the level's first, else the next of the
// track table that is on (games/bmx/sound/soundtrack.h: a row for every
// MUSIC.ZSD wave, so the Xbox's songs and the songs a mod adds come in their
// turn), or the same again when the playlist does not go on. Sound event 3
// asks for a song (the soundtrack's last at most, in play phase 1), setting
// it to start next. The controls ask the same way, from the same frame,
// before it runs.
#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_soundtrack.cpp.h"

#include "overrides/zge_soundtrack.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmx_xxx::soundtrack
{
    namespace
    {
        namespace zs = zge_host::soundtrack;

        // ZGE4's soundtrack record (engine/zge/sound/soundtrack.h) as far as
        // its count of songs (its music bank's waves).
        struct SoundtrackRecord
        {
            uint32_t nameString;
            uint16_t bank;
            uint16_t _06;
            uint32_t newArgument;
            uint32_t trackCount; // +0x0C
        };
        static_assert(offsetof(SoundtrackRecord, trackCount) == 0x0C);

        // A row of the track table (games/bmx/sound/soundtrack.h SoundtrackTrack).
        struct TrackRow
        {
            uint32_t soundtrack;
            uint32_t index;
            uint32_t enabled; // +0x08 the playlist plays it
        };
        static_assert(offsetof(TrackRow, enabled) == 0x08 && sizeof(TrackRow) == 0x0C);

        // The sound mode (sound_mode.h): play and its pause; the play phases
        // the playlist runs in (1 and 3) and event 3 takes (1); the park
        // editor's game state, whose playlist starts no song.
        constexpr int32_t kPlayMode = 2, kPausedMode = 3;
        constexpr int32_t kRidingPhase = 1, kRidingAgainPhase = 3;
        constexpr uint8_t kGameStateParkEditor = 2;

        int32_t word(const uint8_t *rdram, uint32_t address) { return static_cast<int32_t>(zs::guestWord(rdram, address)); }
        int8_t signedByte(const uint8_t *rdram, uint32_t address) { return static_cast<int8_t>(zs::guestByte(rdram, address)); }

        int gameSongs(const uint8_t *rdram)
        {
            const uint32_t soundtrack = zs::guestWord(rdram, sym::bmx::g_gameSoundtrack);
            if (soundtrack == 0u)
                return 0;
            return static_cast<int>(zs::guestWord(rdram, soundtrack + static_cast<uint32_t>(offsetof(SoundtrackRecord, trackCount))));
        }

        bool parkEditor(const uint8_t *rdram) { return zs::guestByte(rdram, sym::bmx::g_nextGameState) == kGameStateParkEditor; }

        zs::Music read(const uint8_t *rdram)
        {
            zs::Music music;
            const int32_t mode = word(rdram, sym::bmx::g_gameMode);
            const int32_t phase = word(rdram, sym::bmx::g_playPhase);
            music.running = (mode == kPlayMode || mode == kPausedMode) && phase == kRidingPhase && !parkEditor(rdram);
            music.playing = signedByte(rdram, sym::bmx::g_currentTrack);
            music.next = signedByte(rdram, sym::bmx::g_nextTrack);
            // (updateGameFrame's own test)
            music.picking = mode == kPlayMode && (phase == kRidingPhase || phase == kRidingAgainPhase) &&
                            music.next == zs::kNoSong && zs::guestWord(rdram, sym::bmx::g_noTracks) == 0u && !parkEditor(rdram) &&
                            zs::streamEnded(rdram, zs::guestWord(rdram, sym::bmx::g_musicStream));
            music.goesOn = zs::guestByte(rdram, sym::bmx::g_playlistAdvances) != 0u;
            return music;
        }

        // The rows that are on.
        std::vector<int> playlist(const uint8_t *rdram)
        {
            std::vector<int> songs;
            const uint32_t table = zs::guestWord(rdram, sym::bmx::g_soundtrackTracks);
            const int count = static_cast<int>(zs::guestWord(rdram, sym::bmx::g_soundtrackTrackCount));
            for (int row = 0; table != 0u && row < count; ++row)
                if (zs::guestWord(rdram, table + static_cast<uint32_t>(row * sizeof(TrackRow) + offsetof(TrackRow, enabled))) != 0u)
                    songs.push_back(row);
            return songs;
        }

        // As event 3: in play phase 1, a song (the game's soundtrack's last
        // at most) set to start next.
        bool ask(uint8_t *rdram, int song)
        {
            const int count = gameSongs(rdram);
            if (count <= 0 || song < 0 || word(rdram, sym::bmx::g_playPhase) != kRidingPhase)
                return false;
            if (song > count - 1)
                song = count - 1;
            if (song == signedByte(rdram, sym::bmx::g_currentTrack) || song > 127)
                return false;
            zs::setGuestByte(rdram, sym::bmx::g_nextTrack, static_cast<uint8_t>(song));
            return true;
        }

        constexpr zs::Game kSoundtrack{
            .tag = "[bmx-xxx]",
            .frame = sym::bmx::updateGameFrame,
            .read = &read,
            .playlist = &playlist,
            .ask = &ask,
            .countsAddedSongs = true,
        };
    }
}

namespace bmx_xxx
{
    bool installSoundtrack(PS2Machine &runtime)
    {
        return zge_host::soundtrack::install(runtime, soundtrack::kSoundtrack);
    }
}
