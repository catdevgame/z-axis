// Aggressive Inline (SLUS-20327): the soundtrack's controls (next, previous,
// shuffle, the added songs; engine/overrides/zge_soundtrack.h) on its music.
//
// The game picks its songs in its sound frame (updateGameFrame,
// games/aggressive/game_frame.cpp): when the song has ended (or the one
// playing was turned off) and the gap after the last start is over, the song
// asked for, else the level's first, else the one after the last of the
// playlist whose bit is on (its 13 songs, round), or the same again when the
// playlist does not go on. Its pause page's Next Track and Previous Track ask
// for a song with sound event 4 (games/aggressive/sound/events.cpp), which in
// play phases 0 .. 2 sets the song to start next and cancels a change the
// playlist asked for. The controls ask the same way, from the same frame,
// before it runs. The songs a mod adds are MUSIC.ZSD's waves after its 47,
// past the 13 the game's playlist has (event 4 takes 13 at most): they are
// set to start next directly, as event 4 sets a song, after the playlist's
// last. Event 9 and 10 (a song's title and artist, which the pause page's
// Playlist shows for the song playing) answer none past the 13: for an added
// song they answer its title and artist (with a mod's added songs only).
#include "aggressive_inline_sound.h"
#include "sym/game_overrides/aggressive_inline_soundtrack.cpp.h"

#include "overrides/zge_sound.h"
#include "overrides/zge_soundtrack.h"

#include "machine.h"
#include "runtime/session.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace aggressive_inline::soundtrack
{
    namespace
    {
        namespace zs = zge_host::soundtrack;

        // The music's record (game_frame.cpp's Music, at g_music) as far as
        // the song to start next.
        struct Music
        {
            uint32_t soundtrack;
            uint32_t stream;    // +0x04 the songs' stream (SoundStream)
            uint8_t _08[0x14];
            int8_t next;        // +0x1C the song to start next, -1 none
            int8_t requested;   // +0x1D the playlist's change asked for (the playing song turned off), -1 none
            uint8_t _1e[2];
            int32_t paused;     // +0x20
            uint8_t _24;
            uint8_t goesOn;     // +0x25 the playlist goes on to its next song (else the song repeats)
        };
        static_assert(offsetof(Music, stream) == 0x04 && offsetof(Music, next) == 0x1C &&
                      offsetof(Music, requested) == 0x1D && offsetof(Music, goesOn) == 0x25);
        constexpr uint32_t musicField(size_t offset) { return sym::g_music + static_cast<uint32_t>(offset); }

        // The game's state (game_state.h GameState): its type and level (the
        // tutorial and the park editor have no music) and the playlist's
        // bits (g_enabledTracks: a song's bit on, it may play).
        constexpr uint8_t kParkEditorType = 5;
        constexpr uint32_t kTutorialLevel = 7;
        constexpr int kSongs = 13; // the playlist's (sound event 5)

        // The sound mode (sound_mode.h): play and its pause; the play phases
        // the music plays in (0 .. 3) and event 4 takes (0 .. 2).
        constexpr int32_t kPlayMode = 2, kPausedMode = 3;
        constexpr int32_t kLastMusicPhase = 3, kLastAskPhase = 2;

        int32_t word(const uint8_t *rdram, uint32_t address) { return static_cast<int32_t>(zs::guestWord(rdram, address)); }
        int8_t signedByte(const uint8_t *rdram, uint32_t address) { return static_cast<int8_t>(zs::guestByte(rdram, address)); }

        bool hasMusic(const uint8_t *rdram)
        {
            return word(rdram, sym::g_levelNumber) != static_cast<int32_t>(kTutorialLevel) &&
                   zs::guestByte(rdram, sym::g_gameTypeByte) != kParkEditorType;
        }

        zs::Music read(const uint8_t *rdram)
        {
            zs::Music music;
            const int32_t mode = word(rdram, sym::g_gameMode);
            const int32_t phase = word(rdram, sym::g_playPhase);
            music.running = (mode == kPlayMode || mode == kPausedMode) && phase >= 0 && phase <= kLastAskPhase && hasMusic(rdram);
            music.playing = signedByte(rdram, sym::g_currentTrack);
            music.next = signedByte(rdram, musicField(offsetof(Music, next)));
            const bool requested = signedByte(rdram, musicField(offsetof(Music, requested))) != zs::kNoSong;
            const bool ended = zs::streamEnded(rdram, zs::guestWord(rdram, musicField(offsetof(Music, stream))));
            // (updateGameFrame's own test, in play)
            music.picking = mode == kPlayMode && phase >= 0 && phase <= kLastMusicPhase && hasMusic(rdram) &&
                            (ended || requested) && music.next == zs::kNoSong;
            music.goesOn = zs::guestByte(rdram, musicField(offsetof(Music, goesOn))) != 0u;
            return music;
        }

        std::vector<int> playlist(const uint8_t *rdram)
        {
            const uint32_t bits = zs::guestByte(rdram, sym::g_enabledTracks) | zs::guestByte(rdram, sym::g_enabledTracks + 1u) << 8;
            std::vector<int> songs;
            for (int song = 0; song < kSongs; ++song)
                if (bits >> song & 1u)
                    songs.push_back(song);
            return songs;
        }

        // As event 4: in play phases 0 .. 2, a song not the one playing set
        // to start next, the playlist's own change cancelled.
        bool ask(uint8_t *rdram, int song)
        {
            const int32_t phase = word(rdram, sym::g_playPhase);
            if (song == signedByte(rdram, sym::g_currentTrack) || phase < 0 || phase > kLastAskPhase || song < 0 || song > 127)
                return false;
            zs::setGuestByte(rdram, musicField(offsetof(Music, next)), static_cast<uint8_t>(song));
            zs::setGuestByte(rdram, musicField(offsetof(Music, requested)), static_cast<uint8_t>(zs::kNoSong));
            return true;
        }

        // ---- an added song's title and artist (sound events 9 and 10) ----

        constexpr int kRegA0 = 4, kRegA1 = 5, kRegRa = 31;
        constexpr uint32_t kTitleEvent = 9, kArtistEvent = 10;
        constexpr size_t kTextLimit = 0x3C; // the playlist's fields (0x40 bytes)

        struct Titles
        {
            PS2Machine::RecompiledFunction soundEvent = nullptr;
            std::map<uint64_t, uint32_t> placed; // (wave, event) -> the text in guest memory
        };
        Titles &titles() { return zx::session::state<Titles>(); }

        // What the pause page's caption script shows: printable ASCII, no
        // quotes or braces (it sets the text with SET Text {"..."}).
        std::string captionText(const std::string &text)
        {
            std::string out;
            for (unsigned char c : text)
            {
                if (c < 0x20 || c >= 0x7F)
                    continue;
                out += c == '"' ? '\'' : c == '{' ? '(' : c == '}' ? ')' : c == '\\' ? '/' : static_cast<char>(c);
                if (out.size() >= kTextLimit)
                    break;
            }
            return out;
        }

        // soundEvent(event, a, ...): for event 9 or 10 of an added song's
        // wave, its title or artist; the game's own event otherwise.
        void soundEventHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            Titles &t = titles();
            const uint32_t event = getRegU32(ctx, kRegA0), wave = getRegU32(ctx, kRegA1);
            if ((event == kTitleEvent || event == kArtistEvent) && wave >= static_cast<uint32_t>(kSongs))
                for (const zge_host::sound::AddedSong &song : zge_host::sound::addedSongs())
                {
                    if (song.wave != wave)
                        continue;
                    uint32_t &at = t.placed[uint64_t(wave) << 32 | event];
                    if (at == 0u)
                    {
                        const std::string text = captionText(event == kTitleEvent ? song.title : song.artist);
                        at = runtime->guestMallocExtended(static_cast<uint32_t>(text.size() + 1u));
                        if (at == 0u)
                            at = runtime->guestMalloc(static_cast<uint32_t>(text.size() + 1u));
                        for (size_t i = 0; at != 0u && i <= text.size(); ++i)
                            if (uint8_t *p = getMemPtr(rdram, at + static_cast<uint32_t>(i)))
                                *p = i < text.size() ? static_cast<uint8_t>(text[i]) : 0u;
                    }
                    if (at == 0u)
                        break;
                    setReturnS32(ctx, static_cast<int32_t>(at));
                    ctx->pc = getRegU32(ctx, kRegRa);
                    return;
                }
            t.soundEvent(rdram, ctx, runtime);
        }

        constexpr zs::Game kSoundtrack{
            .tag = "[aggressive-inline]",
            .frame = sym::updateGameFrame,
            .read = &read,
            .playlist = &playlist,
            .ask = &ask,
            .countsAddedSongs = false,
        };
    }
}

namespace aggressive_inline
{
    bool installSoundtrack(PS2Machine &runtime)
    {
        using namespace soundtrack;
        if (zge_host::sound::hasAddedSongs())
        {
            // The added songs' titles in the pause page's Playlist.
            Titles &t = titles();
            t.soundEvent = runtime.lookupFunction(sym::soundEvent);
            if (!t.soundEvent || !runtime.replaceFunction(sym::soundEvent, &soundEventHook))
            {
                std::cerr << "[aggressive-inline] the added songs' titles not installed" << std::endl;
                t.soundEvent = nullptr;
            }
        }
        return zge_host::soundtrack::install(runtime, kSoundtrack);
    }
}
