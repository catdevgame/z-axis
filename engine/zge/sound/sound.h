#pragma once

// The sound engine's channels, sources and effects (channels.cpp, sources.cpp, effects.cpp; the banks are bank.h,
// the streams streams.h): what the game calls by name.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"
#include "zge/math/types.h"
#include "zge/sound/bank.h"
#include "zge/sound/streams.h"

#include <cstdint>

namespace zge
{
    // (channels.cpp)
    uint32_t soundRpc(uint32_t function, ee::Address send, uint32_t sendBytes, uint32_t replyWanted);
    int32_t holdSoundUpdates();
    int32_t releaseSoundUpdates();
    uint32_t audioFrame();
    uint32_t releaseVoice(uint32_t voice);
    uint32_t iopCommand();
    uint32_t allocateVoice(ee::Ptr<BankSample> sample, uint8_t priority);
    void setVoiceVolume(uint8_t channel, uint8_t volume);
    void setVoicePan(uint8_t channel, int8_t pan);
    void setVoicePitch(uint8_t channel, int16_t cents);
    void startVoice(uint8_t channel);
    void playSample(ee::Ptr<BankSample> sample, int16_t cents, uint8_t volume, int8_t pan, uint8_t priority);
    void stopVoice(uint8_t channel, uint32_t how);
    void disableDoppler();
    void stopChannelsOfOwner(ee::Ptr<BankSample> sample);
    uint32_t releaseAllVoices();
    uint32_t voiceState(uint8_t channel);
    ee::Ptr<BankSample> voiceSound(uint8_t channel);
    uint32_t initSoundChannels();
    uint32_t shutdownSoundChannels();
}

namespace zge
{
    // (sources.cpp)
    uint32_t initSoundSources();
    uint32_t releaseAllSourceVoices();
    uint32_t updateSoundSources();
    uint32_t newGroupSource(ee::Ptr<SoundGroup> group, uint32_t flags);
    uint32_t releaseSourceVoices(uint32_t handle);
    uint32_t restartSourceVoices(uint32_t handle);
    uint32_t stopSoundSource(uint32_t handle);
    uint32_t setSourceMix(uint32_t handle, uint32_t volume, uint32_t pan);
    uint32_t playGroupOnce(ee::Ptr<SoundGroup> group, uint32_t volume, uint32_t pan);
    void resetSoundSources();
}

namespace zge
{
    // (effects.cpp)
    int32_t updateEffects();
    int32_t initSoundEmitters(uint32_t range, uint32_t milliseconds);
    int32_t initSoundBankPool();
    int32_t soundRecordsInUse();
    uint32_t sourceSample(uint32_t emitter, uint32_t sound);
    uint32_t setSourcePosition(uint32_t emitter, ee::Ptr<Vec4> position);
    uint32_t sourcePitch(uint32_t emitter, uint32_t pitch);
    uint32_t sourceVolume(uint32_t emitter, uint32_t loudness);
    uint32_t sourceHeard(ee::Address soundRecord, ee::Ptr<Vec4> position, ee::Ptr<uint8_t> volumeOut, ee::Ptr<int8_t> panOut
                         READABLE_PARAM_ONLY(bmx, ee::Ptr<uint8_t> aheadOut));
    // ZGE2's sourceHeard (effects.cpp): no sound record, the one range, whole units.
    uint32_t positionHeard(ee::Ptr<Vec4> position, ee::Ptr<uint8_t> volumeOut, ee::Ptr<int8_t> panOut);
    uint32_t startSource(ee::Address sound, ee::Address where);
    void stopSource(uint16_t source);
    void cameraMoved(ee::Address listeners);
    void zsndSourceReset();
    uint32_t soundTask(); // (channels.cpp) the voices' frame
}
