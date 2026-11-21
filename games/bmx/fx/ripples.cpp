// BMX XXX's ripples (ripples.h). BMX XXX's own code (fx.c's block).

#include "ripples.h"
#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/container/pointer_array.h"
#include "zge/math/conversions.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/ripples.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kRippleName = sym::rippleName;         // "ripple"
        constexpr uint32_t kRippleDefaults = sym::rippleDefaults;

        // The start's three rings: their sizes, and how much higher (y
        // down) the second is than the first and the third than the second.
        constexpr ee::Float kFirstSize = 0.3f, kSecondSize = 0.2f, kThirdSize = 0.1f;
        constexpr ee::Float kSecondRise = 0.05f, kThirdRise = 0.1f;

        // How many particles are alive, and the effects' budget of them
        // (every emitter's together).
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;

        // A ripple is a plain particle that does not move: where a particle
        // has its velocity's x it keeps its growth, size added a second.
        ee::Float &growth(Particle &ring) { return ring.velocity.x; }

        // The emitter's box before any particle is in it (the class's own numbers).
        constexpr ee::Float kEmptyMin = 999.99f, kEmptyMax = -999.99f;
        // Seconds a millisecond.
        constexpr ee::Float kSecondsPerMs = 0.001f;

        // A ripple fades out over its last 2 seconds.
        constexpr uint32_t kLongestLife = 30000, kFadeMs = 0x800;
        constexpr uint32_t kFullAlpha = 0xFF;
        // drawPrimitiveQuads' flags for a quad in the world.
        constexpr uint32_t kWorldQuads = 2;
    }

    // startRippleEffect: see ripples.h.
    uint32_t startRippleEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<RippleParameters> ripple)
    {
        if (!ripple)
            return game::tail(zge::startNamedEffect, object, kRippleName, 1, 0, kRippleDefaults);
        ripple->texture = 0;
        ripple->lifetime = 1000;
        ripple->size = kFirstSize;
        ripple->growth = 2.5f;
        zge::startNamedEffect(object, kRippleName, 1, 0, ripple);
        ripple->lifetime = 1250;
        ripple->size = kSecondSize;
        ripple->growth = 2.0f;
        ripple->y = ripple->y - kSecondRise;
        zge::startNamedEffect(object, kRippleName, 1, 0, ripple);
        ripple->lifetime = 1500;
        ripple->growth = 1.5f;
        ripple->y = ripple->y - kThirdRise;
        ripple->size = kThirdSize;
        return game::tail(zge::startNamedEffect, object, kRippleName, 1, 0, ripple);
    }
    READABLE_FUNCTION_WAITS(startRippleEffect, Draft);

    // spawnRipple ("ripple" +0x18): one ripple (whatever the count) from the
    // parameters (the defaults when none). Kept bug: with the effects'
    // budget of particles used up no ripple is allocated, and the ripple is
    // written at address 0 and added to the emitter.
    void spawnRipple(uint32_t effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<RippleParameters> given)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const ee::Ptr<RippleParameters> ripple = given ? given : ee::Ptr<RippleParameters>(kRippleDefaults);
        if (!effectClass || !emitter)
            return;
        uint32_t made = 0;
        if (*g_particlesAlive < *g_particleBudget)
        {
            *g_particlesAlive = *g_particlesAlive + 1;
            made = zge::allocateSized(emitter->particleBytes);
        }
        const ee::Ptr<Particle> ring(made);
        ring->born = now;
        ring->flags = 0;
        std::memcpy(ring->colour, ripple->colour, 4);
        ring->position.x = ripple->x;
        ring->position.y = ripple->y;
        ring->position.z = ripple->z;
        growth(*ring) = ripple->growth;
        ring->velocity.y = 0.0f;
        ring->velocity.z = 0.0f;
        ring->size = ripple->size;
        ring->lifetime = ripple->lifetime;
        ring->texture = ripple->texture;
        game::tail(zge::pointerArrayAdd, emitter->particles, ring);
    }
    READABLE_FUNCTION_WAITS(spawnRipple, Draft);

    // moveRipples ("ripple" +0x1C): by the milliseconds since the emitter's
    // last move: a moving particle steps by its velocity and widens the
    // emitter's box (a ripple does not move), each grows, a spinning one
    // turns its axis, and one whose life is over is freed; then the class's
    // affectors run and the move is timed. Returns how many it looked at.
    // Kept as it is: with no emitter the last move's time is read from, and
    // written to, low memory, and the affectors run on no emitter.
    int32_t moveRipples(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        int32_t looked = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const uint32_t elapsed = now - emitter->lastMoved;
        if (emitter && emitter->particles)
        {
            for (uint32_t lane = 0; lane < 3; ++lane)
            {
                emitter->boundsMin[lane] = kEmptyMin;
                emitter->boundsMax[lane] = kEmptyMax;
            }
            uint32_t particle = first(*emitter->particles);
            while (particle)
            {
                Particle &p = *ee::Ptr<Particle>(particle);
                ++looked;
                if (p.flags & kParticleMoves)
                {
                    p.tail = p.position;
                    p.position.x = p.position.x + p.velocity.x * zge::unsignedToFloat(elapsed) * kSecondsPerMs;
                    p.position.y = p.position.y + p.velocity.y * zge::unsignedToFloat(elapsed) * kSecondsPerMs;
                    p.position.z = p.position.z + p.velocity.z * zge::unsignedToFloat(elapsed) * kSecondsPerMs;
                    const ee::Float *const at = &p.position.x;
                    for (uint32_t lane = 0; lane < 3; ++lane)
                        if (emitter->boundsMax[lane] < at[lane])
                            emitter->boundsMax[lane] = at[lane];
                    for (uint32_t lane = 0; lane < 3; ++lane)
                        if (at[lane] < emitter->boundsMin[lane])
                            emitter->boundsMin[lane] = at[lane];
                }
                p.size = p.size + growth(p) * zge::unsignedToFloat(elapsed) * kSecondsPerMs;
                if (p.flags & kParticleSpins)
                    zge::vec3RotateEuler(&p.axis.xyz(), &p.axis.xyz(), &p.spin);
                if (now - p.born < p.lifetime)
                    particle = next(*emitter->particles);
                else
                {
                    zge::pointerArrayRemoveCurrent(emitter->particles);
                    freeParticle(emitter, particle);
                    particle = zge::current(emitter->particles);
                }
            }
        }
        for (ee::Ptr<AffectorLink> link = ee::Ptr<ClassAffectors>(effect)->affectors; link; link = link->next)
            link->affector->apply(link->affector, emitter, link->argument);
        emitter->lastMoved = now;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveRipples, Draft);

    // drawRipples ("ripple" +0x20): each ripple a square flat on the level
    // (x, z) of its size, grey by how long it has left: fading over its last
    // 2 seconds (one with more than 30 seconds left is not seen).
    int32_t drawRipples(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex> plusXMinusZ;
        ee::Local<QuadVertex> minusXMinusZ;
        ee::Local<QuadVertex> minusXPlusZ;
        ee::Local<QuadVertex> plusXPlusZ;
        ee::Local<uint8_t[4]> colour;
        int32_t drawn = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        quad->corners[0] = plusXMinusZ;
        quad->corners[1] = minusXMinusZ;
        quad->corners[2] = minusXPlusZ;
        quad->corners[3] = plusXPlusZ;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles), ++drawn)
        {
            const Particle &ring = *ee::Ptr<Particle>(particle);
            const uint32_t left = ring.lifetime - (now - ring.born);
            const uint32_t kept = left <= kLongestLife ? left : 0;
            const uint8_t shade = static_cast<uint8_t>(kept < kFadeMs ? kept >> 3 : kFullAlpha);
            std::memset(&colour[0], shade, 4);

            std::memcpy(plusXPlusZ->colour, &colour[0], 4);
            plusXPlusZ->position.x = ring.position.x + ring.size;
            plusXPlusZ->position.y = ring.position.y;
            plusXPlusZ->position.z = ring.position.z + ring.size;
            plusXPlusZ->u = 0.0f;
            plusXPlusZ->v = 0.0f;
            std::memcpy(minusXPlusZ->colour, &colour[0], 4);
            minusXPlusZ->position.x = ring.position.x - ring.size;
            minusXPlusZ->position.y = ring.position.y;
            minusXPlusZ->position.z = ring.position.z + ring.size;
            minusXPlusZ->u = 0.0f;
            minusXPlusZ->v = 1.0f;
            std::memcpy(minusXMinusZ->colour, &colour[0], 4);
            minusXMinusZ->position.x = ring.position.x - ring.size;
            minusXMinusZ->position.y = ring.position.y;
            minusXMinusZ->position.z = ring.position.z - ring.size;
            minusXMinusZ->u = 1.0f;
            minusXMinusZ->v = 1.0f;
            std::memcpy(plusXMinusZ->colour, &colour[0], 4);
            plusXMinusZ->position.x = ring.position.x + ring.size;
            plusXMinusZ->position.y = ring.position.y;
            plusXMinusZ->position.z = ring.position.z - ring.size;
            plusXMinusZ->u = 1.0f;
            plusXMinusZ->v = 0.0f;
            quad->texture = ring.texture;
            zge::drawPrimitiveQuads(kWorldQuads, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawRipples, Draft);
}
