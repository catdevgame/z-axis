#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge { struct AnimationEvent; struct SceneObject; struct Vec4; }

namespace aggressive
{
    // (events.cpp)
    int32_t animationPartSound(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::AnimationEvent> event);
    void initSound();
    void loadAudioSync(uint32_t sample, ee::Address listener);
    uint32_t placedVolume(ee::Address sample, ee::Ptr<zge::Vec4> position, uint32_t levelArgument, ee::Ptr<uint8_t> volume, ee::Ptr<int8_t> pan, ee::Ptr<uint8_t> third);
    uint32_t playSelectSound();
    int32_t setGameMode(uint32_t mode);
    int32_t setPlayPhase(uint32_t phase);
    void setSoundEmitterState(uint32_t id, uint32_t on);
    void setupSoundEmitters();
    uint32_t soundBankOfKind(uint32_t kindArgument);
    uint32_t soundEvent(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d);
    int32_t startAudioSync();
    int32_t startRollSound();
    void stopAudioSync();
    int32_t stopRollSound(uint32_t slot);
    void stopSoundEmitters();
    void updateRollSound(uint32_t source, uint32_t sound, ee::Address position, int32_t speed);
    void updateSoundEmitters(ee::Float seconds);
}
