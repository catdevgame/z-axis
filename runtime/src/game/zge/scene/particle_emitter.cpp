// The particle emitter object (zobjPEmit.c: the path string at newParticleEmitterObject;
// BMX XXX PS2 word for word, DM2 Zge2's zobjPEmit.c the same
// walks): a scene object whose class table is at g_particleEmitterObjectClass (+0x04 its
// update, updateParticleEmitters) and whose data (+0x3C) holds the emitters the game's
// effect classes made in it (aggressive/fx/particles.h: a class is a table
// of methods, +0x10 makes an emitter, +0x18 spawns particles into it, +0x1C
// moves them, +0x20 draws them).

#include "scene.h"
#include "effects.h"

#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/platform/tasks.h"
#include "zge/container/pointer_array.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/particle_emitter.cpp.h"

namespace zge
{
    // The object's data (SceneObject +0x3C for this class; 0x50 bytes from
    // the heap, newParticleEmitterObject).
    struct EmitterObjectData
    {
        ee::Ptr<PointerArray> emitters; // +0x00
        ee::Ptr<Task> drawTask;         // +0x04 drawParticleEmitters, every frame
        uint8_t _08[0x08];
        // +0x10 .. +0x40: what the class's moved, rotated, scaled and shown
        // hooks were last given (made (0, 0, 0), no turn, (1, 1, 1), shown;
        // the w of the position and the scale is never written).
        Vec4 position;                  // +0x10
        Quaternion orientation;         // +0x20
        Vec4 scale;                     // +0x30
        uint32_t shown;                 // +0x40
        // +0x44 what the emitters belong to (the game's effect record:
        // aggressive/fx's EffectOwner), set through setEmitterOwner.
        uint32_t owner;
        uint8_t _48[0x08];
    };
    static_assert(offsetof(EmitterObjectData, drawTask) == 0x04 && offsetof(EmitterObjectData, position) == 0x10);
    static_assert(offsetof(EmitterObjectData, orientation) == 0x20 && offsetof(EmitterObjectData, scale) == 0x30);
    static_assert(offsetof(EmitterObjectData, shown) == 0x40 && sizeof(EmitterObjectData) == 0x50);

    // An emitter object's data sits where other objects have their animator.
    inline ee::Ptr<EmitterObjectData> emitterDataOf(ee::Ptr<SceneObject> object)
    {
        return object->animator.cast<EmitterObjectData>();
    }
    static_assert(offsetof(EmitterObjectData, owner) == 0x44);

    namespace
    {
        // The emitter list of an object of this class.
        PointerArray &emittersOf(ee::Ptr<SceneObject> object)
        {
            return *emitterDataOf(object)->emitters;
        }

        // The emitters an object's data lists, walked: ZGE3 and ZGE4 write
        // the walk inline, ZGE2 calls arrayFirst and arrayNext.
        [[gnu::always_inline]] inline uint32_t firstEmitter(ee::Ptr<EmitterObjectData> data)
        {
            if constexpr (game::readable::kIsDm2)
                return arrayFirst(data->emitters);
            else
                return walkFirst(*data->emitters);
        }
        [[gnu::always_inline]] inline void nextEmitter(ee::Ptr<EmitterObjectData> data, uint32_t &emitter)
        {
            if constexpr (game::readable::kIsDm2)
                emitter = arrayNext(data->emitters);
            else
                walkNext(*data->emitters, emitter);
        }
        // The same from the object (its data looked up at each step, as the walks over an object's emitters do).
        [[gnu::always_inline]] inline uint32_t firstEmitter(ee::Ptr<SceneObject> object)
        {
            if constexpr (game::readable::kIsDm2)
                return arrayFirst(emitterDataOf(object)->emitters);
            else
                return walkFirst(emittersOf(object));
        }
        [[gnu::always_inline]] inline void nextEmitter(ee::Ptr<SceneObject> object, uint32_t &emitter)
        {
            if constexpr (game::readable::kIsDm2)
                emitter = arrayNext(emitterDataOf(object)->emitters);
            else
                walkNext(emittersOf(object), emitter);
        }

        constexpr uint32_t kNewEmitterCapacity = game::readable::perGame(0x40u, 0x100u);
        // ZGE2's emitters keep no owner.
        constexpr bool kEmittersKeepOwner = !game::readable::kIsDm2;
    }

    uint32_t updateParticleEmitters(ee::Ptr<SceneObject> object);
    uint32_t drawParticleEmitters(ee::Ptr<SceneObject> object);
    uint32_t emitterOwner(ee::Ptr<SceneObject> object);
    uint32_t startEffect(ee::Ptr<SceneObject> object, ee::Ptr<EffectClass> effectClass, uint32_t start, ee::Address a, ee::Address b);

    // updateParticleEmitters (the class's update): every emitter's particles
    // moved; the object's pose is marked out of date again, so that
    // updateStaleObjects calls this every frame (ZGE2 marks nothing). 1.
    uint32_t updateParticleEmitters(ee::Ptr<SceneObject> object)
    {
        uint32_t emitter = firstEmitter(object);
        while (emitter)
        {
            const ee::Ptr<EffectClass> effectClass = ee::Ptr<EffectEmitter>(emitter)->effectClass;
            effectClass->move(effectClass, ee::Ptr<EffectEmitter>(emitter));
            nextEmitter(object, emitter);
        }
        if constexpr (!game::readable::kIsDm2)
            object->flags |= kObjectPoseStale;
        return 1;
    }
    READABLE_FUNCTION(updateParticleEmitters, Verified);

    // drawParticleEmitters: every emitter drawn (the draws' counts dropped). 1.
    uint32_t drawParticleEmitters(ee::Ptr<SceneObject> object)
    {
        uint32_t emitter = firstEmitter(object);
        while (emitter)
        {
            const ee::Ptr<EffectClass> effectClass = ee::Ptr<EffectEmitter>(emitter)->effectClass;
            effectClass->draw(effectClass, ee::Ptr<EffectEmitter>(emitter), 0);
            nextEmitter(object, emitter);
        }
        return 1;
    }
    READABLE_FUNCTION(drawParticleEmitters, Verified);

    // emitterOwner: what the object's emitters belong to.
    uint32_t emitterOwner(ee::Ptr<SceneObject> object)
    {
        return emitterDataOf(object)->owner;
    }
    READABLE_FUNCTION(emitterOwner, Verified);

    // startEffect (kStartEffect): particles of an effect class spawned in
    // the object (the class's spawn with start, a and b), into its emitter of
    // that class, made first (capacity 0x40, belonging to the object's owner)
    // when it has none. Returns start (a byte), or 0 when no emitter could be
    // made (the original still adds the null emitter and writes its owner at
    // address 0x2C).
    uint32_t startEffect(ee::Ptr<SceneObject> object, ee::Ptr<EffectClass> effectClass, uint32_t start, ee::Address a, ee::Address b)
    {
        start &= 0xFFu;
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        // The emitter of the class, if the object has one.
        uint32_t item = firstEmitter(data);
        while (item && ee::Ptr<EffectEmitter>(item)->effectClass != effectClass)
            nextEmitter(data, item);
        ee::Ptr<EffectEmitter> emitter(item);
        if (!emitter)
        {
            emitter = effectClass->newEmitter(effectClass, kNewEmitterCapacity);
            if constexpr (kEmittersKeepOwner)
                emitter->owner = emitterOwner(object);
            pointerArrayAdd(data->emitters, emitter);
            if (!emitter)
                return 0;
        }
        effectClass->spawn(effectClass, object, emitter, start, a, b);
        return start;
    }
    READABLE_FUNCTION_WAITS(startEffect, Verified);

    // ---- the effect classes by name ------------------------------------------

    namespace
    {
        // The game's named effect classes (a pointer array, walked with arrayFirst and arrayNext).
        inline ee::Global<uint32_t, sym::g_namedEffectClasses> g_namedEffectClasses;

        // spawnNamedEffect's locals.
        struct NamedEffectSearch
        {
            uint32_t nameHash;
            uint32_t a;
            uint32_t b;
            ee::Ptr<NamedEffectClass> effectClass;
        };
    }

    // spawnNamedEffect: the named effect class's spawn method called with (a, b)
    // (aggressive/fx: the user effects, the skater's trails); nothing if no
    // class has that name. Returns what the method did (0 if none ran). Its
    // locals live in its frame, as the unoptimised original keeps them.
    uint32_t spawnNamedEffect(uint32_t nameHash, ee::Address a, ee::Address b)
    {
        ee::Local<NamedEffectSearch> search;
        search->nameHash = nameHash;
        search->a = a;
        search->b = b;
        search->effectClass = ee::Ptr<NamedEffectClass>(arrayFirst(ee::Ptr<PointerArray>(*g_namedEffectClasses)));
        while (search->effectClass)
        {
            if (search->effectClass->nameHash == search->nameHash)
                return search->effectClass->spawn(search->a, search->b);
            search->effectClass = ee::Ptr<NamedEffectClass>(arrayNext(ee::Ptr<PointerArray>(*g_namedEffectClasses)));
        }
        return 0;
    }

    // spawnEffectByName: ZGE2's spawnNamedEffect: the class found by its
    // name's text, not a hash of it; what its spawn method did (0 if no
    // class has that name).
    uint32_t spawnEffectByName(ee::Ptr<const char> name, ee::Address a, ee::Address b)
    {
        const auto classes = [] { return ee::Ptr<PointerArray>(*g_namedEffectClasses); };
        for (ee::Ptr<NamedEffectClass> effectClass(arrayFirst(classes())); effectClass;
             effectClass = ee::Ptr<NamedEffectClass>(arrayNext(classes())))
        {
            if (compareStrings(ee::Ptr<const char>(effectClass->name), name) == 0)
                return effectClass->spawn(a, b);
        }
        return 0;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_OWN(spawnNamedEffect, Verified, dm2, spawnEffectByName)); // (BMX XXX's: game/bmx/fx/named_effects.cpp)

    // setEmitterOwner: what the object's emitters belong to. The data is its
    // result.
    ee::Ptr<EmitterObjectData> setEmitterOwner(ee::Ptr<SceneObject> object, ee::Address owner)
    {
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        data->owner = owner;
        return data;
    }
    READABLE_FUNCTION(setEmitterOwner, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_emitterObjectsReady> g_emitterObjectsReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_sparkPool> g_sparkPool;
        inline ee::Global<ee::Ptr<Pool>, sym::g_billboardPool> g_billboardPool;
        inline ee::Global<ee::Ptr<Pool>, sym::g_polyParticlePool> g_polyParticlePool;
        inline ee::Global<int32_t, sym::g_emitterListReady> g_emitterListReady;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_emitterDefinitions> g_emitterDefinitions;

        void clearCursorArray(ee::Ptr<PointerArray> array, uint32_t freeItem, uint32_t context)
        {
            ee::game(sym::clearCursorArray, array, freeItem, context);
        }
    }

    // initEmitterObjects: the emitter objects ready (beside the emitter
    // object's methods); initSceneObjects.
    uint32_t initEmitterObjects()
    {
        *g_emitterObjectsReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initEmitterObjects, Verified);

    // shutdownEmitterObjects: no longer ready; shutDownSceneObjects.
    uint32_t shutdownEmitterObjects()
    {
        if (*g_emitterObjectsReady == 0)
            return 1;
        *g_emitterObjectsReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownEmitterObjects, Verified);

    // effectNameAllocate: bytes from the heap (a named class's record).
    uint32_t effectNameAllocate(uint32_t bytes)
    {
        return heapAllocate(bytes);
    }
    READABLE_FUNCTION_WAITS(effectNameAllocate, Verified);

    // particleSystemInit, particleSystemReset: nothing.
    void particleSystemInit(uint32_t unused)
    {
    }
    READABLE_FUNCTION(particleSystemInit, Draft);

    void particleSystemReset(uint32_t unused)
    {
    }
    READABLE_FUNCTION(particleSystemReset, Draft);

    // The spark pool destroyed.
    int32_t unregisterSparkParticles()
    {
        const int32_t result = destroyPool(*g_sparkPool);
        *g_sparkPool = ee::Ptr<Pool>();
        return result;
    }
    READABLE_FUNCTION_WAITS(unregisterSparkParticles, Draft);

    // The billboard pool destroyed.
    int32_t unregisterBillboardParticles()
    {
        const int32_t result = destroyPool(*g_billboardPool);
        *g_billboardPool = ee::Ptr<Pool>();
        return result;
    }

    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(unregisterBillboardParticles, Draft)); // (BMX XXX's: game/bmx/fx/particle_kinds.cpp)

    // The poly pool destroyed.
    int32_t unregisterPolyParticles()
    {
        const int32_t result = destroyPool(*g_polyParticlePool);
        *g_polyParticlePool = ee::Ptr<Pool>();
        return result;
    }
    READABLE_FUNCTION_WAITS(unregisterPolyParticles, Draft);

    // freeEmitterListEntry: an emitter definition freed (clearCursorArray's
    // visit). 1.
    uint32_t freeEmitterListEntry(uint32_t block)
    {
        heapFree(block);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeEmitterListEntry, Draft);

    // freeEmitterList: the emitter definitions freed, then their array.
    void freeEmitterList()
    {
        if (*g_emitterListReady != 0)
        {
            const ee::Ptr<PointerArray> definitions = *g_emitterDefinitions;
            *g_emitterListReady = 0;
            clearCursorArray(definitions, sym::freeEmitterListEntry, 0);
            freePointerArray(*g_emitterDefinitions);
        }
    }
    READABLE_FUNCTION_WAITS(freeEmitterList, Verified);

    // clearEmitterList: the emitter definitions freed, the array kept.
    void clearEmitterList()
    {
        clearCursorArray(*g_emitterDefinitions, sym::freeEmitterListEntry, 0);
    }
    READABLE_FUNCTION_WAITS(clearEmitterList, Draft);

    // ---- the object: made, destroyed, moved ---------------------------------

    namespace
    {
        // The kind initSceneObject gives the object (zobjParticleEmitter's
        // objects have the same; ZGE4 numbers the kinds one higher).
        constexpr uint32_t kEmitterObjectKind = game::readable::perGame(2u, 2u, 3u);
        constexpr uint32_t kEmitterListCapacity = 4;
        // The draw task: its stage, put at the stage's end, and the line of
        // the original's source that makes it.
        constexpr uint32_t kDrawTaskStage = 1, kDrawTaskAtEnd = 1;
        constexpr uint32_t kDrawTaskLine = game::readable::perGame(288u, 285u, 282u); // zobjPEmit.c's line of the task's making
    }

    // newParticleEmitterObject: an emitter object (kind 2, class
    // g_particleEmitterObjectClass) with no emitters yet, a task that draws
    // them every frame, at the origin, unturned, unscaled and shown. 0 when
    // no object was free (the data's allocation is not checked).
    ee::Ptr<SceneObject> newParticleEmitterObject()
    {
        ee::Ptr<SceneObject> object;
        if constexpr (game::readable::kIsBmx) // ZGE4's initSceneObject takes the kind alone and makes the object
        {
            object = newObjectOfKind(kEmitterObjectKind);
            if (!object)
                return object;
        }
        else
        {
            object = newSceneObject();
            if (!object)
                return object;
            initSceneObject(object, kEmitterObjectKind);
        }
        object->objectClass = ee::Ptr<SceneObjectClass>(sym::g_particleEmitterObjectClass);
        object->animator = ee::Ptr<Animator>(heapAllocate(sizeof(EmitterObjectData)));
        emitterDataOf(object)->emitters = newCursorArray(kEmitterListCapacity);
        emitterDataOf(object)->drawTask = newTask(kDrawTaskStage, sym::drawParticleEmitters, object, kDrawTaskAtEnd, sym::g_textNgengineZge3ZgeModsZobjPEmit, kDrawTaskLine);
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        data->orientation.x = ee::Float(0.0f);
        data->orientation.y = ee::Float(0.0f);
        data->orientation.z = ee::Float(0.0f);
        data->orientation.w = ee::Float(1.0f);
        data->position.x = ee::Float(0.0f);
        data->position.y = ee::Float(0.0f);
        data->position.z = ee::Float(0.0f);
        data->scale.x = ee::Float(1.0f);
        data->scale.y = ee::Float(1.0f);
        data->scale.z = ee::Float(1.0f);
        data->shown = 1;
        return object;
    }
    READABLE_FUNCTION_WAITS(newParticleEmitterObject, Verified);

    // deleteEffectEmitters (ZGE4 only; destroyEmitterObject and the game's
    // clearEffects): each of the object's emitters taken off its list and
    // deleted by its class.
    inline void deleteEffectEmitters(ee::Ptr<SceneObject> object)
    {
        uint32_t emitter = firstEmitter(object);
        while (emitter)
        {
            pointerArrayRemoveCurrent(emitterDataOf(object)->emitters);
            const ee::Ptr<EffectClass> effectClass = ee::Ptr<EffectEmitter>(emitter)->effectClass;
            effectClass->deleteEmitter(effectClass, ee::Ptr<EffectEmitter>(emitter));
            nextEmitter(object, emitter);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, deleteEffectEmitters);

    // destroyEmitterObject (the class's destroy): each emitter deleted by its
    // class (ZGE4: and taken off the list first, deleteEffectEmitters), the
    // list freed, the draw task deleted and the data freed. 1.
    uint32_t destroyEmitterObject(ee::Ptr<SceneObject> object)
    {
        if constexpr (game::readable::kIsBmx)
            deleteEffectEmitters(object);
        else
        {
            uint32_t emitter = firstEmitter(object);
            while (emitter)
            {
                const ee::Ptr<EffectClass> effectClass = ee::Ptr<EffectEmitter>(emitter)->effectClass;
                effectClass->deleteEmitter(effectClass, ee::Ptr<EffectEmitter>(emitter));
                nextEmitter(object, emitter);
            }
        }
        freePointerArray(emitterDataOf(object)->emitters);
        deleteTask(emitterDataOf(object)->drawTask);
        if (object->animator)
            heapFree(object->animator);
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyEmitterObject, Verified);

    // The class's moved, rotated, scaled and shown hooks: the value kept in
    // the data. 1, or 0 when the object has no data.
    uint32_t setEmitterObjectPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        if (!data)
            return 0;
        aligned(ee::Ptr<Vec4>(&data->position)) = quadwordOf(position); // a quadword move
        return 1;
    }
    READABLE_FUNCTION(setEmitterObjectPosition, Draft);

    uint32_t setEmitterObjectOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        if (!data)
            return 0;
        std::memcpy(&data->orientation, &*orientation, sizeof(Quaternion)); // it may be unaligned
        return 1;
    }
    READABLE_FUNCTION(setEmitterObjectOrientation, Draft);

    uint32_t setEmitterObjectScale(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> scale)
    {
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        if (!data)
            return 0;
        aligned(ee::Ptr<Vec4>(&data->scale)) = quadwordOf(scale); // a quadword move
        return 1;
    }
    READABLE_FUNCTION(setEmitterObjectScale, Draft);

    uint32_t setEmitterObjectShown(ee::Ptr<SceneObject> object, uint32_t shown)
    {
        const ee::Ptr<EmitterObjectData> data = emitterDataOf(object);
        if (!data)
            return 0;
        data->shown = shown;
        return 1;
    }
    READABLE_FUNCTION(setEmitterObjectShown, Draft);

    // startNamedEffect: particles of the effect class registered under the
    // name spawned in the object (startEffect). Returns start (a byte), 0
    // when no class has the name or no emitter could be made.
    uint32_t startNamedEffect(ee::Ptr<SceneObject> object, uint32_t name, uint32_t start, ee::Address a, ee::Address b)
    {
        start &= 0xFFu;
        const ee::Ptr<EffectClass> effectClass = findEffectClassByName(name);
        if (!effectClass)
            return 0;
        return startEffect(object, effectClass, start, a, b);
    }
    READABLE_FUNCTION_WAITS(startNamedEffect, Verified);
}
