// BMX XXX's "ZPE" level object type: a particle emitter placed in the level
// that the level's scripts turn on and off. BMX XXX's own code (its game
// block); names from what each function does and the type's string.

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
#include "sym/bmx/level/particle_emitters.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The emitter's own tag data (objectComponent).
        constexpr uint32_t kEnabledTag = 0; // byte: 1 enabled
        constexpr uint32_t kTagByte = 2;    // the tag accessors' second argument (the value's kind)

        constexpr uint32_t kTypeName = sym::zpeTypeName; // "ZPE"
        constexpr uint32_t kCreateEntity = sym::createEntity;

        // ZGE4's particle emitter data (the object's animator word), as far
        // as this type reads it. The engine's header (zge/scene/
        // particle_types.h ParticleEmitterData) has ZGE3's layout only.
        struct EmitterData
        {
            uint8_t _00[0x98];
            // +0x98 the ZPED chunk's rate, particles a second (0: the emitter
            // is never turned on): ParticleParams::rate of the params
            // readZpedChunk copies to +0x90 (ZGE3 +0x70), the yaw, pitch and
            // their spreads before it; the rate message sets the live rate at
            // +0x138, which the chunk's read starts from this one.
            ee::Float rate;
            uint8_t _9c[0x130 - 0x9C];
            int32_t emitting; // +0x130 (ZGE3 +0x100) 0: it spawns nothing by itself
        };
        // An emitter object keeps that data where other objects keep an animator.
        ee::Ptr<EmitterData> emitterDataOf(ee::Ptr<zge::SceneObject> emitter) { return emitter->animator.cast<EmitterData>(); }
        static_assert(offsetof(EmitterData, rate) == 0x98 && offsetof(EmitterData, emitting) == 0x130);
    }

    uint32_t initZpe(ee::Ptr<zge::SceneObject> emitter);
    uint32_t zpeEnabled(ee::Ptr<zge::SceneObject> emitter);
    void setZpeEnabled(ee::Ptr<zge::SceneObject> emitter, uint32_t enabled);

    // registerZpeType: the "ZPE" type, made as a plain entity and set up by initZpe.
    uint32_t registerZpeType()
    {
        return game::tail(zge::registerLevelObjectType, kTypeName, kCreateEntity, static_cast<uint32_t>(sym::initZpe));
    }
    READABLE_FUNCTION(registerZpeType, Verified);

    // initZpe (the type's init): its messages subscribed, and the emitter
    // put in the state its tag data says. Always 1.
    uint32_t initZpe(ee::Ptr<zge::SceneObject> emitter)
    {
        zge::subscribe(emitter, zge::kSaveSizeMessage, zge::MessageHandler(sym::zpeSaveSize));
        zge::subscribe(emitter, zge::kSaveMessage, zge::MessageHandler(sym::saveZpe));
        zge::subscribe(emitter, zge::kRestoreMessage, zge::MessageHandler(sym::restoreZpe));
        zge::subscribe(emitter, kEnableMessage, zge::MessageHandler(sym::enableZpe));
        zge::subscribe(emitter, kDisableMessage, zge::MessageHandler(sym::disableZpe));
        setZpeEnabled(emitter, zpeEnabled(emitter));
        return 1;
    }
    READABLE_FUNCTION_WAITS(initZpe, Verified);

    // enableZpe (kEnableMessage).
    int32_t enableZpe(ee::Ptr<zge::EngineMessage> message)
    {
        setZpeEnabled(ee::Ptr<zge::SceneObject>(message->target), 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(enableZpe, Verified);

    // disableZpe (kDisableMessage).
    int32_t disableZpe(ee::Ptr<zge::EngineMessage> message)
    {
        setZpeEnabled(ee::Ptr<zge::SceneObject>(message->target), 0);
        return 0;
    }
    READABLE_FUNCTION_WAITS(disableZpe, Verified);

    // zpeSaveSize (the engine's save size message): its enabled word.
    int32_t zpeSaveSize(ee::Ptr<zge::EngineMessage> message)
    {
        *ee::Ptr<uint32_t>(message->data) = sizeof(uint32_t);
        return 0;
    }
    READABLE_FUNCTION(zpeSaveSize, Verified);

    // restoreZpe (the engine's restore message).
    int32_t restoreZpe(ee::Ptr<zge::EngineMessage> message)
    {
        setZpeEnabled(ee::Ptr<zge::SceneObject>(message->target), *ee::Ptr<uint32_t>(message->data));
        return 0;
    }
    READABLE_FUNCTION_WAITS(restoreZpe, Verified);

    // saveZpe (the engine's save message).
    int32_t saveZpe(ee::Ptr<zge::EngineMessage> message)
    {
        const ee::Ptr<uint32_t> saved(message->data);
        *saved = zpeEnabled(ee::Ptr<zge::SceneObject>(message->target));
        return 0;
    }
    READABLE_FUNCTION(saveZpe, Verified);

    // zpeEnabled: its enabled byte.
    uint32_t zpeEnabled(ee::Ptr<zge::SceneObject> emitter)
    {
        return zge::tagByte(zge::objectComponent(emitter), kTagByte, kEnabledTag);
    }
    READABLE_FUNCTION(zpeEnabled, Verified);

    // setZpeEnabled: the emitter emits and is shown when enabled (1) and
    // its chunk's rate is not 0, else neither; the enabled byte kept.
    void setZpeEnabled(ee::Ptr<zge::SceneObject> emitter, uint32_t enabled)
    {
        const ee::Ptr<EmitterData> data = emitterDataOf(emitter);
        uint32_t on = 0;
        if (enabled == 1 && !(data->rate == ee::Float(0.0f)))
            on = 1;
        data->emitting = on;
        zge::setSceneObjectShown(emitter, on);
        zge::markPoseStale(emitter);
        game::tail(zge::setTagByte, zge::objectComponent(emitter), kTagByte, kEnabledTag, enabled & 0xFFu);
    }
    READABLE_FUNCTION_WAITS(setZpeEnabled, Verified);
}
