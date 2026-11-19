// Particle emitter objects (zobjParticleEmitter.c: "../ngengine/ZGE3/zge/mods/
// zobjParticleEmitter.c" names its update task): a level's ZPEO chunk makes a
// scene object of class g_particleEmitterSceneObjectClass, its ZPED child the
// emitter's settings. Not the effect classes' emitters of particle_emitter.cpp
// (zobjPEmit): these run the engine's particle types (registerParticleType:
// spark, billboard, poly), each type making one effect in the object.
//
// Messages: kEmitterStartMessage spawns particles at once, kEmitterRateMessage
// sets how many a second the object spawns by itself.
//
// ZGE4 (BMX XXX) keeps every emitter object on a list of its own
// (g_particleEmitterObjects) and runs one task over them
// (updateParticleEmitterObjects) where ZGE3's objects are updated as any
// other; its data comes from a pool, holds the emitter's matrix (the object
// placed relative to its parent's bone from it; emitterMatrixOf,
// setEmitterPosition, setEmitterOrientation), a live rate apart from the
// chunk's, an emitting switch (setEmitterEmitting) and a start message's
// burst: particles spawn by the rate for burstSeconds, then stop.

#include "scene.h"
#include "message.h"
#include "particle_types.h"

#include "zge/assets/chunk_reader.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene_database.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/platform/tasks.h"
#include "zge/collision/collision.h"
#include "zge/container/cursor_array.h"
#include "zge/container/pointer_array.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/particle_emitter_objects.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<int32_t, sym::g_particleEmitterObjectsReady> g_particleEmitterObjectsReady;
        // ZGE4's: the emitter objects in the order made (updateParticleEmitterObjects walks them), the pool their data
        // comes from (made by the first object), the task running updateParticleEmitterObjects, and the switch
        // setParticlesOn clears (set: no particle spawns; the types' spawners read it).
        inline ee::Global<ee::Ptr<PointerArray>, sym::bmx::g_particleEmitterObjects> g_particleEmitterObjects;
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_particleEmitterDataPool> g_particleEmitterDataPool;
        inline ee::Global<ee::Ptr<Task>, sym::bmx::g_particleEmitterObjectsTask> g_particleEmitterObjectsTask;
        inline ee::Global<uint32_t, sym::bmx::g_particlesOff> g_particlesOff;

        // The kind initSceneObject gives these objects (ZGE4's newObjectOfKind: 4).
        constexpr uint32_t kParticleEmitterKind = game::readable::perGame(2u, 2u, 4u);
        constexpr uint32_t kEmitterDataBytes = sizeof(ParticleEmitterData);
        // The update task: its priority, flags and the line of the original's source (ZGE4's zobjParticleEmitter.c).
        constexpr uint32_t kUpdateTaskPriority = 1, kUpdateTaskFlags = 1;
        constexpr uint32_t kUpdateTaskLine = game::readable::perGame(325u, 325u, 635u);
        // ZGE4's task over every emitter object (registerParticleEmitterChunkHandlers makes it): its priority and
        // source line, and how many objects the list holds.
        constexpr uint32_t kObjectsTaskPriority = 7, kObjectsTaskLine = 450, kEmitterObjectsCapacity = 0xC8;
        constexpr uint32_t kEffectsCapacity = 4, kBindingsCapacity = 0x10;
        // The second word a type's start method is given.
        constexpr uint32_t kStartWord = 0x100;

        // The messages the object takes. Start: the message's data points to a
        // byte, how many particles to spawn now. Rate: to a float, particles a
        // second from now on.
        constexpr uint32_t kEmitterStartMessage = 0xF0;
        constexpr uint32_t kEmitterRateMessage = 0xF1;

        void memset(ee::Address dst, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, dst, byte, bytes); }
        // Scaffolding (address-call): the object registered as a loaded asset of the scene (no entry, form 0, left
        // with the scene) by its address: AI's and BMX XXX's take a sixth word DM2's does not, and a typed call
        // would say so with a per-game word.
        namespace calls
        {
            uint32_t registerLoadedAsset(ee::Ptr<SceneObject> object)
            {
                return ee::game<uint32_t>(sym::registerLoadedAsset, 0, 0, kAssetParticleEmitter, 0, object, 1);
            }
        }
        // ZGE4: the object spawns by its rate while on, and a rate message of 0 turns it off.
        inline void setEmittingByRate(ee::Ptr<ParticleEmitterData> data, ee::Float rate)
        {
            data->emitting = rate == ee::Float(0.0f) ? 0 : 1;
        }
    }

    void startEmitterEffect(ee::Ptr<SceneObject> object, uint32_t typeHash, uint32_t count);
    uint32_t addEmitterEffect(ee::Ptr<ParticleEmitterData> data, ee::Ptr<EmitterDefinition> definition);
    ee::Ptr<SceneObject> newParticleEmitterSceneObject();

    // ---- ZGE4's own (BMX XXX; ZGE3 has none of these) ----

    // emitterMatrixOf: the emitter's matrix, kept in its data.
    inline ee::Ptr<Matrix> emitterMatrixOf(ee::Ptr<SceneObject> object)
    {
        return &emitterMatrixIn(*emitterData(object));
    }
    READABLE_FUNCTION_ONLY(bmx, emitterMatrixOf);

    // setEmitterPosition: the position into the matrix's translation row. 1.
    inline uint32_t setEmitterPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> position)
    {
        aligned(ee::Ptr<Vec4>(&emitterMatrixIn(*emitterData(object)).translation())) = quadwordOf(position);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, setEmitterPosition);

    // setEmitterOrientation: the matrix made from the quaternion (its translation row with it). 1.
    inline uint32_t setEmitterOrientation(ee::Ptr<SceneObject> object, ee::Ptr<Quaternion> orientation)
    {
        quaternionToMatrix(&emitterMatrixIn(*emitterData(object)), orientation.get());
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setEmitterOrientation);

    // setEmitterEmitting: the emitter turned on (it spawns by its rate, and the object counts as moved) or off. 1.
    inline uint32_t setEmitterEmitting(ee::Ptr<SceneObject> object, uint32_t on)
    {
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if (on)
        {
            object->flags = object->flags | kObjectMoved;
            data->emitting = 1;
        }
        else
        {
            object->flags = object->flags & ~kObjectMoved;
            data->emitting = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, setEmitterEmitting);

    // updateParticleEmitterObjects (the emitters' task): every emitter object that moved or is always updated gets
    // its update, in the order they were made, until one's update answers 0: the rest are left this frame. That
    // answer (1 while every update answered), 0 with no array.
    inline uint32_t updateParticleEmitterObjects()
    {
        const ee::Ptr<PointerArray> array = *g_particleEmitterObjects;
        if (!array)
            return 0;
        uint32_t going = 1;
        for (uint32_t item = walkFirst(*array); item; walkNext(*array, item))
        {
            const ee::Ptr<SceneObject> object(item);
            if (object->flags & (kObjectMoved | kObjectAlwaysUpdated))
                going = going != 0 && updateParticleEmitterObject(object) != 0 ? 1u : 0u;
        }
        return going;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, updateParticleEmitterObjects);

    // updateParticleEmitterObject (the class's update; ZGE4's objects task calls it): hung from its parent the
    // first time (when it names one), each effect's particles moved; the object stays out of date (updated again
    // next frame) while particles live or it spawns by itself, and it spawns what its rate has built up. ZGE4
    // hangs it from the model object of the parent's name (made when the name has none) and places it relative
    // to the bone by its matrix; its burst counts down and stops the spawning when over; it is marked stale every
    // frame and kept always updated while particles live. 1.
    uint32_t updateParticleEmitterObject(ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if (data->parentHash != 0 && !data->parent)
        {
            data->parent = ee::Ptr<SceneObject>(findSceneObject(data->parentHash, 0, 0));
            if constexpr (game::readable::kIsBmx)
            {
                if (!data->parent || (data->parent->flags & kObjectKindMask) != kObjectModelKind)
                    data->parent = namedModelObject(data->parentHash, 1);
            }
            data->bone = findModelBone(data->parent, data->boneHash);
            zge::attachObject(object, data->parent, data->bone);
            if constexpr (game::readable::kIsBmx)
            {
                // (ZGE4's frame has room for these three; ZGE3's does not)
                ee::Local<Matrix> boneMatrix;
                ee::Local<Matrix> boneInverse;
                ee::Local<Matrix> local;
                objectBoneMatrix(data->parent, data->bone, boneMatrix, kBoneInWorld);
                matrixInvertRigid(boneInverse, boneMatrix);
                matrixMultiply(local, boneInverse, emitterMatrixOf(object));
                setObjectPosition(object, &local->translation());
                matrixToQuaternion(objectOrientation(object).get(), local);
            }
        }
        if constexpr (game::readable::kIsBmx)
        {
            EmitterBurst &burst = emitterBurst(*data);
            burst.burstLeft = burst.burstLeft - zge::frameSeconds(); // (counted down whether a burst runs or not)
            if (burst.bursting && burst.burstLeft < ee::Float(0.0f))
            {
                data->emitting = 0;
                burst.bursting = 0;
            }
        }
        int32_t living = 0;
        uint32_t effect = walkFirst(*data->effects);
        while (effect)
        {
            living += updateParticleEffect(ee::Ptr<ParticleEffect>(effect), 1, data);
            walkNext(*data->effects, effect);
        }
        if constexpr (game::readable::kIsBmx)
        {
            markPoseStale(object);
            setObjectAlwaysUpdated(object, living != 0 ? 1u : 0u);
            if (data->emitting && ee::Float(0.0f) < emitterBurst(*data).rate)
            {
                const ee::Float seconds = zge::frameSeconds();
                const ee::Ptr<ParticleSettings> settings = data->settings;
                data->owed = data->owed + emitterBurst(*data).rate * seconds;
                startEmitterEffect(object, settings->type->nameHash, static_cast<uint32_t>(ee::toInt(data->owed)) & 0xFFu);
                data->owed = data->owed - ee::toFloat(ee::toInt(data->owed));
            }
        }
        else
        {
            if (living > 0 || ee::Float(0.0f) < data->params.rate)
                object->flags |= kObjectPoseStale;
            if (data->emitting && ee::Float(0.0f) < data->params.rate)
            {
                const ee::Float seconds = zge::frameSeconds();
                const ee::Ptr<ParticleSettings> settings = data->settings;
                data->owed = data->owed + data->params.rate * seconds;
                startEmitterEffect(object, settings->type->nameHash, static_cast<uint32_t>(ee::toInt(data->owed)) & 0xFFu);
                data->owed = data->owed - ee::toFloat(ee::toInt(data->owed));
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateParticleEmitterObject, Verified);

    // destroyParticleEmitterObject (the class's destroy): each effect ended by
    // its type, the bindings freed, the settings released by their type, the
    // update task deleted and the data freed. ZGE4 takes the object off its
    // list first, frees the bindings only when the array's last reference
    // went with it, leaves the emitter list to the scene (leaveScene) and
    // gives the data back to its pool. 1.
    uint32_t destroyParticleEmitterObject(ee::Ptr<SceneObject> object)
    {
        uint32_t effect = walkFirst(*emitterData(object)->effects);
        uint32_t binding = walkFirst(*emitterData(object)->bindings);
        const ee::Ptr<ParticleSettings> settings = emitterData(object)->settings;
        if constexpr (game::readable::kIsBmx)
            pointerArrayRemove(*g_particleEmitterObjects, object);
        while (effect)
        {
            const ee::Ptr<ParticleType> type = ee::Ptr<ParticleEffect>(effect)->type;
            type->endEffect(type, ee::Ptr<ParticleEffect>(effect));
            walkNext(*emitterData(object)->effects, effect);
        }
        freePointerArray(emitterData(object)->effects);
        bool bindingsHeld = false;
        if constexpr (game::readable::kIsBmx)
            bindingsHeld = releaseArrayReference(emitterData(object)->bindings) != 0;
        if (!bindingsHeld)
        {
            while (binding)
            {
                heapFree(binding);
                walkNext(*emitterData(object)->bindings, binding);
            }
            freePointerArray(emitterData(object)->bindings);
        }
        if constexpr (!game::readable::kIsBmx)
            clearEmitterList();
        const ee::Ptr<ParticleType> type = settings->type;
        type->releaseSettings(type, settings);
        deleteTask(emitterData(object)->updateTask);
        if (object->animator)
        {
            if constexpr (game::readable::kIsBmx)
                poolGiveDestroying(&*g_particleEmitterDataPool, object->animator);
            else
                heapFree(object->animator);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(destroyParticleEmitterObject, Draft);

    // particleEmitterObjectId (the class's id): its name's hash.
    uint32_t particleEmitterObjectId(ee::Ptr<SceneObject> object)
    {
        return emitterData(object)->nameHash;
    }
    READABLE_FUNCTION(particleEmitterObjectId, Verified);

    // registerParticleEmitterChunkHandlers: the ZPEC, ZPEO and ZPED chunks'
    // readers, and the WSM chunks'. ZGE4 also turns the particles on, makes
    // the emitter objects' list before the WSM chunks and the task that
    // updates them after. 1.
    uint32_t registerParticleEmitterChunkHandlers()
    {
        *g_particleEmitterObjectsReady = 1;
        if constexpr (game::readable::kIsBmx)
            *g_particlesOff = 0;
        addChunkHandler(chunkTag("ZPEC"), sym::readZpecChunk);
        addChunkHandler(chunkTag("ZPEO"), sym::readZpeoChunk);
        addChunkHandler(chunkTag("ZPED"), sym::readZpedChunk);
        if constexpr (game::readable::kIsBmx)
            *g_particleEmitterObjects = newCursorArray(kEmitterObjectsCapacity);
        registerWsmChunkHandlers();
        if constexpr (game::readable::kIsBmx)
            *g_particleEmitterObjectsTask = newTask(kObjectsTaskPriority, sym::bmx::updateParticleEmitterObjects, 0, kUpdateTaskFlags,
                                                    sym::g_textNgengineZge3ZgeModsZobjParticleEmi, kObjectsTaskLine);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerParticleEmitterChunkHandlers, Verified);

    // shutdownParticleEmitterObjects: the emitter list freed, once set up
    // (ZGE4: its objects task deleted, when made, and the objects' list freed
    // first). 1.
    uint32_t shutdownParticleEmitterObjects()
    {
        if (*g_particleEmitterObjectsReady != 0)
        {
            *g_particleEmitterObjectsReady = 0;
            if constexpr (game::readable::kIsBmx)
            {
                if (*g_particleEmitterObjectsTask)
                    deleteTask(*g_particleEmitterObjectsTask);
                freePointerArray(*g_particleEmitterObjects);
            }
            freeEmitterList();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownParticleEmitterObjects, Verified);

    // updateEmitterEffects (the object's task): each effect drawn. 1.
    uint32_t updateEmitterEffects(ee::Ptr<SceneObject> object)
    {
        uint32_t effect = walkFirst(*emitterData(object)->effects);
        while (effect)
        {
            drawParticleEffect(ee::Ptr<ParticleEffect>(effect));
            walkNext(*emitterData(object)->effects, effect);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateEmitterEffects, Verified);

    // newParticleEmitterSceneObject: a particle emitter object, its data from
    // the heap with no effects yet, hanging from nothing, emitting, with its
    // update task and its two messages. ZGE4's data comes from the data pool
    // and knows its object; it starts a burst when the data's burst time is
    // over 0 (the pool's record as it comes: readZpedChunk sets the time
    // after). 0 when no object was free.
    ee::Ptr<SceneObject> newParticleEmitterSceneObject()
    {
        ee::Ptr<SceneObject> object;
        if constexpr (game::readable::kIsBmx)
        {
            object = newObjectOfKind(kParticleEmitterKind);
            if (!object)
                return object;
        }
        else
        {
            object = ee::Ptr<SceneObject>(zge::newSceneObject());
            if (!object)
                return object;
            zge::initSceneObject(object, kParticleEmitterKind);
        }
        object->objectClass = ee::Ptr<SceneObjectClass>(sym::g_particleEmitterSceneObjectClass);
        if constexpr (game::readable::kIsBmx)
            object->animator = ee::Ptr<Animator>(poolTakeCreating(&*g_particleEmitterDataPool, kEmitterDataBytes));
        else
            object->animator = ee::Ptr<Animator>(heapAllocate(kEmitterDataBytes));
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        data->effects = newCursorArray(kEffectsCapacity);
        data->bindings = newCursorArray(kBindingsCapacity);
        const ee::Ptr<Task> task = newTask(kUpdateTaskPriority, sym::updateEmitterEffects, object, kUpdateTaskFlags, sym::g_textNgengineZge3ZgeModsZobjParticleEmi, kUpdateTaskLine);
        data->owed = ee::Float(0.0f);
        data->parentHash = 0;
        if constexpr (game::readable::kIsBmx)
            emitterObject(*data) = object;
        data->bone = 0;
        data->parent = ee::Ptr<SceneObject>();
        data->updateTask = task;
        data->emitting = 1;
        subscribe(object, kEmitterStartMessage, sym::emitterStartMessage);
        subscribe(object, kEmitterRateMessage, sym::emitterRateMessage);
        if constexpr (game::readable::kIsBmx)
        {
            const ee::Float burst = burstSeconds(*data);
            emitterBurst(*data).burstLeft = burst;
            emitterBurst(*data).bursting = ee::Float(0.0f) < burst ? 1 : 0;
        }
        return object;
    }
    READABLE_FUNCTION_WAITS(newParticleEmitterSceneObject, Verified);

    // emitterPosition (no caller in this build): where the object emits: its
    // parent bone's point when it hangs from one, else its own position.
    // Returns out.
    ee::Ptr<Vec4> emitterPosition(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> out)
    {
        ee::Local<Vec4> origin;
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if (data->parent && data->bone)
        {
            memset(origin, 0, sizeof(Vec4));
            origin->w = ee::Float(1.0f);
            objectBonePoint(data->parent, data->bone, origin, out, 0);
            return out;
        }
        aligned(out) = quadwordOf(objectPosition(object));
        return out;
    }
    READABLE_FUNCTION(emitterPosition, Draft);

    // emitterMatrix (no caller in this build): how the object emits: its
    // parent bone's matrix when it hangs from one, else its orientation as a
    // matrix. Returns the matrix.
    ee::Ptr<Matrix> emitterMatrix(ee::Ptr<SceneObject> object, ee::Ptr<Matrix> matrix)
    {
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if (data->parent && data->bone)
        {
            objectBoneMatrix(data->parent, data->bone, matrix, 0);
            return matrix;
        }
        quaternionToMatrix(matrix.get(), objectOrientation(object).get());
        return matrix;
    }
    READABLE_FUNCTION(emitterMatrix, Draft);

    // emitterStartMessage (kEmitterStartMessage's handler): the particles the
    // message asks for spawned now; the object updated next frame. ZGE4 also
    // starts the burst: emitting while the rate is not 0, for burstSeconds.
    // 0.
    int32_t emitterStartMessage(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> object(message->target);
        const uint8_t count = *ee::Ptr<uint8_t>(message->data);
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        startEmitterEffect(object, data->settings->type->nameHash, count);
        if constexpr (game::readable::kIsBmx)
        {
            setEmittingByRate(data, emitterBurst(*data).rate);
            emitterBurst(*data).bursting = 1;
            emitterBurst(*data).burstLeft = burstSeconds(*data);
            markPoseStale(object);
        }
        else
        {
            object->flags |= kObjectPoseStale;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(emitterStartMessage, Draft);

    // emitterRateMessage (kEmitterRateMessage's handler): the object's rate
    // set (a rate of 0 stops it spawning by itself); updated next frame. ZGE4
    // keeps the live rate apart from the chunk's params and ends any burst.
    // 0.
    int32_t emitterRateMessage(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<SceneObject> object(message->target);
        const ee::Float rate = *ee::Ptr<ee::Float>(message->data);
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if constexpr (game::readable::kIsBmx)
        {
            emitterBurst(*data).rate = rate;
            setEmittingByRate(data, rate);
            emitterBurst(*data).bursting = 0;
            markPoseStale(object);
        }
        else
        {
            data->params.rate = rate;
            setEmittingByRate(data, rate);
            object->flags |= kObjectPoseStale;
        }
        return 0;
    }
    READABLE_FUNCTION(emitterRateMessage, Draft);

    // addEmitterEffect: the definition bound to the object with its behaviour
    // (the one of the definition's kind written for the object's particle
    // type). 1, or 0 when the type or the behaviour is not found.
    uint32_t addEmitterEffect(ee::Ptr<ParticleEmitterData> data, ee::Ptr<EmitterDefinition> definition)
    {
        ee::Ptr<ParticleBehaviour> behaviour;
        const ee::Ptr<ParticleType> type = findParticleType(data->settings->type->nameHash);
        if (type)
            behaviour = findParticleBehaviour(behaviourKindHash(definition->behaviourKind), type->typeIndex);
        if (!behaviour || !type)
            return 0;
        const uint32_t block = heapAllocate(sizeof(EmitterBinding));
        const ee::Ptr<EmitterBinding> binding(block);
        binding->definition = definition;
        binding->behaviour = behaviour;
        pointerArrayPushBack(data->bindings, block);
        return 1;
    }
    READABLE_FUNCTION_WAITS(addEmitterEffect, Draft);

    // startEmitterEffect: `count` particles of the type spawned in the object,
    // through its effect of that type, made by the type first when it has
    // none (the original adds the new effect and sets its object before it
    // checks that the type made one).
    void startEmitterEffect(ee::Ptr<SceneObject> object, uint32_t typeHash, uint32_t count)
    {
        count &= 0xFFu;
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        const ee::Ptr<ParticleType> type = findParticleType(typeHash);
        if (!type)
            return;
        uint32_t effect = walkFirst(*data->effects);
        while (effect && ee::Ptr<ParticleEffect>(effect)->type != type)
            walkNext(*data->effects, effect);
        if (!effect)
        {
            effect = type->start(type, kStartWord);
            ee::Ptr<ParticleEffect>(effect)->object = object;
            pointerArrayAdd(data->effects, effect);
            if (!effect)
                return;
        }
        spawnParticleEffect(ee::Ptr<ParticleEffect>(effect), object, count, data);
    }
    READABLE_FUNCTION_WAITS(startEmitterEffect, Draft);

    // readZpecChunk: the ZPEC chunk, a container: its children read
    // (loadObjectChunks' answer).
    int32_t readZpecChunk(ChunkReader *reader, uint32_t context)
    {
        return loadObjectChunks(reader, context);
    }
    READABLE_FUNCTION_WAITS(readZpecChunk, Draft);

    // readZpeoChunk: a ZPEO chunk: a particle emitter object, registered as
    // a loaded asset (ZGE4: and put on the emitter objects' list), its
    // children (ZPED) read with setEmitterMaterialTextures as the material
    // callback. 1, or 0 when no object was made.
    uint32_t readZpeoChunk(ChunkReader *reader)
    {
        const ee::Ptr<SceneObject> object = newParticleEmitterSceneObject();
        if (!object)
            return 0;
        calls::registerLoadedAsset(object);
        if constexpr (game::readable::kIsBmx)
            pointerArrayPushBack(*g_particleEmitterObjects, object);
        const uint32_t previous = setMaterialCallback(sym::setEmitterMaterialTextures);
        if (enterChunk(reader))
        {
            parseChunks(reader, object);
            leaveChunk(reader);
        }
        setMaterialCallback(previous);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZpeoChunk, Draft);

    // setEmitterMaterialTextures (the material callback while a ZPEO chunk
    // is read): the particle type's set-up of the material, then its two
    // texture slots filled from the scene's textures by their hashes (an
    // empty first slot gets no texture, an empty second is left).
    uint32_t setEmitterMaterialTextures(uint32_t material, ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ParticleSettings> settings = emitterData(object)->settings;
        const ee::Ptr<ParticleType> type = settings->type;
        if (type->setMaterialTextures)
            type->setMaterialTextures(type, settings, material);
        for (uint32_t slot = 0; slot < game::readable::perGame(2u, 2u, 1u); ++slot) // (ZGE4 fills the first slot only)
        {
            const uint32_t hash = materialTextureSlot(ee::Ptr<Material>(material), slot);
            if (hash)
                addMaterialTexture(ee::Ptr<Material>(material), zglSceneFindTextureFromHash(hash));
            else if (slot == 0)
                addMaterialTexture(ee::Ptr<Material>(material), 0);
        }
        return 1;
    }
    READABLE_FUNCTION(setEmitterMaterialTextures, Draft);

    // setupEmitterEffects (readZpedChunk): each emitter definition the chunk
    // names, last first, bound to the object. 1.
    uint32_t setupEmitterEffects(ee::Ptr<ParticleEmitterData> data, ee::Ptr<ZpedRecord> chunk)
    {
        for (int32_t i = chunk->definitionCount - 1; i != -1; --i)
            if (const ee::Ptr<EmitterDefinition> definition = findEmitterDefinition(chunk->definitions[i]))
                addEmitterEffect(data, definition);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupEmitterEffects, Draft);

    // readZpedChunk: a ZPED chunk, the settings of the ZPEO object it is read
    // for: copied into the object's data, then its particle type found by
    // name and given the record to read what it keeps of it (the type's
    // settings), the object placed and turned, and the emitter definitions it
    // names bound (setupEmitterEffects's 1). 0 when no type of that name is
    // registered: the object then has no settings and stays where it was,
    // with no definitions bound. ZGE4 copies the record's five billboard words
    // and burst time too, writes the placement into the data's matrix (its
    // orientation as the rotation, the position its last row) and marks the
    // object stale instead of placing it, starts the live rate from the
    // params' and reads what the chunk holds after the record into the data.
    uint32_t readZpedChunk(ChunkReader *reader, ee::Ptr<SceneObject> object)
    {
        ee::Local<ZpedRecord> record;
        ee::Local<Vec4> position;
        ee::Local<Quaternion> orientation;
        readChunk(reader, record, sizeof(ZpedRecord));
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        data->nameHash = record->nameHash;
        data->parentHash = record->parentHash;
        data->boneHash = record->boneHash;
        data->spawnShape = record->spawnShape;
        if (record->spawnShape == kSpawnInSphere)
            data->spawnSize.x = record->spawnSize.x;
        else if (record->spawnShape == kSpawnInBox)
            data->spawnSize = Vec4{record->spawnSize.x, record->spawnSize.y, record->spawnSize.z, ee::Float(1.0f)};
        data->params = record->params;
        if constexpr (game::readable::kIsBmx)
        {
            billboardWords(*data) = billboardWords(*record);
            burstSeconds(*data) = burstSeconds(*record);
        }
        data->unreadKind = record->unreadKind;
        if (record->unreadKind == 0 || record->unreadKind == 1)
            data->unreadValue = record->unreadValue;
        data->spinPeriod = record->spinPeriod;
        data->spinPeriodSpread = record->spinPeriodSpread;
        data->startAngle = record->startAngle;
        data->startAngleSpread = record->startAngleSpread;
        data->spinAxisKind = record->spinAxisKind;
        if (record->spinAxisKind == kSpinAxisNotSet)
            data->spinAxis.x = record->spinAxis.x;
        else if (record->spinAxisKind == kSpinAboutGivenAxis)
            data->spinAxis = Vec4{record->spinAxis.x, record->spinAxis.y, record->spinAxis.z, ee::Float(1.0f)};
        data->spinAxisSpread = record->spinAxisSpread;
        data->unreadVector = record->unreadVector;
        const ee::Ptr<ParticleType> type = findParticleType(record->typeHash);
        if (!type)
        {
            data->settings = ee::Ptr<ParticleSettings>();
            return 0;
        }
        data->settings = type->readSettings(type, record);
        *position = Vec4{record->position.x, record->position.y, record->position.z, ee::Float(1.0f)};
        *orientation = Quaternion{record->orientation[0], record->orientation[1], record->orientation[2], record->orientation[3]};
        if constexpr (game::readable::kIsBmx)
        {
            quaternionToMatrix(&emitterMatrixIn(*data), orientation);
            emitterMatrixIn(*data).translation() = *position;
            emitterBurst(*data).rate = data->params.rate;
            markPoseStale(object);
            EmitterTrailingChunk &trailing = trailingChunk(*data);
            trailing.at = &trailing.data;
            const uint32_t bytes = chunkDataSize(reader) - recordBytes(*record);
            if (bytes != 0)
            {
                readChunk(reader, trailing.at, bytes);
                trailing.bytes = bytes;
            }
        }
        else
        {
            setObjectPosition(object, position);
            setObjectOrientation(object, orientation);
        }
        return setupEmitterEffects(data, record);
    }
    READABLE_FUNCTION_WAITS(readZpedChunk, Draft);
}
