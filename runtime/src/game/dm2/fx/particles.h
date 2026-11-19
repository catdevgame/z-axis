#pragma once

// DM2's particle effects (fx.c; particles.cpp): the emitter and the particle
// every effect class of the game is built on, the "spark" class (which
// "splash" shares but for its draw), and the two gravity affectors.

#include "ee/ee.h"
#include "zge/container/array.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/scene/effects.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    struct SparkParams; // spawns.h

    // What every class's particle starts with.
    struct Particle
    {
        uint32_t flags;          // +0x00 the engine's particle flags (zge::kParticleMoves, zge::kParticleSpins)
        uint8_t _04[0x0C];
        zge::Vec4 position;      // +0x10
        zge::Vec4 tail;          // +0x20 the position before the last move
        zge::Vec4 velocity;      // +0x30 units a move
        zge::Vec4 axis;          // +0x40 spinning kinds: turned by the spin each move
        zge::EulerAngles spin;   // +0x50
        uint8_t colour[4];       // +0x56 R, G, B, A (unaligned)
        uint8_t _5a[2];
        ee::Float size;          // +0x5C
        uint32_t material;       // +0x60
        uint32_t lifetime;       // +0x64 milliseconds
        uint32_t born;           // +0x68 the effects' clock then
        uint32_t _6c;
    };
    static_assert(offsetof(Particle, position) == 0x10 && offsetof(Particle, tail) == 0x20);
    static_assert(offsetof(Particle, velocity) == 0x30 && offsetof(Particle, axis) == 0x40);
    static_assert(offsetof(Particle, spin) == 0x50 && offsetof(Particle, colour) == 0x56 && offsetof(Particle, size) == 0x5C);
    static_assert(offsetof(Particle, lifetime) == 0x64 && offsetof(Particle, born) == 0x68 && sizeof(Particle) == 0x70);

    // An emitter (0x40 bytes, from the heap): ZGE2's keeps its particles in
    // a pool of its own.
    struct Emitter
    {
        ee::Ptr<zge::PointerArray> particles;  // +0x00
        ee::Ptr<zge::Pool> pool;               // +0x04 the particles' records
        uint8_t _08[0x08];
        zge::Vec4 boundsLow;                   // +0x10 the particles' smallest x, y and z at the last move
        zge::Vec4 boundsHigh;                  // +0x20 and largest
        uint32_t lastMoved;                    // +0x30 the effects' clock at the last move
        ee::Ptr<zge::EffectClass> effectClass; // +0x34
        uint8_t _38[0x08];
    };
    static_assert(offsetof(Emitter, boundsLow) == 0x10 && offsetof(Emitter, boundsHigh) == 0x20);
    // An emitter as the engine's effect emitter the affectors take (the same record).
    inline ee::Ptr<zge::EffectEmitter> effectEmitterOf(ee::Ptr<Emitter> emitter) { return emitter.cast<zge::EffectEmitter>(); }
    static_assert(offsetof(Emitter, lastMoved) == 0x30 && offsetof(Emitter, effectClass) == 0x34 && sizeof(Emitter) == 0x40);

    // A vertex of the primitive API's quads, and a textured quad: its
    // material and its corners.
    struct QuadVertex
    {
        zge::Vec4 position; // +0x00
        ee::Float u, v;     // +0x10
        uint8_t colour[4];  // +0x18
        uint32_t _1c;
    };
    static_assert(sizeof(QuadVertex) == 0x20 && offsetof(QuadVertex, colour) == 0x18);
    struct Quad
    {
        uint32_t material;
        ee::Ptr<QuadVertex> corners[4];
    };
    static_assert(sizeof(Quad) == 0x14);

    // A particle drawn as a square facing the camera, its size each way
    // from its position, with the material and the colour.
    void drawParticleBillboard(ee::Ptr<Particle> particle, uint32_t material, ee::Ptr<uint8_t> colour);

    // "spark"'s emitter: room for `capacity` particles.
    ee::Ptr<Emitter> newSparkEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity);
    // The same for "shard" and "screenshard" (0x120-byte particles), "poster"
    // (0xB0) and "sleep" (0x80).
    ee::Ptr<Emitter> newShardEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newPosterEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity);
    ee::Ptr<Emitter> newSleepEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity);
    // An emitter's particles moved, once per tick of the effects' clock:
    // the ones past their lifetime freed, the moving ones moved, the
    // spinning ones turned, the bounds taken again, then the class's
    // affectors run. Returns how many particles it went through.
    int32_t moveParticles(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // "spark"'s draw: each spark a streak from its position back along its
    // last move (2.75 moves long), facing the camera, its size wide, in the
    // colour of its age, the tail end fading to nothing. Returns how many it drew.
    int32_t drawSparks(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // "splash"'s draw (the class is "spark"'s but for this): each drop an
    // untextured grey square facing the camera, as bright as it is solid,
    // fading over its last 0x400 ms. Returns how many it drew.
    int32_t drawSplash(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // "sleep"'s move: the same.
    int32_t moveSleep(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // "spark"'s spawn: `count` (its low byte) sparks from the parameters
    // (the effect's defaults when none), each with its own lifetime and
    // velocity around the parameters'.
    void spawnSparks(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<SparkParams> params);
    // The affector "gravity": every particle's downward speed grows by the
    // game's gravity, unless the emitter was already moved this tick.
    void applyGravity(ee::Ptr<zge::Affector> affector, ee::Ptr<Emitter> emitter);
    // The affector "gravity_screen": the same with a pull of its own (0.2).
    void applyScreenGravity(ee::Ptr<zge::Affector> affector, ee::Ptr<Emitter> emitter);
}
