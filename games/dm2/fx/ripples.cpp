// DM2's "ripple" effect class (ripples.h). DM2's own code (fx.c);
// Aggressive Inline's ripples (games/aggressive/fx/ripples.cpp) are the
// same lineage with a record of their own.

#include "ripples.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/container/cursor_item.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"

#include <cstdint>
#include "sym/dm2/fx/ripples.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kRippleEffectDefaults = sym::rippleEffectDefaults;
        constexpr uint32_t kRippleName = sym::rippleName; // "ripple"

        // The starter's three rings: lifetime (ms), size, growth, and how
        // far the second and third are raised (y is down) above the one before.
        constexpr int32_t kFirstRingLifetime = 1000, kSecondRingLifetime = 1250, kThirdRingLifetime = 1500;
        constexpr ee::Float kFirstRingSize = 0.3f, kSecondRingSize = 0.2f, kThirdRingSize = 0.1f;
        constexpr ee::Float kFirstRingGrowth = 2.5f, kSecondRingGrowth = 2.0f, kThirdRingGrowth = 1.5f;
        constexpr ee::Float kSecondRingRise = 0.05f, kThirdRingRise = 0.1f;

        // An emitter's bounds before its particles are measured.
        constexpr ee::Float kEmptyBoundsLow = 999.99f, kEmptyBoundsHigh = -999.99f;
        // A ripple's velocity and growth are a second's: this much a millisecond.
        constexpr ee::Float kSecondsPerMs = 0.001f;

        // The draw: a time left over this is taken as none; the fade's length
        // (ms) and its alpha a millisecond (an eighth).
        constexpr uint32_t kLongestLeft = 30000;
        constexpr uint32_t kFadeMs = 0x800;
        constexpr uint8_t kFullAlpha = 0xFF;
        // The primitive API's kind: in the world.
        constexpr uint32_t kWorldPrimitive = 2;

        // An unsigned count as a float, as the compiler converts one.
        ee::Float unsignedToFloat(uint32_t count)
        {
            if (static_cast<int32_t>(count) >= 0)
                return ee::toFloat(static_cast<int32_t>(count));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((count & 1) | (count >> 1)));
            return half + half;
        }
    }

    uint32_t startRippleEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<RippleParams> params)
    {
        if (!params)
            return game::tail(zge::startNamedEffect, object, kRippleName, 1, 0, ee::Address(kRippleEffectDefaults));
        params->material = 0;
        params->lifetime = kFirstRingLifetime;
        params->size = kFirstRingSize;
        params->growth = kFirstRingGrowth;
        zge::startNamedEffect(object, kRippleName, 1, 0, params);
        params->lifetime = kSecondRingLifetime;
        params->size = kSecondRingSize;
        params->growth = kSecondRingGrowth;
        params->position.y = params->position.y - kSecondRingRise;
        zge::startNamedEffect(object, kRippleName, 1, 0, params);
        params->position.y = params->position.y - kThirdRingRise;
        params->lifetime = kThirdRingLifetime;
        params->growth = kThirdRingGrowth;
        params->size = kThirdRingSize;
        return game::tail(zge::startNamedEffect, object, kRippleName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startRippleEffect, Draft);

    void spawnRipple(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<RippleParams> params)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<RippleParams>(kRippleEffectDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<Particle> ripple(zge::poolAlloc(emitter->pool));
        ripple->born = now;
        ripple->flags = 0; // it neither moves nor spins
        for (int i = 0; i < 4; ++i)
            ripple->colour[i] = params->colour[i];
        ripple->position.x = params->position.x;
        ripple->position.y = params->position.y;
        ripple->position.z = params->position.z;
        ripple->velocity.x = params->growth; // a ripple's growth
        ripple->velocity.y = 0.0f;
        ripple->velocity.z = 0.0f;
        ripple->size = params->size;
        ripple->lifetime = static_cast<uint32_t>(params->lifetime);
        ripple->material = params->material;
        game::tail(zge::pointerArrayAdd, emitter->particles, ripple);
    }
    READABLE_FUNCTION_WAITS(spawnRipple, Draft);

    int32_t drawRipples(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<uint8_t[4]> colour;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(vertices) + i;
        int32_t drawn = 0;
        for (ee::Ptr<Particle> ripple(zge::arrayFirst(emitter->particles)); ripple; ripple = ee::Ptr<Particle>(zge::arrayNext(emitter->particles)))
        {
            for (int i = 0; i < 4; ++i)
                colour[i] = ripple->colour[i];
            // Its alpha is its time left, and its grey the same, whatever its colour says.
            uint32_t left = ripple->lifetime - (now - ripple->born);
            if (!(left < kLongestLeft + 1))
                left = 0;
            const uint8_t alpha = left < kFadeMs ? static_cast<uint8_t>(left >> 3) : kFullAlpha;
            colour[3] = alpha;
            colour[0] = alpha;
            colour[2] = alpha;
            colour[1] = alpha;
            ++drawn;

            // The corners, from the last vertex back to the first: (+, +),
            // (-, +), (-, -), (+, -) in x and z; a vertex's w is left as it was.
            const ee::Float size = ripple->size;
            for (int i = 3; i >= 0; --i)
            {
                QuadVertex &vertex = vertices[i];
                const bool farX = i == 3 || i == 0, farZ = i >= 2;
                for (int byte = 0; byte < 4; ++byte)
                    vertex.colour[byte] = colour[byte];
                vertex.position.x = farX ? ripple->position.x + size : ripple->position.x - size;
                vertex.position.y = ripple->position.y;
                vertex.position.z = farZ ? ripple->position.z + size : ripple->position.z - size;
                vertex.u = i < 2 ? 1.0f : 0.0f;
                vertex.v = (i == 1 || i == 2) ? 1.0f : 0.0f;
            }
            quad->material = ripple->material;
            zge::drawPrimitiveQuads(kWorldPrimitive, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawRipples, Draft);

    // Kept from the original: nothing is returned early. With no emitter, or
    // an emitter without a list, the affectors still run and the emitter is
    // still marked moved (the time since its last move is read before either
    // is looked at).
    int32_t moveRipples(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter)
    {
        int32_t visited = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const uint32_t elapsed = now - emitter->lastMoved;
        if (emitter && emitter->particles)
        {
            emitter->boundsLow.x = kEmptyBoundsLow;
            emitter->boundsHigh.x = kEmptyBoundsHigh;
            emitter->boundsLow.z = kEmptyBoundsLow;
            emitter->boundsLow.y = kEmptyBoundsLow;
            emitter->boundsHigh.z = kEmptyBoundsHigh;
            emitter->boundsHigh.y = kEmptyBoundsHigh;

            ee::Ptr<Particle> ripple(zge::arrayFirst(emitter->particles));
            while (ripple)
            {
                ++visited;
                if (ripple->flags & zge::kParticleMoves)
                {
                    ripple->tail = ripple->position;
                    ripple->position.x = ripple->position.x + ripple->velocity.x * unsignedToFloat(elapsed) * kSecondsPerMs;
                    ripple->position.y = ripple->position.y + ripple->velocity.y * unsignedToFloat(elapsed) * kSecondsPerMs;
                    ripple->position.z = ripple->position.z + ripple->velocity.z * unsignedToFloat(elapsed) * kSecondsPerMs;
                    if (emitter->boundsHigh.x < ripple->position.x)
                        emitter->boundsHigh.x = ripple->position.x;
                    if (emitter->boundsHigh.y < ripple->position.y)
                        emitter->boundsHigh.y = ripple->position.y;
                    if (emitter->boundsHigh.z < ripple->position.z)
                        emitter->boundsHigh.z = ripple->position.z;
                    if (ripple->position.x < emitter->boundsLow.x)
                        emitter->boundsLow.x = ripple->position.x;
                    if (ripple->position.y < emitter->boundsLow.y)
                        emitter->boundsLow.y = ripple->position.y;
                    if (ripple->position.z < emitter->boundsLow.z)
                        emitter->boundsLow.z = ripple->position.z;
                }
                // Its growth (kept where a particle has its velocity's x).
                ripple->size = ripple->size + ripple->velocity.x * unsignedToFloat(elapsed) * kSecondsPerMs;
                if (ripple->flags & zge::kParticleSpins)
                    zge::vec3RotateEuler(&ripple->axis.xyz(), &ripple->axis.xyz(), &ripple->spin);
                if (now - ripple->born < ripple->lifetime)
                    ripple = ee::Ptr<Particle>(zge::arrayNext(emitter->particles));
                else
                {
                    // Its time is up: taken off the list, its record freed; the
                    // walk goes on with what took its place.
                    zge::pointerArrayRemoveCurrent(emitter->particles);
                    zge::poolFree(emitter->pool, ripple);
                    ripple = ee::Ptr<Particle>(zge::current(emitter->particles));
                }
            }
        }

        for (ee::Ptr<zge::AffectorLink> link = zge::recordOfNode<zge::AffectorLink>(effectClass->affectors.head); link; link = zge::recordOfNode<zge::AffectorLink>(link->node.next))
            link->affector->apply(link->affector, effectEmitterOf(emitter), link->argument);
        emitter->lastMoved = now;
        return visited;
    }
    READABLE_FUNCTION_WAITS(moveRipples, Draft);
}
