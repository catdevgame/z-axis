// BMX XXX's sound modes (sound_mode.h): BMX XXX's own code (bmx.c).

#include "sound_mode.h"

#include "bmx/profile/players.h"
#include "bmx/sound/sound_state.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/sound/sound_mode.cpp.h"

namespace game::bmx
{
    using namespace sound;

    namespace
    {
        constexpr uint16_t kNoBank = 0xFFFF;
        // Every bank the engine holds, to unloadSoundBank.
        constexpr uint32_t kEveryBank = 0xFFFE;

        constexpr ee::Ptr<const char> kAudioFolder{sym::g_textAudioFolder};      // "assets\audio\"
        constexpr uint32_t kMenuBankFile = sym::bmx::g_textMenuBankFile;          // "SFXMEN%02d.ZSD"
        constexpr uint32_t kSpeechBankFile = sym::bmx::g_textSpeechBankFile;      // "SPEECH%2s.ZSD"
        constexpr uint32_t kCutsceneBankFile = sym::bmx::g_textCutsceneBankFile;  // "CUTSCN%2s.ZSD"
        constexpr uint32_t kLevelBankFile = sym::bmx::g_textLevelBankFile;        // "SFXLVL%2s.ZSD"
        constexpr uint32_t kParkBankFile = sym::bmx::g_textParkBankFile;          // "SFXPRK%2s.ZSD"
        constexpr uint32_t kCharacterBankFile = sym::bmx::g_textCharacterBankFile; // "SFXCHR%02d.ZSD"
        constexpr ee::Ptr<const char> kWeaponBankFile{sym::g_textWeaponBankFile}; // "SFXWEAP.ZSD"

        // Which of the front end's banks the next entry loads (0 again once
        // loaded).
        inline ee::Global<uint8_t, sym::g_menuBankNumber> g_menuBankNumber;
        // The levels' letters, and the park themes' for the park editor's
        // levels (the level numbers past the twelve).
        constexpr ee::Ptr<const LevelInfo> kLevelInfos{sym::g_levels};
        inline ee::Global<uint8_t, sym::g_parkTheme> g_parkTheme;
        struct ParkTheme
        {
            ee::Ptr<const char> code; // +0x00 two letters
            uint8_t _04[0x0C];        // not read here
        };
        static_assert(sizeof(ParkTheme) == 0x10);
        constexpr ee::Ptr<const ParkTheme> kParkThemes{sym::g_parkThemes};
        // Set when a level's sound starts; nothing in the game reads it.
        inline ee::Global<uint8_t, sym::g_levelSoundStarted> g_levelSoundStarted;
        // Cleared when a level's sound ends; nothing else in the game
        // touches it. AI keeps its audio sync's stream in the word after the
        // music's stream and clears it at this step.
        inline ee::Global<uint32_t, sym::g_syncStream> g_syncStream;

        // The IOP's sound driver told {2, a, b}.
        void tellSoundDriver(uint8_t a, uint8_t b)
        {
            *g_iopKind = 2;
            *g_iopA = a;
            *g_iopB = b;
            calls::iopCommand(sym::g_iopKind);
        }
        constexpr uint8_t kDriverLevelA = 1, kDriverLevelB = 0x11;

        // The sample the pause plays as it comes up, the music's first voice
        // flags, the music stream's and the speech channel's buffers.
        constexpr uint32_t kPauseSample = 116;
        constexpr uint32_t kMenuMusicFlags = 0x22;
        constexpr uint32_t kStreamBufferBytes = 0x8000, kMusicStreamOpenFlags = 0x23, kSpeechChannelFlags = 0x11;

        constexpr uint32_t kMusicRampMilliseconds = 250;
        constexpr uint32_t kRampDown = 4; // rampStreamVolume's how

        uint32_t musicStreamVolume() { return (static_cast<uint32_t>((*g_volume)[Music]) * kMusicStreamLevel) >> 7; }

        // The front end's sound ended: its music's voice and its bank.
        void leaveFrontEnd()
        {
            calls::releaseVoice(*g_musicVoice);
            calls::unloadSoundBank(*g_menuBank);
            *g_menuBank = kNoBank;
        }

        // A level's sound ended: the level's sources, the music's stream, the
        // speech channel, the looping sound, the level's six banks, the
        // rolling sounds' slots.
        void leaveLevel()
        {
            for (uint32_t group = 0; group < calls::groupCount(*g_levelBank); ++group)
            {
                calls::releaseSourceVoices(kGroupVoiceTable[group]);
                kGroupVoiceTable[group] = kNoVoice;
            }
            calls::stopStream(*g_musicStream);
            *g_syncStream = 0;
            calls::stopStreamChannel(*g_speechChannel);
            const uint32_t stream = *g_musicStream;
            *g_speechChannel = kNoVoice;
            calls::closeSoundStream(stream);
            const uint32_t loop = *g_loopVoice;
            *g_musicStream = 0;
            if (loop != kNoVoice)
            {
                calls::releaseVoice(loop);
                *g_loopVoice = kNoVoice;
            }
            calls::unloadSoundBank(*g_weaponBank);
            *g_weaponBank = kNoBank;
            for (uint32_t character = 0; character < 2; ++character)
            {
                calls::unloadSoundBank(kCharacterBankTable[character]);
                kCharacterBankTable[character] = kNoBank;
            }
            calls::unloadSoundBank(*g_levelBank);
            const uint32_t cutscenes = *g_cutsceneBank;
            *g_levelBank = kNoBank;
            calls::unloadSoundBank(cutscenes);
            const uint32_t speech = *g_speechBank;
            *g_cutsceneBank = kNoBank;
            calls::unloadSoundBank(speech);
            *g_speechBank = kNoBank;
            calls::closeStreamChannels();
            tellSoundDriver(0, 0);
            calls::zsndSourceReset();
            for (uint32_t slot = 0; slot < kRollSlots; ++slot)
            {
                kRollSlotTable[slot].tyres = kNoSource;
                kRollSlotTable[slot].chain = kNoSource;
            }
        }

        // Play paused: the pause's sample, the level's sources stopped, and
        // the music held if a track plays.
        void pausePlay()
        {
            calls::playSample(calls::sample(*g_gameBank, kPauseSample), 0, (*g_volume)[Effects], 0, kUnplaced);
            calls::pauseChannelStream(*g_speechChannel);
            for (uint32_t group = 0; group < calls::groupCount(*g_levelBank); ++group)
                calls::stopSoundSource(kGroupVoiceTable[group]);
            const int32_t phase = *g_playPhase;
            if ((phase == 1 || phase == 3) && *g_currentTrack != kNoTrack)
                calls::pauseSoundStream(*g_musicStream, 1);
        }

        // A level's sound: its banks loaded, the music's stream opened, the
        // speech channel made, a source for each voice group of the level's
        // bank.
        void enterLevel(ee::Local<char[0x20]> &file)
        {
            *g_playPhase = -1;
            tellSoundDriver(kDriverLevelA, kDriverLevelB);
            *g_levelSoundStarted = 1;
            calls::releaseAllVoices();
            calls::resetStreamChannels();

            zge::formatString(file, kSpeechBankFile, kLevelInfos[*g_currentLevel].code);
            calls::loadBank(calls::joinPath(kAudioFolder, file), 0, sym::g_speechBank);
            zge::formatString(file, kCutsceneBankFile, kLevelInfos[*g_currentLevel].code);
            calls::loadBank(calls::joinPath(kAudioFolder, file), 0, sym::g_cutsceneBank);
            if (*g_currentLevel < kLevels)
                zge::formatString(file, kLevelBankFile, kLevelInfos[*g_currentLevel].code);
            else
                zge::formatString(file, kParkBankFile, kParkThemes[*g_parkTheme].code);
            calls::loadBank(calls::joinPath(kAudioFolder, file), 1, sym::g_levelBank);
            for (uint32_t character = 0; character < 2; ++character)
            {
                zge::formatString(file, kCharacterBankFile, character);
                calls::loadBank(calls::joinPath(kAudioFolder, file), 1, kCharacterBankTable + static_cast<ptrdiff_t>(character));
            }
            calls::loadBank(calls::joinPath(kAudioFolder, kWeaponBankFile), 1, sym::g_weaponBank);

            *g_loopVoice = kNoVoice;
            calls::openSoundStream(sym::g_musicStream, zge::soundtrackTrackLocation(*g_gameSoundtrack, 0), 0, kStreamBufferBytes, kMusicStreamOpenFlags);
            *g_nextTrack = kNoTrack;
            *g_currentTrack = kNoTrack;
            calls::setStreamVolume(*g_musicStream, musicStreamVolume(), 0);
            const uint32_t channel = calls::playStreamSample(*g_speechBank, kStreamBufferBytes, kSpeechChannelFlags);
            *g_speechChannel = static_cast<uint8_t>(channel);
            calls::setChannelVolume(channel & 0xFFu, (static_cast<uint32_t>((*g_volume)[Speech]) * kSpeechChannelLevel) >> 7, 0);
            *g_speechHold = 0;
            *g_speechPlaced = 0;
            // (Only each slot's first source is marked free here.)
            for (uint32_t slot = 0; slot < kRollSlots; ++slot)
                kRollSlotTable[slot].tyres = kNoSource;
            for (uint32_t group = 0; group < calls::groupCount(*g_levelBank); ++group)
            {
                const uint32_t source = calls::newGroupSource(calls::group(*g_levelBank, group), 0);
                kGroupVoiceTable[group] = static_cast<uint8_t>(source);
                calls::setSourceMix(source & 0xFFu, (*g_volume)[Effects], 0);
            }
        }

        // Play back from the pause: the music as its phase has it, the
        // level's sources and the speech going again.
        void resumePlay()
        {
            const int32_t phase = *g_playPhase;
            if (phase == 0)
                calls::stopStream(*g_musicStream);
            else if ((phase == 1 || phase == 3) && *g_currentTrack != kNoTrack)
                calls::playStream(*g_musicStream);
            for (uint32_t group = 0; group < calls::groupCount(*g_levelBank); ++group)
                calls::restartSourceVoices(kGroupVoiceTable[group]);
            calls::resumeChannelStream(*g_speechChannel);
        }
    }

    int32_t setGameMode(uint32_t mode)
    {
        // The frame's one local: a bank's file name.
        ee::Local<char[0x20]> file;

        const int32_t old = *g_gameMode;
        if (old != static_cast<int32_t>(mode))
        {
            // What the mode left had.
            if (old == kFrontEndMode)
                leaveFrontEnd();
            else if (old == kPlayMode || old == kPausedMode)
            {
                if (mode < 2)
                    leaveLevel();
                else if (mode == static_cast<uint32_t>(kPausedMode))
                    pausePlay();
            }
            // What the mode entered needs. The front end: its bank loaded,
            // its first sample started as the music.
            if (mode == static_cast<uint32_t>(kFrontEndMode))
            {
                zge::formatString(file, kMenuBankFile, *g_menuBankNumber);
                calls::loadBank(calls::joinPath(kAudioFolder, file), 1, sym::g_menuBank);
                calls::releaseAllVoices();
                const uint32_t voice = calls::allocateVoice(calls::sample(*g_menuBank, 0), kMenuMusicFlags);
                *g_musicVoice = static_cast<uint8_t>(voice);
                calls::setVoiceVolume(voice & 0xFFu, (*g_volume)[Music]);
                calls::startVoice(*g_musicVoice);
                *g_menuBankNumber = 0;
            }
            else if (mode == static_cast<uint32_t>(kPlayMode))
            {
                const int32_t from = *g_gameMode;
                if (from >= 0)
                {
                    if (from < 2)
                        enterLevel(file);
                    else if (from == kPausedMode)
                        resumePlay();
                }
            }
            *g_gameMode = static_cast<int32_t>(mode);
        }
        calls::soundTask(0);
        return *g_gameMode;
    }
    READABLE_FUNCTION_WAITS(setGameMode, Verified);

    int32_t setPlayPhase(int32_t phase)
    {
        const int32_t old = *g_playPhase;
        if (old == phase)
            return old;
        if (*g_gameMode == kPlayMode)
        {
            if (old == 2)
            {
                // Back from the cutscene's sound: the music's stream cleared
                // for the playlist.
                if (phase == 0 || phase == 1 || phase == 3)
                {
                    calls::stopStream(*g_musicStream);
                    calls::setStreamVolume(*g_musicStream, musicStreamVolume(), 0);
                }
            }
            else if (old == 1 || old == 3)
            {
                // From the playlist: stopped, or faded out with the next
                // track held back.
                if (phase == 4)
                    calls::stopStream(*g_musicStream);
                else
                {
                    if (phase == 2)
                    {
                        calls::stopFinishedStream(*g_speechChannel);
                        calls::rampStreamVolume(*g_musicStream, kMusicRampMilliseconds, 0, kRampDown);
                    }
                    calls::rampStreamVolume(*g_musicStream, kMusicRampMilliseconds, 0, kRampDown);
                    *g_trackWait = kTrackStartWait;
                }
            }
            if (phase == 1 || phase == 3)
            {
                // To the playlist: the speech channel's stream stopped, and
                // on a level's first phase its sources set going.
                calls::stopFinishedStream(*g_speechChannel);
                calls::stopStream(calls::streamChannelStream(*g_speechChannel));
                if (*g_playPhase == -1)
                    for (uint32_t group = 0; group < calls::groupCount(*g_levelBank); ++group)
                        calls::restartSourceVoices(kGroupVoiceTable[group]);
            }
        }
        *g_playPhase = phase;
        return phase;
    }
    READABLE_FUNCTION_WAITS(setPlayPhase, Verified);

    void shutdownSound()
    {
        if (*g_gameMode)
            setGameMode(kSoundModeNone);
        calls::unloadSoundBank(kEveryBank);
        calls::closeAllSoundStreams();
        calls::resetSoundSources();
        calls::releaseAllSourceVoices();
        calls::shutdownSoundChannels();
        if (*g_soundtrackTracks)
        {
            calls::heapFree(*g_soundtrackTracks);
            *g_soundtrackTracks = ee::Ptr<SoundtrackTrack>();
        }
        *g_gameMode = kNoSoundMode;
    }
    READABLE_FUNCTION_WAITS(shutdownSound, Verified);
}
