#pragma once
// BMX XXX's particle emitters (particles.cpp): what its game code keeps of an
// emitter. Aggressive Inline's record (games/aggressive/fx/particles.h, Emitter)
// at the same offsets where BMX XXX's code reads them; the rest is not laid
// out yet.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/math/types.h"
#include "zge/render/primitives.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // The engine's array with a cursor (zge's PointerArray), the emitter's
    // particles: first and next walk it as every effect method does inline.
    using CursorArray = zge::PointerArray;
    inline uint32_t first(CursorArray &a)
    {
        uint32_t item = 0;
        if (a.count)
        {
            a.cursor = 0;
            item = a.items[0];
        }
        else
            a.cursor = -1;
        return item;
    }
    inline uint32_t next(CursorArray &a)
    {
        const int32_t at = a.cursor;
        if (at >= 0)
        {
            a.cursor = at + 1;
            if (at + 1 < a.count)
                return a.items[at + 1];
        }
        a.cursor = -1;
        return 0;
    }

    // A particle: Aggressive Inline's record, at the offsets BMX XXX's move
    // reads (its other fields are not read by anything written here yet).
    struct Particle
    {
        uint32_t flags;        // +0x00 kParticleMoves, kParticleSpins
        uint8_t _04[0x0C];
        zge::Vec4 position;    // +0x10 the head
        zge::Vec4 tail;        // +0x20 last tick's position (a streak's tail)
        zge::Vec4 velocity;    // +0x30 units a tick
        zge::Vec4 axis;        // +0x40 spinning kinds: turned by the spin each tick
        zge::EulerAngles spin; // +0x50
        uint8_t colour[4];     // +0x56 RGBA (unaligned)
        uint8_t _5a[2];
        ee::Float size;        // +0x5C
        uint32_t texture;      // +0x60 its material
        uint32_t lifetime;     // +0x64 milliseconds
        uint32_t born;         // +0x68 the effects' clock
    };
    static_assert(offsetof(Particle, position) == 0x10 && offsetof(Particle, tail) == 0x20);
    static_assert(offsetof(Particle, velocity) == 0x30 && offsetof(Particle, axis) == 0x40);
    static_assert(offsetof(Particle, spin) == 0x50 && offsetof(Particle, lifetime) == 0x64 && offsetof(Particle, born) == 0x68);
    constexpr uint32_t kParticleMoves = 0x1;
    constexpr uint32_t kParticleSpins = 0x2;

    // A vertex of the primitive API's textured primitives (Aggressive
    // Inline's QuadVertex), and a quad for its quad draw: the texture and
    // the four corners.
    struct QuadVertex
    {
        zge::Vec4 position;
        ee::Float u, v;
        uint8_t colour[4];
        uint32_t _1c;
    };
    static_assert(sizeof(QuadVertex) == 0x20 && offsetof(QuadVertex, colour) == 0x18);
    // A quad corner as the primitive vertex it is laid out as (the lines' and triangles' vertices).
    inline ee::Ptr<zge::PrimitiveVertex> primitiveVertexOf(QuadVertex *corner) { return ee::Ptr<QuadVertex>(corner).cast<zge::PrimitiveVertex>(); }
    struct Quad
    {
        uint32_t texture;
        ee::Ptr<QuadVertex> corners[4];
    };
    static_assert(sizeof(Quad) == 0x14);

    struct Emitter
    {
        // The box of its particles' positions, found again by every move
        // (BMX XXX's own: Aggressive Inline's emitter keeps none).
        ee::Float boundsMin[3];         // +0x00
        uint8_t _0c[4];
        ee::Float boundsMax[3];         // +0x10
        uint8_t _1c[4];
        ee::Ptr<CursorArray> particles; // +0x20 the particles alive
        uint32_t particleBytes;         // +0x24 its class's particle size (the pool it allocates from)
        uint32_t lastMoved;             // +0x28 the clock at the last move (moves happen once a tick)
        uint8_t _2c[4];
        uint32_t effectClass;           // +0x30 the class it was made for
        uint8_t _34[0x0C];
    };
    static_assert(offsetof(Emitter, boundsMax) == 0x10 && offsetof(Emitter, particles) == 0x20 && offsetof(Emitter, lastMoved) == 0x28);
    static_assert(offsetof(Emitter, effectClass) == 0x30 && sizeof(Emitter) == 0x40);

    // An affector (gravity, the floor ...): its function, run on an emitter
    // with the argument its link to the class gives; and one of an effect
    // class's affectors, in the class's list (Aggressive Inline's records).
    struct Affector
    {
        uint32_t _00;
        ee::FunctionPtr<void(ee::Ptr<Affector> affector, ee::Ptr<Emitter> emitter, uint32_t argument)> apply; // +0x04
    };
    static_assert(offsetof(Affector, apply) == 0x04);
    struct AffectorLink
    {
        ee::Ptr<AffectorLink> next; // +0x00
        uint32_t _04;
        ee::Ptr<Affector> affector; // +0x08
        uint32_t argument;          // +0x0C
    };
    static_assert(offsetof(AffectorLink, affector) == 0x08 && offsetof(AffectorLink, argument) == 0x0C);
    // An effect class as the moves see it: its affectors.
    struct ClassAffectors
    {
        uint8_t _00[0x24];
        ee::Ptr<AffectorLink> affectors; // +0x24
    };
    static_assert(offsetof(ClassAffectors, affectors) == 0x24);

    // The move most effect classes share: returns how many particles it looked at.
    int32_t moveParticles(uint32_t effect, ee::Ptr<Emitter> emitter);
    // The "sleep" class's move: moveParticles with its own empty box.
    int32_t moveSleep(uint32_t effect, ee::Ptr<Emitter> emitter);
    // A particle back to its emitter's pool.
    void freeParticle(ee::Ptr<Emitter> emitter, ee::Address particle);
    // The "gravity_screen" affector: every particle falls faster once a tick
    // by the game's gravity; the shards' affector by the shards' step.
    void applyScreenGravity(uint32_t affector, ee::Ptr<Emitter> emitter);
    void applyShardGravity(uint32_t affector, ee::Ptr<Emitter> emitter);

    // The classes' emitters: a list for `capacity` particles of the class's
    // size ("spark", "tracer", "shard" and "screenshard", "poster", "sleep").
    ee::Ptr<Emitter> newSparkEmitter(uint32_t effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newTracerEmitter(uint32_t effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newShardEmitter(uint32_t effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newPosterEmitter(uint32_t effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newSleepEmitter(uint32_t effectClass, uint16_t capacity);
    // The spark class's spawn: `count` sparks from the parameters (0: the
    // class's defaults), each thrown a little off the parameters' velocity.
    struct SparkParams;
    void spawnSparks(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<SparkParams> params);
    // The spark class's draw; returns how many it drew.
    int32_t drawSparks(uint32_t effectClass, ee::Ptr<Emitter> emitter);
    // An emitter freed with every particle it still has.
    void freeEmitter(uint32_t effectClass, ee::Ptr<Emitter> emitter);
}
