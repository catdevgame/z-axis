// BMX XXX's pool of particle emitter objects (emitter_objects.h). BMX XXX's
// own code (fx.c's block): DM2 and Aggressive Inline have no such pool.

#include "emitter_objects.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/fx/emitter_objects.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kSlots = kEmitterObjectSlots;
        constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
        inline auto &g_kinds = g_emitterObjectKinds;
        inline auto &g_slots = g_emitterObjects;
        inline auto &g_nextSlot = g_nextEmitterObject;
        // The effects' clock, float milliseconds.
        inline ee::Global<ee::Float, sym::g_effectsClockMs> g_clockMs;

        // The emitter's start message; its data is the count of particles asked for.
        constexpr uint32_t kEmitterStartMessage = 0xF0;

        // Scaffolding (address-call): the engine's copy of a particle emitter
        // object, and whether an emitter object still has a particle alive
        // (names inferred from their code; the engine has no body for them yet).
        namespace calls
        {
        ee::Ptr<zge::SceneObject> copyParticleEmitterObject(ee::Ptr<zge::SceneObject> source)
        {
            return ee::Ptr<zge::SceneObject>(ee::game<uint32_t>(sym::copyParticleEmitterObject, source));
        }
        bool particleEmitterObjectAlive(ee::Ptr<zge::SceneObject> object)
        {
            return ee::game<uint32_t>(sym::particleEmitterObjectAlive, object) != 0;
        }
        }

        // The slot's object put at the point, shown, and started (with no
        // particle asked for: it emits by its own rate).
        void startEmitterObject(ee::Ptr<EmitterObjectSlot> slot, ee::Ptr<zge::Vec4> position, ee::Ptr<zge::EngineMessage> message,
                                ee::Ptr<uint8_t> count)
        {
            zge::setObjectPosition(slot->object, position);
            zge::setSceneObjectShown(slot->object, 1);
            *count = 0;
            message->flags = zge::kMessageNow;
            message->target = slot->object;
            message->type = kEmitterStartMessage;
            message->data = count;
            message->sender = 0;
            zge::sendEngineMessage(message);
        }
    }

    // takeEmitterObject: see emitter_objects.h. The search goes once round
    // the pool from the cursor. A free slot found moves the cursor past it
    // and takes the kind; a slot taken over keeps the kind it had. The old
    // object is deleted and a copy of the kind's made in its place. Kept as
    // it is: when the copy cannot be made the slot is still returned, with
    // no object, and the cursor is set to the slot that would have been
    // taken over (none: all ones).
    ee::Ptr<EmitterObjectSlot> takeEmitterObject(uint32_t kind)
    {
        EmitterObjectKindRecord &wanted = (*g_kinds)[kind];
        if (!wanted.source)
            return {};
        ee::Float oldest = *g_clockMs;
        uint32_t oldestSlot = kNoSlot;
        ee::Ptr<EmitterObjectSlot> slot{};
        uint32_t index = *g_nextSlot;
        do
        {
            const ee::Ptr<EmitterObjectSlot> candidate(&(*g_slots)[index]);
            if (!candidate->object || !calls::particleEmitterObjectAlive(candidate->object))
            {
                slot = candidate;
                slot->kind = kind;
                *g_nextSlot = index + 1;
                if (index + 1 == kSlots)
                    *g_nextSlot = 0;
                break;
            }
            if (candidate->clockMs <= oldest && !(wanted.flags & kKindNeverTakesOver))
            {
                oldest = candidate->clockMs;
                oldestSlot = index;
            }
            if (++index == kSlots)
                index = 0;
        } while (index != *g_nextSlot);
        if (!slot)
        {
            if (oldestSlot == kNoSlot)
                return {};
            slot = ee::Ptr<EmitterObjectSlot>(&(*g_slots)[oldestSlot]);
        }
        if (slot->object)
            zge::deleteSceneObject(slot->object);
        slot->object = calls::copyParticleEmitterObject(wanted.source);
        if (!slot->object)
            *g_nextSlot = oldestSlot;
        return slot;
    }
    READABLE_FUNCTION_WAITS(takeEmitterObject, Verified);

    // freeEmitterObject: the slot's object deleted, if it has one.
    void freeEmitterObject(ee::Ptr<EmitterObjectSlot> slot)
    {
        if (slot->object)
        {
            zge::deleteSceneObject(slot->object);
            slot->object = {};
        }
    }
    READABLE_FUNCTION_WAITS(freeEmitterObject, Verified);

    // spawnConcreteDust: a puff of concrete dust at the point (the
    // "ZPE_SFX_concrete_dust" emitter object).
    void spawnConcreteDust(ee::Ptr<zge::Vec4> position)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint8_t> count;
        const ee::Ptr<EmitterObjectSlot> slot = takeEmitterObject(kEmitterConcreteDust);
        if (slot)
            startEmitterObject(slot, position, message, count);
    }
    READABLE_FUNCTION_WAITS(spawnConcreteDust, Verified);

    // spawnDust: a puff of dust at the point (the "ZPE_SFX_dust"
    // emitter object).
    void spawnDust(ee::Ptr<zge::Vec4> position)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint8_t> count;
        const ee::Ptr<EmitterObjectSlot> slot = takeEmitterObject(kEmitterDust);
        if (slot)
            startEmitterObject(slot, position, message, count);
    }
    READABLE_FUNCTION_WAITS(spawnDust, Verified);

    namespace
    {
        // Scaffolding (address-call): docs/SCAFFOLDING.md. ZGE4's own setters of an emitter object's settings (its
        // data at the object's +0x3C), each multiplying one by the factor, nothing for a factor below 0, and
        // returning 1. Named by the fields they scale: ZGE4's ZPED reader copies the record into the data in ZGE3's
        // ParticleParams order 0x90 further on (zge/scene/particle_types.h), so +0xA0 is the speed (ZGE4's
        // particleSpawnVelocity reads it beside the yaw and pitch), +0xA8 the life and +0xB0 the size (the spark
        // type's settings read them at those record places), and +0x138 the rate it emits at (its update adds it
        // times the seconds to what it owes). The size's setter scales the word after the size too. Not in the
        // engine's sources yet: ZGE4's emitter data has no layout there.
        namespace calls
        {
        void scaleEmitterSpeed(ee::Ptr<zge::SceneObject> object, ee::Float factor) { ee::game(sym::scaleEmitterSpeed, object, factor); }
        void scaleEmitterRate(ee::Ptr<zge::SceneObject> object, ee::Float factor) { ee::game(sym::scaleEmitterRate, object, factor); }
        void scaleEmitterSize(ee::Ptr<zge::SceneObject> object, ee::Float factor) { ee::game(sym::scaleEmitterSize, object, factor); }
        void scaleEmitterLife(ee::Ptr<zge::SceneObject> object, ee::Float factor) { ee::game(sym::scaleEmitterLife, object, factor); }
        }

        // The slot's object started (with no particle asked for: it emits by
        // its own rate), as startEmitterObject's last step.
        void sendEmitterStart(ee::Ptr<EmitterObjectSlot> slot, ee::Ptr<zge::EngineMessage> message, ee::Ptr<uint8_t> count)
        {
            *count = 0;
            message->flags = zge::kMessageNow;
            message->target = slot->object;
            message->type = kEmitterStartMessage;
            message->data = count;
            message->sender = 0;
            zge::sendEngineMessage(message);
        }
    }

    // spawnSmoke: smoke (the "ZPE_SFX_smoke" emitter object) at the
    // point (0: where the library's object is), turned by the matrix (0: as
    // it is), shown, its speed, rate, size and life scaled.
    void spawnSmoke(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Matrix> rotation, ee::Float speed, ee::Float rate,
                          ee::Float size, ee::Float life)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint8_t> count;
        const ee::Ptr<EmitterObjectSlot> slot = takeEmitterObject(kEmitterSmoke);
        if (!slot)
            return;
        if (position)
            zge::setObjectPosition(slot->object, position);
        if (rotation)
            zge::setObjectRotationMatrix(slot->object, rotation);
        zge::setSceneObjectShown(slot->object, 1);
        calls::scaleEmitterSpeed(slot->object, speed);
        calls::scaleEmitterRate(slot->object, rate);
        calls::scaleEmitterSize(slot->object, size);
        calls::scaleEmitterLife(slot->object, life);
        sendEmitterStart(slot, message, count);
    }
    READABLE_FUNCTION_WAITS(spawnSmoke, Verified);

    // spawnExplosionFire: an explosion's fire (the "ZPE_SFX_explosion_fire"
    // emitter object) at the point, shown, its rate and size scaled.
    void spawnExplosionFire(ee::Ptr<zge::Vec4> position, ee::Float rate, ee::Float size)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<uint8_t> count;
        const ee::Ptr<EmitterObjectSlot> slot = takeEmitterObject(kEmitterExplosionFire);
        if (!slot)
            return;
        zge::setObjectPosition(slot->object, position);
        zge::setSceneObjectShown(slot->object, 1);
        calls::scaleEmitterRate(slot->object, rate);
        calls::scaleEmitterSize(slot->object, size);
        sendEmitterStart(slot, message, count);
    }
    READABLE_FUNCTION_WAITS(spawnExplosionFire, Verified);
}
