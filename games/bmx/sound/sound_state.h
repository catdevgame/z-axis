#pragma once
// BMX XXX's sound, shared by its files (events.cpp, sound_mode.cpp,
// roll_sound.cpp): the engine's sound calls and the game's sound state.
// The banks are named for their files (setGameMode loads them).

#include "bmx/sound/sound_mode.h"
#include "bmx/sound/soundtrack.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/sound/bank.h"
#include "zge/sound/sound.h"
#include "zge/sound/soundtrack.h"
#include "zge/sound/streams.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/sound/sound_state.h.h"

namespace game::bmx
{
// (In a namespace of its own: these names are the sound files' alone.)
namespace sound
{
    // The engine's sound library, sources and streams, each taking and
    // answering plain words as the original passes them: the engine's
    // functions by name with their own types (zge/sound).
    namespace calls
    {
        // The sample banks, by handle.
        inline uint32_t sampleCount(uint32_t bank) { return zge::sampleCount(static_cast<uint16_t>(bank)); }
        inline uint32_t groupCount(uint32_t bank) { return zge::groupCount(static_cast<uint16_t>(bank)); }
        inline uint32_t sample(uint32_t bank, uint32_t index) { return ee::Address(zge::sample(static_cast<uint16_t>(bank), index)); }
        inline uint32_t group(uint32_t bank, uint32_t index) { return ee::Address(zge::group(static_cast<uint16_t>(bank), index)); }
        // A bank's sample, playlist or wave by its name's hash (0: none).
        inline uint32_t sampleByHash(uint32_t bank, uint32_t nameHash) { return ee::Address(zge::sampleByHash(static_cast<uint16_t>(bank), nameHash)); }
        inline uint32_t playlistByHash(uint32_t bank, uint32_t nameHash) { return ee::Address(zge::playlistByHash(static_cast<uint16_t>(bank), nameHash)); }
        inline uint32_t waveLocationByHash(uint32_t bank, uint32_t nameHash) { return ee::Address(zge::waveLocationByHash(static_cast<uint16_t>(bank), nameHash)); }
        // Voices.
        inline uint32_t allocateVoice(uint32_t sample, uint32_t flags) { return zge::allocateVoice(ee::Ptr<zge::BankSample>(sample), static_cast<uint8_t>(flags)); }
        inline void playSample(uint32_t sample, int32_t pitch, uint32_t volume, int32_t pan, uint32_t flags) { zge::playSample(ee::Ptr<zge::BankSample>(sample), static_cast<int16_t>(pitch), static_cast<uint8_t>(volume), static_cast<int8_t>(pan), static_cast<uint8_t>(flags)); }
        inline void startVoice(uint32_t voice) { zge::startVoice(static_cast<uint8_t>(voice)); }
        inline void stopVoice(uint32_t voice, uint32_t how) { zge::stopVoice(static_cast<uint8_t>(voice), how); }
        inline void setVoiceVolume(uint32_t voice, uint32_t volume) { zge::setVoiceVolume(static_cast<uint8_t>(voice), static_cast<uint8_t>(volume)); }
        inline void setVoicePan(uint32_t voice, int32_t pan) { zge::setVoicePan(static_cast<uint8_t>(voice), static_cast<int8_t>(pan)); }
        inline void playGroupOnce(uint32_t group, uint32_t volume, uint32_t pan) { zge::playGroupOnce(ee::Ptr<zge::SoundGroup>(group), volume, pan); }
        inline void setSourceMix(uint32_t handle, uint32_t volume, uint32_t pan) { zge::setSourceMix(handle, volume, pan); }
        inline void iopCommand(uint32_t /*record*/) { zge::iopCommand(); } // (the original hands the record in a0; the IOP command reads its own)
        // Placed sources (effects.cpp).
        inline uint32_t startSource(uint32_t sample, ee::Address position, uint32_t /*zero*/) { return zge::startSource(ee::Address(sample), position); }
        inline void stopSource(uint32_t source) { zge::stopSource(static_cast<uint16_t>(source)); }
        inline void sourceVolume(uint32_t source, uint32_t volume) { zge::sourceVolume(source, volume); }
        inline void setSourcePosition(uint32_t source, ee::Address position) { zge::setSourcePosition(source, ee::Ptr<zge::Vec4>(position)); }
        inline uint32_t sourceHeard(uint32_t sample, ee::Address position, ee::Address volume, ee::Address pan, ee::Address ahead) { return zge::sourceHeard(ee::Address(sample), ee::Ptr<zge::Vec4>(position), ee::Ptr<uint8_t>(volume), ee::Ptr<int8_t>(pan) READABLE_PARAM_ONLY(bmx, ee::Ptr<uint8_t>(ahead))); }
        // Streams and their channels.
        inline void openStream(uint32_t stream, ee::Address wave, uint32_t flags) { zge::openStream(ee::Ptr<zge::SoundStream>(stream), ee::Ptr<zge::WaveLocation>(wave), flags); }
        inline void playStream(uint32_t stream) { zge::playStream(ee::Ptr<zge::SoundStream>(stream)); }
        inline void stopStream(uint32_t stream) { zge::stopStream(ee::Ptr<zge::SoundStream>(stream)); }
        inline void setStreamVolume(uint32_t stream, uint32_t volume, uint32_t /*zero*/) { zge::setStreamVolume(ee::Ptr<zge::SoundStream>(stream), volume); }
        inline void streamStatus(uint32_t stream, ee::Address status) { zge::streamStatus(ee::Ptr<zge::SoundStream>(stream), ee::Ptr<uint32_t>(status)); }
        inline int32_t playChannelStream(uint32_t channel, uint32_t playlist) { return zge::playChannelStream(static_cast<uint8_t>(channel), ee::Ptr<zge::Playlist>(playlist)); }
        inline void setChannelVolume(uint32_t channel, uint32_t volume, int32_t pan) { zge::setChannelVolume(static_cast<uint8_t>(channel), static_cast<uint8_t>(volume), static_cast<uint32_t>(pan)); }
        inline int32_t soundVoiceState(uint32_t channel) { return zge::soundVoiceState(static_cast<uint8_t>(channel)); }
        // ZGE4's: a channel's playlists made new.
        inline void resetStreamChannelPlaylists(uint32_t channel) { zge::resetStreamChannelPlaylists(channel); }
        inline void sourceSample(uint32_t source, uint32_t sample) { zge::sourceSample(source, sample); }
        inline void sourcePitch(uint32_t source, int32_t pitch) { zge::sourcePitch(source, static_cast<uint32_t>(pitch)); }
        inline uint32_t newGroupSource(uint32_t group, uint32_t zero) { return zge::newGroupSource(ee::Ptr<zge::SoundGroup>(group), zero); }
        inline void releaseSourceVoices(uint32_t groupSource) { zge::releaseSourceVoices(groupSource); }
        inline void restartSourceVoices(uint32_t groupSource) { zge::restartSourceVoices(groupSource); }
        inline void stopSoundSource(uint32_t groupSource) { zge::stopSoundSource(groupSource); }
        inline void resetSoundSources() { zge::resetSoundSources(); }
        inline void releaseAllSourceVoices() { zge::releaseAllSourceVoices(); }
        inline void zsndSourceReset() { zge::zsndSourceReset(); }
        inline void releaseVoice(uint32_t voice) { zge::releaseVoice(voice); }
        inline void releaseAllVoices() { zge::releaseAllVoices(); }
        inline void openSoundStream(ee::Address stream, ee::Address wave, uint32_t zero, uint32_t bufferBytes, uint32_t flags) { zge::openSoundStream(ee::Ptr<ee::Ptr<zge::SoundStream>>(stream), ee::Ptr<zge::WaveLocation>(wave), zero, bufferBytes, flags); }
        inline void closeSoundStream(uint32_t stream) { zge::closeSoundStream(ee::Ptr<zge::SoundStream>(stream)); }
        inline void closeAllSoundStreams() { zge::closeAllSoundStreams(); }
        inline void pauseSoundStream(uint32_t stream, uint32_t paused) { zge::pauseSoundStream(ee::Ptr<zge::SoundStream>(stream), paused); }
        inline void rampStreamVolume(uint32_t stream, uint32_t milliseconds, uint32_t volume, uint32_t fadeFlags) { zge::rampStreamVolume(ee::Ptr<zge::SoundStream>(stream), milliseconds, volume, fadeFlags); }
        inline uint32_t playStreamSample(uint32_t bank, uint32_t bufferBytes, uint32_t flags) { return zge::playStreamSample(static_cast<uint16_t>(bank), bufferBytes, flags); }
        inline uint32_t streamChannelStream(uint32_t channel) { return ee::Address(zge::streamChannelStream(static_cast<uint8_t>(channel))); }
        inline void stopStreamChannel(uint32_t channel) { zge::stopStreamChannel(static_cast<uint8_t>(channel)); }
        inline void stopFinishedStream(uint32_t channel) { zge::stopFinishedStream(static_cast<uint8_t>(channel)); }
        inline void pauseChannelStream(uint32_t channel) { zge::pauseChannelStream(static_cast<uint8_t>(channel)); }
        inline void resumeChannelStream(uint32_t channel) { zge::resumeChannelStream(static_cast<uint8_t>(channel)); }
        inline void resetStreamChannels() { zge::resetStreamChannels(); }
        inline void closeStreamChannels() { zge::closeStreamChannels(); }
        inline void shutdownSoundChannels() { zge::shutdownSoundChannels(); }
        inline void unloadSoundBank(uint32_t bank) { zge::unloadSoundBank(static_cast<uint16_t>(bank)); }
        inline void loadBank(uint32_t path, uint32_t inSoundMemory, ee::Address bankId) { zge::loadBank(path, inSoundMemory, ee::Ptr<uint16_t>(bankId)); }
        inline uint32_t joinPath(ee::Address folder, ee::Address name) { return zge::joinPath(folder, name); }
        inline void soundTask(uint32_t /*zero*/) { zge::soundTask(); }
        inline void heapFree(ee::Address block) { zge::heapFree(block); }
        // The frame's steps.
        inline void updateEffects() { zge::updateEffects(); }
        inline void soundFrame() { zge::soundFrame(); }
        inline void streamsFrame() { zge::streamsFrame(); }
        inline void audioFrame() { zge::audioFrame(); }
        // (zge/math, zge/text)
        inline uint32_t randomNumber() { return zge::randomNumber(); }
        inline uint32_t nameHash(ee::Address name) { return zge::nameHash(ee::Ptr<const char>(name).get()); }
    }

    constexpr uint32_t kNoVoice = 0xFF;
    constexpr uint32_t kNoSource = 0xFFFF;
    // playSample's flags: a voice of its own, and the same placed (with
    // a pitch and a pan).
    constexpr uint32_t kUnplaced = 0x21, kPlaced = 0x20;

    // The options' volumes: effects, music, speech (0 .. 127).
    inline ee::Global<uint8_t[3], sym::g_masterVolume> g_volume;
    enum Volume : uint32_t
    {
        Effects = 0,
        Music = 1,
        Speech = 2,
    };
    // The music's stream plays at 75/128 of the music volume, the speech
    // channel at 67/128 of the speech volume.
    constexpr uint32_t kMusicStreamLevel = 75, kSpeechChannelLevel = 67;

    // The sound mode (setGameMode): -1 before the game runs, no sound.
    inline ee::Global<int32_t, sym::g_gameMode> g_gameMode;
    // Play's phase (setPlayPhase): the music plays in 1 and 3.
    inline ee::Global<int32_t, sym::g_playPhase> g_playPhase;

    // The banks, each with its file: the common effects (SFXCOM) and the
    // game's (SFXGAME), loaded once; the front end's (SFXMEN and a number);
    // and a level's: the weapons' (SFXWEAP), the two characters' (SFXCHR00,
    // SFXCHR01), the level's own (SFXLVL and the level's letters, or SFXPRK
    // and the park theme's: its voice groups and its named voices), the
    // speech's phrases (SPEECH) and the cutscenes' waves (CUTSCN).
    inline ee::Global<uint16_t, sym::g_menuBank> g_menuBank;
    inline ee::Global<uint16_t, sym::g_commonBank> g_commonBank;
    inline ee::Global<uint16_t, sym::g_gameBank> g_gameBank;
    inline ee::Global<uint16_t, sym::g_weaponBank> g_weaponBank;
    constexpr ee::Ptr<uint16_t> kCharacterBankTable{sym::g_characterBanks}; // two
    inline ee::Global<uint16_t, sym::g_levelBank> g_levelBank;
    inline ee::Global<uint16_t, sym::g_speechBank> g_speechBank;
    inline ee::Global<uint16_t, sym::g_cutsceneBank> g_cutsceneBank;

    // The music: the game's soundtrack, the track table (soundtrack.h),
    // the stream it plays on.
    inline ee::Global<ee::Ptr<zge::Soundtrack>, sym::g_gameSoundtrack> g_gameSoundtrack;
    inline ee::Global<ee::Ptr<SoundtrackTrack>, sym::g_soundtrackTracks> g_soundtrackTracks;
    inline ee::Global<uint32_t, sym::g_soundtrackTrackCount> g_soundtrackTrackCount;
    inline ee::Global<uint32_t, sym::g_musicStream> g_musicStream;
    // The track playing and the track asked for (-1: none yet, none).
    inline ee::Global<int8_t, sym::g_currentTrack> g_currentTrack;
    inline ee::Global<int8_t, sym::g_nextTrack> g_nextTrack;
    constexpr int8_t kNoTrack = -1;
    // Frames before the playlist may start another track: 120 from a
    // track's start, 2 from the first track turned on after none was.
    inline ee::Global<int32_t, sym::g_trackWait> g_trackWait;
    constexpr int32_t kTrackStartWait = 120, kFirstTrackWait = 2;
    // Set to 1 when the playing track is turned off and to -1 when the
    // playlist moves past a track; nothing written yet reads it.
    inline ee::Global<int32_t, sym::g_trackChange> g_trackChange;
    // Set when the last track was turned off.
    inline ee::Global<uint32_t, sym::g_noTracks> g_noTracks;
    // Whether the playlist moves on to the next track that is on when
    // one ends (else the same track plays again) [inferred from the
    // frame's use].
    inline ee::Global<uint8_t, sym::g_playlistAdvances> g_playlistAdvances;
    // Each level's first track.
    constexpr ee::Ptr<const uint8_t> kLevelTracks{sym::g_levelTracks};
    inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
    // The front end's music is a voice.
    inline ee::Global<uint8_t, sym::g_musicVoice> g_musicVoice;
    // A source per voice group of the level's bank.
    constexpr ee::Ptr<uint8_t> kGroupVoiceTable{sym::g_groupVoices};

    // The one looping placed sound (event 39): its voice and sample.
    inline ee::Global<uint8_t, sym::g_loopVoice> g_loopVoice;
    inline ee::Global<uint16_t, sym::g_loopSample> g_loopSample;

    // The speech: its stream channel, the frames a phrase holds the
    // channel against the next one, and where the phrase is heard from.
    inline ee::Global<uint8_t, sym::g_speechChannel> g_speechChannel;
    inline ee::Global<int32_t, sym::g_speechHold> g_speechHold;
    constexpr int32_t kSpeechHoldFrames = 30;
    inline ee::Global<uint32_t, sym::g_speechPlaced> g_speechPlaced;
    inline ee::Global<zge::Vec4, sym::g_speechPosition> g_speechPosition;

    // The IOP command's record: {2, a, b}.
    inline ee::Global<uint32_t, sym::g_iopKind> g_iopKind;
    inline ee::Global<uint8_t, sym::g_iopA> g_iopA;
    inline ee::Global<uint8_t, sym::g_iopB> g_iopB;

    // The rolling sounds' slots (roll_sound.h): the two sources of each.
    // The sound's mode frees them all (kNoSource) around a level.
    struct RollSlot
    {
        uint16_t tyres; // kNoSource: free
        uint16_t chain;
    };
    static_assert(sizeof(RollSlot) == 4);
    constexpr uint32_t kRollSlots = 16;
    constexpr ee::Ptr<RollSlot> kRollSlotTable{sym::rollSources};
}
}
