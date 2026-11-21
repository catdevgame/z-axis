// The engine's side of the effect classes (Zfx, compiled without optimisation):
// the named classes' registry, an emitter's move and draw through its class, the
// methods every class takes from the engine (its affectors, deleteEmitter, and
// the default make and move of an emitter), and the engine's two affectors,
// gravity and the floor. zge/scene/effects.h has the records.

#include "zge/scene/effects.h"

#include "zge/container/array.h"
#include "zge/container/list.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "zge/collision/collision.h"
#include "zge/render/fx_manager.h"
#include "zge/scene/particle_types.h"
#include "zge/container/cursor_array.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/effect_classes.cpp.h"

namespace zge
{
    // The pointer arrays' functions this file calls (zge/container/array.cpp).
    uint32_t current(ee::Ptr<PointerArray> array);
    void freePointerArray(ee::Ptr<PointerArray> array);

    namespace
    {
        // An effect class kept under its name (registerEffectClassName's
        // record, on g_effectClassNames).
        struct EffectClassName
        {
            ee::Ptr<EffectClass> effectClass; // +0x00
            char name[0x40];                  // +0x04
            uint32_t nameHash;                // +0x44
        };
        static_assert(offsetof(EffectClassName, nameHash) == 0x44 && sizeof(EffectClassName) == 0x48);

        // An affector's name (registerEffectTypeName's record, on
        // g_affectorNames).
        struct AffectorName
        {
            ee::Ptr<Affector> affector; // +0x00
            char name[0x40];            // +0x04
            uint32_t nameHash;          // +0x44
        };
        static_assert(offsetof(AffectorName, nameHash) == 0x44 && sizeof(AffectorName) == 0x48);

        // The floor affector's argument: the floor's height and how much of a
        // particle's fall it bounces back with.
        struct Floor
        {
            ee::Float height; // +0x00 (y grows downwards: below it is larger)
            ee::Float bounce; // +0x04
        };

        inline ee::Global<ee::Ptr<PointerArray>, sym::g_namedEffectClasses> g_namedEffectClasses;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_effectClassNames> g_effectClassNames;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_effectTypeNames> g_affectorNames;
        inline ee::Global<int32_t, sym::g_particlesAlive> g_particlesAlive;
        // The affectors' defaults: gravity (0, 9.81, 0) and the floor (height 0, bounce 0.8).
        constexpr uint32_t kDefaultGravity = sym::g_particleGravity;
        constexpr uint32_t kDefaultFloor = sym::g_particleFloor;

        constexpr uint32_t kEngineParticleBytes = sizeof(EffectParticle);

        uint32_t firstCursorItem(ee::Ptr<PointerArray> array) { return ee::game<uint32_t>(sym::firstCursorItem, array); }
    }

    namespace
    {
        // ZGE2 and ZGE4 keep the registry without ZGE3's helpers: a name record's memory is asked of the heap
        // itself (no effectNameAllocate), the register functions leave through pointerArrayAdd and initEffectClass
        // through initList.
        constexpr bool kRegistryWithoutHelpers = game::readable::kIsDm2 || game::readable::kIsBmx;
        inline uint32_t allocateNameRecord(uint32_t bytes)
        {
            if constexpr (kRegistryWithoutHelpers)
                return heapAllocate(bytes);
            else
                return effectNameAllocate(bytes);
        }
        // A registry's walk: ZGE4 has it in place where ZGE2 and ZGE3 call arrayFirst and arrayNext.
        inline uint32_t firstRecord(ee::Ptr<PointerArray> registry)
        {
            if constexpr (game::readable::kIsBmx)
                return walkFirst(*registry);
            else
                return arrayFirst(registry);
        }
        inline uint32_t nextRecord(ee::Ptr<PointerArray> registry)
        {
            if constexpr (game::readable::kIsBmx)
            {
                uint32_t record;
                walkNext(*registry, record);
                return record;
            }
            else
                return arrayNext(registry);
        }
    }

    // ---- the named classes -------------------------------------------------

    // registerNamedEffectClass: a class's spawn function kept under its name
    // (initInlineFx: the user effects, the skater's trails), for
    // spawnNamedEffect.
    void registerNamedEffectClass(uint32_t spawn, uint32_t name)
    {
        const ee::Ptr<NamedEffectClass> named(allocateNameRecord(sizeof(NamedEffectClass)));
        named->spawn = spawn;
        stringCopy(named->name, name);
        named->nameHash = nameHash(named->name);
        if constexpr (kRegistryWithoutHelpers)
            game::tail(zge::pointerArrayAdd, *g_namedEffectClasses, named);
        else
            pointerArrayAdd(*g_namedEffectClasses, named);
    }
    READABLE_FUNCTION_WAITS(registerNamedEffectClass, Verified);

    // unregisterNamedEffectClass: the class of that name taken out and freed.
    // Returns its spawn function, 0 if no class has the name.
    NamedEffectSpawn unregisterNamedEffectClass(uint32_t name)
    {
        for (ee::Ptr<NamedEffectClass> named(arrayFirst(*g_namedEffectClasses)); named;
             named = ee::Ptr<NamedEffectClass>(arrayNext(*g_namedEffectClasses)))
        {
            if (compareStrings(named->name, name) == 0)
            {
                const NamedEffectSpawn spawn = named->spawn;
                heapFree(named);
                pointerArrayRemoveCurrent(*g_namedEffectClasses);
                return spawn;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(unregisterNamedEffectClass, Draft);

    // findAffectorByName: the affector registered under the name
    // (registerEffectTypeName), 0 if none.
    ee::Ptr<Affector> findAffectorByName(uint32_t name)
    {
        for (ee::Ptr<AffectorName> entry(arrayFirst(*g_affectorNames)); entry;
             entry = ee::Ptr<AffectorName>(arrayNext(*g_affectorNames)))
        {
            if (compareStrings(entry->name, name) == 0)
                return entry->affector;
        }
        return ee::Ptr<Affector>();
    }
    READABLE_FUNCTION(findAffectorByName, Verified);

    // ---- the classes and the affectors by name --------------------------------

    // registerEffectClassName: an effect class kept under its name
    // (initInlineFx: aggressive/fx's "spark", "smoke" ...), for
    // findEffectClassByName.
    void registerEffectClassName(ee::Ptr<EffectClass> effectClass, uint32_t name)
    {
        const ee::Ptr<EffectClassName> entry(allocateNameRecord(sizeof(EffectClassName)));
        entry->effectClass = effectClass;
        stringCopy(entry->name, name);
        entry->nameHash = nameHash(entry->name);
        if constexpr (kRegistryWithoutHelpers)
            game::tail(zge::pointerArrayAdd, *g_effectClassNames, entry);
        else
            pointerArrayAdd(*g_effectClassNames, entry);
    }
    READABLE_FUNCTION_WAITS(registerEffectClassName, Verified);

    // unregisterEffectClassName: the class of that name taken off the list
    // and its entry freed (shutdownInlineFx). Returns the class, 0 if no class
    // has the name.
    ee::Ptr<EffectClass> unregisterEffectClassName(uint32_t name)
    {
        for (ee::Ptr<EffectClassName> entry(firstRecord(*g_effectClassNames)); entry;
             entry = ee::Ptr<EffectClassName>(nextRecord(*g_effectClassNames)))
        {
            if (compareStrings(entry->name, name) == 0)
            {
                const ee::Ptr<EffectClass> effectClass = entry->effectClass;
                heapFree(entry);
                pointerArrayRemoveCurrent(*g_effectClassNames);
                return effectClass;
            }
        }
        return ee::Ptr<EffectClass>();
    }
    READABLE_FUNCTION_WAITS(unregisterEffectClassName, Verified);

    // findEffectClassByName: the class registered under the name, 0 if none.
    ee::Ptr<EffectClass> findEffectClassByName(uint32_t name)
    {
        for (ee::Ptr<EffectClassName> entry(firstRecord(*g_effectClassNames)); entry;
             entry = ee::Ptr<EffectClassName>(nextRecord(*g_effectClassNames)))
        {
            if (compareStrings(entry->name, name) == 0)
                return entry->effectClass;
        }
        return ee::Ptr<EffectClass>();
    }
    READABLE_FUNCTION(findEffectClassByName, Verified);

    // registerEffectTypeName: an affector kept under its name (initInlineFx),
    // for findAffectorByName and a class's addAffectorByName.
    void registerEffectTypeName(ee::Ptr<Affector> affector, uint32_t name)
    {
        const ee::Ptr<AffectorName> entry(allocateNameRecord(sizeof(AffectorName)));
        entry->affector = affector;
        stringCopy(entry->name, name);
        entry->nameHash = nameHash(entry->name);
        if constexpr (kRegistryWithoutHelpers)
            game::tail(zge::pointerArrayAdd, *g_affectorNames, entry);
        else
            pointerArrayAdd(*g_affectorNames, entry);
    }
    READABLE_FUNCTION_WAITS(registerEffectTypeName, Verified);

    // unregisterEffectTypeName: the affector of that name taken off the list
    // and its entry freed. Returns the affector, 0 if none has the name.
    // Nothing calls it in AI (freeNamedEffectClasses frees the whole list
    // instead).
    ee::Ptr<Affector> unregisterEffectTypeName(uint32_t name)
    {
        for (ee::Ptr<AffectorName> entry(arrayFirst(*g_affectorNames)); entry;
             entry = ee::Ptr<AffectorName>(arrayNext(*g_affectorNames)))
        {
            if (compareStrings(entry->name, name) == 0)
            {
                const ee::Ptr<Affector> affector = entry->affector;
                heapFree(entry);
                pointerArrayRemoveCurrent(*g_affectorNames);
                return affector;
            }
        }
        return ee::Ptr<Affector>();
    }
    READABLE_FUNCTION_WAITS(unregisterEffectTypeName, Draft);

    // ---- an emitter through its class ---------------------------------------

    // moveEmitter: the emitter's particles moved by its class. Returns what
    // the move did, 0 with no emitter or class.
    uint32_t moveEmitter(ee::Ptr<EffectEmitter> emitter)
    {
        if (emitter && emitter->effectClass)
            return emitter->effectClass->move(emitter->effectClass, emitter);
        return 0;
    }
    READABLE_FUNCTION(moveEmitter, Draft);

    // drawEmitter: the emitter drawn by its class. Returns what the draw did
    // (particles drawn), 0 with no emitter or class.
    uint32_t drawEmitter(ee::Ptr<EffectEmitter> emitter, uint32_t mode)
    {
        if (emitter && emitter->effectClass)
            return emitter->effectClass->draw(emitter->effectClass, emitter, mode);
        return 0;
    }
    READABLE_FUNCTION(drawEmitter, Draft);

    // ---- the methods every class takes from the engine -----------------------

    // initEffectClass: the class's affector list made empty.
    void initEffectClass(ee::Ptr<EffectClass> effectClass)
    {
        if constexpr (kRegistryWithoutHelpers)
            game::tail(zge::initList, &effectClass->affectors);
        else
            initList(&effectClass->affectors);
    }
    READABLE_FUNCTION(initEffectClass, Verified);

    // resetEffectClass (+0x00): the class's affectors cleared.
    void resetEffectClass(ee::Ptr<EffectClass> effectClass)
    {
        effectClass->clearAffectors(effectClass);
    }
    READABLE_FUNCTION(resetEffectClass, Verified);

    // addEffectClassAffector (+0x04): the affector put on the class's list
    // with its argument.
    void addEffectClassAffector(ee::Ptr<EffectClass> effectClass, ee::Ptr<Affector> affector, uint32_t argument)
    {
        // (ZGE2 asks the heap itself and leaves through listPush)
        const ee::Ptr<AffectorLink> link(game::readable::kIsDm2 ? heapAllocate(sizeof(AffectorLink)) : effectAllocate(sizeof(AffectorLink)));
        link->affector = affector;
        link->argument = argument;
        if constexpr (game::readable::kIsDm2)
            game::tail(zge::listPush, &effectClass->affectors, link);
        else
            listPush(&effectClass->affectors, link);
    }
    READABLE_FUNCTION_WAITS(addEffectClassAffector, Verified);

    // addEffectClassAffectorByName (+0x08): the same with the affector
    // registered under the name; nothing if there is none.
    void addEffectClassAffectorByName(ee::Ptr<EffectClass> effectClass, uint32_t name, uint32_t argument)
    {
        if (const ee::Ptr<Affector> affector = findAffectorByName(name))
        {
            if constexpr (game::readable::kIsDm2) // (ZGE2's leaves through it)
                game::tail(zge::addEffectClassAffector, effectClass, affector, argument);
            else
                addEffectClassAffector(effectClass, affector, argument);
        }
    }
    READABLE_FUNCTION_WAITS(addEffectClassAffectorByName, Verified);

    // clearEffectClassAffectors (+0x0C): every affector taken off the class's
    // list and freed. 0.
    int32_t clearEffectClassAffectors(ee::Ptr<EffectClass> effectClass)
    {
        while (const ee::Ptr<ListNode> link = effectClass->affectors.head)
        {
            listRemove(&effectClass->affectors, link);
            heapFree(link);
        }
        return 0;
    }

    // emptyEffectClassAffectors: ZGE2's clearEffectClassAffectors: the same,
    // handing back nothing.
    void emptyEffectClassAffectors(ee::Ptr<EffectClass> effectClass)
    {
        while (const ee::Ptr<ListNode> link = effectClass->affectors.head)
        {
            listRemove(&effectClass->affectors, link);
            heapFree(link);
        }
    }
    READABLE_FUNCTION_WAITS_OWN(clearEffectClassAffectors, Verified, dm2, emptyEffectClassAffectors);

    // setEmitterParticleBytes: the size of the emitter's particles (the
    // capacity and the last argument go unread).
    void setEmitterParticleBytes(ee::Ptr<EffectEmitter> emitter, uint32_t particleBytes, uint32_t capacity, uint32_t unused)
    {
        emitter->particleBytes = particleBytes;
    }
    READABLE_FUNCTION(setEmitterParticleBytes, Draft);

    // newEffectEmitter (a class's +0x10): an emitter of the class with room
    // for capacity particles of the engine's own size, its clock started.
    // 0 when there was no memory for it.
    ee::Ptr<EffectEmitter> newEffectEmitter(ee::Ptr<EffectClass> effectClass, uint16_t capacity)
    {
        const ee::Ptr<EffectEmitter> emitter(effectAllocate(sizeof(EffectEmitter)));
        if (emitter)
        {
            emitter->particles = newCursorArray(capacity);
            setEmitterParticleBytes(emitter, kEngineParticleBytes, capacity, 0);
            emitter->effectClass = effectClass;
            emitter->lastMoved = collisionClock();
        }
        return emitter;
    }

    namespace
    {
        // ZGE2's particle records: a pool of EffectParticles per emitter.
        constexpr uint32_t kZge2ParticleBytes = sizeof(EffectParticle);
        // The emitter's own pool of particle records (ZGE2's alone: no other version's emitter has one).
        template <typename E>
        void setParticlePool([[maybe_unused]] E &emitter, [[maybe_unused]] uint32_t pool)
        {
            if constexpr (requires { emitter.pool; })
                emitter.pool = ee::Ptr<Pool>(pool);
        }
        template <typename E>
        ee::Ptr<Pool> particlePool([[maybe_unused]] const E &emitter)
        {
            if constexpr (requires { emitter.pool; })
                return emitter.pool;
            else
                return {};
        }
    }

    // newEffectEmitterWithPool: ZGE2's newEffectEmitter: the emitter from
    // the heap, with a pool of its own for its particles' records (no
    // particle size kept); its clock started. 0 when there was no memory for it.
    ee::Ptr<EffectEmitter> newEffectEmitterWithPool(ee::Ptr<EffectClass> effectClass, uint16_t capacity)
    {
        const ee::Ptr<EffectEmitter> emitter(heapAllocate(kEffectEmitterBytes));
        if (emitter)
        {
            emitter->particles = newCursorArray(capacity);
            setParticlePool(*emitter, createPool(kZge2ParticleBytes, capacity, 0));
            emitter->effectClass = effectClass;
            emitter->lastMoved = collisionClock();
        }
        return emitter;
    }
    READABLE_FUNCTION_WAITS_OWN(newEffectEmitter, Draft, dm2, newEffectEmitterWithPool);

    // clearEmitterParticleBytes: the emitter's particle size cleared as it goes.
    // Returns the emitter.
    ee::Ptr<EffectEmitter> clearEmitterParticleBytes(ee::Ptr<EffectEmitter> emitter)
    {
        emitter->particleBytes = 0;
        return emitter;
    }
    READABLE_FUNCTION(clearEmitterParticleBytes, Verified);

    // freeParticleRecord: one particle of the emitter's given back to its size
    // class, and one fewer counted alive.
    void freeParticleRecord(ee::Ptr<EffectEmitter> emitter, ee::Address particle)
    {
        *g_particlesAlive = *g_particlesAlive - 1;
        freeSized(particle, emitter->particleBytes);
    }
    READABLE_FUNCTION_WAITS(freeParticleRecord, Verified);

    // deleteEmitter (+0x14): every particle freed, then the array and the
    // emitter.
    void deleteEmitter(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)
    {
        if (emitter->particles)
        {
            for (uint32_t particle = firstCursorItem(emitter->particles); particle; particle = current(emitter->particles))
            {
                pointerArrayRemoveCurrent(emitter->particles);
                freeParticleRecord(emitter, particle);
            }
            freePointerArray(emitter->particles);
        }
        clearEmitterParticleBytes(emitter);
        heapFree(emitter);
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Address block) { ee::tailCall(sym::heapFree, block); }
        }
    }

    // deleteEmitterAndPool: ZGE2's deleteEmitter: the particle array freed
    // and the emitter's pool with its particles' records, then the emitter.
    void deleteEmitterAndPool(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)
    {
        if (emitter->particles)
            freePointerArray(emitter->particles);
        destroyPool(particlePool(*emitter));
        tail::heapFree(emitter);
    }
    READABLE_FUNCTION_WAITS_OWN(deleteEmitter, Verified, dm2, deleteEmitterAndPool);

    // ---- the default move ------------------------------------------------------

    // particleCopyVec: the quadword at source copied to destination.
    void particleCopyVec(ee::Ptr<Vec4> destination, ee::Ptr<Vec4> source)
    {
        aligned(destination) = quadwordOf(source);
    }
    READABLE_FUNCTION(particleCopyVec, Draft);

    // particleAddVecs: out = a + b, all four lanes.
    void particleAddVecs(ee::Ptr<Vec4> out, ee::Ptr<Vec4> a, ee::Ptr<Vec4> b)
    {
        aligned(out) = quadwordOf(a) + quadwordOf(b);
    }
    READABLE_FUNCTION(particleAddVecs, Draft);

    // particleScaleVec: out = v times s (x, y and z; w what the last
    // operation left). Returns out. The original brings s into VU0 through a
    // register whose upper half it does not set: that half lands in a work
    // register's z and w, which nothing reads; here they are 0.
    ee::Ptr<Vec4> particleScaleVec(ee::Ptr<Vec4> out, ee::Ptr<Vec4> v, ee::Float s)
    {
        aligned(out) = quadwordOf(v) * s;
        return out;
    }
    READABLE_FUNCTION(particleScaleVec, Draft);

    // particleListNext: the walk's next particle (array.h's walkNext).
    uint32_t particleListNext(ee::Ptr<PointerArray> particles)
    {
        uint32_t particle;
        walkNext(*particles, particle);
        return particle;
    }
    READABLE_FUNCTION(particleListNext, Draft);

    // The effects' clock's milliseconds since then, as seconds.
    namespace
    {
        ee::Float secondsSince(uint32_t now, uint32_t then)
        {
            const uint32_t elapsed = now - then;
            ee::Float milliseconds;
            if (static_cast<int32_t>(elapsed) >= 0)
                milliseconds = ee::toFloat(static_cast<int32_t>(elapsed));
            else
            {
                const ee::Float half = ee::toFloat(static_cast<int32_t>((elapsed & 1u) | elapsed >> 1));
                milliseconds = half + half;
            }
            return milliseconds * ee::Float(0.001f);
        }
    }

    // moveEmitterParticles (a class's +0x1C, the engine's): each moving
    // particle carried along its velocity for the time since the last move
    // (its tail left where it was) and the emitter's bounds made to hold
    // them, each spinning one's axis turned, each one past its lifetime
    // freed; then every affector of the class applied to the emitter. Returns
    // the particles there were.
    uint32_t moveEmitterParticles(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)
    {
        const uint32_t now = collisionClock();
        const ee::Float seconds = secondsSince(now, emitter->lastMoved);
        uint32_t particles = 0;
        if (emitter && emitter->particles)
        {
            emitter->boundsLow.z = emitter->boundsLow.y = emitter->boundsLow.x = ee::Float(99999.0f);
            emitter->boundsHigh.z = emitter->boundsHigh.y = emitter->boundsHigh.x = ee::Float(-99999.0f);
            ee::Local<Vec4> step;
            ee::Ptr<EffectParticle> particle(firstCursorItem(emitter->particles));
            while (particle)
            {
                particles++;
                if (particle->flags & kParticleMoves)
                {
                    particleCopyVec(&particle->tail, &particle->position);
                    particleScaleVec(&*step, &particle->velocity, seconds);
                    particleAddVecs(&particle->position, &particle->position, &*step);
                    Vec4 &low = emitter->boundsLow, &high = emitter->boundsHigh;
                    const Vec4 &at = particle->position;
                    if (high.x < at.x)
                        high.x = at.x;
                    if (high.y < at.y)
                        high.y = at.y;
                    if (high.z < at.z)
                        high.z = at.z;
                    if (at.x < low.x)
                        low.x = at.x;
                    if (at.y < low.y)
                        low.y = at.y;
                    if (at.z < low.z)
                        low.z = at.z;
                }
                if (particle->flags & kParticleSpins)
                    vec3RotateEuler(&particle->axis.xyz(), &particle->axis.xyz(), &particle->spin);
                if (now - particle->born >= particle->lifetime)
                {
                    pointerArrayRemoveCurrent(emitter->particles);
                    freeParticleRecord(emitter, particle);
                    particle = ee::Ptr<EffectParticle>(current(emitter->particles));
                }
                else
                    particle = ee::Ptr<EffectParticle>(particleListNext(emitter->particles));
            }
        }
        for (ee::Ptr<AffectorLink> link = recordOfNode<AffectorLink>(effectClass->affectors.head); link;
             link = recordOfNode<AffectorLink>(link->node.next))
            link->affector->apply(link->affector, emitter, link->argument);
        emitter->lastMoved = now;
        return particles;
    }

    // movePoolParticles: ZGE2's moveEmitterParticles: the same move, with the
    // vector steps written out, the particles walked with arrayFirst and
    // arrayNext, and one past its lifetime handed back to the emitter's pool.
    uint32_t movePoolParticles(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)
    {
        ee::Local<Vec4> step;
        const uint32_t now = collisionClock();
        const ee::Float seconds = secondsSince(now, emitter->lastMoved);
        uint32_t particles = 0;
        if (emitter && emitter->particles)
        {
            emitter->boundsLow.z = emitter->boundsLow.y = emitter->boundsLow.x = ee::Float(99999.0f);
            emitter->boundsHigh.z = emitter->boundsHigh.y = emitter->boundsHigh.x = ee::Float(-99999.0f);
            ee::Ptr<EffectParticle> particle(arrayFirst(emitter->particles));
            while (particle)
            {
                particles++;
                if (particle->flags & kParticleMoves)
                {
                    particle->tail = particle->position;
                    *step = particle->velocity * seconds;
                    particle->position = particle->position + *step;
                    Vec4 &low = emitter->boundsLow, &high = emitter->boundsHigh;
                    const Vec4 &at = particle->position;
                    if (high.x < at.x)
                        high.x = at.x;
                    if (high.y < at.y)
                        high.y = at.y;
                    if (high.z < at.z)
                        high.z = at.z;
                    if (at.x < low.x)
                        low.x = at.x;
                    if (at.y < low.y)
                        low.y = at.y;
                    if (at.z < low.z)
                        low.z = at.z;
                }
                if (particle->flags & kParticleSpins)
                    vec3RotateEuler(&particle->axis.xyz(), &particle->axis.xyz(), &particle->spin);
                if (now - particle->born >= particle->lifetime)
                {
                    pointerArrayRemoveCurrent(emitter->particles);
                    poolFree(particlePool(*emitter), particle);
                    particle = ee::Ptr<EffectParticle>(current(emitter->particles));
                }
                else
                    particle = ee::Ptr<EffectParticle>(arrayNext(emitter->particles));
            }
        }
        for (ee::Ptr<AffectorLink> link = recordOfNode<AffectorLink>(effectClass->affectors.head); link;
             link = recordOfNode<AffectorLink>(link->node.next))
            link->affector->apply(link->affector, emitter, link->argument);
        emitter->lastMoved = now;
        return particles;
    }
    READABLE_FUNCTION_WAITS_OWN(moveEmitterParticles, Draft, dm2, movePoolParticles);

    // ---- the engine's affectors --------------------------------------------------

    // particleSystemAddVecs: out = a + b, all four lanes (the affectors' copy).
    void particleSystemAddVecs(ee::Ptr<Vec4> out, ee::Ptr<Vec4> a, ee::Ptr<Vec4> b)
    {
        aligned(out) = quadwordOf(a) + quadwordOf(b);
    }
    READABLE_FUNCTION(particleSystemAddVecs, Draft);

    // particleSystemFirst: the walk's first particle (array.h's walkFirst).
    uint32_t particleSystemFirst(ee::Ptr<PointerArray> particles)
    {
        return walkFirst(*particles);
    }
    READABLE_FUNCTION(particleSystemFirst, Draft);

    // particleSystemNext: the walk's next particle (array.h's walkNext).
    uint32_t particleSystemNext(ee::Ptr<PointerArray> particles)
    {
        uint32_t particle;
        walkNext(*particles, particle);
        return particle;
    }
    READABLE_FUNCTION(particleSystemNext, Draft);

    // applyParticleGravity (the gravity affector): every particle's velocity
    // given the gravity (a Vec3, 0: the default's) for the time since the
    // emitter's last move. The step's w is not set: the velocities' w gain
    // whatever the stack held there.
    uint32_t applyParticleGravity(ee::Ptr<Affector> affector, ee::Ptr<EffectEmitter> emitter, ee::Ptr<Vec3> gravity)
    {
        const ee::Float seconds = secondsSince(collisionClock(), emitter->lastMoved);
        if (!gravity)
            gravity = ee::Ptr<Vec3>(kDefaultGravity);
        ee::Local<Vec4> step;
        step->x = gravity->x * seconds;
        step->y = gravity->y * seconds;
        step->z = gravity->z * seconds;
        if (!affector)
            return 0;
        if (!emitter)
            return 0;
        ee::Ptr<EffectParticle> particle(particleSystemFirst(emitter->particles));
        while (particle)
        {
            particleSystemAddVecs(&particle->velocity, &particle->velocity, &*step);
            particle = ee::Ptr<EffectParticle>(particleSystemNext(emitter->particles));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(applyParticleGravity, Draft);

    // bounceParticlesOffFloor (the floor affector): every particle at or below
    // the floor (0: the default's) put on it, its fall turned back up times
    // the bounce.
    uint32_t bounceParticlesOffFloor(ee::Ptr<Affector> affector, ee::Ptr<EffectEmitter> emitter, ee::Ptr<Floor> floor)
    {
        if (!floor)
            floor = ee::Ptr<Floor>(kDefaultFloor);
        const ee::Float height = floor->height;
        const ee::Float bounce = floor->bounce;
        if (!affector)
            return 0;
        if (!emitter)
            return 0;
        ee::Ptr<EffectParticle> particle(particleSystemFirst(emitter->particles));
        while (particle)
        {
            if (height <= particle->position.y)
            {
                particle->position.y = height;
                particle->velocity.y = -(bounce * particle->velocity.y);
            }
            particle = ee::Ptr<EffectParticle>(particleSystemNext(emitter->particles));
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(bounceParticlesOffFloor, Draft);

    // ---- the registries' start and end (initInlineFx, ShutdownInlineFx) -----

    namespace
    {
        inline ee::Global<int32_t, sym::g_effectTablesReady> g_effectTablesReady;
        inline ee::Global<int32_t, sym::g_particleBudget> g_particleBudget;
        inline ee::Global<int32_t, sym::g_effectCount> g_effectCount;
        constexpr int32_t kParticleBudget = 0x8000;
        constexpr uint32_t kEffectClassNamesCapacity = 0x40;
        constexpr uint32_t kAffectorNamesCapacity = 0x10;
        constexpr uint32_t kNamedEffectClassesCapacity = 0x10;

        int32_t freeRegistryArray(ee::Ptr<PointerArray> registry) { return ee::game<int32_t>(sym::freePointerArray, registry); }

        // A registry's records freed, each taken off the front (the first,
        // freed, then removed at the cursor) until none is left, then the
        // registry itself: freePointerArray's result.
        int32_t freeRegistry(ee::Ptr<PointerArray> registry)
        {
            for (uint32_t record = firstRecord(registry); record != 0; record = firstRecord(registry))
            {
                heapFree(record);
                pointerArrayRemoveCurrent(registry);
            }
            return freeRegistryArray(registry);
        }
    }

    // initEffectTables (initInlineFx's first step): the three name
    // registries made empty, the particle budget set and nothing counted
    // alive yet; the budget. ZGE2 keeps no budget or counts: its tables
    // alone. (Only ZGE3's unoptimised code leaves the budget as a result.)
    READABLE_RESULT_ONLY(ai, int32_t) initEffectTables()
    {
        *g_effectClassNames = newCursorArray(kEffectClassNamesCapacity);
        *g_affectorNames = newCursorArray(kAffectorNamesCapacity);
        *g_namedEffectClasses = newCursorArray(kNamedEffectClassesCapacity);
        *g_effectTablesReady = 1;
        if constexpr (!game::readable::kIsDm2)
        {
            *g_particleBudget = kParticleBudget;
            *g_particlesAlive = 0;
            *g_effectCount = 0;
        }
        return READABLE_RETURN_ONLY(ai, kParticleBudget);
    }
    READABLE_FUNCTION_WAITS(initEffectTables, Verified);

    // freeNamedEffectClasses (ShutdownInlineFx's): the three registries
    // freed with their records, and the tables no longer ready. The last
    // registry's freePointerArray result; 0 when they were not ready.
    int32_t freeNamedEffectClasses()
    {
        if (*g_effectTablesReady == 0)
            return 0;
        freeRegistry(*g_effectClassNames);
        freeRegistry(*g_affectorNames);
        const int32_t result = freeRegistry(*g_namedEffectClasses);
        *g_effectTablesReady = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(freeNamedEffectClasses, Verified);
}
