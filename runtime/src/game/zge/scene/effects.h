#pragma once

// The engine's particle effects (Zfx): an effect class is a 0x38-byte table of
// methods with its list of affectors; an emitter (0x40 bytes) holds the
// particles a class spawned and moves them once a tick; an affector (gravity,
// the floor) is a function the class's move hands each emitter to. The game's
// classes (aggressive/fx: "spark", "smoke", ...) take the engine's methods for
// what they do not change: the affector methods, deleteEmitter, and the
// default make and move (newEffectEmitter, moveEmitterParticles).

#include "ee/readable_waits.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "zge/container/array.h"
#include "zge/container/list.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct EffectClass;
    struct EffectEmitter;
    struct Pool; // (zge/memory/pool.h)
    struct SceneObject;

    // An affector: +0x04 what it does to an emitter's particles, with the
    // argument the class keeps for it (0: the affector's own default).
    struct Affector
    {
        uint32_t _00;
        ee::FunctionPtr<void(ee::Ptr<Affector> affector, ee::Ptr<EffectEmitter> emitter, uint32_t argument)> apply; // +0x04
    };
    static_assert(offsetof(Affector, apply) == 0x04);

    // A class's affector (0x10 bytes from effectAllocate, on its list).
    struct AffectorLink
    {
        ListNode node;              // +0x00
        ee::Ptr<Affector> affector; // +0x08
        uint32_t argument;          // +0x0C
    };
    static_assert(offsetof(AffectorLink, affector) == 0x08 && sizeof(AffectorLink) == 0x10);

    // An effect class's methods (0x38 bytes; the game's tables, aggressive/fx).
    struct EffectClass
    {
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass)> reset;                                                  // +0x00
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass, ee::Ptr<Affector> affector, uint32_t argument)> addAffector; // +0x04
        // +0x08 the affector registered under that name (registerEffectTypeName)
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass, uint32_t name, uint32_t argument)> addAffectorByName;
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass)> clearAffectors;                                         // +0x0C
        ee::FunctionPtr<ee::Ptr<EffectEmitter>(ee::Ptr<EffectClass> effectClass, uint32_t capacity)> newEmitter;        // +0x10
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)> deleteEmitter;         // +0x14
        // +0x18
        ee::FunctionPtr<void(ee::Ptr<EffectClass> effectClass, ee::Ptr<SceneObject> object, ee::Ptr<EffectEmitter> emitter, uint32_t start, uint32_t a, uint32_t b)> spawn;
        ee::FunctionPtr<uint32_t(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter)> move; // +0x1C its particles moved
        ee::FunctionPtr<uint32_t(ee::Ptr<EffectClass> effectClass, ee::Ptr<EffectEmitter> emitter, uint32_t mode)> draw; // +0x20 (mode 0)
        List affectors;                                                                                    // +0x24 AffectorLink
        uint8_t _30[0x08];
    };
    static_assert(offsetof(EffectClass, newEmitter) == 0x10 && offsetof(EffectClass, draw) == 0x20);
    static_assert(offsetof(EffectClass, affectors) == 0x24 && sizeof(EffectClass) == 0x38);

    // A named effect class (0x48 bytes, registerNamedEffectClass's record, on
    // g_namedEffectClasses): a spawn function kept under its name, which
    // spawnNamedEffect finds by its hash (ZGE2's by its text).
    using NamedEffectSpawn = ee::FunctionPtr<uint32_t(uint32_t a, uint32_t b)>; // -> what it spawned
    struct NamedEffectClass
    {
        NamedEffectSpawn spawn;                                  // +0x00
        char name[0x40];                                         // +0x04
        uint32_t nameHash;                                       // +0x44
    };
    static_assert(offsetof(NamedEffectClass, nameHash) == 0x44 && sizeof(NamedEffectClass) == 0x48);

    // An emitter (0x40 bytes, from effectAllocate; aggressive::fx::Emitter).
    struct EffectEmitter
    {
#if ZX_GAME_DM2
        // ZGE2's emitter (0x40 bytes, kEffectEmitterBytes; game/dm2/fx/particles.h's Emitter) keeps its
        // particles' records in a pool of its own, and no particle size or owner. ZGE3's two fields at its end
        // stand past the end of DM2's record: nothing of DM2's reads them.
        ee::Ptr<PointerArray> particles; // DM2 +0x00
        ee::Ptr<Pool> pool;             // DM2 +0x04 the particles' records
        uint8_t _08[0x08];
        Vec4 boundsLow;                 // DM2 +0x10
        Vec4 boundsHigh;                // DM2 +0x20
        uint32_t lastMoved;             // DM2 +0x30
        ee::Ptr<EffectClass> effectClass; // DM2 +0x34
        uint8_t _38[0x08];
        uint32_t particleBytes;
        uint32_t owner;
#else
        Vec4 boundsLow;                 // +0x00 the moving particles' smallest x, y and z at the last move
        Vec4 boundsHigh;                // +0x10 and largest
        ee::Ptr<PointerArray> particles; // +0x20
        uint32_t particleBytes;         // +0x24 a particle's size (the size class it is freed to)
        uint32_t lastMoved;             // +0x28 the effects' clock at the last move
        uint32_t owner;                 // +0x2C what it belongs to (the emitter object's owner)
        ee::Ptr<EffectClass> effectClass; // +0x30
        uint8_t _34[0x0C];
#endif
    };
    inline constexpr size_t kEffectEmitterBytes = 0x40;
    static_assert(offsetof(EffectEmitter, particles) == game::readable::perGame(0x20u, 0x00u, 0x20u) &&
                  offsetof(EffectEmitter, boundsLow) == game::readable::perGame(0x00u, 0x10u, 0x00u) &&
                  offsetof(EffectEmitter, lastMoved) == game::readable::perGame(0x28u, 0x30u, 0x28u) &&
                  offsetof(EffectEmitter, effectClass) == game::readable::perGame(0x30u, 0x34u, 0x30u));
    static_assert(game::readable::kIsDm2 || (offsetof(EffectEmitter, owner) == 0x2C && sizeof(EffectEmitter) == kEffectEmitterBytes));

    // A particle's flags.
    enum : uint32_t
    {
        kParticleMoves = 1u, // moved by its velocity each tick, the tail left where it was
        kParticleSpins = 2u, // its axis turned by its spin each tick
    };

    // What every particle starts with (0x70 bytes, the engine's own size;
    // the classes add their fields past it).
    struct EffectParticle
    {
        uint32_t flags;    // +0x00
        uint8_t _04[0x0C];
        Vec4 position;     // +0x10 the head
        Vec4 tail;         // +0x20 the position before the last move
        Vec4 velocity;     // +0x30 units a second
        Vec4 axis;         // +0x40
        EulerAngles spin;  // +0x50 the axis' turn a tick
        uint8_t _56[0x0E];
        uint32_t lifetime; // +0x64 milliseconds
        uint32_t born;     // +0x68 the effects' clock
        uint32_t _6c;
    };
    static_assert(offsetof(EffectParticle, position) == 0x10 && offsetof(EffectParticle, tail) == 0x20);
    static_assert(offsetof(EffectParticle, velocity) == 0x30 && offsetof(EffectParticle, axis) == 0x40);
    static_assert(offsetof(EffectParticle, spin) == 0x50 && offsetof(EffectParticle, lifetime) == 0x64);
    static_assert(offsetof(EffectParticle, born) == 0x68 && sizeof(EffectParticle) == 0x70);
}

namespace zge
{
    // (effect_classes.cpp)
    void registerNamedEffectClass(uint32_t spawn, uint32_t name);
    void registerEffectClassName(ee::Ptr<EffectClass> effectClass, uint32_t name);
    ee::Ptr<EffectClass> unregisterEffectClassName(uint32_t name);
    ee::Ptr<EffectClass> findEffectClassByName(uint32_t name);
    void registerEffectTypeName(ee::Ptr<Affector> affector, uint32_t name);
    void initEffectClass(ee::Ptr<EffectClass> effectClass);
    READABLE_RESULT_ONLY(ai, int32_t) initEffectTables(); // (ZGE2's and ZGE4's answer nothing)
    int32_t freeNamedEffectClasses();
}
