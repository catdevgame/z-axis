// The game's sound events (readable round gamehand2): soundEvent, the one
// function the game calls for every sound (65 callers): the event number
// picks what to play or set, from the sample banks the level loaded, at
// the volumes the options set (effects, music, speech: 0 .. 127), placed
// by position or not; the music's tracks (13: enabled, picked, named);
// the sound sources that follow objects (the traffic cars' engines). BMX
// XXX's version of it is kbmxsndEvent (its messages, "kbmxsndEventSetTrack
// - attempting to set track to a disabled track"); the Xbox and GameCube
// builds of AI print the same track errors, the PS2 build none.
//
// Names past the offsets are inferred from what the code does; the sound
// library (allocateVoice ..) and the sources and streams (playChannelStream ..) are the
// engine's.

#include "zge/sound/streams.h"
#include "aggressive/conversions.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/scene/locators.h"
#include "zge/assets/tag_table.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/assets/file.h"
#include "zge/sound/sound.h"
#include "zge/sound/soundtrack.h"
#include "zge/scene/animation.h"
#include "aggressive/sound/sound_mode.h"
#include "sce/libgcc.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/sound/events.cpp.h"

namespace aggressive
{
    namespace
    {
        // The voices and streams.
        constexpr uint32_t kStreamVolume = sym::setStreamVolume;  // (stream, volume, 0)
        constexpr uint32_t kStopGroup = sym::playGroupOnce;     // (group, 127, 0)
        // The sound sources (records of 0x30 at emitters: a sample at a position).
        constexpr uint32_t kSourcePosition = sym::setSourcePosition; // (source, position *)
        // The game's.
        constexpr uint32_t kPlayOneShot = sym::playOneShot;   // (sample, volume, flags) on one of the 16 one-shot voices
        constexpr uint32_t kPlacedVolume = sym::placedVolume;  // (sample, position *, volume, uint8 *volume, int8 *pan, *) by the listener

        constexpr uint32_t kNoVoice = 0xFF;
        constexpr uint32_t kNoSource = 0xFFFF;
        constexpr uint32_t kTracks = 13;

        // The options' volumes: effects, music, speech (0 .. 127).
        inline ee::Global<uint8_t[3], sym::g_masterVolume> g_volume;
        enum Volume : uint32_t
        {
            Effects = 0,
            Music = 1,
            Speech = 2,
        };
        // The tracks the player has on (a bit each).
        inline ee::Global<uint16_t, sym::g_enabledTracks> g_enabledTracks;
        // Read as words here (the level index's byte and the next three).
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelWord;
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameType;  // 4 split screen, 5 [inferred: no music]
        inline ee::Global<int32_t, sym::g_gameMode> g_gameMode;  // the sound mode (setGameMode): -1 before the game runs, no sound
        constexpr uint32_t kFrontEndMode = 1, kPlayMode = 2, kPausedMode = 3, kPlacedSoundsMode = 4;
        inline ee::Global<int32_t, sym::g_playPhase> g_playPhase;
        inline ee::Global<ee::Float, sym::g_levelTime> g_levelTime;
        // Event 44's warnings: at most every 0.3 s (their last times at g_lastWarning).
        inline ee::Global<uint64_t, sym::g_warningInterval> g_warningInterval; // the double 0.3
        inline ee::Global<uint8_t, sym::g_pausedSound> g_pausedSound;      // event 55 done
        // The banks.
        inline ee::Global<uint16_t, sym::g_menuBank> g_menuBank;
        inline ee::Global<uint16_t, sym::g_mainBank> g_mainBank;
        inline ee::Global<uint16_t, sym::g_placedBank> g_placedBank;
        inline ee::Global<uint16_t, sym::g_crowdBank> g_crowdBank;
        inline ee::Global<uint16_t, sym::g_sourceBank> g_sourceBank;
        inline ee::Global<uint16_t, sym::g_streamBank> g_streamBank;
        // The music: the soundtrack its tracks are played from.
        inline ee::Global<ee::Ptr<zge::Soundtrack>, sym::g_music> g_music;
        inline ee::Global<ee::Ptr<zge::SoundStream>, sym::g_musicStream> g_musicStream;
        inline ee::Global<ee::Ptr<zge::SoundStream>, sym::g_syncStream> g_syncStream;  // the audio sync's (below)
        inline ee::Global<ee::Float, sym::g_trackFade> g_trackFade;
        inline ee::Global<int8_t, sym::g_nextTrack> g_nextTrack;      // -1: none asked for
        inline ee::Global<uint8_t, sym::g_trackChange> g_trackChange;
        inline ee::Global<uint32_t, sym::g_noTracks> g_noTracks;     // set when the last track was turned off
        inline ee::Global<uint8_t, sym::g_speechChannel> g_speechChannel;
        inline ee::Global<uint8_t, sym::g_secondChannel> g_secondChannel;
        inline ee::Global<ee::Float, sym::g_musicFade> g_musicFade;
        inline ee::Global<uint32_t, sym::g_secondStream> g_secondStream;
        inline ee::Global<uint32_t, sym::g_musicFading> g_musicFading;
        inline ee::Global<uint8_t, sym::g_musicVoice> g_musicVoice;
        inline ee::Global<uint8_t, sym::g_effectsVoice> g_effectsVoice;
        inline ee::Global<uint8_t[32], sym::groupVoices> g_groupVoices; // a source per group of the level's bank
        // The one looping placed sound (event 33): its voice and sample.
        inline ee::Global<uint8_t, sym::g_loopVoice> g_loopVoice;
        inline ee::Global<uint16_t, sym::g_loopSample> g_loopSample;
        inline ee::Global<int8_t, sym::g_currentTrack> g_currentTrack;   // -1: not picked yet
        // The IOP command's record: {2, a, b}.
        inline ee::Global<uint32_t, sym::g_iopKind> g_iopKind;
        inline ee::Global<uint8_t, sym::g_iopA> g_iopA;
        inline ee::Global<uint8_t, sym::g_iopB> g_iopB;

        // A volume scaled by `level` (0 .. 128 is full): the options' effects
        // volume times it, over 128.
        uint32_t effectsTimes(uint32_t level)
        {
            return (static_cast<uint32_t>((*g_volume)[Effects]) * level) >> 7 & 0xFFu;
        }

        // How the listener hears a placed sample (placedVolume's outputs;
        // its third it sets to 0 for a sound with no position, and nothing
        // reads it).
        struct Placed
        {
            uint8_t volume;
            int8_t pan;
            uint8_t third;
        };
    }

    int32_t playOneShot(ee::Ptr<zge::BankSample> sample, uint32_t volumeArgument, uint32_t flagsArgument);

    uint32_t placedVolume(ee::Address sample, ee::Ptr<zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan, ee::Ptr<uint8_t> third);

    namespace
    {
        constexpr uint32_t kStartVoice = sym::startVoice;    // (voice)
        // Scaffolding (address-call): playGroupOnce is a Draft (zge/sound/sources.cpp), by its address until it is
        // proven.
        namespace calls
        {
            void playGroupOnce(ee::Ptr<zge::SoundGroup> group, uint32_t volume, uint32_t pan) { ee::game(kStopGroup, group, volume, pan); }
        }
        // startVoice, whose result register playOneShot hands on (the callee is void).
        int32_t startVoiceAnswer(uint32_t voice) { return ee::game<int32_t>(kStartVoice, voice); }
    }

    // soundEvent (event, a, b, c, d): the one function the game calls for a
    // sound (65 callers). The event picks what to play or set and what a .. d
    // are (each case says). Returns the event's answer (a volume, a track, a
    // name, a source) or 0; 1 for the events this build leaves alone, 2 for a
    // volume kind past speech. Nothing before the sounds start (sound mode
    // -1); in play's music phase 3 nothing for events 58 .. 71, in the pause
    // nothing for 25 .. 54.
    //
    // The original keeps its five arguments across its calls in registers
    // its callees save, d as its caller handed it, all 64 bits (some callers
    // leave a float's upper half in it). That shows only in the save slots
    // its callees write in their own frames, which nothing reads after they
    // return: here they are the parameters.
    uint32_t soundEvent(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d)
    {
        // The frame's two locals: a copy of the position a points to (made
        // for events 47, 52 and 55 .. 57; the sound sources use it), and how
        // the listener hears a placed sample.
        ee::Local<zge::Vec4> position;
        ee::Local<Placed> placed;
        const auto copyPosition = [&] { *position = zge::aligned(ee::Ptr<zge::Vec4>(a)); };
        const ee::Ptr<uint8_t> volumes(sym::g_masterVolume); // by a's kind (Volume)
        const auto effects = [] { return static_cast<uint32_t>((*g_volume)[Effects]); };

        // The shared tails, each answering 0: a sample on a voice of its own
        // (flags 33, or 0x20 with a pitch and pan), a one-shot.
        const auto playUnplaced = [&](ee::Ptr<zge::BankSample> sample, uint32_t volume) {
            zge::playSample(sample, 0, volume, 0, 33);
            return 0u;
        };
        const auto play = [&](ee::Ptr<zge::BankSample> sample, uint32_t pitch, uint32_t volume, uint32_t pan) {
            zge::playSample(sample, pitch, volume, pan, 0x20);
            return 0u;
        };
        const auto oneShot = [&](uint32_t bank, uint32_t index) {
            const ee::Ptr<zge::BankSample> sample = zge::sample(bank, index);
            playOneShot(sample, effects(), 33);
            return 0u;
        };
        const auto oneShotAt = [&](ee::Ptr<zge::BankSample> sample, uint32_t volumeTimesLevel) {
            playOneShot(sample, volumeTimesLevel >> 7 & 0xFFu, 0x20);
            return 0u;
        };
        // How the listener hears `sample` at the position a points to, at
        // `volume`.
        const auto hear = [&](ee::Ptr<zge::BankSample> sample, uint32_t volume) {
            placedVolume(sample, ee::Ptr<zge::Vec4>(a), volume, &placed->volume, &placed->pan, &placed->third);
        };
        // A sample placed at a, played as the listener hears it.
        const auto placedSample = [&](ee::Ptr<zge::BankSample> sample, uint32_t volume) {
            hear(sample, volume);
            return play(sample, 0, placed->volume, static_cast<uint32_t>(static_cast<int32_t>(placed->pan)));
        };

        if (*g_gameMode == -1)
            return 0;
        const uint32_t random = zge::randomNumber();
        const int32_t mode = *g_gameMode;
        if (mode == static_cast<int32_t>(kPlayMode))
        {
            if (*g_playPhase == 3 && event - 58u < 14u)
                return 0;
        }
        else if (mode == static_cast<int32_t>(kPausedMode))
        {
            if (event - 25u < 30u)
                return 0;
        }
        if (event > 80u)
            return 1;

        int32_t gameMode = mode;
        switch (event)
        {
        case 0: // the options' volume of kind a set to b (c: play a sample at it), and set on what plays
        {
            volumes[a] = static_cast<uint8_t>(b);
            if (a == Effects)
            {
                const uint32_t bank = *g_mainBank;
                if (c)
                {
                    zge::playSample(zge::sample(bank, 89), 0, effects(), 0, 33);
                    gameMode = *g_gameMode;
                }
                if (gameMode == static_cast<int32_t>(kFrontEndMode))
                    return 0;
                if (gameMode <= 0)
                    return 0;
                const uint32_t voice = *g_effectsVoice;
                if (gameMode >= static_cast<int32_t>(kPlacedSoundsMode))
                    return 0;
                // In play or the pause: the effects voice, each group's source and the audio sync.
                zge::setVoiceVolume(voice, effects());
                for (uint32_t index = 0; index < zge::groupCount(*g_sourceBank); ++index)
                    zge::setSourceMix((*g_groupVoices)[index], effects(), 0);
                zge::setStreamVolume(*g_syncStream, effects());
                return 0;
            }
            if (a == Music)
            {
                const uint32_t bank = *g_mainBank;
                if (c)
                {
                    zge::playSample(zge::sample(bank, 103), 0, (*g_volume)[Music], 0, 33);
                    gameMode = *g_gameMode;
                }
                if (gameMode == static_cast<int32_t>(kFrontEndMode))
                {
                    // The front end's music is a voice.
                    zge::setVoiceVolume(*g_musicVoice, (*g_volume)[Music]);
                    return 0;
                }
                if (gameMode <= 0 || gameMode >= static_cast<int32_t>(kPlacedSoundsMode) || *g_levelWord == 7 || *g_gameType == 5)
                    return 0;
                zge::setStreamVolume(*g_musicStream, (*g_volume)[Music]);
                return 0;
            }
            if (a != Speech)
                return 2;
            const uint32_t bank = *g_mainBank;
            if (c)
            {
                zge::playSample(zge::sample(bank, 104), 0, (*g_volume)[Speech], 0, 33);
                gameMode = *g_gameMode;
            }
            if (gameMode >= static_cast<int32_t>(kPlacedSoundsMode) || gameMode < static_cast<int32_t>(kPlayMode))
                return 0;
            zge::setChannelVolume(*g_speechChannel, (*g_volume)[Speech], 0);
            return 0;
        }
        case 1: // the options' volume of kind a
            return volumes[a];
        case 2: // the IOP's sound driver told {2, a, b}
            *g_iopKind = 2;
            *g_iopA = static_cast<uint8_t>(a);
            *g_iopB = static_cast<uint8_t>(b);
            zge::iopCommand();
            return 0;
        case 3: // the track asked for, else the current one (the level's first track at the start)
        {
            const int8_t next = *g_nextTrack;
            if (next != -1)
                return static_cast<uint32_t>(static_cast<int32_t>(next));
            int8_t track = *g_currentTrack;
            if (track == -1)
            {
                track = static_cast<int8_t>(ee::Ptr<uint8_t>(sym::levelTrack)[static_cast<uint32_t>(*g_levelWord)]);
                *g_currentTrack = track;
            }
            return static_cast<uint32_t>(static_cast<int32_t>(track));
        }
        case 4: // the next track: a (13 at most); the current one cancels the change
        {
            const uint32_t track = a >= 14u ? kTracks : static_cast<uint32_t>(a);
            const int32_t phase = *g_playPhase;
            if (static_cast<int32_t>(track) != *g_currentTrack)
            {
                if (phase >= 3 || phase < 0)
                    return 0;
                *g_nextTrack = static_cast<int8_t>(track);
                *g_trackChange = 0xFF;
                return 0;
            }
            *g_trackChange = 0xFF;
            *g_nextTrack = -1;
            return 0;
        }
        case 5: // how many tracks
            return kTracks;
        case 6: // track a on (the first after none plays it)
        {
            if (a >= kTracks)
                return 0;
            const uint32_t wasNone = *g_noTracks;
            *g_enabledTracks = static_cast<uint16_t>(*g_enabledTracks | 1u << (a & 0x1Fu));
            if (!wasNone)
                return 0;
            *g_nextTrack = static_cast<int8_t>(a);
            *g_trackFade = ee::Float(2.0f);
            *g_noTracks = 0;
            return 0;
        }
        case 7: // track a off (the music stops with the last one; the current one is changed)
        {
            if (a >= kTracks)
                return 0;
            const uint32_t enabled = (*g_enabledTracks & ~(1u << (a & 0x1Fu))) & 0xFFFFu;
            *g_enabledTracks = static_cast<uint16_t>(enabled);
            uint32_t on = 0;
            for (uint32_t track = 0; track < kTracks; ++track)
                on += enabled >> track & 1u;
            if (on == 0)
            {
                const int32_t level = *g_levelWord;
                *g_noTracks = 1;
                if (level == 7 || *g_gameType == 5)
                    return 0;
                zge::stopStream(*g_musicStream);
                return 0;
            }
            if (*g_currentTrack != static_cast<int8_t>(a))
                return 0;
            *g_trackChange = 1;
            return 0;
        }
        case 8: // track a on or not
            if (a >= kTracks)
                return 0;
            return *g_enabledTracks & 1u << (a & 0x1Fu);
        case 9: // track a's name
        case 10: // and its artist
        {
            const ee::Ptr<zge::Soundtrack> music = *g_music;
            if (a >= kTracks)
                return 0;
            return ee::Address(event == 9 ? zge::soundtrackTrackTitle(music, a) : zge::soundtrackTrackArtist(music, a));
        }
        case 11: // the soundtrack's index
            return zge::soundtrackIndex(*g_music);
        case 12: // the music loaded: soundtrack a
            *g_music = zge::soundtrackAt(a);
            return 0;
        case 13: case 14: case 15: case 16: case 17: case 18: // the menu's sounds
        {
            static constexpr uint32_t kMenuSample[6] = {1, 2, 3, 0, 6, 6};
            return playUnplaced(zge::sample(*g_menuBank, kMenuSample[event - 13]), effects());
        }
        case 23: // the menu bank's first group played once
        {
            const uint32_t groups = zge::groupCount(*g_menuBank);
            const uint32_t bank = *g_menuBank;
            if (!groups)
                return 0;
            calls::playGroupOnce(zge::group(bank, 0), 127, 0);
            return 0;
        }
        case 24: // the menu bank's sample b (a must be 0)
        {
            if (a != 0)
                return 0;
            const uint32_t count = zge::sampleCount(*g_menuBank);
            const uint32_t bank = *g_menuBank;
            if (b >= count)
                return 0;
            return play(zge::sample(bank, b), 0, effects(), 0);
        }
        case 25: // the main bank's one-shots: b from 32, 18, 70, at level c
        case 27:
        case 29:
        {
            const uint32_t index = (b & 0xFFu) + (event == 25 ? 32u : event == 27 ? 18u : 70u);
            if (event == 27)
                zge::sample(*g_mainBank, index); // asked twice
            const ee::Ptr<zge::BankSample> sample = zge::sample(*g_mainBank, index);
            return oneShotAt(sample, effects() * c);
        }
        case 30: // b from 84: the skater running into an NPC. This event places nothing, so the level c scales the
                 // placed volume's byte as the frame left it, as the original does (a bug kept). From npcHitBySkater
                 // that byte is playerUpdate's: the lowest byte of its turn target's third row x (skater/player.cpp,
                 // updateDrawnOrientation's `target`, the frame's first local, in the same place as the
                 // original's), which nothing deeper writes before the hit in the same frame. From an animation's
                 // event (handleAnimationEvent) it is 0 on every call measured.
        {
            const uint32_t bank = *g_mainBank;
            placed->volume = static_cast<uint8_t>((((c << 5) + 31) * static_cast<uint32_t>(placed->volume)) >> 7);
            const ee::Ptr<zge::BankSample> sample = zge::sample(bank, b + 84);
            return oneShotAt(sample, effects() * static_cast<uint32_t>(placed->volume));
        }
        case 31: // one of the crowd bank's four at random, placed at a, pitched by c
        {
            const uint32_t bank = *g_crowdBank;
            // (A level over the frame's byte again, but placedVolume then writes it.)
            placed->volume = static_cast<uint8_t>((((c << 4) + 79) * static_cast<uint32_t>(placed->volume)) >> 7);
            const ee::Ptr<zge::BankSample> sample = zge::sample(bank, random & 0x3u);
            hear(sample, effects());
            return play(sample, static_cast<uint32_t>(static_cast<int32_t>(c << 22) >> 16), placed->volume,
                        static_cast<uint32_t>(static_cast<int32_t>(placed->pan)));
        }
        case 33: // the looping placed sound: b from 4 (a new sample stops the old one)
        {
            const uint32_t index = b + 4;
            const ee::Ptr<zge::BankSample> sample = zge::sample(*g_placedBank, index);
            hear(sample, effects());
            uint32_t voice = *g_loopVoice;
            bool start = true;
            if (voice != kNoVoice)
            {
                if (*g_loopSample != index)
                {
                    zge::stopVoice(voice, 3);
                    *g_loopVoice = kNoVoice;
                    voice = kNoVoice;
                }
                start = voice == kNoVoice;
            }
            if (start)
            {
                const uint32_t allocated = zge::allocateVoice(sample, 33);
                *g_loopVoice = static_cast<uint8_t>(allocated);
                if ((allocated & 0xFFu) == kNoVoice)
                    return 0;
                *g_loopSample = static_cast<uint16_t>(index);
            }
            zge::setVoiceVolume(*g_loopVoice, placed->volume);
            zge::setVoicePan(*g_loopVoice, placed->pan);
            zge::startVoice(*g_loopVoice);
            return 0;
        }
        case 32: case 34: case 35: case 36: case 37: case 38: // the placed bank's sets of b, placed at a
        {
            static constexpr uint32_t kFirstSample[7] = {0, 0, 8, 12, 0x10, 20, 24};
            return placedSample(zge::sample(*g_placedBank, kFirstSample[event - 32] + b), effects());
        }
        case 39: // the placed bank's 28, placed at a (40: 29; 41: 30 or 31 at random)
            return placedSample(zge::sample(*g_placedBank, 28), effects());
        case 40:
            return placedSample(zge::sample(*g_placedBank, 29), effects());
        case 41:
            return placedSample(zge::sample(*g_placedBank, (random & 0x1u) | 0x1Eu), effects());
        case 42: // the main bank's a from 50
            return playUnplaced(zge::sample(*g_mainBank, a + 50), effects());
        case 43: // the source bank's a from 128, a one-shot
            return oneShot(*g_sourceBank, a + 128);
        case 44: // warning a (of two): at most once in 0.3 s of level time
        {
            if (a >= 2)
                return 0;
            const ee::Ptr<ee::Float> lastWarning = ee::Ptr<ee::Float>(sym::g_lastWarning) + a;
            const uint64_t since = sce::libgcc::fptodp(*g_levelTime - *lastWarning);
            const int32_t later = sce::libgcc::dpcmp(since, *g_warningInterval);
            const uint32_t bank = *g_mainBank;
            if (later <= 0)
                return 0;
            zge::playSample(zge::sample(bank, 46), 0, effects(), 0, 33);
            *lastWarning = *g_levelTime;
            return 0;
        }
        case 45: // the main bank's 89, not in split screen
        {
            const uint32_t bank = *g_mainBank;
            if (*g_gameType == 4)
                return 0;
            return playUnplaced(zge::sample(bank, 89), effects());
        }
        // The main bank's one-shots.
        case 46: return oneShot(*g_mainBank, 90);
        case 63: return oneShot(*g_mainBank, 94);
        case 64: return oneShot(*g_mainBank, 92);
        case 65: return oneShot(*g_mainBank, 93);
        case 66: return oneShot(*g_mainBank, 106);
        case 71: return oneShot(*g_mainBank, a + 92);
        case 73: return oneShot(*g_mainBank, a + 96);
        case 75: return oneShot(*g_mainBank, 105);
        case 76: return oneShot(*g_mainBank, 103);
        case 77: return oneShot(*g_mainBank, 104);
        case 78: return oneShot(*g_mainBank, 91);
        case 47: // the main bank's b placed at a, at level c
        {
            copyPosition();
            const uint32_t count = zge::sampleCount(*g_mainBank);
            const uint32_t bank = *g_mainBank;
            if (b >= count)
                return 0;
            return placedSample(zge::sample(bank, b), effectsTimes(c));
        }
        case 48: // the main bank's a
        {
            const uint32_t count = zge::sampleCount(*g_mainBank);
            const uint32_t bank = *g_mainBank;
            if (a >= count)
                return 0;
            return play(zge::sample(bank, a), 0, effects(), 0);
        }
        case 51: // the main bank's 107
            return play(zge::sample(*g_mainBank, 107), 0, effects(), 0);
        case 49: // the main bank's 110 placed at a, at its placed volume times 127/128
        {
            const ee::Ptr<zge::BankSample> sample = zge::sample(*g_mainBank, 110);
            hear(sample, effects());
            const uint32_t volume = placed->volume;
            zge::playSample(sample, 0, ((volume << 7) - volume) >> 7 & 0xFFu, placed->pan, 0x20);
            return 0;
        }
        case 50: // source a's volume: level b
        {
            const uint32_t source = a & 0xFFFFu;
            if (source == kNoSource)
                return 0;
            zge::sourceVolume(source, effectsTimes(b));
            return 0;
        }
        case 52: // the source bank's b placed at a, at level c
        {
            copyPosition();
            const uint32_t count = zge::sampleCount(*g_sourceBank);
            const uint32_t bank = *g_sourceBank;
            if (b >= count)
                return 0;
            return placedSample(zge::sample(bank, b), effectsTimes(c));
        }
        case 53: // the main bank's 111 (a 0) or 112 (a 1)
        {
            if (a > 1)
                return 0;
            return playUnplaced(zge::sample(*g_mainBank, a == 0 ? 111u : 112u), effects());
        }
        case 55: // into the pause: the driver told {2, 2, 1}, the source bank's groups down to 20/128 but the first
        {
            copyPosition();
            if (*g_pausedSound != 0)
                return 0;
            *g_iopKind = 2;
            *g_iopA = 2;
            *g_iopB = 1;
            zge::iopCommand();
            for (uint32_t index = 0; index < zge::groupCount(*g_sourceBank); ++index)
            {
                const uint32_t voice = (*g_groupVoices)[index];
                const uint32_t volume = index == 0 ? (effects() * 0x14) >> 7 : effects();
                zge::setSourceMix(voice, volume, 0);
            }
            *g_pausedSound = 1;
            return 0;
        }
        case 56: // the sound sources: b 0 start the source bank's c at a (level d; returns it),
        case 57: //   1 stop source c, 2 its level d and position a, 3 the bank's c placed at a once
        {
            copyPosition();
            const uint32_t source = c & 0xFFFFu;
            switch (b)
            {
            case 0:
            {
                const uint32_t started = zge::startSource(zge::sample(*g_sourceBank, c), position);
                if (started != kNoSource)
                    zge::sourceVolume(started, effectsTimes(d));
                return started;
            }
            case 1:
                if (source != kNoSource)
                    zge::stopSource(source);
                return 0;
            case 2:
                if (source == kNoSource)
                    return 0;
                zge::sourceVolume(source, effectsTimes(d));
                zge::setSourcePosition(source, position);
                return 0;
            case 3:
            {
                const uint32_t count = zge::sampleCount(*g_sourceBank);
                const uint32_t bank = *g_sourceBank;
                if (c >= count)
                    return 0;
                return placedSample(zge::sample(bank, c), effectsTimes(d));
            }
            default:
                return 0;
            }
        }
        case 58: // the main bank's 90 (a 0) or 89 (a 1 .. 10)
        {
            if (a == 0)
                return playUnplaced(zge::sample(*g_mainBank, 90), effects());
            const uint32_t bank = *g_mainBank;
            if (a > 10)
                return 0;
            return playUnplaced(zge::sample(bank, 89), effects());
        }
        case 62: // the main bank's first
            return playUnplaced(zge::sample(*g_mainBank, 0), effects());
        case 74:
            return playUnplaced(zge::sample(*g_mainBank, 95), effects());
        case 79: // the speech channel's stream a (the music silenced, to fade back)
        case 80: // the second channel's stream a, unless one plays (b: it)
        {
            uint32_t bank = 0;
            uint32_t channel = 0;
            if (event == 79)
            {
                bank = *g_streamBank;
                if (*g_levelWord != 7)
                {
                    const ee::Ptr<zge::SoundStream> stream = *g_musicStream;
                    if (*g_gameType != 5)
                    {
                        zge::setStreamVolume(stream, 0);
                        *g_musicFading = 1;
                        *g_musicFade = ee::Float(1.0f);
                    }
                    bank = *g_streamBank;
                }
                channel = *g_speechChannel;
            }
            else
            {
                if (*g_secondStream != 0)
                    return 0;
                *g_secondStream = b;
                bank = *g_streamBank;
                channel = *g_secondChannel;
            }
            zge::playChannelStream(channel, zge::bankPlaylist(bank, a));
            return 0;
        }
        case 19: case 20: case 54: case 61: // events this build leaves alone, answered 1
            return 1;
        default: // 21, 22, 26, 28, 59, 60, 67 .. 70, 72: nothing
            return 0;
        }
    }
    READABLE_FUNCTION_WAITS(soundEvent, Verified);

    namespace
    {
        constexpr uint32_t kGameModeOf = sym::gameModeOf;      // () -> the game mode
        constexpr uint32_t kSoundEvent = sym::soundEvent;      // soundEvent

        // A sound emitter the level places (a list, by +0x30):
        // a sample played every `interval` seconds where it stands, as loud as
        // the listener hears it there; or, with no repeat, a sound source that
        // plays all the time, kept at its position.
        struct SoundEmitter
        {
            uint32_t id;            // +0x00 its scene point's name hash
            uint32_t on;            // +0x04 0: silent
            uint32_t repeats;       // +0x08 0: the source plays all the time
            ee::Float interval;     // +0x0C seconds between two plays
            ee::Float countdown;    // +0x10 seconds to the next
            ee::Ptr<zge::BankSample> sample; // +0x14
            uint16_t source;        // +0x18 its sound source
            uint8_t _1a[0x06];
            zge::Vec4 position;     // +0x20
            ee::Ptr<SoundEmitter> next; // +0x30
        };
        static_assert(offsetof(SoundEmitter, sample) == 0x14 && offsetof(SoundEmitter, source) == 0x18);
        static_assert(offsetof(SoundEmitter, position) == 0x20 && offsetof(SoundEmitter, next) == 0x30);
        inline ee::Global<uint32_t, sym::g_soundEmitters> g_soundEmitters;
        constexpr int32_t kGameModePlaying = 2; // gameModeOf's: play (game_state.h PlayModeId)

        // How the listener hears a sound (sourceHeard's two outputs).
        struct Heard
        {
            uint8_t volume;
            int8_t pan;
        };
    }

    // updateSoundEmitters: the level's sound emitters, each frame of play (`seconds`
    // the frame's time): a repeating one counts down and, when it is due, is
    // played where it stands at the volume the listener hears it with (times
    // the options' effects volume) and starts its next wait (what it overran
    // taken off); a continuous one has its source moved to it and set to full
    // level (soundEvent 50).
    void updateSoundEmitters(ee::Float seconds)
    {
        ee::Local<Heard> heard;
        if (gameModeOf() != kGameModePlaying || *g_soundEmitters == 0)
            return;
        for (ee::Ptr<SoundEmitter> emitter(*g_soundEmitters); emitter; emitter = ee::Ptr<SoundEmitter>(emitter->next))
        {
            if (emitter->on == 0)
                continue;
            if (emitter->repeats == 0)
            {
                zge::setSourcePosition(static_cast<uint32_t>(emitter->source), &emitter->position);
                soundEvent(50, static_cast<uint32_t>(emitter->source), 127, 0, 0);
            }
            else
            {
                const ee::Float countdown = emitter->countdown;
                if (countdown <= seconds)
                {
                    emitter->countdown = countdown + (emitter->interval - seconds);
                    zge::sourceHeard(emitter->sample, &emitter->position, &heard->volume, &heard->pan);
                    heard->volume = static_cast<uint8_t>(static_cast<uint32_t>(heard->volume) * (*g_volume)[Effects] >> 7);
                    zge::playSample(emitter->sample, 0, heard->volume, heard->pan, 0x20);
                }
                else
                    emitter->countdown = countdown - seconds;
            }
        }
    }
    READABLE_FUNCTION_WAITS(updateSoundEmitters, Verified);

    uint32_t soundBankOfKind(uint32_t kindArgument); // below

    namespace
    {
        // The list as it is made: its first and its last emitter.
        inline ee::Global<ee::Ptr<SoundEmitter>, sym::g_soundEmitters> g_firstSoundEmitter;
        inline ee::Global<ee::Ptr<SoundEmitter>, sym::g_lastSoundEmitter> g_lastSoundEmitter;
        // A sound emitter's scene point: its kind, and its tags. Tag 2: the
        // sample (byte 0, counted from the bank's 0x60th), on (byte 8), repeats
        // (byte 1); tag 8: the seconds between two plays (the float at 4).
        constexpr uint32_t kSoundEmitterKind = 0x093CEC55u;
        constexpr uint32_t kEmitterTag = 2, kEmitterSampleByte = 0, kEmitterOnByte = 8, kEmitterRepeatsByte = 1;
        constexpr uint32_t kEmitterTimingTag = 8, kEmitterIntervalFloat = 4;
        constexpr uint32_t kFirstEmitterSample = 0x60;
        constexpr uint32_t kEmitterSampleKind = 5; // soundBankOfKind's

        ee::Ptr<SoundEmitter> allocateEmitter() { return ee::Ptr<SoundEmitter>(allocateLockedBlock(sizeof(SoundEmitter))); }
    }

    // A sound emitter made for a scene point of their kind, on the end of the
    // list: a repeating one keeps its sample; a continuous one starts its
    // source there now.
    void createSoundEmitter(ee::Ptr<zge::ScenePoint> point)
    {
        if (point->kindHash != kSoundEmitterKind)
            return;
        const ee::Ptr<SoundEmitter> emitter = allocateEmitter();
        if (!*g_lastSoundEmitter)
            *g_firstSoundEmitter = emitter;
        else
            (*g_lastSoundEmitter)->next = emitter;
        *g_lastSoundEmitter = emitter;
        emitter->next = ee::Ptr<SoundEmitter>();
        emitter->id = point->nameHash;
        const uint32_t sampleIndex = (zge::tagByte(point->tags, kEmitterTag, kEmitterSampleByte) + kFirstEmitterSample) & 0xFF;
        emitter->position = point->position;
        emitter->on = zge::tagByte(point->tags, kEmitterTag, kEmitterOnByte);
        emitter->repeats = zge::tagByte(point->tags, kEmitterTag, kEmitterRepeatsByte);
        emitter->interval = zge::tagFloat(point->tags, kEmitterTimingTag, kEmitterIntervalFloat);
        emitter->countdown = ee::Float(0.0f);
        if (emitter->repeats)
            emitter->sample = zge::sample(soundBankOfKind(kEmitterSampleKind), sampleIndex);
        else
            emitter->source = static_cast<uint16_t>(zge::startSource(zge::sample(soundBankOfKind(kEmitterSampleKind), sampleIndex), &emitter->position));
    }
    READABLE_FUNCTION(createSoundEmitter, Verified);

    // The level's sound emitters, made from its scene points (the level's
    // start).
    void setupSoundEmitters()
    {
        ee::Local<zge::LocatorList> locators;
        zge::initList(locators);
        zge::findLocators(locators, 0, 0);
        *g_lastSoundEmitter = ee::Ptr<SoundEmitter>();
        *g_firstSoundEmitter = ee::Ptr<SoundEmitter>();
        for (ee::Ptr<zge::Locator> locator = locators->head; locator; locator = locator->next)
            createSoundEmitter(ee::Ptr<zge::ScenePoint>(locator->point));
        zge::freeLocators(locators);
    }
    READABLE_FUNCTION_WAITS(setupSoundEmitters, Verified);

    // The emitters gone (the level's end): the continuous ones' sources
    // stopped, each record given back. The list's head is left as it was.
    void stopSoundEmitters()
    {
        ee::Ptr<SoundEmitter> emitter = *g_firstSoundEmitter;
        while (emitter)
        {
            if (!emitter->repeats)
                zge::stopSource(emitter->source);
            const ee::Ptr<SoundEmitter> next = emitter->next;
            releaseZoneBlock(emitter);
            emitter = next;
        }
    }
    READABLE_FUNCTION(stopSoundEmitters, Verified);

    // The script's sound source state (nativeSoundSourceSetState): in play,
    // the emitter of that id turned on or off.
    void setSoundEmitterState(uint32_t id, uint32_t on)
    {
        if (gameModeOf() != 2)
            return;
        for (ee::Ptr<SoundEmitter> emitter = *g_firstSoundEmitter; emitter; emitter = emitter->next)
        {
            if (emitter->id == id)
            {
                emitter->on = on;
                return;
            }
        }
    }
    READABLE_FUNCTION(setSoundEmitterState, Verified);

    namespace
    {
        // The rolling sounds' sources (16 slots; the player's rollSound picks one).
        constexpr uint32_t kRollSources = sym::rollSources;
        struct RollSlot
        {
            uint16_t source; // 0xFFFF: free
            uint16_t spare;  // cleared with the source when play ends (setGameMode); nothing reads it
        };
        static_assert(sizeof(RollSlot) == 4);
        inline ee::Global<ee::Float, sym::g_frameSeconds6> g_frameSeconds;
        // The speed (units a frame at 60 frames a second) from which the wheels sound at full volume.
        inline ee::Global<ee::Float, sym::g_fullRollSpeed> g_fullRollSpeed;

        // The surface kinds of the rolling sound (the sound's high byte;
        // its low byte the surface's own sample).
        enum RollKind : uint32_t
        {
            RollPlain = 0,
            RollKind1 = 1,   // the sample + 14, if that is below 62
            RollKind2 = 2,   // + 0x34
            RollKind4 = 4,   // + 0x38
            RollWallTap = 0x20, // sample 11
            RollKind40 = 0x40,  // sample 88
        };
        // A rolling sound's word: the surface's own sample in the low byte,
        // its RollKind in the next.
        constexpr uint32_t surfaceSampleOf(uint32_t sound) { return sound & 0xFFu; }
        constexpr uint32_t rollKindOf(uint32_t sound) { return sound >> 8 & 0xFFu; }
        // Where each kind's samples start in the main bank, after the plain ones.
        constexpr uint32_t kKind1Samples = 14, kKind1SamplesEnd = 62, kKind2Samples = 0x34, kKind4Samples = 0x38;
        constexpr uint32_t kWallTapRollSample = 11, kKind40RollSample = 88;
        constexpr int32_t kTopRollPitch = 1800;
    }

    namespace
    {
        namespace tail
        {
        void setSourcePosition(uint32_t source, ee::Address position) { ee::tailCall(kSourcePosition, source, position); }
        }
    }

    // updateRollSound: the rolling sound of one source (the wheels on the
    // ground): its sample by the surface, its volume by the speed (full from
    // the full roll speed on, by the frame's time), its pitch the speed (at
    // most 1800), at `position`.
    void updateRollSound(uint32_t source, uint32_t sound, ee::Address position, int32_t speed)
    {
        const uint32_t surfaceSample = surfaceSampleOf(sound);
        uint32_t volume = (*g_volume)[Effects];
        const RollSlot &slot = ee::Ptr<RollSlot>(kRollSources)[source];
        const uint32_t kind = rollKindOf(sound);
        const int32_t full = ee::toInt(*g_frameSeconds * ee::Float(60.0f) * *g_fullRollSpeed);
        if (speed < 0)
            speed = -speed;
        const int32_t fullSpeed = static_cast<int16_t>(full);
        if (speed < fullSpeed)
        {
            const int32_t scaled = static_cast<int32_t>(volume * static_cast<uint32_t>(speed));
            volume = static_cast<uint32_t>(scaled / fullSpeed) & 0xFFu;
        }
        const int32_t pitch = static_cast<int16_t>(speed <= kTopRollPitch ? speed : kTopRollPitch);

        // The sample by the kind (all in the main bank).
        uint32_t index = surfaceSample;
        switch (kind)
        {
        case RollKind1:
            if (surfaceSample + kKind1Samples < kKind1SamplesEnd)
                index = surfaceSample + kKind1Samples;
            break;
        case RollKind2: index = surfaceSample + kKind2Samples; break;
        case RollKind4: index = surfaceSample + kKind4Samples; break;
        case RollWallTap: index = kWallTapRollSample; break;
        case RollKind40: index = kKind40RollSample; break;
        default: break;
        }
        // (The source's id is read again for each call.)
        zge::sourceSample(slot.source, ee::Address(zge::sample(*g_mainBank, index)));
        zge::sourceVolume(slot.source, volume);
        zge::sourcePitch(slot.source, static_cast<uint32_t>(pitch));
        tail::setSourcePosition(slot.source, position);
    }
    READABLE_FUNCTION(updateRollSound, Verified);
}

// The sound system's rest (gamehand13; were the sweep's drafts of fitTrackText ..
// stopAudioSync and soundBankOfKind .. animationPartSound): its start and end, the one-shot
// voices, the menu's sounds, each game mode's banks, voices and streams,
// the play phase's music, the rolling sounds' sources, the audio sync,
// the banks by kind, the placed volume, an animation's part sound, and the
// pause menu's soundtrack page (its script functions).
namespace zge
{
}

namespace aggressive
{
    uint32_t soundEvent(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d);

    namespace
    {
        // The sound library's (ps2Chan, ps2Strm, zsnd: the engine's).
        constexpr uint32_t kRampStreamVolume = sym::rampStreamVolume;   // (stream, milliseconds, volume, how)
        constexpr uint32_t kStopChannelStream = sym::stopFinishedStream;  // (channel): its stream stopped unless done
        constexpr uint32_t kSoundtrackCount = sym::soundRecordsInUse;
        constexpr uint32_t kSoundtrackName = sym::soundtrackName;     // (soundtrack) -> its name (the record's start)
        constexpr uint32_t kSoundtrackTracks = sym::soundtrackTrackCount;   // (soundtrack) -> its tracks
        // The engine's and the game's.
        constexpr uint32_t kSendEngineMessage = sym::sendEngineMessage;  // (EngineMessage *), by address (hooked)
        constexpr uint32_t kSetVariable = sym::setVariable;        // zgui setVariable (widget, name, value *), by address (hooked)
        constexpr uint32_t kRunScriptAt = sym::runScript;        // zgui runScript, by address where its result is ours
        constexpr uint32_t kMemset = sym::memset;
        constexpr uint32_t kSetGameMode = sym::setGameMode;    // setGameMode, as its callers here call it
        constexpr uint32_t kPlayPhaseAt = sym::playPhase;
        constexpr uint32_t kFitTrackText = sym::fitTrackText;
        constexpr uint32_t kRegisterSoundMenuFunctions = sym::registerSoundMenuFunctions;

        constexpr uint32_t kAudioFolder = sym::audioFolder;        // "assets\\audio\\"
        inline ee::Global<uint16_t, sym::g_frontEndBank> g_frontEndBank; // SFXMEN%02d.ZSD: the front end's music and sounds
        inline ee::Global<uint16_t, sym::g_musicBank> g_musicBank;    // MUSIC.ZSD
        inline ee::Global<uint8_t, sym::g_frontEndBankNumber> g_frontEndBankNumber; // its %02d
        // The audio sync (the script's initializeAudioSync): a MUSIC.ZSD
        // sample streamed (g_syncStream) at the effects volume with the
        // music turned down: 0 none, 1 loading, 2 playing.
        inline ee::Global<uint32_t, sym::g_syncState> g_syncState;
        inline ee::Global<ee::Float, sym::g_syncDelay> g_syncDelay;   // 0.5 s before its status is asked for, 1.5 s after it starts
        inline ee::Global<uint32_t, sym::g_syncListener> g_syncListener; // the object told "syncStart" when it starts (0: none)
        inline ee::Global<ee::Ptr<zge::SoundStream>, sym::g_speechChannelStream> g_speechChannelStream;
        constexpr uint32_t kOneShots = 16;
        inline ee::Global<uint8_t[kOneShots], sym::g_oneShotVoices> g_oneShotVoices; // kNoVoice: free
        inline ee::Global<int32_t, sym::g_soundRate> g_soundRate;      // 60
        constexpr uint32_t kSoundEmitterRecords = 0x50;
        constexpr uint32_t kRollSourceCount = 16;
        inline ee::Global<ee::Float, sym::g_lastWarning> g_lastWarning;   // event 44's two warnings' last times
        inline ee::Global<ee::Float, sym::g_lastWarning2> g_lastWarning2;
        inline ee::Global<ee::Float, sym::rollStart> g_rollStart;     // 20000: the height a rolling sound's source starts at
        inline ee::Global<RollSlot[kRollSourceCount], sym::rollSources> g_rollSlots;

        constexpr uint32_t kNoBank = 0xFFFF;
        constexpr uint32_t kSpeechWait = 0x3F000000u;              // (0.5)
        // No music in the Tutorial (level 7, TUT) nor the Park Editor (game
        // type 5).
        constexpr int32_t kTutorialLevel = 7;
        constexpr uint8_t kParkEditorType = 5;
        bool levelHasMusic()
        {
            return *g_levelWord != kTutorialLevel && *g_gameType != kParkEditorType;
        }
    }

    namespace
    {
        inline ee::Global<int32_t, sym::g_streamChannels> g_streamChannels; // g_streamChannels; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_streamChannelPlaying> g_streamChannelPlaying; // g_streamChannelPlaying; its name is a low-confidence guess (readable_names.tsv)
        constexpr uint32_t kTopStateLevelOffset = 0x10u; // level::TopState::level +0x10 (aggressive/level/park_editor.cpp)
        inline ee::Global<int32_t, sym::g_topState> g_topStateWord; // g_topStateWord ((anonymous)::g_topStateWord, (anonymous)::g_topStateWord)
        inline ee::Global<int32_t, sym::g_animalCallTicks> g_animalCallTicks; // g_animalCallTicks; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_soundEmitters> g_soundEmittersWord; // g_soundEmittersWord ((anonymous)::g_soundEmittersWord)
        inline ee::Global<int32_t, sym::g_lastSoundEmitter> g_soundEmitterRecords;
    }

}

namespace aggressive
{
    namespace
    {
        // The two texts the soundtrack page shows cut short (22 characters
        // and "..."), in turn.
        constexpr uint32_t kFittedTexts = sym::fittedTexts; // char[2][32]
        inline ee::Global<int32_t, sym::g_fittedTextCount> g_fittedTextCount;
        constexpr uint32_t kFittedLength = 22;
        constexpr uint32_t kEllipsis = sym::ellipsis;    // "..."
        uint32_t fittedText(int32_t count)
        {
            return kFittedTexts + (static_cast<uint32_t>(count % 2) << 5);
        }
    }

    // fitTrackText (text): the text to show, cut to 22 characters and
    // "..." when longer (into one of two buffers in turn); a "$KEY" (the
    // string table's) as it is.
    uint32_t fitTrackText(uint32_t text)
    {
        if (text != 0 && *ee::Ptr<const char>(text) == '$')
            return text;
        const uint32_t length = zge::stringLength(text);
        uint32_t fitted = text;
        if (length >= kFittedLength)
        {
            const int32_t count = *g_fittedTextCount + 1;
            *g_fittedTextCount = count;
            zge::stringCopyN(fittedText(count), text, kFittedLength);
            zge::appendString(fittedText(*g_fittedTextCount), kEllipsis);
            fitted = fittedText(*g_fittedTextCount);
        }
        return fitted;
    }
    READABLE_FUNCTION(fitTrackText, Verified);

    namespace
    {
        // The pause menu's soundtrack page (InGameUI.FrameOptions.Menu):
        // the track and the soundtrack chosen, each a CurValue of MaxValue
        // and a caption.
        constexpr uint32_t kMusicTrackWidget = sym::musicTrackWidget;     // "InGameUI.FrameOptions.Menu.MusicTrack"
        constexpr uint32_t kSoundtrackWidget = sym::soundtrackWidget;     // "InGameUI.FrameOptions.Menu.MusicSoundTrack"
        constexpr ee::Ptr<const char> kCurValue{sym::curValue2};             // "CurValue"
        constexpr ee::Ptr<const char> kMaxValue{sym::maxValue};             // "MaxValue"
        constexpr uint32_t kTrackCaption = sym::trackCaption;         // "SET Caption.Text {"\"%s\"\n%s"}": the track's name and artist
        constexpr uint32_t kSoundtrackCaption = sym::soundtrackCaption;    // "SET Caption.Text {"%s"}"
        // soundEvent's: 3 the track (asked for, else the current), 4 the
        // next track a, 11 the music now, 12 the music loaded (soundtrack a).
        constexpr uint32_t kTrackEvent = 3, kNextTrackEvent = 4, kMusicNowEvent = 11, kLoadMusicEvent = 12;
        // (the high halves the original keeps of the texts' addresses)
        constexpr uint32_t kZguiHigh = ee::high(sym::g_zgui), kTextsHigh = ee::high(sym::curValue2);

    }
}

namespace aggressive
{
    namespace
    {
        // Scaffolding (address-call): soundtrackTrackCount and soundtrackName (zge/sound/music.cpp) and
        // soundRecordsInUse (zge/sound/effects.cpp) are Drafts, by their addresses until they are proven.
        namespace calls
        {
            uint32_t soundtrackTrackCount(ee::Ptr<zge::Soundtrack> soundtrack) { return ee::game<uint32_t>(kSoundtrackTracks, soundtrack); }
            uint32_t soundRecordsInUse() { return ee::game<uint32_t>(kSoundtrackCount); }
            ee::Ptr<char> soundtrackName(ee::Ptr<zge::Soundtrack> soundtrack) { return ee::game<ee::Ptr<char>>(kSoundtrackName, soundtrack); }
        }
        // runScript, whose answer the caller hands on.
        int32_t runScriptAnswer(ee::Address widget, ee::Address text, uint32_t argument) { return ee::game<int32_t>(kRunScriptAt, widget, text, argument); }
    }

    // inlinesoundUIUpdate, the soundtrack page's update (the script function
    // "inlinesoundUIUpdate"): the track and the soundtrack chosen shown,
    // each with its largest value, the track's name and artist and the
    // soundtrack's name in the captions. Returns the last script's.
    int32_t inlinesoundUIUpdate()
    {
        ee::Local<char[0x100]> caption;
        ee::Local<ee::Float> track;
        ee::Local<ee::Float> largest;
        ee::Local<ee::Float> soundtrackChosen;
        *track = unsignedToFloat(soundEvent(kTrackEvent, 0, 0, 0, 0));
        *soundtrackChosen = unsignedToFloat(soundEvent(kMusicNowEvent, 0, 0, 0, 0));
        const uint32_t chosen = sce::libgcc::fptoui(*soundtrackChosen);
        const ee::Ptr<zge::Soundtrack> soundtrack = zge::soundtrackAt(chosen);

        // The track: CurValue, MaxValue (its soundtrack's tracks less one),
        // the caption.
        const ee::Ptr<zgui::Node> trackWidget = zgui::findNode(*zgui::g_root, ee::Ptr<const char>(kMusicTrackWidget), uint64_t{1});
        zgui::setVariable(trackWidget, kCurValue, track);
        *largest = unsignedToFloat(calls::soundtrackTrackCount(soundtrack) - 1);
        zgui::setVariable(trackWidget, kMaxValue, largest);
        uint32_t trackNumber = sce::libgcc::fptoui(*track);
        const ee::Address trackName = zge::soundtrackTrackTitle(soundtrack, trackNumber);
        const uint32_t nameText = fitTrackText(trackName);
        trackNumber = sce::libgcc::fptoui(*track);
        const ee::Address artist = zge::soundtrackTrackArtist(soundtrack, trackNumber);
        const uint32_t artistText = fitTrackText(artist);
        zge::formatString(caption, kTrackCaption, nameText, artistText);
        zgui::runScript(trackWidget, caption, 0);

        // The soundtrack: CurValue, MaxValue (the soundtracks less one),
        // the caption.
        const ee::Ptr<zgui::Node> soundtrackWidget = zgui::findNode(*zgui::g_root, ee::Ptr<const char>(kSoundtrackWidget), uint64_t{1});
        zgui::setVariable(soundtrackWidget, kCurValue, soundtrackChosen);
        *largest = unsignedToFloat(calls::soundRecordsInUse() - 1);
        zgui::setVariable(soundtrackWidget, kMaxValue, largest);
        const ee::Address soundtrackName = calls::soundtrackName(soundtrack);
        const uint32_t soundtrackText = fitTrackText(soundtrackName);
        zge::formatString(caption, kSoundtrackCaption, soundtrackText);
        return runScriptAnswer(soundtrackWidget, caption, 0);
    }
    READABLE_FUNCTION_WAITS(inlinesoundUIUpdate, Verified);

    // inlinesoundChangedTrack ("inlinesoundChangedTrack"): the track page's value the
    // next track. Returns soundEvent's.
    int32_t inlinesoundChangedTrack()
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<zgui::Node> widget = zgui::findNode(*zgui::g_root, ee::Ptr<const char>(kMusicTrackWidget), uint64_t{1});
        zgui::copyVariable(widget, ee::Ptr<const char>(kCurValue), value);
        const uint32_t track = sce::libgcc::fptoui(*value);
        return static_cast<int32_t>(soundEvent(kNextTrackEvent, track, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(inlinesoundChangedTrack, Verified);

    // inlinesoundChangedSoundtrack ("inlinesoundChangedSoundtrack"): the soundtrack page's
    // value loaded, its first track next. Returns soundEvent's.
    int32_t inlinesoundChangedSoundtrack()
    {
        ee::Local<ee::Float> value;
        const ee::Ptr<zgui::Node> widget = zgui::findNode(*zgui::g_root, ee::Ptr<const char>(kSoundtrackWidget), uint64_t{1});
        zgui::copyVariable(widget, ee::Ptr<const char>(kCurValue), value);
        const uint32_t soundtrack = sce::libgcc::fptoui(*value);
        soundEvent(kLoadMusicEvent, soundtrack, 0, 0, 0);
        return static_cast<int32_t>(soundEvent(kNextTrackEvent, 0, 0, 0, 0));
    }
    READABLE_FUNCTION_WAITS(inlinesoundChangedSoundtrack, Verified);

    namespace
    {
        // The banks loaded at the start (assets\audio\): the menu's, the
        // main one, the music's, the speech's.
        constexpr uint32_t kCommonSounds = sym::commonSounds; // "SFXCOM.ZSD"
        constexpr uint32_t kGameSounds = sym::gameSounds;   // "SFXGAME.ZSD"
        constexpr uint32_t kMusicFile = sym::musicFile;    // "MUSIC.ZSD"
        constexpr uint32_t kSpeechFile = sym::speechFile;   // "SPEECH.ZSD"
        // The soundtrack: its name and its 13 tracks' names (no artists).
        constexpr uint32_t kSoundtrackTitle = sym::soundtrackTitle; // "Inline Soundtrack"
        constexpr uint32_t kNoArtist = sym::noArtist;        // ""
        constexpr uint32_t kTrackNames[kTracks] = {
            sym::g_textFallingForYou, // "falling for you"
            sym::g_textCrawlingInTheDark, // "Crawling in the Dark"
            sym::g_textTrack13, // "Track 13"
            sym::g_textDonTSweatTheTechnique, // "Don't Sweat the Technique"
            sym::g_textWrongWay, // "Wrong Way"
            sym::g_textAnIdeaForAMovie, // "An Idea for a Movie"
            sym::g_textYourDisease, // "Your Disease"
            sym::g_textPassingMeBy, // "Passing me by"
            sym::g_textSellout, // "Sellout"
            sym::g_textMondayMorning, // "Monday Morning"
            sym::g_textYouthOfTheNation, // "Youth of the Nation"
            sym::g_textIMACloud, // "I'm a Cloud"
            sym::g_textTheChoiceIsYours, // "The choice is yours"
        };
        constexpr uint8_t kFullVolume = 127;
        constexpr int32_t kSoundRate = 60;
    }

    void registerSoundMenuFunctions();

    // initSound: the sound system started (the game's start): the
    // channels at the effects' full volume, the sources and emitters, the
    // streams, the four banks, no game mode, the soundtrack and its tracks
    // (all on), the one-shot voices free, the menu's sound hooks.
    void initSound()
    {
        ee::Local<uint8_t> volumes;
        zge::memoryFill(volumes, 0, 3);
        (&*volumes)[Music] = 0;
        (&*volumes)[Effects] = kFullVolume;
        (&*volumes)[Speech] = 0;
        // (the original hands the volumes to initSoundChannels, which does not read them)
        zge::initSoundChannels();
        *g_soundRate = kSoundRate;
        zge::initSoundSources();
        zge::initSoundEmitters(kSoundEmitterRecords, kSoundRate);
        zge::initSoundStreams();
        const auto loadBank = [&](uint32_t file, uint32_t intoSampleMemory, ee::Ptr<uint16_t> bank) {
            const uint32_t path = zge::joinPath(kAudioFolder, file);
            zge::loadBank(path, intoSampleMemory, bank);
        };
        loadBank(kCommonSounds, 1, &*g_menuBank);
        loadBank(kGameSounds, 1, &*g_mainBank);
        loadBank(kMusicFile, 0, &*g_musicBank);
        loadBank(kSpeechFile, 0, &*g_streamBank);
        *g_gameMode = 0;
        zge::initSoundBankPool();
        *g_music = zge::newSoundtrack(*g_musicBank, 1);
        zge::setSoundtrackName(*g_music, kSoundtrackTitle);
        for (uint32_t trackIndex = 0; trackIndex < kTracks; ++trackIndex)
            zge::setPlaylistEntry(*g_music, trackIndex, kTrackNames[trackIndex], kNoArtist);
        for (uint32_t bit = 0;;)
        {
            const uint32_t on = 1u << (bit & 0x1Fu);
            ++bit;
            *g_enabledTracks = static_cast<uint16_t>(*g_enabledTracks | on);
            if (!(bit < kTracks))
                break;
        }
        for (uint8_t &voice : *g_oneShotVoices)
            voice = kNoVoice;
        registerSoundMenuFunctions();
    }
    READABLE_FUNCTION_WAITS(initSound, Verified);

    // playOneShot (sample, volume, flags): the sample on a free one-shot
    // voice (of 16) at the volume. Returns the voice's start, or 255 (0 with
    // none free).
    int32_t playOneShot(ee::Ptr<zge::BankSample> sample, uint32_t volumeArgument, uint32_t flagsArgument)
    {
        const uint32_t volume = volumeArgument & 0xFFu;
        const uint32_t flags = flagsArgument & 0xFFu;
        uint8_t *const voices = *g_oneShotVoices;
        uint32_t slot = 0;
        while (slot < kOneShots && voices[slot] != kNoVoice)
            ++slot;
        if (slot == kOneShots)
            return 0; // none free (the original's answer: its slot test, false)
        const uint32_t voice = zge::allocateVoice(sample, flags);
        voices[slot] = static_cast<uint8_t>(voice);
        if ((voice & 0xFFu) == kNoVoice)
            return static_cast<int32_t>(kNoVoice);
        zge::setVoiceVolume(voices[slot], volume);
        return startVoiceAnswer(voices[slot]);
    }
    READABLE_FUNCTION_WAITS(playOneShot, Verified);

    namespace
    {
        // zgui's sound hooks (its globals +0x270 ..: the widgets' input
        // handlers call them): select, cancel, move, change, a move that
        // cannot be made.
        inline ee::Global<uint32_t, sym::g_zguiSelectSound> g_zguiSelectSound;
        inline ee::Global<uint32_t, sym::g_zguiCancelSound> g_zguiCancelSound;
        inline ee::Global<uint32_t, sym::g_zguiMoveSound> g_zguiMoveSound;
        inline ee::Global<uint32_t, sym::g_changeSoundHook> g_zguiChangeSound;
        inline ee::Global<uint32_t, sym::g_zguiBlockedSound> g_zguiBlockedSound;
        constexpr ee::Ptr<const char> kUiUpdateName{sym::uiUpdateName};         // "inlinesoundUIUpdate"
        constexpr ee::Ptr<const char> kChangedTrackName{sym::changedTrackName};     // "inlinesoundChangedTrack"
        constexpr ee::Ptr<const char> kChangedSoundtrackName{sym::changedSoundtrackName}; // "inlinesoundChangedSoundtrack"
        // soundEvent's menu sounds (the menu bank's samples 1, 2, 3, 0, 6).
        constexpr uint32_t kMoveSoundEvent = 13, kChangeSoundEvent = 14, kSelectSoundEvent = 15, kCancelSoundEvent = 16,
                           kBlockedSoundEvent = 17;
    }

    // registerSoundMenuFunctions: the soundtrack page's script functions registered, and
    // zgui's sound hooks set.
    void registerSoundMenuFunctions()
    {
        zge::zgui::registerScriptFunction(kUiUpdateName, sym::inlinesoundUIUpdate);
        zge::zgui::registerScriptFunction(kChangedTrackName, sym::inlinesoundChangedTrack);
        zge::zgui::registerScriptFunction(kChangedSoundtrackName, sym::inlinesoundChangedSoundtrack);
        *g_zguiBlockedSound = sym::playBlockedSound;
        *g_zguiSelectSound = sym::playSelectSound;
        *g_zguiCancelSound = sym::playCancelSound;
        *g_zguiMoveSound = sym::playMoveSound;
        *g_zguiChangeSound = sym::playChangeSound;
    }
    READABLE_FUNCTION_WAITS(registerSoundMenuFunctions, Verified);

    // playSelectSound .. playBlockedSound: zgui's sound hooks.
    uint32_t playSelectSound()
    {
        return game::tail(aggressive::soundEvent, kSelectSoundEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(playSelectSound, Verified);

    uint32_t playCancelSound()
    {
        return game::tail(aggressive::soundEvent, kCancelSoundEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(playCancelSound, Verified);

    uint32_t playMoveSound()
    {
        return game::tail(aggressive::soundEvent, kMoveSoundEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(playMoveSound, Verified);

    uint32_t playChangeSound()
    {
        return game::tail(aggressive::soundEvent, kChangeSoundEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(playChangeSound, Verified);

    uint32_t playBlockedSound()
    {
        return game::tail(aggressive::soundEvent, kBlockedSoundEvent, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(playBlockedSound, Verified);
}

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kFrontEndSounds = sym::frontEndSounds; // "SFXMEN%02d.ZSD"
        constexpr uint32_t kPlacedSounds = sym::placedSounds;   // "SFXWEAP.ZSD"
        constexpr uint32_t kLevelSounds = sym::levelSounds;    // "SFXLEV%02d.ZSD"
        constexpr uint32_t kCrowdSounds = sym::crowdSounds;    // "SFXCHR%02d.ZSD"
        constexpr uint32_t kSourceAmbience = 0xA0;        // the level bank's sample the effects voice loops
        constexpr uint32_t kMusicVoiceFlags = 34, kEffectsVoiceFlags = 0x20;
        constexpr uint32_t kStreamFlags = 0x8000, kStreamKind = 35, kChannelKind = 9;
        constexpr uint32_t kLevelIopB = 17;
        // The play phases (g_playPhase; setPlayPhase): 0 .. 3 the music's
        // (0 the level's start), 4 the music stopped (loadLevel sets it after
        // play starts), 6 a fly-thru from the pause menu
        // (inlinePausePlayFlyThru): pausing and resuming leave the sounds
        // alone; 5 (no caller sets it in this build): a resume stops the
        // music and the audio sync.
        constexpr uint32_t kMusicStopPhase = 4, kStopOnResumePhase = 5, kFlyThruPhase = 6;
    }

    // setGameMode (mode): the game mode set (g_gameMode), its sounds
    // with it. Leaving the front end: its music voice and bank. Leaving play
    // or the pause for the front end or none: every source, one-shot,
    // channel, stream and level bank, and the IOP's driver told. Pausing:
    // the sources, one-shots and channels stopped, the music and the audio
    // sync paused. Leaving mode 4: its loop voice and bank. Entering the
    // front end: its bank (SFXMEN%02d) and music voice. Play from the front
    // end: the level's banks (SFXLEV%02d, SFXCHR00), the music stream (the
    // soundtrack's first track), the audio sync's stream, the speech channels, the
    // rolling sounds' sources freed, the effects voice and a source for each
    // of the level bank's groups. Play from the pause: the streams and
    // voices resumed. Mode 4: SFXWEAP.ZSD. Returns the mode.
    int32_t setGameMode(uint32_t mode)
    {
        ee::Local<uint8_t[0xB0]> out;
        const int32_t old = *g_gameMode;
        if (old == static_cast<int32_t>(mode))
            return old;

        const auto stopOneShots = [&] {
            for (uint8_t &voice : *g_oneShotVoices)
            {
                if (voice == kNoVoice)
                    continue;
                zge::stopVoice(voice, 1);
                zge::releaseVoice(voice);
                voice = kNoVoice;
            }
        };

        // Out of play: every source, voice, stream and bank of the level.
        const auto leavePlay = [&](uint32_t effectsVoice) {
            zge::releaseVoice(effectsVoice);
            for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
            {
                zge::releaseSourceVoices((*g_groupVoices)[group]);
                (*g_groupVoices)[group] = kNoVoice;
            }
            stopOneShots();
            if (levelHasMusic())
                zge::stopStream(*g_musicStream);
            zge::stopStreamChannel(*g_speechChannel);
            zge::stopStreamChannel(*g_secondChannel);
            const bool tutorial = *g_levelWord == kTutorialLevel;
            *g_secondChannel = kNoVoice;
            *g_speechChannel = kNoVoice;
            if (!tutorial && *g_gameType != kParkEditorType)
                zge::closeSoundStream(*g_musicStream);
            const ee::Ptr<zge::SoundStream> syncStream = *g_syncStream;
            *g_musicStream = ee::Ptr<zge::SoundStream>();
            zge::stopStream(syncStream);
            zge::closeSoundStream(*g_syncStream);
            const uint32_t crowd = *g_crowdBank;
            *g_syncStream = ee::Ptr<zge::SoundStream>();
            *g_syncState = 0;
            zge::unloadSoundBank(crowd);
            const uint32_t sources = *g_sourceBank;
            *g_crowdBank = kNoBank;
            zge::unloadSoundBank(sources);
            *g_sourceBank = kNoBank;
            zge::closeStreamChannels();
            *g_iopKind = 2;
            *g_iopA = 0;
            *g_iopB = 0;
            zge::iopCommand();
            zge::zsndSourceReset();
            for (uint32_t index = 0; index < kRollSourceCount; ++index)
            {
                RollSlot &slot = ee::Ptr<RollSlot>(kRollSources)[index];
                slot.source = kNoSource;
                slot.spare = kNoSource;
            }
        };

        // Paused: the sources, one-shots and channels stopped (unless the
        // play phase is a fly-thru's), the music and the audio sync paused.
        const auto pauseSounds = [&] {
            if (static_cast<uint32_t>(playPhase()) != kFlyThruPhase)
            {
                zge::stopVoice(*g_effectsVoice, 1);
                for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                    zge::stopSoundSource((*g_groupVoices)[group]);
                stopOneShots();
                zge::pauseChannelStream(*g_speechChannel);
                zge::pauseChannelStream(*g_secondChannel);
            }
            const int32_t phase = *g_playPhase;
            if (phase >= 4 || phase < 0)
                return;
            if (*g_currentTrack != -1 && levelHasMusic())
                zge::pauseSoundStream(*g_musicStream, 1);
            if (*g_syncState != 0)
                zge::pauseSoundStream(*g_syncStream, 1);
        };

        // Play from the front end: the level's banks, the music and audio
        // sync streams, the channels, the rolling sounds' sources, the effects
        // voice and the groups' sources.
        const auto startPlaySounds = [&] {
            *g_iopKind = 2;
            *g_lastWarning = ee::Float(0.0f);
            *g_iopB = kLevelIopB;
            *g_playPhase = 0;
            *g_levelTime = ee::Float(0.0f);
            *g_lastWarning2 = ee::Float(0.0f);
            *g_iopA = 1;
            zge::iopCommand();
            *g_pausedSound = 1;
            zge::releaseAllVoices();
            zge::resetStreamChannels();
            zge::formatString(out, kLevelSounds, *g_levelWord);
            zge::loadBank(zge::joinPath(kAudioFolder, out), 1, &*g_sourceBank);
            zge::formatString(out, kCrowdSounds, 0);
            zge::loadBank(zge::joinPath(kAudioFolder, out), 1, &*g_crowdBank);
            const bool tutorial = *g_levelWord == kTutorialLevel;
            if (!tutorial && *g_gameType != kParkEditorType)
            {
                const ee::Ptr<zge::WaveLocation> file = zge::soundtrackTrackLocation(*g_music, 0);
                zge::openSoundStream(&*g_musicStream, file, 0, kStreamFlags, kStreamKind);
                const uint32_t volume = (*g_volume)[Music];
                const ee::Ptr<zge::SoundStream> stream = *g_musicStream;
                *g_nextTrack = -1;
                *g_currentTrack = -1;
                zge::setStreamVolume(stream, volume);
            }
            const ee::Ptr<zge::WaveLocation> syncFile = zge::bankSampleInfo(*g_musicBank, 0);
            zge::openSoundStream(&*g_syncStream, syncFile, 0, kStreamFlags, kStreamKind);
            zge::setStreamVolume(*g_syncStream, (*g_volume)[Effects]);
            zge::stopStream(*g_syncStream);
            const uint32_t streamBank = *g_streamBank;
            *g_syncState = 0;
            const uint32_t channel = zge::playStreamSample(streamBank, kStreamFlags, kChannelKind);
            *g_speechChannel = static_cast<uint8_t>(channel);
            zge::setChannelVolume(channel & 0xFFu, (*g_volume)[Speech], 0);
            *g_speechChannelStream = zge::streamChannelStream(*g_speechChannel);
            zge::setMusicParameter(0, 0);
            const uint32_t second = zge::playStreamSample(*g_streamBank, kStreamFlags, kChannelKind);
            *g_secondChannel = static_cast<uint8_t>(second);
            zge::setChannelVolume(second & 0xFFu, (*g_volume)[Speech], 0);
            for (uint32_t index = 0; index < kRollSourceCount; ++index)
                ee::Ptr<RollSlot>(kRollSources)[index].source = kNoSource;
            const ee::Ptr<zge::BankSample> ambience = zge::sample(*g_sourceBank, kSourceAmbience);
            const uint32_t voice = zge::allocateVoice(ambience, kEffectsVoiceFlags);
            *g_effectsVoice = static_cast<uint8_t>(voice);
            zge::setVoiceVolume(voice & 0xFFu, (*g_volume)[Effects]);
            zge::startVoice(*g_effectsVoice);
            for (uint32_t index = 0; index < zge::groupCount(*g_sourceBank); ++index)
            {
                const uint32_t source = zge::newGroupSource(zge::group(*g_sourceBank, index), 0);
                (*g_groupVoices)[index] = static_cast<uint8_t>(source);
                zge::setSourceMix(source & 0xFFu, (*g_volume)[Effects], 0);
                zge::restartSourceVoices((*g_groupVoices)[index]);
            }
        };

        // Play from the pause: the music and the audio sync started again
        // (by the play phase), the effects voice, the groups' sources and
        // the channels. A fly-thru's phase leaves the rest alone.
        const auto resumePlaySounds = [&] {
            const int32_t phase = *g_playPhase;
            if (phase == static_cast<int32_t>(kStopOnResumePhase))
            {
                if (levelHasMusic())
                    zge::stopStream(*g_musicStream);
                if (*g_syncState != 0)
                {
                    zge::stopStream(*g_syncStream);
                    *g_syncState = 0;
                }
            }
            else if (phase >= 0 && phase < 4)
            {
                const bool musicPlays = *g_currentTrack != -1 && *g_noTracks == 0 && *g_levelWord != kTutorialLevel;
                if (musicPlays && *g_gameType != kParkEditorType && *g_nextTrack == -1)
                    zge::playStream(*g_musicStream);
                if (*g_syncState != 0)
                    zge::playStream(*g_syncStream);
            }
            if (*g_playPhase == static_cast<int32_t>(kFlyThruPhase))
                return;
            zge::startVoice(*g_effectsVoice);
            for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                zge::restartSourceVoices((*g_groupVoices)[group]);
            zge::resumeChannelStream(*g_speechChannel);
            zge::resumeChannelStream(*g_secondChannel);
        };

        // Leaving the old mode.
        if (old == static_cast<int32_t>(kFrontEndMode))
        {
            // Its music voice and bank.
            zge::releaseVoice(*g_musicVoice);
            zge::stopVoice(static_cast<uint8_t>(*g_frontEndBank), 1); // (the bank's handle's low byte: bug kept)
            zge::unloadSoundBank(*g_frontEndBank);
            *g_frontEndBank = kNoBank;
            zge::releaseAllVoices();
        }
        else if (old == static_cast<int32_t>(kPlacedSoundsMode))
        {
            // Its loop voice and bank.
            if (*g_loopVoice != kNoVoice)
            {
                zge::releaseVoice(*g_loopVoice);
                *g_loopVoice = kNoVoice;
            }
            zge::unloadSoundBank(*g_placedBank);
            *g_placedBank = kNoBank;
        }
        else if (old == static_cast<int32_t>(kPlayMode) || old == static_cast<int32_t>(kPausedMode))
        {
            const uint32_t effectsVoice = *g_effectsVoice;
            if (mode < kPlayMode)
                leavePlay(effectsVoice);
            else if (mode == kPausedMode)
                pauseSounds();
        }

        // Entering the new mode.
        if (mode == kFrontEndMode)
        {
            // Its bank and its music voice.
            zge::formatString(out, kFrontEndSounds, static_cast<uint32_t>(*g_frontEndBankNumber));
            zge::loadBank(zge::joinPath(kAudioFolder, out), 1, &*g_frontEndBank);
            zge::releaseAllVoices();
            const ee::Ptr<zge::BankSample> music = zge::sample(*g_frontEndBank, 0);
            *g_musicVoice = static_cast<uint8_t>(zge::allocateVoice(music, kMusicVoiceFlags));
            zge::setVoiceVolume(*g_musicVoice, (*g_volume)[Music]);
            zge::startVoice(*g_musicVoice);
            *g_frontEndBankNumber = 0;
        }
        else if (mode == kPlacedSoundsMode)
        {
            // SFXWEAP.ZSD into the placed bank.
            zge::loadBank(zge::joinPath(kAudioFolder, kPlacedSounds), 1, &*g_placedBank);
            *g_loopVoice = kNoVoice;
        }
        else if (mode == kPlayMode)
        {
            if (old >= 0 && old < static_cast<int32_t>(kPlayMode))
                startPlaySounds();
            else if (old == static_cast<int32_t>(kPausedMode))
                resumePlaySounds();
        }
        *g_gameMode = static_cast<int32_t>(mode);
        return static_cast<int32_t>(mode);
    }
    READABLE_FUNCTION_WAITS(setGameMode, Verified);
}

namespace aggressive
{

    namespace
    {
        constexpr uint32_t kMusicRampMilliseconds = 750; // the music turned down over it
        constexpr uint32_t kRampDown = 2;                 // kRampStreamVolume's how
    }

    namespace
    {
        // Scaffolding (address-call): rampStreamVolume and stopFinishedStream are Drafts (zge/sound/streams.cpp), by
        // their addresses until they are proven.
        namespace calls
        {
            void rampStreamVolume(ee::Ptr<zge::SoundStream> stream, uint32_t milliseconds, uint32_t volume, uint32_t how) { ee::game(kRampStreamVolume, stream, milliseconds, volume, how); }
        }
        void stopFinishedStream(uint32_t channel, uint32_t phase) { ee::game(kStopChannelStream, channel, phase); }
        void stopFinishedStream(uint32_t channel) { ee::game(kStopChannelStream, channel); }
    }

    // setPlayPhase (phase): the play phase set. In play, from a music
    // phase (0 .. 3): to phase 4 the music and the audio sync stopped, to
    // any other the music (and the sync) turned down over 750 ms (the track
    // fade 2 s); to phases 1 .. 3 the speech channels' streams stopped.
    // Returns the phase.
    int32_t setPlayPhase(uint32_t phase)
    {
        const int32_t old = *g_playPhase;
        if (old == static_cast<int32_t>(phase))
            return old;
        if (*g_gameMode == static_cast<int32_t>(kPlayMode))
        {
            if (old < 4 && old >= 0)
            {
                if (phase != kMusicStopPhase)
                {
                    if (levelHasMusic())
                        calls::rampStreamVolume(*g_musicStream, kMusicRampMilliseconds, 0, kRampDown);
                    const uint32_t syncing = *g_syncState;
                    *g_trackFade = ee::Float(2.0f);
                    if (syncing != 0)
                    {
                        calls::rampStreamVolume(*g_syncStream, kMusicRampMilliseconds, 0, kRampDown);
                        *g_syncState = 0;
                    }
                }
                else
                {
                    if (levelHasMusic())
                        zge::stopStream(*g_musicStream);
                    const uint32_t syncing = *g_syncState;
                    if (syncing != 0)
                    {
                        zge::stopStream(*g_syncStream);
                        *g_syncState = 0;
                    }
                }
            }
            if (phase != 0)
            {
                if (!(phase < 4))
                {
                    *g_playPhase = static_cast<int32_t>(phase);
                    return static_cast<int32_t>(phase);
                }
                stopFinishedStream(*g_speechChannel, phase);
                stopFinishedStream(*g_secondChannel);
            }
        }
        *g_playPhase = static_cast<int32_t>(phase);
        return static_cast<int32_t>(phase);
    }
    READABLE_FUNCTION_WAITS(setPlayPhase, Verified);

    // startRollSound: a skater's rolling sound (Player::rollSound; its
    // updates updateRollSound): the main bank's sample 0 as a source at
    // (0, 20000, 0) in the first free of the 16 slots. Returns the slot, or
    // -1 with none free or no source.
    int32_t startRollSound()
    {
        ee::Local<zge::Vec4> start; // where the source starts: high above the level
        RollSlot *const slots = &(*g_rollSlots)[0];
        start->y = *g_rollStart;
        start->x = 0.0f;
        start->z = 0.0f;
        uint32_t slot = 0;
        while (slot < kRollSourceCount && slots[slot].source != kNoSource)
            ++slot;
        if (slot == kRollSourceCount)
            return -1;
        const ee::Ptr<zge::BankSample> sample = zge::sample(*g_mainBank, 0);
        const uint32_t source = zge::startSource(sample, start);
        slots[slot].source = static_cast<uint16_t>(source);
        if ((source & 0xFFFFu) != kNoSource)
            return static_cast<int32_t>(slot);
        return -1;
    }
    READABLE_FUNCTION(startRollSound, Verified);

    // stopRollSound (slot): a rolling sound's source stopped, its slot
    // freed. Returns 0xFFFF.
    int32_t stopRollSound(uint32_t slot)
    {
        RollSlot &roll = ee::Ptr<RollSlot>(&(*g_rollSlots)[0])[slot];
        zge::stopSource(roll.source);
        roll.source = kNoSource;
        return static_cast<int32_t>(kNoSource);
    }
    READABLE_FUNCTION(stopRollSound, Verified);

    namespace
    {
        namespace tail
        {
        void setStreamVolume(ee::Ptr<zge::SoundStream> stream, uint32_t volume, uint32_t value) { ee::tailCall(kStreamVolume, stream, volume, value); }
        }
    }

    // loadAudioSync (sample, listener; the script's initializeAudioSync):
    // MUSIC.ZSD's sample opened as the audio sync's stream (loading, its
    // status asked for in 0.5 s), the music turned off; `listener` told
    // "syncStart" when it plays.
    void loadAudioSync(uint32_t sample, ee::Address listener)
    {
        *g_syncListener = listener;
        const ee::Ptr<zge::WaveLocation> file = zge::bankSampleInfo(*g_musicBank, sample & 0xFFu);
        zge::openStream(*g_syncStream, file, 34);
        const bool tutorial = *g_levelWord == kTutorialLevel;
        *g_syncState = 1;
        *g_syncDelay = ee::Float(0.5f);
        if (!tutorial)
        {
            if (*g_gameType == kParkEditorType)
                return;
            tail::setStreamVolume(*g_musicStream, 0, 0);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(loadAudioSync, Verified);

    namespace
    {
        constexpr uint32_t kStreamReady = 4;         // streamStatus's: loaded (1 and 4 done, 2 playing)
        inline ee::Global<char, sym::syncStart> g_syncStartName; // "syncStart"
    }

    // startAudioSync (each frame while the audio sync is loading): after its
    // delay, once its stream is loaded: played at the effects volume
    // (playing, 1.5 s before its end is looked for), the listener told
    // "syncStart". Returns 1 when it starts.
    int32_t startAudioSync()
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint32_t> status;
        const ee::Float delay = *g_syncDelay;
        if (!(delay == ee::Float(0.0f)))
        {
            const ee::Float seconds = *g_frameSeconds;
            if (!(delay <= seconds))
            {
                *g_syncDelay = delay - seconds;
                return 0;
            }
            *g_syncDelay = ee::Float(0.0f);
        }
        zge::streamStatus(*g_syncStream, status);
        if (*status != kStreamReady)
            return 0;
        zge::setStreamVolume(*g_syncStream, (*g_volume)[Effects]);
        zge::playStream(*g_syncStream);
        const uint32_t listener = *g_syncListener;
        *g_syncState = 2;
        *g_syncDelay = ee::Float(1.5f);
        if (listener != 0)
        {
            message->target = listener;
            message->flags = zge::kMessageNow;
            message->sender = 0;
            message->data = 0;
            message->type = zge::nameHash(&*g_syncStartName);
            zge::sendEngineMessage(message);
            *g_syncListener = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(startAudioSync, Verified);

    // stopAudioSync (checkFlyThruSkip's): the audio sync stopped, the music
    // back at its volume.
    void stopAudioSync()
    {
        zge::stopStream(*g_syncStream);
        *g_syncState = 0;
        if (*g_levelWord != kTutorialLevel)
        {
            if (*g_gameType == kParkEditorType)
                return;
            tail::setStreamVolume(*g_musicStream, (*g_volume)[Music], 0);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(stopAudioSync, Verified);

    // soundBankOfKind (kind): a sound bank's handle by its kind (the table at
    // soundBankKinds): 0 (and past 7) the menu's, 1 the front end's, 2 the main
    // one, 3 the placed sounds', 4 the crowd's, 5 the level's, 6 the
    // music's, 7 the speech's.
    uint32_t soundBankOfKind(uint32_t kindArgument)
    {
        switch (kindArgument & 0xFFu)
        {
        case 1: return *g_frontEndBank;
        case 2: return *g_mainBank;
        case 3: return *g_placedBank;
        case 4: return *g_crowdBank;
        case 5: return *g_sourceBank;
        case 6: return *g_musicBank;
        case 7: return *g_streamBank;
        default: return *g_menuBank;
        }
    }
    READABLE_FUNCTION(soundBankOfKind, Verified);

    // placedVolume (sample, position, level, uint8 *volume, int8 *pan, *):
    // how loud and where the listener hears a sample at `position`, the
    // volume times level / 128; with no position, the level itself,
    // centred. Returns sourceHeard's (1 with no position). With no listener
    // sourceHeard writes neither output and the volume scaled is what the
    // caller's frame held; a listener is there from the first camera update,
    // and one was on all 676 calls measured.
    uint32_t placedVolume(ee::Address sample, ee::Ptr<zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan, ee::Ptr<uint8_t> third)
    {
        const uint32_t level = levelArgument & 0xFFu;
        if (position)
        {
            const uint32_t heard = zge::sourceHeard(sample, position, volume, pan);
            *volume = static_cast<uint8_t>((static_cast<uint32_t>(*volume) * level) >> 7);
            return heard;
        }
        *volume = static_cast<uint8_t>(level);
        *pan = 0;
        *third = 0;
        return 1;
    }
    READABLE_FUNCTION(placedVolume, Verified);

    namespace
    {
        inline ee::Global<char, sym::partName> g_partName;       // "PART": the bone the sound is at
        inline ee::Global<char, sym::soundIdName> g_soundIdName; // "SOUND_ID": the level bank's sample less 0x80
        constexpr uint32_t kPlacedLevelSample = 52;    // soundEvent's: the level bank's b placed at a, level c
        constexpr uint32_t kFullLevel = 127;
        constexpr uint32_t kLevelSoundIdBase = 0x80; // SOUND_ID 0 is the level bank's sample 0x80
    }

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // animationPartSound (scene object, event; handleAnimationEvent's): an
    // animation event's sound: the level bank's sample SOUND_ID + 0x80
    // placed at the bone PART's point. Returns soundEvent's (0 with no scene
    // or bone).
    int32_t animationPartSound(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::AnimationEvent> event)
    {
        ee::Local<zge::Vec4> origin;
        ee::Local<zge::Vec4> point; // the bone's point in the world
        memset(origin, 0, sizeof(zge::Vec4));
        const uint32_t part = zge::eventInt(event, zge::nameHash(&*g_partName));
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(ee::Ptr<zge::SceneObject>(object));
        if (!scene)
            return 0;
        const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, part);
        if (!bone)
            return 0;
        zge::bonePoint(scene, bone, origin, point, zge::kBoneInWorld);
        const uint32_t soundId = zge::eventInt(event, zge::nameHash(&*g_soundIdName));
        return static_cast<int32_t>(soundEvent(kPlacedLevelSample, point, soundId + kLevelSoundIdBase, kFullLevel, 0));
    }
    READABLE_FUNCTION_WAITS(animationPartSound, Verified);
}
