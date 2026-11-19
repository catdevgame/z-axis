// BMX XXX's sound events (events.h): BMX XXX's own code (bmx.c). The
// events' numbers are BMX XXX's; what an event's arguments mean is read from
// what each case does with them, and from AI's version where the two do the
// same thing.

#include "events.h"

#include "bmx/sound/sound_state.h"
#include "bmx/states/game_state.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/sound/events.cpp.h"

namespace game::bmx
{
    using namespace sound;

    namespace
    {
        constexpr uint32_t kEntityVoiceKey = sym::bmx::g_textEntityVoiceKey; // "voice.entity.%s"
        constexpr uint32_t kPhraseKey = sym::bmx::g_textPhraseKey;           // "phrase.%s"

        // The stream's status while it plays.
        constexpr uint32_t kStreamPlaying = 2;
        // The stream's statuses that let the playlist start a track
        // [inferred: stopped, and ended].
        constexpr uint32_t kStreamIdle = 1, kStreamEnded = 4;
        // A stream channel's state while its stream plays.
        constexpr int32_t kChannelPlaying = 2;
        constexpr uint32_t kMusicStreamFlags = 0x22;

        // How the listener hears a placed sample (placedVolume's outputs;
        // its third it sets to 0 for a sound with no position, and nothing
        // reads it).
        struct Placed
        {
            uint8_t volume;
            int8_t pan;
            uint8_t third;
        };

        // The options' effects volume times `level` (128 is full).
        uint32_t effectsTimes(uint32_t level)
        {
            return (static_cast<uint32_t>((*g_volume)[Effects]) * level) >> 7 & 0xFFu;
        }
    }

    // In play's phase 3 nothing for events 56 .. 68, in the pause nothing
    // for 31 .. 52; nothing at all before the sounds start.
    uint32_t soundEvent(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d)
    {
        // The frame's locals: a copy of the position a points to (the sound
        // sources use it), a sample's name, how the listener hears a placed
        // sample, and a word (a count in event 0, the stream's status in 77).
        ee::Local<zge::Vec4> position;
        ee::Local<char[0x40]> name;
        ee::Local<Placed> placed;
        ee::Local<uint32_t> word;
        const auto copyPosition = [&] { *position = zge::aligned(ee::Ptr<zge::Vec4>(a)); };
        const auto effects = [] { return static_cast<uint32_t>((*g_volume)[Effects]); };

        // The shared tails, each answering 0.
        const auto playUnplaced = [&](uint32_t sample, uint32_t volume) {
            calls::playSample(sample, 0, volume, 0, kUnplaced);
            return 0u;
        };
        const auto hear = [&](uint32_t sample, uint32_t volume) {
            placedVolume(sample, ee::Ptr<const zge::Vec4>(a), volume, &placed->volume, &placed->pan, &placed->third);
        };
        // A sample placed at a, played as the listener hears it.
        const auto placedSample = [&](uint32_t sample) {
            hear(sample, effects());
            calls::playSample(sample, 0, placed->volume, placed->pan, kPlaced);
            return 0u;
        };
        const auto common = [&](uint32_t index) { return playUnplaced(calls::sample(*g_commonBank, index), effects()); };
        const auto game = [&](uint32_t index) { return playUnplaced(calls::sample(*g_gameBank, index), effects()); };
        // The game bank's sample placed at a, at level c of what is heard.
        const auto gamePlaced = [&](uint32_t index) {
            const uint32_t sample = calls::sample(*g_gameBank, index);
            hear(sample, effects());
            calls::playSample(sample, 0, (static_cast<uint32_t>(placed->volume) * c) >> 7 & 0xFFu, placed->pan, kPlaced);
            return 0u;
        };
        const auto tracks = [&] { return soundEvent(4, 0, 0, 0, 0); };

        if (*g_gameMode == kNoSoundMode)
            return 0;
        const uint32_t random = calls::randomNumber();
        const int32_t mode = *g_gameMode;
        if (mode == kPlayMode)
        {
            if (*g_playPhase == 3 && event - 56u < 13u)
                return 0;
        }
        else if (mode == kPausedMode)
        {
            if (event - 31u < 22u)
                return 0;
        }
        if (event >= 78u)
            return 1;

        switch (event)
        {
        case 0: // the options' volume of kind a set to b (c: play a sample at it), and set on what plays
        {
            const ee::Ptr<uint8_t> volumes(sym::g_masterVolume); // by a's kind (Volume)
            volumes[a] = static_cast<uint8_t>(b);
            if (a == Music)
            {
                if (mode == kFrontEndMode)
                {
                    calls::setVoiceVolume(*g_musicVoice, (*g_volume)[Music]);
                    return 0;
                }
                if (mode <= 0 || mode >= kPlacedSoundsMode)
                    return 0;
                calls::setStreamVolume(*g_musicStream, (static_cast<uint32_t>((*g_volume)[Music]) * kMusicStreamLevel) >> 7, 0);
                return 0;
            }
            if (a == Effects)
            {
                int32_t now = mode;
                if (c)
                {
                    calls::playSample(calls::sample(*g_commonBank, 1), 0, effects(), 0, kUnplaced);
                    now = *g_gameMode;
                }
                if (now == kFrontEndMode || now <= 0 || now >= kPlacedSoundsMode)
                    return 0;
                // In play or the pause: every group's source.
                for (*word = 0; *word < calls::groupCount(*g_levelBank); *word = *word + 1)
                    calls::setSourceMix(kGroupVoiceTable[*word], effects(), 0);
                return 0;
            }
            if (a != Speech)
                return 2;
            if (mode < kPlacedSoundsMode && mode >= kPlayMode)
            {
                calls::setChannelVolume(*g_speechChannel, (static_cast<uint32_t>((*g_volume)[Speech]) * kSpeechChannelLevel) >> 7, 0);
                return 0;
            }
            if (!c)
                return 0;
            return playUnplaced(calls::sample(*g_commonBank, 1), (*g_volume)[Speech]);
        }
        case 1: // the IOP's sound driver told {2, a, b}
            *g_iopKind = 2;
            *g_iopA = static_cast<uint8_t>(a);
            *g_iopB = static_cast<uint8_t>(b);
            calls::iopCommand(sym::g_iopKind);
            return 0;
        case 2: // the track asked for, else the one playing, within the game's soundtrack
        {
            const uint32_t count = zge::soundtrackTrackCount(*g_gameSoundtrack);
            const int8_t track = *g_nextTrack != kNoTrack ? *g_nextTrack : *g_currentTrack;
            return static_cast<uint32_t>(static_cast<int32_t>(track)) % count;
        }
        case 3: // the next track: a (the game's soundtrack's last at most), in play's phase 1
        {
            uint32_t track = a;
            if (track >= zge::soundtrackTrackCount(*g_gameSoundtrack) - 1)
                track = zge::soundtrackTrackCount(*g_gameSoundtrack) - 1;
            if (*g_playPhase != 1)
                return 0;
            // Asked and not used: the original's message for a track that is
            // off is not in this build.
            trackEnabled(static_cast<int32_t>(track));
            *g_nextTrack = static_cast<int8_t>(track);
            return 0;
        }
        case 4: // how many tracks
            return *g_soundtrackTrackCount;
        case 5: // track a on (the first after none is asked for at once)
        {
            if (a >= tracks())
                return 0;
            setTrackEnabled(static_cast<int32_t>(a), 1);
            if (!*g_noTracks)
                return 0;
            *g_nextTrack = static_cast<int8_t>(a);
            *g_trackWait = kFirstTrackWait;
            *g_noTracks = 0;
            return 0;
        }
        case 6: // track a off (the music stops with the last one, but in the park editor)
        {
            const uint32_t count = tracks();
            if (a >= count)
                return 0;
            setTrackEnabled(static_cast<int32_t>(a), 0);
            uint32_t on = 0;
            for (uint32_t track = 0; track < count; ++track)
                if (trackEnabled(static_cast<int32_t>(track)))
                    ++on;
            if (on == 0)
            {
                *g_noTracks = 1;
                if (!*g_musicStream)
                    return 0;
                if (gameState() == kGameStateParkEditor)
                    return 0;
                calls::stopStream(*g_musicStream);
                return 0;
            }
            if (*g_currentTrack != static_cast<int8_t>(a))
                return 0;
            *g_trackChange = 1;
            return 0;
        }
        case 7: // track a on or not
            if (a >= tracks())
                return 0;
            return trackEnabled(static_cast<int32_t>(a));
        case 8: // track a's title
        case 9: // and its artist
        {
            if (a >= tracks())
                return 0;
            const ee::Ptr<SoundtrackTrack> table = *g_soundtrackTracks;
            if (!table)
                return 0;
            const SoundtrackTrack &track = table[a];
            return ee::Address(event == 8 ? zge::soundtrackTrackTitle(track.soundtrack, track.index)
                                          : zge::soundtrackTrackArtist(track.soundtrack, track.index));
        }
        case 10: // the game's soundtrack's place among the soundtracks
            return zge::soundtrackIndex(*g_gameSoundtrack);
        case 11: // soundtrack a made the game's
            *g_gameSoundtrack = zge::soundtrackAt(a);
            return 0;
        case 12: case 13: case 14: case 16: case 17: // the menus' sounds: the common bank's first six by event
            return common(event - 12);
        case 15: // the common bank's 5 in the front end, 3 elsewhere
            return common(mode == kFrontEndMode ? 5 : 3);
        case 22: // the common bank's first group played once
            if (calls::groupCount(*g_commonBank))
                calls::playGroupOnce(calls::group(*g_commonBank, 0), 127, 0);
            return 0;
        case 24: return common(2);
        case 25: return common(3);
        case 26: return common(5);
        case 27: return common(1);
        case 28: return common(1);
        case 29: return common(4);
        case 30: // the common bank's sample b (a must be 0)
        {
            if (a != 0)
                return 0;
            if (b >= calls::sampleCount(*g_commonBank))
                return 0;
            calls::playSample(calls::sample(*g_commonBank, b), 0, effects(), 0, kPlaced);
            return 0;
        }
        // The game bank's sets, placed at a, at level c: b's low byte picks
        // within a set, its second byte the set's kind.
        case 31: return gamePlaced((b & 0xFFu) + 30);
        case 32: return gamePlaced((b & 0xFFu) + 21);
        case 33:
        {
            const uint32_t which = b & 0xFFu;
            switch (b >> 8 & 0xFFu)
            {
            case 2: return gamePlaced(which + 58);
            case 3: return gamePlaced(67);
            case 4: return gamePlaced(69);
            case 5: return gamePlaced(68);
            default: return gamePlaced(which + 49);
            }
        }
        case 34: return gamePlaced((b & 0xFFu) + 40);
        case 35: // three of a kind by how hard (c: under 32, under 64, the rest)
        {
            const uint32_t kind = b >> 8 & 0xFFu;
            const uint32_t strength = c >> 5 < 3 ? c >> 5 : 2;
            return gamePlaced(kind * 3 + strength + 70);
        }
        case 36: // the game bank's b from 97, placed at a, at level c * 32 + 31
        {
            const uint32_t sample = calls::sample(*g_gameBank, b + 97);
            hear(sample, effects());
            placed->volume = static_cast<uint8_t>(((c << 5) + 31) * placed->volume >> 7);
            calls::playSample(sample, 0, placed->volume, placed->pan, kPlaced);
            return 0;
        }
        case 37: // one of character d's bank's four at random, placed at a, pitched and levelled by c
        {
            const uint32_t sample = calls::sample(kCharacterBankTable[d], random & 3);
            hear(sample, effects());
            placed->volume = static_cast<uint8_t>(((c << 4) + 79) * placed->volume >> 7);
            calls::playSample(sample, static_cast<int16_t>(c << 6), placed->volume, placed->pan, kPlaced);
            return 0;
        }
        case 39: // the looping placed sound: b from 8 (a new sample stops the old one)
        {
            const uint32_t index = b + 8;
            const uint32_t sample = calls::sample(*g_weaponBank, index);
            hear(sample, effects());
            uint32_t voice = *g_loopVoice;
            if (voice != kNoVoice && *g_loopSample != index)
            {
                calls::stopVoice(voice, 3);
                *g_loopVoice = kNoVoice;
                voice = kNoVoice;
            }
            if (voice == kNoVoice)
            {
                const uint32_t allocated = calls::allocateVoice(sample, kUnplaced);
                *g_loopVoice = static_cast<uint8_t>(allocated);
                if ((allocated & 0xFFu) == kNoVoice)
                    return 0;
                *g_loopSample = static_cast<uint16_t>(index);
            }
            calls::setVoiceVolume(*g_loopVoice, placed->volume);
            calls::setVoicePan(*g_loopVoice, placed->pan);
            calls::startVoice(*g_loopVoice);
            return 0;
        }
        // The weapons' bank's sets of b, placed at a.
        case 38: return placedSample(calls::sample(*g_weaponBank, b));
        case 40: return placedSample(calls::sample(*g_weaponBank, 16 + b));
        case 41: return placedSample(calls::sample(*g_weaponBank, 24 + b));
        case 42: return placedSample(calls::sample(*g_weaponBank, 32 + b));
        case 43: return placedSample(calls::sample(*g_weaponBank, 40 + b));
        case 44: return placedSample(calls::sample(*g_weaponBank, 48 + b));
        case 45: return placedSample(calls::sample(*g_weaponBank, 80));
        case 46: return placedSample(calls::sample(*g_weaponBank, 81));
        case 47: // a sound source started at a: the weapons' bank's b from 56 (returns it)
        {
            copyPosition();
            const uint32_t source = calls::startSource(calls::sample(*g_weaponBank, b + 56), position, 0);
            if (source != kNoSource)
                calls::sourceVolume(source, effects());
            return source;
        }
        case 48: // source b stopped
            if ((b & 0xFFFFu) != kNoSource)
                calls::stopSource(b & 0xFFFFu);
            return 0;
        case 49: // source b moved to a
            if ((b & 0xFFFFu) == kNoSource)
                return 0;
            copyPosition();
            calls::setSourcePosition(b & 0xFFFFu, position);
            return 0;
        case 50: // the weapons' bank's b from 64; for b 0, one of two at random: 84 and 85 for c 1, else 82 and 83
        {
            uint32_t index = b + 64;
            if (b == 0)
                index = (c == 1 ? 84 : 82) + (random & 1);
            return placedSample(calls::sample(*g_weaponBank, index));
        }
        case 51: return placedSample(calls::sample(*g_weaponBank, b + 72));
        case 53:
            copyPosition();
            return 0;
        case 54: // a named voice of the level's bank ("voice.entity." and c): b 0 a source of it started at a
        case 55: //   (level d; returns it), 1 source c stopped, 2 its level d and position a, 3 played once at a
        {
            copyPosition();
            const uint32_t source = c & 0xFFFFu;
            switch (b)
            {
            case 0:
            {
                zge::formatString(name, kEntityVoiceKey, c);
                const uint32_t bank = *g_levelBank;
                const uint32_t sample = calls::sampleByHash(bank, calls::nameHash(name));
                if (!sample)
                    return kNoSource;
                const uint32_t started = calls::startSource(sample, position, 0);
                if (started != kNoSource)
                    calls::sourceVolume(started, effectsTimes(d));
                return started;
            }
            case 1:
                if (source != kNoSource)
                    calls::stopSource(source);
                return 0;
            case 2:
                if (source == kNoSource)
                    return 0;
                calls::sourceVolume(source, effectsTimes(d));
                calls::setSourcePosition(source, position);
                return 0;
            case 3:
            {
                zge::formatString(name, kEntityVoiceKey, c);
                const uint32_t bank = *g_levelBank;
                const uint32_t sample = calls::sampleByHash(bank, calls::nameHash(name));
                if (!sample)
                    return 0;
                hear(sample, effectsTimes(d));
                calls::playSample(sample, 0, placed->volume, placed->pan, kPlaced);
                return 1;
            }
            default:
                return 0;
            }
        }
        case 56: // the game bank's 102 (a 0) or 101 (a 1 .. 10)
            if (a == 0)
                return game(102);
            if (a > 10)
                return 0;
            return game(101);
        case 57: // the game bank's 103, pitched up by a * 33 (500 at most)
        {
            const uint32_t sample = calls::sample(*g_gameBank, 103);
            const uint32_t pitch = a * 33 <= 500u ? a * 33 : 500u;
            calls::playSample(sample, static_cast<int16_t>(pitch), effects(), 0, kUnplaced);
            return 0;
        }
        case 60: return game(a == 1 ? 104 : a == 0 ? 105 : 106);
        case 61: return game(107);
        case 62: return game(108);
        case 63: return game(109);
        case 64: return game(110);
        case 65: case 66: case 67: return game(event + 46);
        case 68: return game(a + 114);
        case 69: return game(117);
        case 70: return game(118);
        case 71: // the speech's phrases may all be said again
            calls::resetStreamChannelPlaylists(*g_speechChannel);
            return 0;
        case 72: // the phrase named "phrase." and b said at a
        {
            zge::formatString(name, kPhraseKey, b);
            const uint32_t bank = *g_speechBank;
            const uint32_t phrase = calls::playlistByHash(bank, calls::nameHash(name));
            if (phrase)
                playSpeech(phrase, ee::Ptr<const zge::Vec4>(a));
            return 0;
        }
        case 73: // the phrase whose name hashes to b said at a, while the level is ridden
        {
            if (mode != kPlayMode || *g_playPhase != 1)
                return 0;
            const uint32_t phrase = calls::playlistByHash(*g_speechBank, b);
            if (phrase)
                playSpeech(phrase, ee::Ptr<const zge::Vec4>(a));
            return 0;
        }
        case 74: // the same over a phrase still held; with no such phrase, the level's bank's sample of that name
        {
            if (mode != kPlayMode || *g_playPhase != 1)
                return 0;
            const uint32_t phrase = calls::playlistByHash(*g_speechBank, b);
            if (phrase)
            {
                *g_speechHold = 0;
                playSpeech(phrase, ee::Ptr<const zge::Vec4>(a));
                return 0;
            }
            const uint32_t sample = calls::sampleByHash(*g_levelBank, b);
            if (!sample)
                return 0;
            return playUnplaced(sample, effects());
        }
        case 75: // the cutscenes' bank's wave whose name hashes to a opened on the music's stream
        {
            const uint32_t wave = calls::waveLocationByHash(*g_cutsceneBank, a);
            if (wave)
                calls::openStream(*g_musicStream, wave, kMusicStreamFlags);
            return 0;
        }
        case 76: // and played, at the effects volume
            calls::setStreamVolume(*g_musicStream, effects(), 0);
            calls::playStream(*g_musicStream);
            return 0;
        case 77: // whether the music's stream plays
            calls::streamStatus(*g_musicStream, word);
            return *word == kStreamPlaying;
        case 18: case 19: case 52: // events this build leaves alone, answered 1
            return 1;
        default: // 20, 21, 23, 58, 59: nothing
            return 0;
        }
    }
    READABLE_FUNCTION_WAITS(soundEvent, Verified);

    // With no listener the engine writes neither output and the volume
    // scaled is what the caller's frame held, as in AI's.
    uint32_t placedVolume(uint32_t sample, ee::Ptr<const zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan, ee::Ptr<uint8_t> third)
    {
        const uint32_t level = levelArgument & 0xFFu;
        if (position)
        {
            const uint32_t heard = calls::sourceHeard(sample, position, volume, pan, third);
            *volume = static_cast<uint8_t>((static_cast<uint32_t>(*volume) * level) >> 7);
            return heard;
        }
        *volume = static_cast<uint8_t>(level);
        *pan = 0;
        *third = 0;
        return 1;
    }
    READABLE_FUNCTION(placedVolume, Verified);

    int32_t playSpeech(uint32_t phrase, ee::Ptr<const zge::Vec4> position)
    {
        ee::Local<Placed> placed;

        if (*g_speechHold)
            return kSpeechHeld;
        placedVolume(0, position, (*g_volume)[Speech], &placed->volume, &placed->pan, &placed->third);
        const int32_t answer = calls::playChannelStream(*g_speechChannel, phrase);
        if (answer != 0)
            return answer;
        *g_speechHold = kSpeechHoldFrames;
        calls::setChannelVolume(*g_speechChannel, placed->volume, placed->pan);
        if (!position)
        {
            *g_speechPlaced = 0;
            return 0;
        }
        *g_speechPosition = zge::aligned(position);
        *g_speechPlaced = 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(playSpeech, Verified);

    namespace
    {
        // The playlist's next track from the one playing: the next one that
        // is on, around the table; the same one when no other is on.
        // g_currentTrack counts along as the original's does.
        int8_t nextEnabledTrack(uint32_t count)
        {
            const int8_t from = *g_currentTrack;
            int8_t track = from;
            for (;;)
            {
                track = static_cast<int8_t>(track + 1);
                *g_currentTrack = track;
                if (static_cast<int32_t>(track) >= static_cast<int32_t>(count))
                {
                    *g_currentTrack = 0;
                    track = 0;
                }
                if (trackEnabled(track))
                    return *g_currentTrack;
                track = *g_currentTrack;
                if (track == from)
                    return track;
            }
        }

        // While the level is ridden with the music on: a track started when
        // one is asked for or the stream has none playing.
        void playlistFrame()
        {
            const uint32_t count = soundEvent(4, 0, 0, 0, 0);
            const int8_t playing = *g_currentTrack;
            if (*g_nextTrack == kNoTrack)
            {
                if (playing == kNoTrack)
                {
                    // The level's own first track, or the next one on.
                    const uint8_t first = kLevelTracks[*g_currentLevel];
                    *g_currentTrack = static_cast<int8_t>(first);
                    if (trackEnabled(static_cast<int8_t>(first)))
                        *g_nextTrack = *g_currentTrack;
                    else
                        *g_nextTrack = nextEnabledTrack(count);
                }
                else if (!*g_playlistAdvances)
                    *g_nextTrack = playing;
                else
                {
                    const int8_t next = nextEnabledTrack(count);
                    *g_trackChange = -1;
                    *g_nextTrack = next;
                }
            }
            if (gameState() == kGameStateParkEditor)
                return;
            ee::Ptr<zge::Soundtrack> soundtrack = *g_gameSoundtrack;
            uint32_t index = static_cast<uint32_t>(static_cast<int32_t>(*g_nextTrack));
            if (const ee::Ptr<SoundtrackTrack> table = *g_soundtrackTracks)
            {
                const SoundtrackTrack &track = *(table + static_cast<ptrdiff_t>(*g_nextTrack));
                index = track.index;
                soundtrack = track.soundtrack;
            }
            calls::openStream(*g_musicStream, zge::soundtrackTrackLocation(soundtrack, index), kMusicStreamFlags);
            calls::setStreamVolume(*g_musicStream, (static_cast<uint32_t>((*g_volume)[Music]) * kMusicStreamLevel) >> 7, 0);
            calls::playStream(*g_musicStream);
            const int8_t started = *g_nextTrack;
            *g_nextTrack = kNoTrack;
            *g_currentTrack = started;
            *g_trackWait = kTrackStartWait;
        }
    }

    void updateGameFrame()
    {
        ee::Local<Placed> placed;
        ee::Local<uint32_t> status;

        if (*g_trackWait)
            *g_trackWait = *g_trackWait - 1;
        const int32_t mode = *g_gameMode;
        if (mode == kNoSoundMode)
            return;
        if (mode == kPlayMode)
        {
            if (*g_playPhase == 1 || *g_playPhase == 3)
            {
                calls::streamStatus(*g_musicStream, status);
                const bool wanted = *status == kStreamIdle || *status == kStreamEnded || *g_nextTrack != kNoTrack;
                if (wanted && !*g_trackWait && !*g_noTracks)
                    playlistFrame();
                if (*g_speechHold)
                    *g_speechHold = *g_speechHold - 1;
                // A placed phrase is heard from where it was said as the
                // listener moves.
                if (calls::soundVoiceState(*g_speechChannel) == kChannelPlaying && *g_speechPlaced)
                {
                    placedVolume(0, ee::Ptr<const zge::Vec4>(sym::g_speechPosition),
                                 (static_cast<uint32_t>((*g_volume)[Speech]) * kSpeechChannelLevel) >> 7,
                                 &placed->volume, &placed->pan, &placed->third);
                    calls::setChannelVolume(*g_speechChannel, placed->volume, placed->pan);
                }
            }
            calls::updateEffects();
        }
        else if (mode == kPausedMode)
            calls::updateEffects();
        calls::soundFrame();
        calls::streamsFrame();
        calls::audioFrame();
    }
    READABLE_FUNCTION_WAITS(updateGameFrame, Verified);
}
