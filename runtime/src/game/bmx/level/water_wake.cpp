// BMX XXX's water wakes: the rings a level's dynamics object leaves on the
// water it moves through. BMX XXX's own code (its game block), in the line
// of Aggressive Inline's effect classes (game/aggressive/fx: a class's spawn
// and draw methods, its particles); the class's name, "WaterWake", is the
// game's own, and the functions are named from it and from what each does.
//
// The effects' set-up (fx.c's, not on this line yet) loads the wake's
// material, makes the one emitter object every wake is spawned in and
// registers the "WaterWake" class, whose spawn is spawnWaterWake and whose
// draw is drawWaterWake. A second class record with the same spawn and
// drawWaterWakeFacing as its draw is never registered.

#include "water_wake.h"

#include "bmx/fx/particles.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/math/conversions.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/level/water_wake.cpp.h"

namespace game::bmx
{
    namespace
    {
        // What the wakes keep (the effects' set-up fills it).
        struct WaterWakeState
        {
            uint32_t ready;                      // +0x00 1 once set up
            uint32_t material;                   // +0x04 the wake's material
            uint8_t _08[0x08];
            ee::Ptr<zge::SceneObject> emitter;   // +0x10 the emitter object every wake is spawned in
        };
        static_assert(offsetof(WaterWakeState, emitter) == 0x10);
        ee::Ptr<WaterWakeState> waterWake() { return ee::Ptr<WaterWakeState>(sym::g_waterWake); }
        constexpr uint32_t kClassName = sym::waterWakeClassName; // "WaterWake"
        // How many times startWaterWake has been called.
        inline ee::Global<uint32_t, sym::g_waterWakeCalls> g_waterWakeCalls;

        // What a wake starts from (the named effect's parameters).
        struct WaterWakeParameters
        {
            ee::Float x, y, z;       // +0x00 from the emitter object's position
            uint32_t _0c;
            ee::Float velocityX, velocityY, velocityZ; // +0x10 units a second
            uint32_t _1c;
            ee::Float size;          // +0x20
            ee::Float velocitySpread; // +0x24 each particle's velocity gets up to this more or less on every axis
            uint32_t lifetime;       // +0x28 milliseconds
            uint8_t colour[4];       // +0x2C RGBA (the alpha is left unset: the draws set their own)
            uint32_t material;       // +0x30
        };
        static_assert(offsetof(WaterWakeParameters, size) == 0x20 && offsetof(WaterWakeParameters, lifetime) == 0x28);
        static_assert(offsetof(WaterWakeParameters, colour) == 0x2C && offsetof(WaterWakeParameters, material) == 0x30);

        // A wake's particle (Aggressive Inline's Particle, the fields BMX
        // XXX's wake code reads).
        struct WakeParticle
        {
            uint32_t flags;          // +0x00 kWakeParticleMoves
            uint8_t _04[0x0C];
            zge::Vec4 position;      // +0x10
            zge::Vec4 tail;          // +0x20 last tick's position
            zge::Vec4 velocity;      // +0x30 units a second
            uint8_t _40[0x16];
            uint8_t colour[4];       // +0x56 RGBA (unaligned)
            uint8_t _5a[2];
            ee::Float size;          // +0x5C
            uint32_t material;       // +0x60
            uint32_t lifetime;       // +0x64 milliseconds
            uint32_t born;           // +0x68 the game's clock
        };
        static_assert(offsetof(WakeParticle, position) == 0x10 && offsetof(WakeParticle, velocity) == 0x30);
        static_assert(offsetof(WakeParticle, colour) == 0x56 && offsetof(WakeParticle, size) == 0x5C);
        static_assert(offsetof(WakeParticle, material) == 0x60 && offsetof(WakeParticle, born) == 0x68);
        constexpr uint32_t kWakeParticleMoves = 1; // moved by its velocity each tick (fx/particles.h kParticleMoves on the game lane: this copy goes when that header is here)

        // An emitter object's data (the engine's EmitterObjectData,
        // zge/scene/particle_emitter.cpp), as far as the spawn reads it.
        struct EmitterPlace
        {
            uint8_t _00[0x10];
            zge::Vec4 position; // +0x10 where the object was last put
        };
        // An emitter object keeps that data where other objects keep an animator.
        ee::Ptr<EmitterPlace> emitterPlaceOf(ee::Ptr<zge::SceneObject> object) { return object->animator.cast<EmitterPlace>(); }

        // The effects' budget: most particles alive at once, and how many are.
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;

        // A body faster than this along x or z leaves no wake (5), and one
        // slower than the slow speed on both (0.05, a float's, as a double)
        // only a small ring now and then.
        constexpr uint64_t kDoubleZero = 0;
        constexpr uint64_t kFastSpeed = 0x4014000000000000ull; // 5.0
        inline ee::Global<uint64_t, sym::g_wakeSlowSpeed> g_wakeSlowSpeed;
        constexpr uint32_t kOneInCalls = 3;      // a call in three, at random, makes a wake
        constexpr uint32_t kSlowWakeEvery = 30;  // a slow body's: every thirtieth call
        constexpr uint32_t kWakeLifetime = 800;  // milliseconds
        inline constexpr ee::Float kWakeLifetimeMs = 800.0f;
        constexpr uint8_t kWakeRed = 0xF0, kWakeGreen = 0xFA, kWakeBlue = 0xFA;
        inline constexpr ee::Float kFirstSize = 3.0f;      // set first; both kinds of wake set their own
        inline constexpr ee::Float kSizeForSpeed = 10.0f;  // a moving body's ring: its x and z speeds summed, times this
        inline constexpr ee::Float kSlowSize = 0.5f;
        inline constexpr ee::Float kSideSpeed = 5.0f;      // the first ring drifts sideways at the body's speed times this
        inline constexpr ee::Float kRandomByteCentre = 128.0f;
        inline constexpr ee::Float kPlaceStep = 1.0f / 512.0f;  // a ring is placed up to a quarter unit off on x and z
        inline constexpr ee::Float kSpreadStep = 1.0f / 128.0f;
        // drawWaterWake: a ring's half side grows by its size times this each
        // millisecond (1 / 700); its alpha is the milliseconds it has left
        // times the fade (0.08); each ring is drawn the step (0.001) higher
        // than the one before (y points down).
        inline ee::Global<ee::Float, sym::g_wakeGrowth> g_wakeGrowth;
        inline ee::Global<ee::Float, sym::g_wakeFade> g_wakeFade;
        inline ee::Global<ee::Float, sym::g_wakeLayerStep> g_wakeLayerStep;
        // The engine's primitive flag (zge/render/ps2/primitive_packets.cpp).
        constexpr uint32_t kPrimitiveDepthFromVertex = 1u << 2;
        constexpr uint8_t kOpaque = 0xFF;

        // A quad as drawPrimitiveQuads takes it (its material, then its
        // corners), with the corners, as the draws build it on their stack.
        struct WakeQuad
        {
            uint32_t material;                           // +0x00
            ee::Ptr<zge::PrimitiveVertex> corners[4];    // +0x04
            uint8_t _14[0x0C];
            zge::PrimitiveVertex vertices[4];            // +0x20
        };
        static_assert(offsetof(WakeQuad, vertices) == 0x20 && sizeof(WakeQuad) == 0xA0);

        // |value|, in double precision as the C has it.
        uint64_t absolute(ee::Float value)
        {
            uint64_t wide = sce::libgcc::fptodp(value);
            if (sce::libgcc::dpcmp(wide, kDoubleZero) < 0)
                wide = sce::libgcc::dpsub(kDoubleZero, wide);
            return wide;
        }

        // A random byte about its centre: -128 .. 127.
        ee::Float randomAboutCentre()
        {
            return zge::unsignedToFloat(zge::randomNumber() & 0xFFu) - kRandomByteCentre;
        }

        void placeAtRandom(WaterWakeParameters &wake)
        {
            wake.x = randomAboutCentre() * kPlaceStep;
            wake.z = randomAboutCentre() * kPlaceStep;
        }

        void setCorner(zge::PrimitiveVertex &corner, const uint8_t *colour, ee::Float x, ee::Float y, ee::Float z, ee::Float u, ee::Float v)
        {
            std::memcpy(corner.rgba, colour, 4);
            corner.x = x;
            corner.y = y;
            corner.z = z;
            corner.u = u;
            corner.v = v;
        }
    }

    // startWaterWake (dynamicsObjectContact's, for a body on a water face:
    // the point on the water, the body's velocity scaled): counts the call;
    // one call in three at random, for a body no faster than 5 along x and
    // along z, the wakes' emitter is put at the point and rings are spawned
    // in it. A moving body leaves two rings as large as its speed, the first
    // drifting sideways (to one side and the other on alternate calls); a
    // body slower than 0.05 on both axes leaves one small ring every
    // thirtieth call.
    void startWaterWake(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec4> velocity)
    {
        ee::Local<WaterWakeParameters> wake;
        *g_waterWakeCalls = *g_waterWakeCalls + 1;
        if (zge::randomNumber() % kOneInCalls != 0)
            return;
        if (sce::libgcc::dpcmp(absolute(velocity->z), kFastSpeed) > 0)
            return;
        if (sce::libgcc::dpcmp(absolute(velocity->x), kFastSpeed) > 0)
            return;
        zge::setObjectPosition(waterWake()->emitter, position);
        wake->material = waterWake()->material;
        wake->size = kFirstSize;
        wake->colour[0] = kWakeRed;
        wake->colour[2] = kWakeBlue;
        wake->lifetime = kWakeLifetime;
        wake->colour[1] = kWakeGreen;
        wake->velocitySpread = ee::Float(0.0f);
        wake->y = ee::Float(0.0f);
        const uint64_t slow = *g_wakeSlowSpeed;
        if (sce::libgcc::dpcmp(absolute(velocity->z), slow) > 0 || sce::libgcc::dpcmp(absolute(velocity->x), slow) > 0)
        {
            const uint64_t alongZ = absolute(velocity->z);
            const uint64_t alongX = absolute(velocity->x);
            wake->size = sce::libgcc::dptofp(sce::libgcc::dpadd(alongZ, alongX)) * kSizeForSpeed;
            if (*g_waterWakeCalls & 1)
            {
                wake->velocityY = ee::Float(0.0f);
                wake->velocityX = velocity->z * kSideSpeed;
                wake->velocityZ = -velocity->x * kSideSpeed;
            }
            else
            {
                wake->velocityY = ee::Float(0.0f);
                wake->velocityZ = velocity->x * kSideSpeed;
                wake->velocityX = -velocity->z * kSideSpeed;
            }
            placeAtRandom(*wake);
            zge::startNamedEffect(waterWake()->emitter, kClassName, 1, 0, wake);
            wake->velocityX = ee::Float(0.0f);
            wake->velocityY = ee::Float(0.0f);
            wake->velocityZ = ee::Float(0.0f);
            placeAtRandom(*wake);
            zge::startNamedEffect(waterWake()->emitter, kClassName, 1, 0, wake);
        }
        else if (*g_waterWakeCalls % kSlowWakeEvery == 0)
        {
            wake->lifetime = kWakeLifetime;
            wake->size = kSlowSize;
            wake->velocityX = ee::Float(0.0f);
            wake->velocityY = ee::Float(0.0f);
            wake->velocityZ = ee::Float(0.0f);
            placeAtRandom(*wake);
            zge::startNamedEffect(waterWake()->emitter, kClassName, 1, 0, wake);
        }
    }
    READABLE_FUNCTION_WAITS(startWaterWake, Verified);

    // spawnWaterWake ("WaterWake" +0x18): `count` particles at the emitter
    // object's place plus the wake's own, with its size, lifetime, colour and
    // material, and its velocity with one random amount of the spread added
    // on every axis. A particle past the effects' budget is not allocated,
    // and the original writes it all the same, at address 0: kept.
    void spawnWaterWake(uint32_t effectClass, ee::Ptr<zge::SceneObject> object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<WaterWakeParameters> wake)
    {
        const uint32_t wanted = count & 0xFFu;
        const uint32_t clock = zge::collisionClock();
        const ee::Ptr<EmitterPlace> place = emitterPlaceOf(object);
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            uint32_t allocated = 0;
            if (*g_particlesAlive < *g_particleBudget)
            {
                *g_particlesAlive = *g_particlesAlive + 1;
                allocated = zge::allocateSized(emitter->particleBytes);
            }
            const ee::Ptr<WakeParticle> particle(allocated);
            particle->born = clock;
            particle->flags = kWakeParticleMoves;
            std::memcpy(particle->colour, wake->colour, 4);
            particle->size = wake->size;
            particle->lifetime = wake->lifetime;
            particle->material = wake->material;
            particle->position.x = place->position.x + wake->x;
            particle->tail.x = particle->position.x;
            particle->position.y = place->position.y + wake->y;
            particle->tail.y = particle->position.y;
            particle->position.z = place->position.z + wake->z;
            particle->tail.z = particle->position.z;
            const ee::Float byte = zge::unsignedToFloat(zge::randomNumber() & 0xFFu);
            const ee::Float more = (byte - kRandomByteCentre) * (wake->velocitySpread * kSpreadStep);
            particle->velocity.x = wake->velocityX + more;
            particle->velocity.y = wake->velocityY + more;
            particle->velocity.z = wake->velocityZ + more;
            zge::pointerArrayAdd(emitter->particles, particle);
        }
    }
    READABLE_FUNCTION_WAITS(spawnWaterWake, Verified);

    // drawWaterWakeFacing (the unregistered class's draw): each particle as
    // an opaque square two units a side in the camera's space, about where
    // the particle is there. Returns how many it drew.
    int32_t drawWaterWakeFacing(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<WakeQuad> quad;
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> at;
        const ee::Float one(1.0f);
        const ee::Float none(0.0f);
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &quad->vertices[i];
        int32_t drawn = 0;
        uint32_t item = first(*emitter->particles);
        const ee::Ptr<zge::Matrix> fromCamera = zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(*zge::g_currentCamera), 1);
        const ee::Ptr<zge::Matrix> toCamera = zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(*zge::g_currentCamera), 0);
        while (item)
        {
            const ee::Ptr<WakeParticle> particle(item);
            *at = zge::transformed(*toCamera, particle->position);
            std::memcpy(&colour[0], particle->colour, 4);
            colour[3] = kOpaque;
            setCorner(quad->vertices[0], &colour[0], at->x - one, at->y - one, at->z, none, none);
            setCorner(quad->vertices[1], &colour[0], at->x + one, at->y - one, at->z, one, none);
            setCorner(quad->vertices[2], &colour[0], at->x + one, at->y + one, at->z, one, one);
            setCorner(quad->vertices[3], &colour[0], at->x - one, at->y + one, at->z, none, one);
            quad->material = particle->material;
            zge::drawPrimitiveQuads(0, quad, 1, fromCamera);
            item = next(*emitter->particles);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawWaterWakeFacing, Draft);

    // drawWaterWake ("WaterWake" +0x20): each particle as a square flat on
    // the water about its position, growing with its age and fading as its
    // 800 milliseconds run out, each a little above the one drawn before.
    // Returns how many it drew.
    int32_t drawWaterWake(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<WakeQuad> quad;
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> at;
        const ee::Float one(1.0f);
        const ee::Float none(0.0f);
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &quad->vertices[i];
        int32_t drawn = 0;
        ee::Float lift(0.0f);
        for (uint32_t item = first(*emitter->particles); item; item = next(*emitter->particles))
        {
            const ee::Ptr<WakeParticle> particle(item);
            *at = particle->position;
            std::memcpy(&colour[0], particle->colour, 4);
            const ee::Float age = zge::unsignedToFloat(zge::collisionClock() - particle->born);
            const ee::Float half = age * (particle->size * *g_wakeGrowth);
            const ee::Float ageNow = zge::unsignedToFloat(zge::collisionClock() - particle->born);
            colour[3] = static_cast<uint8_t>(ee::toInt((kWakeLifetimeMs - ageNow) * *g_wakeFade));
            setCorner(quad->vertices[0], &colour[0], at->x - half, at->y + lift, at->z - half, none, none);
            setCorner(quad->vertices[1], &colour[0], at->x + half, at->y + lift, at->z - half, none, one);
            setCorner(quad->vertices[2], &colour[0], at->x + half, at->y + lift, at->z + half, one, one);
            setCorner(quad->vertices[3], &colour[0], at->x - half, at->y + lift, at->z + half, one, none);
            quad->material = particle->material;
            zge::drawPrimitiveQuads(kPrimitiveDepthFromVertex, quad, 1, 0);
            ++drawn;
            lift = lift - *g_wakeLayerStep;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawWaterWake, Verified);
}
