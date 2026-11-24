// Dave Mirra Freestyle BMX 2 (SLUS-20159): the soundtrack's controls (next,
// previous, shuffle; engine/overrides/zge_soundtrack.h) on its music.
//
// The game picks its songs in its sound frame (musicFrame,
// games/dm2/sound/sound_mode.cpp): in a level, when the song has ended and
// the wait after the last start is over, the song asked for, else the
// level's first, else the one after the last, round its soundtrack (every
// MUSIC.ZSD wave: the songs a mod adds come in their turn), or the same again
// when the soundtrack does not go on. Its music menu asks for a song with
// sound event 3 (games/dm2/sound/sound_events.cpp: the soundtrack's last at
// most, not the one playing, in play phases 0 and 1), which sets the song to
// start next. The controls ask the same way, from the same frame, before it
// runs.
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_soundtrack.cpp.h"

#include "overrides/zge_soundtrack.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dave_mirra::soundtrack
{
    namespace
    {
        namespace zs = zge_host::soundtrack;

        // A soundtrack's record (engine/zge/sound/soundtrack.h Soundtrack,
        // ZGE2's and ZGE3's) as far as its count of songs (its music bank's
        // waves).
        struct SoundtrackRecord
        {
            char name[0x40];
            uint16_t bank;
            uint16_t _42;
            uint32_t newArgument;
            uint32_t trackCount; // +0x48
        };
        static_assert(offsetof(SoundtrackRecord, trackCount) == 0x48);

        // The sound mode (sound_mode.h): a level and its pause; the play
        // phases the music plays in (0 .. 2) and event 3 takes (0, 1).
        constexpr int32_t kLevelMode = 2, kPausedMode = 3;
        constexpr int32_t kLastMusicPhase = 2, kLastAskPhase = 1;

        int32_t word(const uint8_t *rdram, uint32_t address) { return static_cast<int32_t>(zs::guestWord(rdram, address)); }
        int8_t signedByte(const uint8_t *rdram, uint32_t address) { return static_cast<int8_t>(zs::guestByte(rdram, address)); }

        int songCount(const uint8_t *rdram)
        {
            const uint32_t soundtrack = zs::guestWord(rdram, sym::dm2::g_music);
            if (soundtrack == 0u)
                return 0;
            return static_cast<int>(zs::guestWord(rdram, soundtrack + static_cast<uint32_t>(offsetof(SoundtrackRecord, trackCount))));
        }

        zs::Music read(const uint8_t *rdram)
        {
            zs::Music music;
            const int32_t mode = word(rdram, sym::dm2::g_gameMode);
            const int32_t phase = word(rdram, sym::dm2::g_playPhase);
            music.running = (mode == kLevelMode || mode == kPausedMode) && phase >= 0 && phase <= kLastAskPhase;
            music.playing = signedByte(rdram, sym::dm2::g_currentTrack);
            music.next = signedByte(rdram, sym::dm2::g_musicNextTrack);
            // (musicFrame's own test)
            music.picking = mode == kLevelMode && phase >= 0 && phase <= kLastMusicPhase && music.next == zs::kNoSong &&
                            zs::streamEnded(rdram, zs::guestWord(rdram, sym::dm2::g_musicStream));
            music.goesOn = zs::guestByte(rdram, sym::dm2::g_musicTracksAdvance) != 0u;
            return music;
        }

        std::vector<int> playlist(const uint8_t *rdram)
        {
            std::vector<int> songs;
            for (int song = 0, count = songCount(rdram); song < count; ++song)
                songs.push_back(song);
            return songs;
        }

        // As event 3: in play phases 0 and 1, a song (the last at most) not
        // the one playing set to start next.
        bool ask(uint8_t *rdram, int song)
        {
            const int count = songCount(rdram);
            const int32_t phase = word(rdram, sym::dm2::g_playPhase);
            if (count <= 0 || song < 0)
                return false;
            if (song > count - 1)
                song = count - 1;
            if (song == signedByte(rdram, sym::dm2::g_currentTrack) || phase < 0 || phase > kLastAskPhase || song > 127)
                return false;
            zs::setGuestByte(rdram, sym::dm2::g_musicNextTrack, static_cast<uint8_t>(song));
            return true;
        }

        constexpr zs::Game kSoundtrack{
            .tag = "[dave-mirra]",
            .frame = sym::dm2::musicFrame,
            .read = &read,
            .playlist = &playlist,
            .ask = &ask,
            .countsAddedSongs = true,
        };
    }
}

namespace dave_mirra
{
    bool installSoundtrack(PS2Machine &runtime)
    {
        return zge_host::soundtrack::install(runtime, soundtrack::kSoundtrack);
    }
}
