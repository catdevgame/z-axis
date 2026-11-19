// The game's sound mode and play phase, and the sound system's end as DM2
// calls it (sound_mode.h): DM2's own code (its game block), word for word
// Aggressive Inline's (game/aggressive/sound/sound_mode.cpp); and the front
// end's two script functions that move the sound mode (AI's
// inlineMenuStopMusic and inlineMenuStartMusic, front_end.cpp).

#include "sound_mode.h"
#include "sound_events.h"
#include "sound_state.h"
#include "../action/action_state.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/sound/streams.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/sound/sound_mode.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint16_t kEveryBank = 0xFFFE;
    }

    // shutdownSound: the sound system's end, as the game calls it: its sound
    // mode left (0), every bank freed, the emitters, sources and channels
    // stopped; no sound mode (-1, which it returns).
    int32_t shutdownSound()
    {
        if (*g_gameMode != 0)
            setGameMode(kSoundModeOff);
        zge::unloadSoundBank(kEveryBank);
        zge::resetSoundSources();
        zge::releaseAllSourceVoices();
        zge::shutdownSoundChannels();
        *g_gameMode = -1;
        return -1;
    }
    READABLE_FUNCTION_WAITS(shutdownSound, Verified);

    // soundUnusedHook: an empty hook (no caller).
    void soundUnusedHook()
    {
    }
    READABLE_FUNCTION(soundUnusedHook, Draft);

    // gameModeOf: the game's sound mode (-1 before the game), on the word
    // shutdownSound reads.
    int32_t gameModeOf()
    {
        return *g_gameMode;
    }
    READABLE_FUNCTION(gameModeOf, Draft);

    // playPhase: the play phase, on the word after the sound mode.
    int32_t playPhase()
    {
        return *g_playPhase;
    }
    READABLE_FUNCTION(playPhase, Draft);

    // bmxMenuMusicOff: the script's "BmxMenuMusicOff": the sound mode left.
    uint32_t bmxMenuMusicOff()
    {
        return static_cast<uint32_t>(game::tail(setGameMode, kSoundModeOff));
    }
    READABLE_FUNCTION_WAITS(bmxMenuMusicOff, Verified);

    // bmxMenuMusicOn: the script's "BmxMenuMusicOn": the front end's sound
    // mode, with its music.
    uint32_t bmxMenuMusicOn()
    {
        return static_cast<uint32_t>(game::tail(setGameMode, kSoundModeFrontEnd));
    }
    READABLE_FUNCTION_WAITS(bmxMenuMusicOn, Verified);

    namespace
    {
        // The level's music (sound_state.h): the frames to wait before a
        // new track can start, whether the tracks follow one another (else
        // the track repeats), and each level's first track.
        inline ee::Global<int32_t, sym::g_musicTrackWait> g_trackWait;
        inline ee::Global<uint8_t, sym::g_musicTracksAdvance> g_tracksAdvance;
        inline ee::Global<uint8_t[9], sym::g_levelFirstTrack> g_levelFirstTrack;
        constexpr int32_t kTrackWaitFrames = 120;
        // The stream's status when its track has ended, and the open's flags
        // (stereo at 44100 Hz).
        constexpr uint32_t kStreamFinished = 1;
        constexpr uint32_t kOpenMusicStream = 0x140;

    }

    // musicFrame: the sound's frame (playFrame's): in a level, once the
    // track has ended (or one is asked for) and the wait is over, the next
    // track: the one asked for, else the level's first, else the one after
    // the last (round the soundtrack) or the same again; then the emitters
    // (paused, only them); and in any mode but none, the streams' and the audio's frames.
    void musicFrame()
    {
        ee::Local<uint32_t> status;
        if (*g_trackWait != 0)
            --*g_trackWait;
        if (*g_gameMode == -1)
            return;
        if (*g_gameMode == static_cast<int32_t>(kSoundModeLevel))
        {
            const int32_t phase = *g_playPhase;
            if (phase >= 0 && phase <= kLastMusicPhase)
            {
                zge::streamStatus(*g_musicStream, status);
                if ((*status == kStreamFinished || *g_nextTrack != kNoTrack) && *g_trackWait == 0)
                {
                    int8_t track = *g_nextTrack;
                    if (track == kNoTrack)
                    {
                        if (*g_currentTrack == kNoTrack)
                        {
                            *g_currentTrack = static_cast<int8_t>((*g_levelFirstTrack)[*g_sessionLevel]);
                        }
                        else if (*g_tracksAdvance != 0)
                        {
                            const int8_t next = static_cast<int8_t>(*g_currentTrack + 1);
                            *g_currentTrack = next;
                            if (!(next < static_cast<int32_t>(zge::soundtrackTrackCount(*g_music))))
                                *g_currentTrack = 0;
                        }
                        track = *g_currentTrack;
                    }
                    *g_nextTrack = track;
                    zge::openStream(*g_musicStream, zge::soundtrackTrackLocation(*g_music, track), kOpenMusicStream);
                    zge::setStreamVolume(*g_musicStream, (*g_musicVolume * kMusicVolumeScale) >> 7);
                    zge::playStream(*g_musicStream);
                    *g_currentTrack = *g_nextTrack;
                    *g_nextTrack = kNoTrack;
                    *g_trackWait = kTrackWaitFrames;
                }
            }
            zge::updateEffects(); // the level's sound emitters' frame
        }
        else if (*g_gameMode == static_cast<int32_t>(kSoundModePaused))
        {
            zge::updateEffects();
        }
        zge::streamsFrame();
        zge::audioFrame();
    }
    READABLE_FUNCTION(musicFrame, Verified);

    namespace
    {
        // The play phase that stops the music (setPlayPhase): from a music
        // phase to any other the music is turned down instead, over 750 ms.
        constexpr int32_t kMusicStopPhase = 3;
        constexpr uint32_t kMusicRampMilliseconds = 750;
        constexpr uint32_t kRampDown = 2; // rampStreamVolume's how
    }

    // setPlayPhase (phase): the play phase set. In a level, from a music
    // phase (0 .. 2): to phase 3 the music stopped, to any other turned down
    // over 750 ms, the next track waiting its 120 frames. Returns the phase.
    int32_t setPlayPhase(int32_t phase)
    {
        const int32_t old = *g_playPhase;
        if (old != phase && *g_gameMode == static_cast<int32_t>(kSoundModeLevel) && old <= kLastMusicPhase && old >= 0)
        {
            if (phase == kMusicStopPhase)
                zge::stopStream(*g_musicStream);
            else
            {
                zge::rampStreamVolume(*g_musicStream, kMusicRampMilliseconds, 0, kRampDown);
                *g_trackWait = kTrackWaitFrames;
            }
        }
        *g_playPhase = phase;
        return phase;
    }
    READABLE_FUNCTION_WAITS(setPlayPhase, Verified);

    namespace
    {
        // The sound's rate (its emitters' updates a second) and how many
        // emitters there are.
        inline ee::Global<int32_t, sym::g_soundRate> g_soundRate;
        constexpr int32_t kSoundRate = 60;
        constexpr uint32_t kSoundEmitters = 0x28;
        constexpr uint32_t kEmitterMilliseconds = 100;
        // The banks loaded at the start (assets\audio\): the menu's, the
        // main one (both into sound memory), the music's and the speech's.
        constexpr ee::Ptr<const char> kAudioFolder{sym::audioFolder};   // "assets\audio\"
        constexpr ee::Ptr<const char> kCommonSounds{sym::commonSounds}; // "SFXCOM.ZSD"
        constexpr ee::Ptr<const char> kGameSounds{sym::gameSounds};     // "SFXGAME.ZSD"
        constexpr ee::Ptr<const char> kMusicFile{sym::musicFile};       // "MUSIC.ZSD"
        constexpr ee::Ptr<const char> kSpeechFile{sym::speechFile};     // "SPEECH.ZSD"
        constexpr ee::Ptr<uint16_t> kMenuBank{sym::g_menuBank};
        constexpr ee::Ptr<uint16_t> kMainBank{sym::g_mainBank};
        constexpr ee::Ptr<uint16_t> kMusicBank{sym::g_musicBank};
        constexpr ee::Ptr<uint16_t> kStreamBank{sym::g_streamBank};
        inline ee::Global<uint16_t, sym::g_musicBank> g_musicBank;
        // The soundtrack: its name and its eleven tracks' texts (the string
        // table's keys "$KSTR_SONGINFO_<n>_SONGNAME" and "_ARTIST").
        constexpr uint32_t kSoundtrackTitle = sym::soundtrackTitle; // "Dave Mirra Freestyle BMX2"
        constexpr uint32_t kTracks = 11;
        struct TrackKeys
        {
            uint32_t name, artist;
        };
        constexpr TrackKeys kTrackKeys[kTracks] = {
            {sym::songInfoNameKey0, sym::songInfoArtistKey0},
            {sym::songInfoNameKey1, sym::songInfoArtistKey1},
            {sym::songInfoNameKey2, sym::songInfoArtistKey2},
            {sym::songInfoNameKey3, sym::songInfoArtistKey3},
            {sym::songInfoNameKey4, sym::songInfoArtistKey4},
            {sym::songInfoNameKey5, sym::songInfoArtistKey5},
            {sym::songInfoNameKey6, sym::songInfoArtistKey6},
            {sym::songInfoNameKey7, sym::songInfoArtistKey7},
            {sym::songInfoNameKey8, sym::songInfoArtistKey8},
            {sym::songInfoNameKey9, sym::songInfoArtistKey9},
            {sym::songInfoNameKey10, sym::songInfoArtistKey10},
        };
        constexpr uint8_t kFullVolume = 0x7F;

    }

    // initSound: the sound system started (the game's start): the channels
    // (handed the volumes effects 127, music and speech 0, which the
    // driver's start does not read), the sources and 40 emitters, the
    // streams, the four banks, no game mode, the soundtrack and its eleven
    // tracks' texts. Always 1.
    uint32_t initSound()
    {
        ee::Local<uint8_t[3]> volumes; // effects, music, speech
        zge::memoryFill(volumes, 0, 3);
        volumes[1] = 0;
        volumes[0] = kFullVolume;
        volumes[2] = 0;
        zge::initSoundChannels();
        *g_soundRate = kSoundRate;
        zge::initSoundSources();
        zge::initSoundEmitters(kSoundEmitters, kEmitterMilliseconds);
        zge::initSoundStreams();
        zge::loadBank(zge::joinPath(kAudioFolder, kCommonSounds), 1, kMenuBank);
        zge::loadBank(zge::joinPath(kAudioFolder, kGameSounds), 1, kMainBank);
        zge::loadBank(zge::joinPath(kAudioFolder, kMusicFile), 0, kMusicBank);
        zge::loadBank(zge::joinPath(kAudioFolder, kSpeechFile), 0, kStreamBank);
        *g_gameMode = 0;
        zge::initSoundBankPool();
        *g_music = zge::newSoundtrack(*g_musicBank, 1);
        zge::setSoundtrackName(*g_music, kSoundtrackTitle);
        for (uint32_t track = 0; track < kTracks; ++track)
            zge::setPlaylistEntry(*g_music, track, kTrackKeys[track].name, kTrackKeys[track].artist);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initSound, Verified);
}

namespace game::dm2
{
    namespace
    {
        // The front end's bank's number (SFXMEN<n>.ZSD; 0 again once it is
        // loaded). The main bank has the pause's sound (sample 119).
        inline ee::Global<uint8_t, sym::g_frontEndBankNumber> g_frontEndBankNumber;

        // The banks' files (in assets\audio\): the front end's by number,
        // the placed sounds', a level's by number (the park editor's park:
        // SFXLEV1<theme>.ZSD) and the crowd's (SFXCHR00).
        constexpr ee::Ptr<const char> kFrontEndSounds{sym::frontEndSounds};      // "SFXMEN%02d.ZSD"
        constexpr ee::Ptr<const char> kPlacedSounds{sym::placedSounds};          // "SFXWEAP.ZSD"
        constexpr ee::Ptr<const char> kParkLevelSounds{sym::parkLevelSounds};    // "SFXLEV1%1d.ZSD"
        constexpr ee::Ptr<const char> kLevelSounds{sym::levelSoundsByNumber};    // "SFXLEV%02d.ZSD"
        constexpr ee::Ptr<const char> kCrowdSounds{sym::crowdSounds};            // "SFXCHR%02d.ZSD"
        inline ee::Global<uint32_t, sym::g_parkThemePosition> g_parkThemePosition;
        constexpr uint32_t kFileNameBytes = 0x20;
        constexpr uint32_t kInSoundMemory = 1;
        // The voices' priorities: the front end's music, the pause's sound.
        constexpr uint8_t kMusicVoicePriority = 0x22, kPauseSoundPriority = 0x21;
        constexpr uint32_t kPauseSound = 119;
        // The level's music stream, opened on the soundtrack's first track
        // (stereo at 44100 Hz, and 0x04: no meaning found in ZGE2's stream
        // code); the open's fourth argument, which it does not read.
        constexpr uint32_t kMusicStreamFlags = 0x144;
        constexpr uint32_t kUnreadOpenArgument = 0x8000;
        // The music stream paused (pauseSoundStream's value 1); the play
        // phase whose resume stops it instead of playing it on (4, the one
        // endTurnEarly sets).
        constexpr uint32_t kStreamPaused = 1;
        constexpr int32_t kStopOnResumePhase = 4;
    }

    // setGameMode (mode): the sound mode set, its sounds with it; returns
    // the mode (the old one when it does not change).
    // Leaving the front end: its music voice and bank. Leaving the placed
    // sounds: their loop voice and bank. Leaving a level (playing or
    // paused) for none or the front end: the groups' sources, the music
    // stream, the crowd's and the level's banks, the driver told {0, 0}, the
    // emitters and the rolling sounds' sources freed. Pausing: the pause's
    // sound, the groups' sources stopped, the music (while one plays)
    // paused.
    // Entering the front end: its bank and music voice. The placed sounds:
    // SFXWEAP.ZSD. A level, from none or the front end: the driver told
    // {1, 17} ({2, 70} in Devil's Peak), the level's and the crowd's banks,
    // the music stream (no track picked yet), the rolling sounds' sources
    // free, a source for each of the level bank's groups. A level from its
    // pause: the music played again (or stopped, past an early turn end),
    // the groups' sources restarted.
    int32_t setGameMode(uint32_t mode)
    {
        ee::Local<char[kFileNameBytes]> file;
        const int32_t old = *g_gameMode;
        if (old == static_cast<int32_t>(mode))
            return old;

        const auto loadAudioBank = [](ee::Address name, ee::Ptr<uint16_t> bank) {
            zge::loadBank(zge::joinPath(kAudioFolder, name), kInSoundMemory, bank);
        };

        // Leaving the old mode.
        if (old == static_cast<int32_t>(kSoundModeFrontEnd))
        {
            zge::releaseVoice(*g_musicVoice);
            zge::unloadSoundBank(*g_frontEndBank);
            *g_frontEndBank = kNoBank;
        }
        else if (old == static_cast<int32_t>(kSoundModePlacedSounds))
        {
            if (*g_loopVoice != kNoVoice)
            {
                zge::releaseVoice(*g_loopVoice);
                *g_loopVoice = kNoVoice;
            }
            zge::unloadSoundBank(*g_placedBank);
            *g_placedBank = kNoBank;
        }
        else if (old == static_cast<int32_t>(kSoundModeLevel) || old == static_cast<int32_t>(kSoundModePaused))
        {
            if (mode < kSoundModeLevel)
            {
                for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                {
                    zge::releaseSourceVoices((*g_groupVoices)[group]);
                    (*g_groupVoices)[group] = kNoVoice;
                }
                zge::stopStream(*g_musicStream);
                zge::closeSoundStream(*g_musicStream);
                *g_musicStream = ee::Ptr<zge::SoundStream>();
                zge::unloadSoundBank(*g_crowdBank);
                *g_crowdBank = kNoBank;
                zge::unloadSoundBank(*g_sourceBank);
                *g_sourceBank = kNoBank;
                tellSoundDriver(0, 0);
                zge::zsndSourceReset();
                for (RollSlot &slot : *g_rollSlots)
                {
                    slot.source = kNoSource;
                    slot.secondSource = kNoSource;
                }
            }
            else if (mode == kSoundModePaused)
            {
                zge::playSample(zge::sample(*g_mainBank, kPauseSound), 0, *g_effectsVolume, 0, kPauseSoundPriority);
                for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                    zge::stopSoundSource((*g_groupVoices)[group]);
                const int32_t phase = *g_playPhase;
                if (phase >= 0 && phase <= kLastMusicPhase && *g_currentTrack != kNoTrack)
                    zge::pauseSoundStream(*g_musicStream, kStreamPaused);
            }
        }

        // Entering the new mode.
        if (mode == kSoundModeFrontEnd)
        {
            zge::formatString(file, kFrontEndSounds, static_cast<uint32_t>(*g_frontEndBankNumber));
            loadAudioBank(file, &*g_frontEndBank);
            zge::releaseAllVoices();
            *g_musicVoice = static_cast<uint8_t>(zge::allocateVoice(zge::sample(*g_frontEndBank, 0), kMusicVoicePriority));
            zge::setVoiceVolume(*g_musicVoice, *g_musicVolume);
            zge::startVoice(*g_musicVoice);
            *g_frontEndBankNumber = 0;
        }
        else if (mode == kSoundModePlacedSounds)
        {
            loadAudioBank(kPlacedSounds, &*g_placedBank);
            *g_loopVoice = kNoVoice;
        }
        else if (mode == kSoundModeLevel && old >= 0 && old < static_cast<int32_t>(kSoundModeLevel))
        {
            const bool devilsPeak = *g_sessionLevel == kDevilsPeakLevel;
            *g_playPhase = 0;
            if (devilsPeak)
                tellSoundDriver(2, 70);
            else
                tellSoundDriver(1, 17);
            *g_firstGroupQuiet = devilsPeak ? 0 : 1;
            zge::releaseAllVoices();
            if (*g_sessionLevel == kParkLevel)
                zge::formatString(file, kParkLevelSounds, *g_parkThemePosition);
            else
                zge::formatString(file, kLevelSounds, static_cast<uint32_t>(*g_sessionLevel));
            loadAudioBank(file, &*g_sourceBank);
            zge::formatString(file, kCrowdSounds, 0u);
            loadAudioBank(file, &*g_crowdBank);
            zge::openSoundStream(&*g_musicStream,
                                 zge::soundtrackTrackLocation(*g_music, 0), 0, kUnreadOpenArgument, kMusicStreamFlags);
            *g_nextTrack = kNoTrack;
            *g_currentTrack = kNoTrack;
            zge::setStreamVolume(*g_musicStream, (*g_musicVolume * kMusicVolumeScale) >> 7);
            for (RollSlot &slot : *g_rollSlots)
                slot.source = kNoSource;
            for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
            {
                (*g_groupVoices)[group] = static_cast<uint8_t>(zge::newGroupSource(zge::group(*g_sourceBank, group), 0));
                zge::setSourceMix((*g_groupVoices)[group], levelGroupVolume(group), 0);
                zge::restartSourceVoices((*g_groupVoices)[group]);
            }
        }
        else if (mode == kSoundModeLevel && old == static_cast<int32_t>(kSoundModePaused))
        {
            const int32_t phase = *g_playPhase;
            if (phase >= 0)
            {
                if (phase <= kLastMusicPhase)
                {
                    if (*g_currentTrack != kNoTrack)
                        zge::playStream(*g_musicStream);
                }
                else if (phase == kStopOnResumePhase)
                {
                    zge::stopStream(*g_musicStream);
                }
            }
            for (uint32_t group = 0; group < zge::groupCount(*g_sourceBank); ++group)
                zge::restartSourceVoices((*g_groupVoices)[group]);
        }
        *g_gameMode = static_cast<int32_t>(mode);
        return static_cast<int32_t>(mode);
    }
    READABLE_FUNCTION_WAITS(setGameMode, Verified);
}
