#pragma once
// DM2's sound state, shared by its sound mode (sound_mode.cpp) and its sound
// events (sound_events.cpp): the sound mode and play phase, the volumes, the
// banks, the voices and sources the modes make, the music.

#include "../action/action_state.h"

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/sound/soundtrack.h"
#include "zge/sound/streams.h"

#include <cstdint>
#include "sym/dm2/sound/sound_state.h.h"

namespace game::dm2
{
    // The game's sound mode (setGameMode; sound_mode.h's kSoundMode*): -1
    // before the game runs and after its end, no sound.
    inline ee::Global<int32_t, sym::g_gameMode> g_gameMode;
    // The play phase (setPlayPhase): 0 .. 2 the music's (the turn's and the
    // free ride's), 3 the music stopped, 4 the turn ended early.
    inline ee::Global<int32_t, sym::g_playPhase> g_playPhase;
    constexpr int32_t kLastMusicPhase = 2;

    // The volumes the sounds play at (0 .. 127): the effects', the music's
    // and the speech's, one byte each.
    inline ee::Global<uint8_t, sym::g_soundEffectsVolume> g_effectsVolume;
    inline ee::Global<uint8_t, sym::g_soundMusicVolume> g_musicVolume;
    constexpr uint32_t kVolumeKinds = 3;

    // The banks: the menu's and the main one (loaded at the start), the
    // front end's (its music is the bank's sample 0), the placed sounds',
    // the crowd's and the level's (a source for each of its groups), which
    // the sound modes load and free.
    inline ee::Global<uint16_t, sym::g_menuBank> g_menuBank;
    inline ee::Global<uint16_t, sym::g_mainBank> g_mainBank;
    inline ee::Global<uint16_t, sym::g_frontEndBank> g_frontEndBank;
    inline ee::Global<uint16_t, sym::g_placedBank> g_placedBank;
    inline ee::Global<uint16_t, sym::g_crowdBank> g_crowdBank;
    inline ee::Global<uint16_t, sym::g_sourceBank> g_sourceBank;
    constexpr uint16_t kNoBank = 0xFFFF;

    // The voices: the front end's music; the placed sounds' looping one and
    // the sample it plays; a source for each group of the level's bank.
    inline ee::Global<uint8_t, sym::g_musicVoice> g_musicVoice;
    inline ee::Global<uint8_t, sym::g_loopVoice> g_loopVoice;
    inline ee::Global<uint16_t, sym::g_loopSample> g_loopSample;
    inline ee::Global<uint8_t[16], sym::groupVoices> g_groupVoices;
    constexpr uint8_t kNoVoice = 0xFF;

    // The level's music: the soundtrack, its stream, the track playing (-1
    // none) and the one asked for next (-1 none). It plays at 75/128 of the
    // music volume.
    inline ee::Global<ee::Ptr<zge::Soundtrack>, sym::g_music> g_music;
    inline ee::Global<ee::Ptr<zge::SoundStream>, sym::g_musicStream> g_musicStream;
    inline ee::Global<int8_t, sym::g_currentTrack> g_currentTrack;
    inline ee::Global<int8_t, sym::g_musicNextTrack> g_nextTrack;
    constexpr int8_t kNoTrack = -1;
    constexpr uint32_t kMusicVolumeScale = 75;

    // Which way round the level bank's groups play in Devil's Peak (level
    // 4; soundEvent 36 changes it by where the camera is): 1 its first group
    // turned down (to 20/128 of the effects volume) and the others at the
    // effects volume, 0 the other way round. Another level sets it to 1 and
    // plays every group at the effects volume.
    inline ee::Global<uint8_t, sym::g_firstGroupQuiet> g_firstGroupQuiet;
    constexpr uint8_t kDevilsPeakLevel = 4;
    constexpr uint32_t kQuietGroupVolume = 20; // of 128

    // A level group's volume: the effects volume, in Devil's Peak one side
    // of the groups turned down.
    inline uint32_t levelGroupVolume(uint32_t group)
    {
        const uint32_t effects = *g_effectsVolume;
        if (*g_sessionLevel != kDevilsPeakLevel)
            return effects;
        const bool quiet = group == 0 ? *g_firstGroupQuiet == 1 : *g_firstGroupQuiet == 0;
        return quiet ? (effects * kQuietGroupVolume) >> 7 : effects;
    }

    // The request the sound modes and events hand the sound driver
    // (iopCommand, which this build's driver call ignores): command 2 with
    // two values: {1, 17} in a level, {2, 70} and {2, 1} in Devil's Peak's
    // two sides, {0, 0} after a level; soundEvent 1 sends its own.
    struct SoundDriverRequest
    {
        uint32_t command;
        uint8_t values[2];
    };
    inline ee::Global<SoundDriverRequest, sym::g_iopKind> g_soundDriverRequest;
    constexpr uint32_t kDriverCommand = 2;

    inline void tellSoundDriver(uint8_t first, uint8_t second)
    {
        g_soundDriverRequest->command = kDriverCommand;
        g_soundDriverRequest->values[0] = first;
        g_soundDriverRequest->values[1] = second;
        zge::iopCommand();
    }
}
