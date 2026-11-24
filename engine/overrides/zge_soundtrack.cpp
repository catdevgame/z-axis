// The soundtrack's controls (zge_soundtrack.h): next, previous, shuffle and
// the added songs, from the game's sound frame, as the game's own event asks
// for a song.

#include "zge_soundtrack.h"
#include "zge_sound.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/play_recording.h"
#include "runtime/session.h"
#include "runtime/settings_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace zge_host::soundtrack
{
    namespace
    {
        namespace hi = ps2_host_input;
        using zge_host::sound::AddedSong;

        // The Shuffle row (the app's settings, collection/app.cpp).
        constexpr const char kShuffleRow[] = "audio.shuffle";
        // The shuffle's seed, a recorded read so a replay shuffles the same
        // (play_recording.h Channel::Random's stream for it: "SHUF").
        constexpr uint32_t kShuffleSeedStream = 0x53485546u;
        // How many songs Previous Track can go back with shuffle on.
        constexpr size_t kHistory = 64;

        // A sound stream's record as the engine keeps it (SoundStream,
        // engine/zge/sound/streams.h): the driver's state the IOP writes.
        struct SoundStreamRecord
        {
            uint16_t flags;
            uint16_t _02;
            uint32_t id;
            uint32_t file;
            uint32_t loops;
            uint32_t clearedAtOpen[2];
            uint8_t _18[0x18];
            uint32_t state; // 0 .. 8
            uint8_t _34[0x0C];
        };
        static_assert(offsetof(SoundStreamRecord, state) == 0x30 && sizeof(SoundStreamRecord) == 0x40);

        struct State
        {
            const Game *game = nullptr;
            PS2Machine::RecompiledFunction frame = nullptr;
            uint32_t seen[3] = {};       // the three keys' presses (next, previous, shuffle) as last seen
            bool seenTaken = false;
            bool shuffleWithoutRow = false; // ZX_MUSIC_SHUFFLE, and the key, when the app has no Shuffle row
            int playing = kNoSong;       // the song last seen playing
            std::string cause;           // what asked for the song to start next ("" the game itself)
            int asked = kNoSong;         // and that song
            std::vector<int> history;    // the songs played before it (shuffle's Previous Track)
            bool back = false;           // the next song is Previous Track's from the history
            std::vector<int> bag;        // shuffle's songs not yet played this round
            std::mt19937 random;
            bool seeded = false;
            std::vector<AddedSong> added; // the added songs, once MUSIC.ZSD's head is made
            bool addedKnown = false;
        };
        State &state() { return zx::session::state<State>(); }

        bool shuffleOn()
        {
            State &st = state();
            if (zx::settings::has(kShuffleRow))
                return zx::settings::value(kShuffleRow, 0) != 0;
            return st.shuffleWithoutRow;
        }

        // Every song the game would play: its playlist, and the added songs
        // after it where it does not count them itself.
        std::vector<int> songs(const uint8_t *rdram)
        {
            State &st = state();
            std::vector<int> list = st.game->playlist(rdram);
            if (!st.game->countsAddedSongs)
                for (const AddedSong &song : st.added)
                    list.push_back(static_cast<int>(song.wave));
            return list;
        }

        // The song after `from` in the list, round it (from not in it: the
        // first after it by number, else the first).
        int songAfter(const std::vector<int> &list, int from)
        {
            if (list.empty())
                return kNoSong;
            const auto at = std::find(list.begin(), list.end(), from);
            if (at != list.end())
                return std::next(at) == list.end() ? list.front() : *std::next(at);
            for (int song : list)
                if (song > from)
                    return song;
            return list.front();
        }

        // The song before `from`, round the list (from not in it: the last
        // before it by number, else the last).
        int songBefore(const std::vector<int> &list, int from)
        {
            if (list.empty())
                return kNoSong;
            const auto at = std::find(list.begin(), list.end(), from);
            if (at != list.end())
                return at == list.begin() ? list.back() : *std::prev(at);
            for (auto song = list.rbegin(); song != list.rend(); ++song)
                if (*song < from)
                    return *song;
            return list.back();
        }

        // A song of the list at random, none twice until each has played,
        // never the one playing when there is another.
        int shuffledSong(const std::vector<int> &list, int playing)
        {
            State &st = state();
            if (list.empty())
                return kNoSong;
            if (!st.seeded)
            {
                std::random_device device;
                const uint32_t seed = play_recording::read(play_recording::Channel::Random, static_cast<uint32_t>(device()),
                                                           kShuffleSeedStream);
                st.random.seed(seed);
                st.seeded = true;
            }
            // (a song the playlist no longer has leaves the round)
            st.bag.erase(std::remove_if(st.bag.begin(), st.bag.end(),
                                        [&list](int song) { return std::find(list.begin(), list.end(), song) == list.end(); }),
                         st.bag.end());
            st.bag.erase(std::remove(st.bag.begin(), st.bag.end(), playing), st.bag.end());
            if (st.bag.empty())
            {
                st.bag = list;
                if (st.bag.size() > 1u)
                    st.bag.erase(std::remove(st.bag.begin(), st.bag.end(), playing), st.bag.end());
            }
            std::uniform_int_distribution<size_t> pick(0, st.bag.size() - 1u);
            return st.bag[pick(st.random)];
        }

        // The song asked for leaves shuffle's round.
        void played(int song)
        {
            std::vector<int> &bag = state().bag;
            bag.erase(std::remove(bag.begin(), bag.end(), song), bag.end());
        }

        std::string songName(int song)
        {
            if (song == kNoSong)
                return "no song";
            for (const AddedSong &added : state().added)
                if (static_cast<int>(added.wave) == song)
                    return "track " + std::to_string(song) + " (added: \"" + added.title + "\")";
            return "track " + std::to_string(song);
        }

        // A song asked for, as the game's event asks: true when it took it.
        bool ask(uint8_t *rdram, int song, const char *cause)
        {
            State &st = state();
            if (song == kNoSong || !st.game->ask(rdram, song))
                return false;
            st.cause = cause;
            st.asked = song;
            played(song);
            std::cout << "[soundtrack] " << cause << ": " << songName(song) << " asked for (" << songName(st.playing) << " playing)"
                      << std::endl;
            return true;
        }

        // Whether a key was pressed since the last frame.
        bool pressed(int hotkey, uint32_t &seen)
        {
            const uint32_t presses = PSPadBackend::hotkeyPresses(hotkey);
            const bool was = presses != seen;
            seen = presses;
            return was;
        }

        // The game's sound frame: the keys and the songs the controls ask
        // for, then the game's own frame, which starts them.
        void frame(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const Game &game = *st.game;
            if (!st.addedKnown)
            {
                st.added = zge_host::sound::addedSongs();
                st.addedKnown = !st.added.empty();
            }
            const Music music = game.read(rdram);

            // A song started since the last frame: said, with what asked for it.
            if (music.playing != st.playing && music.playing != kNoSong)
            {
                const char *by = music.playing == st.asked && !st.cause.empty() ? st.cause.c_str() : "the game's playlist";
                std::cout << "[soundtrack] " << songName(music.playing) << " playing (" << by << ")" << std::endl;
                if (st.playing != kNoSong && !st.back)
                {
                    st.history.push_back(st.playing);
                    if (st.history.size() > kHistory)
                        st.history.erase(st.history.begin());
                }
                st.back = false;
                st.cause.clear();
                st.asked = kNoSong;
                st.playing = music.playing;
            }
            else if (music.playing == kNoSong && st.playing != kNoSong)
                st.playing = kNoSong; // (a level left: its next starts afresh)

            if (!st.seenTaken)
            {
                // (presses before the game's music was ever seen are not the controls')
                st.seen[0] = PSPadBackend::hotkeyPresses(hi::kHotkeyNextTrack);
                st.seen[1] = PSPadBackend::hotkeyPresses(hi::kHotkeyPreviousTrack);
                st.seen[2] = PSPadBackend::hotkeyPresses(hi::kHotkeyShuffle);
                st.seenTaken = true;
            }
            const bool next = pressed(hi::kHotkeyNextTrack, st.seen[0]);
            const bool previous = pressed(hi::kHotkeyPreviousTrack, st.seen[1]);
            if (pressed(hi::kHotkeyShuffle, st.seen[2]))
            {
                const bool on = !shuffleOn();
                if (zx::settings::has(kShuffleRow))
                    zx::settings::setFromGame(kShuffleRow, on ? 1 : 0);
                else
                    st.shuffleWithoutRow = on;
                st.bag.clear();
                zx::settings::notify(on ? "Shuffle On" : "Shuffle Off");
                std::cout << "[soundtrack] shuffle " << (on ? "on" : "off") << std::endl;
            }

            if (music.running && (next || previous))
            {
                const std::vector<int> list = songs(rdram);
                const int from = music.next != kNoSong ? music.next : music.playing;
                if (next)
                {
                    const int song = shuffleOn() ? shuffledSong(list, from) : songAfter(list, from);
                    if (ask(rdram, song, "next track"))
                        zx::settings::notify("Next Track");
                }
                else
                {
                    int song = kNoSong;
                    const bool back = shuffleOn() && !st.history.empty();
                    if (back)
                        song = st.history.back();
                    else
                        song = songBefore(list, from);
                    if (ask(rdram, song, "previous track"))
                    {
                        zx::settings::notify("Previous Track");
                        if (back)
                        {
                            st.history.pop_back();
                            st.back = true;
                        }
                    }
                }
            }
            else if (music.running && music.picking)
            {
                // The game is about to pick a song of its own.
                const std::vector<int> list = songs(rdram);
                if (shuffleOn() && (music.goesOn || music.playing == kNoSong))
                    ask(rdram, shuffledSong(list, music.playing), "shuffle");
                else if (!game.countsAddedSongs && !st.added.empty() && music.goesOn && music.playing != kNoSong)
                {
                    // After the playlist's last song (or an added one) the
                    // added songs, in turn: the game's own pick would go
                    // round its playlist.
                    const std::vector<int> own = game.playlist(rdram);
                    const bool last = !own.empty() && music.playing == own.back();
                    const bool added = std::find(own.begin(), own.end(), music.playing) == own.end();
                    if (last || added)
                        ask(rdram, songAfter(list, music.playing), "added songs");
                }
            }
            st.frame(rdram, ctx, runtime);
        }
    }

    uint32_t guestWord(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, address))
            std::memcpy(&value, p, sizeof(value));
        return value;
    }

    uint8_t guestByte(const uint8_t *rdram, uint32_t address)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        return p ? *p : 0u;
    }

    void setGuestByte(uint8_t *rdram, uint32_t address, uint8_t value)
    {
        if (uint8_t *p = getMemPtr(rdram, address))
            *p = value;
    }

    bool streamEnded(const uint8_t *rdram, uint32_t stream)
    {
        if (stream == 0u)
            return false;
        const uint32_t state = guestWord(rdram, stream + static_cast<uint32_t>(offsetof(SoundStreamRecord, state)));
        return state == 0u || state == 2u || state == 5u || state == 7u;
    }

    bool install(PS2Machine &runtime, const Game &game)
    {
        State &st = state();
        st.game = &game;
        if (const char *shuffle = zx::boot::text("ZX_MUSIC_SHUFFLE"))
            st.shuffleWithoutRow = shuffle[0] == '1';
        st.frame = runtime.lookupFunction(game.frame);
        if (!st.frame || !runtime.replaceFunction(game.frame, &frame))
        {
            std::cerr << game.tag << " the soundtrack's controls not installed" << std::endl;
            st.frame = nullptr;
            return false;
        }
        return true;
    }
}
