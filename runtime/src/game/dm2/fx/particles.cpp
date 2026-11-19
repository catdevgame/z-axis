// DM2's particle effects (particles.h). DM2's own code (fx.c); Aggressive
// Inline's game/aggressive/fx/particles.cpp descends from it.

#include "particles.h"

#include "effects_clock.h"
#include "spawns.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/container/cursor_item.h"
#include "zge/container/pointer_array.h"
#include "zge/container/list.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/platform/platform.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/fx/particles.cpp.h"

namespace game::dm2::calls
{
    // Scaffolding (address-call): ZGE2's own function, no readable body yet.
    // A scene object's matrix of the kind `which`, worked out when it is stale (ZGE2's own function: the engine's
    // cachedMatrix is another, for a matrix cache).
    inline ee::Ptr<zge::Matrix> objectCachedMatrix(uint32_t object, uint32_t which) { return ee::game<ee::Ptr<zge::Matrix>>(sym::objectCachedMatrix, object, which); }
}

namespace game::dm2
{
    namespace
    {
        // An emitter's bounds before its particles are measured: nothing is
        // lower than the first, nothing higher than the second (99999 and -99999).
        inline ee::Global<ee::Float, sym::g_emptyBoundsLow> g_emptyBoundsLow;
        inline ee::Global<ee::Float, sym::g_emptyBoundsHigh> g_emptyBoundsHigh;
        // "sleep"'s move has its own copies of the two.
        inline ee::Global<ee::Float, sym::g_sleepEmptyBoundsLow> g_sleepEmptyBoundsLow;
        inline ee::Global<ee::Float, sym::g_sleepEmptyBoundsHigh> g_sleepEmptyBoundsHigh;

        // The game's gravity, and the screen particles' (0.2), a move.
        inline ee::Global<ee::Float, sym::g_gravity> g_gravity;
        inline ee::Global<ee::Float, sym::g_screenGravity> g_screenGravity;

        // "spark": its material and the parameters a spawn without any gets.
        inline ee::Global<uint32_t, sym::g_sparkMaterial> g_sparkMaterial;
        constexpr uint32_t kSparkEffectDefaults = sym::sparkEffectDefaults;

        // A spark lives its lifetime give or take up to a half, in sixteenths.
        constexpr ee::Float kLifetimeStep = 0.0625f;
        // A spark's velocity is off by up to the spread each way, in 128ths.
        constexpr ee::Float kSpreadStep = 0.0078125f;

        // A random number in [-1, 1) in steps of 1/128.
        ee::Float randomUnit()
        {
            const uint32_t r = zge::randomNumber();
            return ee::toFloat(static_cast<int32_t>((r & 0xFFu) - 0x80)) * kSpreadStep;
        }

        // Every particle's fall speed raised by `pull`, unless the emitter
        // was already moved this tick.
        template <typename Pull>
        void pullParticles(ee::Ptr<zge::Affector> affector, ee::Ptr<Emitter> emitter, Pull pull)
        {
            if (static_cast<uint32_t>(effectsClock()) == emitter->lastMoved || !affector || !emitter)
                return;
            // (The pull is read only when there is a particle to pull, as the original reads it.)
            for (ee::Ptr<Particle> particle(zge::arrayFirst(emitter->particles)); particle;
                 particle = ee::Ptr<Particle>(zge::arrayNext(emitter->particles)))
                particle->velocity.y = particle->velocity.y + pull();
        }
    }

    void drawParticleBillboard(ee::Ptr<Particle> particle, uint32_t material, ee::Ptr<uint8_t> colour)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> corners;
        ee::Local<zge::Matrix> facing; // the camera's turn, placed at the particle
        ee::Local<zge::Vec4> corner;   // a corner in the camera's space (its w is whatever the stack held)
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(corners) + i;
        quad->material = material;
        const ee::Ptr<zge::Matrix> cameraMatrix = calls::objectCachedMatrix(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)), 1);
        zge::copyRotationAndTranslation(facing, cameraMatrix);
        facing->translation() = particle->position;
        // Round the square: right low, left low, left high, right high.
        for (uint32_t i = 0; i < 4; ++i)
        {
            QuadVertex &vertex = corners[i];
            const bool left = ((i + 1) & 2) != 0, high = (i & 2) != 0;
            for (int byte = 0; byte < 4; ++byte)
                vertex.colour[byte] = colour[byte];
            vertex.u = left ? 0.0f : 1.0f;
            vertex.v = high ? 1.0f : 0.0f;
            const ee::Float size = particle->size;
            corner->x = left ? -size : size;
            corner->y = high ? size : -size;
            corner->z = 0.0f;
            zge::transformPoint(&vertex.position, facing, corner);
        }
        zge::drawPrimitiveQuads(0, quad, 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawParticleBillboard, Verified);

    namespace
    {
        // Each class's particle record: the common start, then the class's own.
        constexpr uint32_t kShardBytes = 0x120, kPosterBytes = 0xB0, kSleepBytes = 0x80;

        // Every class's emitter: a list and a pool for `capacity` particles of its size.
        ee::Ptr<Emitter> newEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity, uint32_t particleBytes)
        {
            const ee::Ptr<Emitter> emitter(zge::heapAllocate(sizeof(Emitter)));
            if (emitter)
            {
                emitter->particles = zge::newPointerArray(capacity);
                emitter->pool = ee::Ptr<zge::Pool>(zge::createPool(particleBytes, capacity, 0));
                emitter->effectClass = effectClass;
                emitter->lastMoved = static_cast<uint32_t>(effectsClock());
            }
            return emitter;
        }
    }

    namespace
    {
        // A spark's colour over its life, in sixteen steps.
        inline ee::Global<uint8_t[16][4], sym::g_sparkColours> g_sparkColours;
        constexpr ee::Float kColourSteps = 15.0f;
        constexpr uint32_t kLastColour = 15;
        // A spark is drawn as a streak this many moves long.
        constexpr ee::Float kStreakMoves = 2.75f;
        // The primitive API's kind for quads in the world.
        constexpr uint32_t kWorldQuads = 2;

        // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose
        // upper half the last call left.
        zge::Vec4 scaled(const zge::Vec4 &v, ee::Float factor)
        {
            return zge::scaledCarrying(v, factor, zge::scaleLeftover(zge::LeftBy::lastCall));
        }
    }

    int32_t drawSparks(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<Quad> quad;
        ee::Local<zge::Matrix> facing;   // the camera's turn, placed at the spark
        ee::Local<zge::Matrix> viewTurn; // the world's directions in the camera's space
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> streak;     // from the head back to where the tail is drawn
        ee::Local<zge::Vec4> direction;  // the streak's direction across the screen
        ee::Local<zge::Vec4> side;       // across the streak (its w is whatever the stack held)
        ee::Local<zge::Vec4> head;       // the spark's position and its last one, in the camera's space
        ee::Local<zge::Vec4> tail;
        ee::Local<zge::Vec4> along;      // a corner's two terms
        ee::Local<zge::Vec4> across;
        // (The original keeps these two in its frame.)
        ee::Local<ee::Ptr<Emitter>> emitterKept;
        ee::Local<int32_t> clockKept;

        // The quad's corners are the vertices from the last back to the first.
        quad->corners[0] = ee::Ptr<QuadVertex>(vertices) + 3;
        *emitterKept = emitter;
        int32_t drawn = 0;
        quad->corners[1] = ee::Ptr<QuadVertex>(vertices) + 2;
        quad->corners[2] = ee::Ptr<QuadVertex>(vertices) + 1;
        quad->corners[3] = ee::Ptr<QuadVertex>(vertices);
        const ee::Ptr<zge::Matrix> cameraMatrix = calls::objectCachedMatrix(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)), 1);
        zge::copyRotationAndTranslation(facing, cameraMatrix);
        zge::copyRotationAndTranslation(viewTurn, facing);
        zge::matrixTranspose3(viewTurn, viewTurn);
        viewTurn->translation() = zge::kZero;
        const int32_t clock = effectsClock();
        *clockKept = clock;

        for (ee::Ptr<Particle> spark(zge::arrayFirst(emitter->particles)); spark; spark = ee::Ptr<Particle>(zge::arrayNext((*emitterKept)->particles)))
        {
            const uint32_t step = static_cast<uint32_t>(
                ee::toInt(ee::toFloat(clock - static_cast<int32_t>(spark->born)) * kColourSteps / ee::toFloat(static_cast<int32_t>(spark->lifetime))));
            const uint32_t colourStep = step < kLastColour + 1 ? step : kLastColour;
            for (int i = 0; i < 4; ++i)
                colour[i] = (*g_sparkColours)[colourStep][i];
            facing->translation() = spark->position;

            zge::transformPoint(head, viewTurn, ee::Ptr<const zge::Vec4>(&spark->position));
            zge::transformPoint(tail, viewTurn, ee::Ptr<const zge::Vec4>(&spark->tail));
            *streak = *tail - *head;
            *streak = scaled(*streak, kStreakMoves);
            *direction = *streak;
            direction->z = 0.0f;
            const ee::Float length = zge::vec3Length(&direction->xyz());
            if (ee::Float(0.0f) < length)
                *direction = scaled(*direction, ee::Float(1.0f) / length);
            else
            {
                // No streak to follow: it lies along the screen's x.
                direction->z = 0.0f;
                direction->x = 1.0f;
                direction->y = 0.0f;
            }
            ++drawn;
            side->z = 0.0f;
            side->x = direction->y;
            side->y = -direction->x;

            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = vertices[i];
                const bool tailEnd = (i & 2) != 0, farSide = ((i + 1) & 2) != 0;
                for (int byte = 0; byte < 4; ++byte)
                    vertex.colour[byte] = colour[byte];
                vertex.u = tailEnd ? 1.0f : 0.0f;
                vertex.v = farSide ? 1.0f : 0.0f;
                const ee::Float size = spark->size;
                *along = scaled(*direction, tailEnd ? -size : size);
                *across = scaled(*side, farSide ? -size : size);
                vertex.position = *along + *across;
                if (tailEnd)
                {
                    // The tail end is the streak away, and fades to nothing.
                    vertex.position = vertex.position + *streak;
                    vertex.colour[3] = 0;
                }
            }
            quad->material = spark->material;
            zge::drawPrimitiveQuads(kWorldQuads, quad, 1, facing);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSparks, Verified);

    namespace
    {
        // A splash drop is gone from the screen with more than this left
        // to live; it fades over its last 0x400 ms, an eighth of a step each.
        constexpr uint32_t kDropTimeLimit = 0x7531;
        constexpr uint32_t kDropFadeTime = 0x400;
        constexpr uint8_t kDropFullAlpha = 0x80;
    }

    int32_t drawSplash(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> facing; // the camera's turn, placed at the drop
        ee::Local<uint8_t[4]> colour;
        ee::Local<int32_t> clockKept;  // (the original keeps the clock in its frame)
        int32_t drawn = 0;
        const int32_t clock = effectsClock();
        *clockKept = clock;
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = ee::Ptr<QuadVertex>(vertices) + i;
        const ee::Ptr<zge::Matrix> cameraMatrix = calls::objectCachedMatrix(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)), 1);
        zge::copyRotationAndTranslation(facing, cameraMatrix);

        for (ee::Ptr<Particle> drop(zge::arrayFirst(emitter->particles)); drop; drop = ee::Ptr<Particle>(zge::arrayNext(emitter->particles)))
        {
            facing->translation() = drop->position;
            for (int i = 0; i < 4; ++i)
                colour[i] = drop->colour[i];
            uint32_t left = drop->lifetime - (static_cast<uint32_t>(clock) - drop->born);
            if (!(left < kDropTimeLimit))
                left = 0;
            // A grey as bright as it is solid.
            const uint8_t alpha = left < kDropFadeTime ? static_cast<uint8_t>(left >> 3) : kDropFullAlpha;
            colour[3] = alpha;
            colour[0] = alpha;
            ++drawn;
            colour[2] = alpha;
            colour[1] = alpha;

            // A corner: its colour, its place in the camera's space taken to
            // the world (the place's w is whatever the stack held), then its
            // texture coordinates.
            const auto corner = [&](uint32_t index, ee::Float x, ee::Float y, ee::Float u, ee::Float v)
            {
                QuadVertex &vertex = vertices[index];
                for (int byte = 0; byte < 4; ++byte)
                    vertex.colour[byte] = colour[byte];
                vertex.position.x = x;
                vertex.position.z = 0.0f;
                vertex.position.y = y;
                const ee::Ptr<zge::Vec4> place(&vertex.position);
                zge::transformPoint(place, facing, place);
                vertex.u = u;
                vertex.v = v;
            };
            corner(3, -drop->size, -drop->size, 0.0f, 0.0f);
            corner(2, -drop->size, drop->size, 0.0f, 1.0f);
            corner(1, drop->size, drop->size, 1.0f, 1.0f);
            corner(0, drop->size, -drop->size, 1.0f, 0.0f);
            quad->material = 0;
            zge::drawPrimitiveQuads(kWorldQuads, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSplash, Draft);

    ee::Ptr<Emitter> newSparkEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, sizeof(Particle));
    }
    READABLE_FUNCTION_WAITS(newSparkEmitter, Verified);

    ee::Ptr<Emitter> newShardEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kShardBytes);
    }
    READABLE_FUNCTION_WAITS(newShardEmitter, Verified);

    ee::Ptr<Emitter> newPosterEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kPosterBytes);
    }
    READABLE_FUNCTION_WAITS(newPosterEmitter, Verified);

    ee::Ptr<Emitter> newSleepEmitter(ee::Ptr<zge::EffectClass> effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kSleepBytes);
    }
    READABLE_FUNCTION_WAITS(newSleepEmitter, Verified);

    namespace
    {
        // moveParticles and moveSleep: the move, with the caller's own copies
        // of the empty bounds.
        int32_t moveEmitter(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter, bool sleepsBounds);
    }

    int32_t moveParticles(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter)
    {
        return moveEmitter(effectClass, emitter, false);
    }
    READABLE_FUNCTION_WAITS(moveParticles, Verified);

    int32_t moveSleep(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter)
    {
        return moveEmitter(effectClass, emitter, true);
    }
    READABLE_FUNCTION_WAITS(moveSleep, Verified);

    namespace
    {
    int32_t moveEmitter(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter, bool sleepsBounds)
    {
        int32_t visited = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!emitter || !emitter->particles || !(emitter->lastMoved < now))
            return 0;

        // (Read here, after the tests, as the original reads them.)
        const ee::Float low = sleepsBounds ? *g_sleepEmptyBoundsLow : *g_emptyBoundsLow;
        const ee::Float high = sleepsBounds ? *g_sleepEmptyBoundsHigh : *g_emptyBoundsHigh;
        emitter->boundsLow.x = low;
        emitter->boundsHigh.x = high;
        emitter->boundsLow.z = low;
        emitter->boundsLow.y = low;
        emitter->boundsHigh.z = high;
        emitter->boundsHigh.y = high;

        ee::Ptr<Particle> particle(zge::arrayFirst(emitter->particles));
        while (particle)
        {
            if (!(now - particle->born < particle->lifetime))
            {
                // Its time is up: taken off the list, its record freed; the
                // walk goes on with what took its place.
                zge::pointerArrayRemoveCurrent(emitter->particles);
                zge::poolFree(emitter->pool, particle);
                particle = ee::Ptr<Particle>(zge::current(emitter->particles));
            }
            else
            {
                if (particle->flags & zge::kParticleMoves)
                {
                    particle->tail = particle->position;
                    particle->position = particle->position + particle->velocity;
                }
                emitter->boundsLow.x = ee::min(particle->position.x, emitter->boundsLow.x);
                emitter->boundsLow.y = ee::min(particle->position.y, emitter->boundsLow.y);
                emitter->boundsLow.z = ee::min(particle->position.z, emitter->boundsLow.z);
                emitter->boundsHigh.x = ee::max(particle->position.x, emitter->boundsHigh.x);
                emitter->boundsHigh.y = ee::max(particle->position.y, emitter->boundsHigh.y);
                emitter->boundsHigh.z = ee::max(particle->position.z, emitter->boundsHigh.z);
                if (particle->flags & zge::kParticleSpins)
                    zge::vec3RotateEuler(&particle->axis.xyz(), &particle->axis.xyz(), &particle->spin);
                particle = ee::Ptr<Particle>(zge::arrayNext(emitter->particles));
            }
            ++visited;
        }

        for (ee::Ptr<zge::AffectorLink> link = zge::recordOfNode<zge::AffectorLink>(effectClass->affectors.head); link; link = zge::recordOfNode<zge::AffectorLink>(link->node.next))
            link->affector->apply(link->affector, effectEmitterOf(emitter), link->argument);
        emitter->lastMoved = now;
        return visited;
    }
    }

    void spawnSparks(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<SparkParams> params)
    {
        uint32_t left = count & 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<SparkParams>(kSparkEffectDefaults);
        if (!effectClass || !emitter)
            return;
        while (left != 0)
        {
            const ee::Ptr<Particle> spark(zge::poolAlloc(emitter->pool));
            spark->born = now;
            spark->flags = zge::kParticleMoves;
            spark->material = *g_sparkMaterial;
            spark->size = params->size;
            for (int i = 0; i < 4; ++i)
                spark->colour[i] = params->colour[i];
            const ee::Float lifetime = ee::toFloat(params->lifetime);
            --left;
            const uint32_t r = zge::randomNumber();
            spark->lifetime = static_cast<uint32_t>(ee::toInt(ee::toFloat(static_cast<int32_t>((r & 0xFu) - 8)) * kLifetimeStep * lifetime + lifetime));
            spark->tail = params->position;
            spark->position = spark->tail;
            spark->velocity.x = params->velocity.x + params->spread * randomUnit();
            spark->velocity.y = params->velocity.y + params->spread * randomUnit();
            spark->velocity.z = params->velocity.z + params->spread * randomUnit();
            zge::pointerArrayAdd(emitter->particles, spark);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSparks, Verified);

    void applyGravity(ee::Ptr<zge::Affector> affector, ee::Ptr<Emitter> emitter)
    {
        pullParticles(affector, emitter, [] { return *g_gravity; });
    }
    READABLE_FUNCTION(applyGravity, Verified);

    void applyScreenGravity(ee::Ptr<zge::Affector> affector, ee::Ptr<Emitter> emitter)
    {
        pullParticles(affector, emitter, [] { return *g_screenGravity; });
    }
    READABLE_FUNCTION(applyScreenGravity, Draft);
}
