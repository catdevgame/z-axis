// BMX XXX's "SoundEmitter" level object type: a sound that plays at a place
// in the level for as long as the object is there. BMX XXX's own code (its
// game block); names from what each function does and the type's strings.

#include "entity_messages.h"
#include "level_calls.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/animation.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/sound_emitters.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The emitter's own tag data (objectComponent).
        constexpr uint32_t kSoundNameTag = 0; // text: the middle of its sound's name
        constexpr uint32_t kSoundTag = 0x40;  // word: the playing sound's handle
        constexpr uint32_t kTagWord = 8, kTagText = 0x10; // the tag accessors' second argument (the value's kind)

        constexpr uint32_t kTypeName = sym::soundEmitterTypeName;                         // "SoundEmitter"
        constexpr uint32_t kIdleSoundFormat = sym::bmx::soundEmitterIdleSoundFormat; // "%s.%s.idle"
        constexpr uint32_t kCreateEntity = sym::createEntity;
    }

    uint32_t initSoundEmitter(ee::Ptr<zge::SceneObject> emitter);

    // registerSoundEmitterType: the "SoundEmitter" type, made as a plain
    // entity and set up by initSoundEmitter.
    uint32_t registerSoundEmitterType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initSoundEmitter));
    }
    READABLE_FUNCTION(registerSoundEmitterType, Verified);

    // initSoundEmitter (the type's init): its sound ("SoundEmitter.<its
    // name>.idle") started where it stands, the handle kept. Always 1.
    uint32_t initSoundEmitter(ee::Ptr<zge::SceneObject> emitter)
    {
        ee::Local<char[0x40]> soundName;
        ee::Local<char[0x40]> ownName;
        zge::subscribe(emitter, zge::kRemovedMessage, zge::MessageHandler(sym::soundEmitterRemoved));
        zge::subscribe(emitter, zge::kUpdateMessage, zge::MessageHandler(sym::updateSoundEmitter));
        zge::tagString(zge::objectComponent(emitter), kTagText, kSoundNameTag, ownName.address());
        zge::formatString(soundName, kIdleSoundFormat, kTypeName, ownName.address());
        const ee::Ptr<zge::Vec4> position = zge::objectPosition(emitter);
        const uint32_t sound = soundEvent(kObjectSoundEvent, position, kObjectSoundStart, soundName.address(), kFullVolume);
        zge::setTagWord(zge::objectComponent(emitter), kTagWord, kSoundTag, sound);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initSoundEmitter, Verified);

    // soundEmitterRemoved (the engine's removed message): its sound stopped.
    int32_t soundEmitterRemoved(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> emitter(message->target);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(emitter), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(emitter), kObjectSoundStop, sound, 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(soundEmitterRemoved, Verified);

    // updateSoundEmitter (the frame's update): its sound follows it.
    int32_t updateSoundEmitter(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<zge::SceneObject> emitter(message->target);
        const uint32_t sound = static_cast<uint32_t>(zge::tagWord(zge::objectComponent(emitter), kTagWord, kSoundTag));
        soundEvent(kObjectSoundEvent, zge::objectPosition(emitter), kObjectSoundMove, sound, kFullVolume);
        return 0;
    }
    READABLE_FUNCTION_WAITS(updateSoundEmitter, Verified);
}
