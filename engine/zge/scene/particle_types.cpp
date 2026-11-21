// The particle types and their behaviours (zge/scene/particle_types.h): the
// two registries (g_particleTypes, g_particleBehaviours: initEffectArrays
// makes them), a behaviour bound to a type, a type's effect spawned, updated
// and drawn, and where and how fast an emitter object's particles start.

#include "zge/scene/particle_types.h"

#include "zge/container/array.h"
#include "zge/container/list.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/container/pointer_array.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/particle_types.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<int32_t, sym::g_effectArraysReady> g_effectArraysReady;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_particleTypes> g_particleTypes;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_particleBehaviours> g_particleBehaviours;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_emitterDefinitions> g_emitterDefinitions;
        // The behaviour kinds' names and their hashes, made on the first ask.
        inline ee::Global<int32_t, sym::g_behaviourKindHashesPending> g_behaviourKindHashesPending;
        inline ee::Global<ee::Ptr<const char>[kParticleBehaviourKinds], sym::g_behaviourKindNames> g_behaviourKindNames;
        inline ee::Global<uint32_t[kParticleBehaviourKinds], sym::g_behaviourKindHashes> g_behaviourKindHashes;

        void randomPointInSphere(ee::Ptr<BoundingSphere> sphere, ee::Ptr<Vec4> out) { ee::game(sym::randomPointInSphere, sphere, out); }
        void randomPointInBox(ee::Ptr<Box> box, ee::Ptr<Vec4> out) { ee::game(sym::randomPointInBox, box, out); }
        namespace tail
        {
            void pointerArrayAdd(ee::Ptr<PointerArray> array, ee::Address item) { ee::tailCall<void>(sym::pointerArrayAdd, array, item); }
        }

        // A random angle in (-spread, spread] taken off around + spread, as
        // a halfword: the yaw and the pitch.
        Angle spreadAngle(Angle around, int16_t spread)
        {
            int32_t angle = static_cast<int16_t>(static_cast<uint16_t>(around) + static_cast<uint16_t>(spread));
            if (spread != 0)
            {
                const int16_t random = static_cast<int16_t>(randomNumber() % 0xFFFFu);
                angle = static_cast<int16_t>(angle - random % (spread * 2));
            }
            return static_cast<Angle>(angle);
        }
    }

    // ---- the types -----------------------------------------------------------

    // findParticleType: the type with that name's hash, 0 if none.
    ee::Ptr<ParticleType> findParticleType(uint32_t nameHash)
    {
        PointerArray &types = **g_particleTypes;
        uint32_t item = walkFirst(types);
        while (item && ee::Ptr<ParticleType>(item)->nameHash != nameHash)
            walkNext(types, item);
        return ee::Ptr<ParticleType>(item);
    }
    READABLE_FUNCTION(findParticleType, Draft);

    // registerParticleType: a copy of the definition registered as a type.
    void registerParticleType(ee::Ptr<ParticleType> definition)
    {
        const ee::Ptr<ParticleType> type(heapAllocate(sizeof(ParticleType)));
        memoryCopy(type, definition, sizeof(ParticleType));
        if (type->registryHook)
            type->registryHook(type);
        tail::pointerArrayAdd(*g_particleTypes, type);
    }
    READABLE_FUNCTION_WAITS(registerParticleType, Draft);

    // unregisterParticleType: the type with that name's hash taken out and
    // freed; nothing if there is none.
    void unregisterParticleType(uint32_t nameHash)
    {
        PointerArray &types = **g_particleTypes;
        for (uint32_t item = walkFirst(types); item; walkNext(types, item))
        {
            const ee::Ptr<ParticleType> type(item);
            if (type->nameHash == nameHash)
            {
                pointerArrayRemoveCurrent(*g_particleTypes);
                if (type->registryHook)
                    type->registryHook(type);
                heapFree(type);
                return;
            }
        }
    }
    READABLE_FUNCTION_WAITS(unregisterParticleType, Draft);

    // ---- the behaviours --------------------------------------------------------

    // findParticleBehaviour: the behaviour of that kind written for that
    // type, 0 if none.
    ee::Ptr<ParticleBehaviour> findParticleBehaviour(uint32_t kindHash, uint16_t typeIndex)
    {
        PointerArray &behaviours = **g_particleBehaviours;
        for (uint32_t item = walkFirst(behaviours); item; walkNext(behaviours, item))
        {
            const ee::Ptr<ParticleBehaviour> behaviour(item);
            if (behaviour->kindHash == kindHash && behaviour->typeIndex == typeIndex)
                return behaviour;
        }
        return ee::Ptr<ParticleBehaviour>();
    }
    READABLE_FUNCTION(findParticleBehaviour, Draft);

    // registerParticleBehaviour: a copy of the definition registered as a
    // behaviour.
    void registerParticleBehaviour(ee::Ptr<ParticleBehaviour> definition)
    {
        const ee::Ptr<ParticleBehaviour> behaviour(heapAllocate(sizeof(ParticleBehaviour)));
        memoryCopy(behaviour, definition, sizeof(ParticleBehaviour));
        if (behaviour->registered)
            behaviour->registered(behaviour);
        tail::pointerArrayAdd(*g_particleBehaviours, behaviour);
    }
    READABLE_FUNCTION_WAITS(registerParticleBehaviour, Draft);

    // unregisterParticleBehaviour: the behaviour of that kind for that type
    // taken out and freed; nothing if there is none.
    void unregisterParticleBehaviour(uint32_t kindHash, uint16_t typeIndex)
    {
        PointerArray &behaviours = **g_particleBehaviours;
        for (uint32_t item = walkFirst(behaviours); item; walkNext(behaviours, item))
        {
            const ee::Ptr<ParticleBehaviour> behaviour(item);
            if (behaviour->kindHash == kindHash && behaviour->typeIndex == typeIndex)
            {
                pointerArrayRemoveCurrent(*g_particleBehaviours);
                if (behaviour->released)
                    behaviour->released(behaviour);
                heapFree(behaviour);
                return;
            }
        }
    }
    READABLE_FUNCTION_WAITS(unregisterParticleBehaviour, Draft);

    // shutdownParticleTypes: every type and every behaviour unregistered and
    // both registries freed (ShutdownInlineFx's). 1.
    uint32_t shutdownParticleTypes()
    {
        if (*g_effectArraysReady == 0)
            return 1;
        if constexpr (game::readable::kIsBmx) // (ZGE4 takes the three kinds' pools down here, in the kinds' order)
        {
            unregisterSparkParticles();
            unregisterBillboardParticles();
            unregisterPolyParticles();
        }
        while (const uint32_t item = walkFirst(**g_particleTypes))
            unregisterParticleType(ee::Ptr<ParticleType>(item)->nameHash);
        freePointerArray(*g_particleTypes);
        while (const uint32_t item = walkFirst(**g_particleBehaviours))
        {
            const ee::Ptr<ParticleBehaviour> behaviour(item);
            unregisterParticleBehaviour(behaviour->kindHash, behaviour->typeIndex);
        }
        freePointerArray(*g_particleBehaviours);
        *g_effectArraysReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownParticleTypes, Verified);

    // behaviourKindHash: the hash of a behaviour kind's name
    // (ParticleBehaviourKind), the three hashed on the first ask.
    uint32_t behaviourKindHash(uint32_t kind)
    {
        if (*g_behaviourKindHashesPending != 0)
        {
            *g_behaviourKindHashesPending = 0;
            for (uint32_t i = 0; i < kParticleBehaviourKinds; i++)
                (*g_behaviourKindHashes)[i] = nameHash((*g_behaviourKindNames)[i]);
        }
        return (*g_behaviourKindHashes)[kind];
    }
    READABLE_FUNCTION(behaviourKindHash, Draft);

    // bindParticleBehaviour: the behaviour of that kind for the type put on
    // the type's list with data. 1, or 0 when the type or the behaviour is
    // not found.
    uint32_t bindParticleBehaviour(uint32_t typeHash, uint32_t kindHash, uint32_t data)
    {
        ee::Ptr<ParticleBehaviour> behaviour;
        const ee::Ptr<ParticleType> type = findParticleType(typeHash);
        if (type)
            behaviour = findParticleBehaviour(kindHash, type->typeIndex);
        if (!behaviour || !type)
            return 0;
        const ee::Ptr<BehaviourLink> link(heapAllocate(sizeof(BehaviourLink)));
        link->data = data;
        link->behaviour = behaviour;
        listPush(&type->behaviours, link);
        return 1;
    }
    READABLE_FUNCTION_WAITS(bindParticleBehaviour, Draft);

    // unbindParticleBehaviour: the type's first binding of that behaviour
    // taken off its list and freed. 1, or 0 when there is none.
    uint32_t unbindParticleBehaviour(uint32_t typeHash, uint32_t kindHash)
    {
        ee::Ptr<ParticleBehaviour> behaviour;
        const ee::Ptr<ParticleType> type = findParticleType(typeHash);
        if (type)
            behaviour = findParticleBehaviour(kindHash, type->typeIndex);
        if (!behaviour || !type)
            return 0;
        for (ee::Ptr<BehaviourLink> link = recordOfNode<BehaviourLink>(type->behaviours.head); link;
             link = recordOfNode<BehaviourLink>(link->node.next))
        {
            if (link->behaviour == behaviour)
            {
                listRemove(&type->behaviours, &link->node);
                heapFree(link);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(unbindParticleBehaviour, Draft);

    // ---- an effect through its type -----------------------------------------------

    // spawnParticleEffect: count particles of the effect's type spawned in the
    // object. Returns what the spawn did.
    uint32_t spawnParticleEffect(ee::Ptr<ParticleEffect> effect, ee::Ptr<SceneObject> object, uint8_t count, ee::Address data)
    {
        const ee::Ptr<ParticleType> type = effect->type;
        return type->spawn(type, object, effect, count, data);
    }
    READABLE_FUNCTION(spawnParticleEffect, Draft);

    // updateParticleEffect: the effect's particles moved by its type. Returns
    // the particles still alive.
    uint32_t updateParticleEffect(ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Address data)
    {
        const ee::Ptr<ParticleType> type = effect->type;
        return type->update(type, effect, word, data);
    }
    READABLE_FUNCTION(updateParticleEffect, Draft);

    // drawParticleEffect: the effect drawn by its type.
    uint32_t drawParticleEffect(ee::Ptr<ParticleEffect> effect)
    {
        const ee::Ptr<ParticleType> type = effect->type;
        return type->draw(type, effect);
    }
    READABLE_FUNCTION(drawParticleEffect, Draft);

    // ---- where and how fast a particle starts -------------------------------------

    // particleSpawnPoint: where an emitter object's particle starts, around
    // the point by the object's spawn shape (ParticleEmitterData): at
    // random in the sphere or the box, or the point itself (no data, or a
    // sphere of no radius). Another shape leaves out as it was. Returns out.
    ee::Ptr<Vec4> particleSpawnPoint(ee::Ptr<Vec4> out, ee::Ptr<Vec4> point, ee::Ptr<SceneObject> object)
    {
        ee::Local<BoundingSphere> sphere; // (the frame's bottom; the box above it, as the original keeps them)
        ee::Local<Box> box;
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        if (data)
        {
            if (data->spawnShape == kSpawnInBox)
            {
                const Vec4 &centre = *point;
                const ee::Float halfZ = data->spawnSize.z * ee::Float(0.5f);
                const ee::Float halfY = data->spawnSize.y * ee::Float(0.5f);
                const ee::Float halfX = data->spawnSize.x * ee::Float(0.5f);
                box->high.z = centre.z + halfZ;
                box->high.y = centre.y + halfY;
                box->high.x = centre.x + halfX;
                box->low.x = centre.x - halfX;
                box->low.y = centre.y - halfY;
                box->low.z = centre.z - halfZ;
                randomPointInBox(&*box, out);
                return out;
            }
            if (data->spawnShape != kSpawnInSphere)
                return out;
            const ee::Float radius = data->spawnSize.x;
            if (ee::Float(0.0f) < radius)
            {
                sphere->radius = radius;
                sphere->centre = quadwordOf(point);
                randomPointInSphere(&*sphere, out);
                return out;
            }
        }
        aligned(out) = quadwordOf(point);
        return out;
    }
    READABLE_FUNCTION(particleSpawnPoint, Draft);

    // particleSpawnVelocity: how fast an emitter object's particle starts: its
    // speed up (y is down), turned by the object's yaw about Z and then its
    // pitch about Y, each with its random spread, then into the node's frame
    // by its matrix. Returns out.
    ee::Ptr<Vec4> particleSpawnVelocity(ee::Ptr<Vec4> out, ee::Ptr<Matrix> nodeMatrix, ee::Ptr<SceneObject> object)
    {
        const ee::Ptr<ParticleEmitterData> data = emitterData(object);
        out->x = ee::Float(0.0f);
        out->z = ee::Float(0.0f);
        const ee::Float speed = data->params.speed;
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 varies the speed by its spread: speed * (1 - speedSpread * u), u at random in [-1, 1).
            constexpr ee::Float kUnit = 1.0f / 2147483648.0f; // 2^-31: a 31-bit random number to [0, 1)
            const ee::Float unit = ee::toFloat(static_cast<int32_t>(randomNumber() & 0x7FFFFFFFu)) * kUnit;
            const ee::Float u = (unit + unit) - ee::Float(1.0f);
            out->w = ee::Float(1.0f);
            out->y = (data->params.speedSpread * u + ee::Float(-1.0f)) * speed;
        }
        else
        {
            out->w = ee::Float(1.0f);
            out->y = -speed;
        }
        ee::Local<Matrix> turn;
        const Angle yaw = spreadAngle(data->params.yaw, data->params.yawSpread);
        matrixIdentity(&*turn);
        matrixRotateZ(&*turn, &*turn, yaw);
        const Angle pitch = spreadAngle(data->params.pitch, data->params.pitchSpread);
        matrixRotateY(&*turn, &*turn, pitch);
        if constexpr (game::readable::kIsBmx) // (ZGE4 turns the direction by the rotations alone, no translation)
        {
            aligned(out) = rotated(*turn, quadwordOf(out));
            aligned(out) = rotated(*nodeMatrix, quadwordOf(out));
        }
        else
        {
            aligned(out) = transformed(*turn, quadwordOf(out));
            aligned(out) = transformed(*nodeMatrix, quadwordOf(out));
        }
        return out;
    }
    READABLE_FUNCTION(particleSpawnVelocity, Draft);

    // findEmitterDefinition: the emitter definition with that name's hash, 0
    // if none.
    ee::Ptr<EmitterDefinition> findEmitterDefinition(uint32_t nameHash)
    {
        const int32_t capacity = cursorArrayCapacity(*g_emitterDefinitions);
        for (int32_t index = 0; index < capacity; index++)
        {
            const ee::Ptr<EmitterDefinition> definition(pointerArrayAt(*g_emitterDefinitions, index));
            if (definition->nameHash == nameHash)
                return definition;
        }
        return ee::Ptr<EmitterDefinition>();
    }
    READABLE_FUNCTION(findEmitterDefinition, Draft);
}
