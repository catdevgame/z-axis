// BMX XXX's particle emitters (particles.h): a particle freed to its pool.
// BMX XXX's own code (fx.c's block), word for word Aggressive Inline's
// (game/aggressive/fx/particles.cpp).

#include "particles.h"

#include "effects_clock.h"
#include "spawns.h"
#include "zge/math/random.h"
#include "zge/scene/scene.h"
#include "zge/scene/node.h"
#include "zge/scene/camera.h"
#include "zge/render/primitives.h"
#include "zge/math/matrix.h"
#include <cstring>

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/container/cursor_array.h"
#include "zge/container/pointer_array.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"

#include <cstdint>
#include "sym/bmx/fx/particles.cpp.h"

namespace game::bmx
{
    namespace
    {
        // How many particles are alive (every emitter's together).
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
    }

    // freeParticle: a particle back to its emitter's pool (its class's size).
    void freeParticle(ee::Ptr<Emitter> emitter, ee::Address particle)
    {
        const uint32_t bytes = emitter->particleBytes;
        *g_particlesAlive = *g_particlesAlive - 1;
        game::tail(zge::freeSized, particle, bytes);
    }
    READABLE_FUNCTION_WAITS(freeParticle, Verified);

    namespace
    {
        // The step the screen gravity affector adds to every particle each
        // tick. In BMX XXX this datum is the game's gravity, the rider's
        // (game/bmx/rider names it g_gravity); Aggressive Inline's is its
        // own fixed 0.2, and its name is what binds here.
        inline ee::Global<ee::Float, sym::g_screenGravityStep> g_gravity;
        // How much faster a shard falls each tick (a float of its own, read
        // by the shards' gravity affector alone).
        inline ee::Global<ee::Float, sym::g_shardGravityStep> g_shardGravityStep;
    }

    // applyScreenGravity: the "gravity_screen" affector: once a tick every
    // particle falls faster by the game's gravity. Aggressive Inline's words
    // (its step is a fixed 0.2).
    void applyScreenGravity(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles);
            if (particle)
            {
                const ee::Float step = *g_gravity;
                for (;;)
                {
                    Particle &p = *ee::Ptr<Particle>(particle);
                    p.velocity.y = p.velocity.y + step;
                    particle = next(*emitter->particles);
                    if (!particle)
                        break;
                }
            }
        }
    }
    READABLE_FUNCTION(applyScreenGravity, Verified);

    // applyShardGravity: the shards' gravity affector: once a tick every
    // shard falls faster by the shards' step. Aggressive Inline's skips a
    // shard lying still and scales the tuning table's gravity by the tick;
    // BMX XXX's adds its step to every shard.
    void applyShardGravity(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles);
            if (particle)
            {
                const ee::Float step = *g_shardGravityStep;
                for (;;)
                {
                    Particle &shard = *ee::Ptr<Particle>(particle);
                    shard.velocity.y = shard.velocity.y + step;
                    particle = next(*emitter->particles);
                    if (!particle)
                        break;
                }
            }
        }
    }
    READABLE_FUNCTION(applyShardGravity, Verified);

    namespace
    {
        // An emitter's box before any particle is in it.
        inline ee::Global<ee::Float, sym::emptyBoundsMin> emptyBoundsMin;
        inline ee::Global<ee::Float, sym::emptyBoundsMax> emptyBoundsMax;
        inline ee::Global<ee::Float, sym::sleepEmptyBoundsMin> sleepEmptyBoundsMin;
        inline ee::Global<ee::Float, sym::sleepEmptyBoundsMax> sleepEmptyBoundsMax;
    }

    // moveParticles: the move most classes share, once a clock tick at most:
    // particles whose life is over are freed, moving ones step by their
    // velocity (the tail left at the old position), every one widens the
    // emitter's box, spinning ones turn their axis; then the effect's
    // affectors (gravity, floor collision ...) each run on the emitter.
    // Returns how many particles it looked at. Aggressive Inline's, with the
    // step not scaled by the tick's time and the emitter's box kept.
    namespace
    {
    int32_t moveEmitter(uint32_t effect, ee::Ptr<Emitter> emitter, const ee::Float &emptyMin, const ee::Float &emptyMax)
    {
        int32_t looked = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < now))
            return 0;
        for (uint32_t lane = 0; lane < 3; ++lane)
        {
            emitter->boundsMin[lane] = emptyMin;
            emitter->boundsMax[lane] = emptyMax;
        }
        for (uint32_t particle = first(*emitter->particles); particle; ++looked)
        {
            Particle &p = *ee::Ptr<Particle>(particle);
            if (now - p.born >= p.lifetime)
            {
                zge::pointerArrayRemoveCurrent(emitter->particles);
                freeParticle(emitter, particle);
                particle = zge::current(emitter->particles);
                continue;
            }
            if (p.flags & kParticleMoves)
            {
                p.tail = p.position;
                p.position = p.position + p.velocity;
            }
            const ee::Float *const at = &p.position.x;
            for (uint32_t lane = 0; lane < 3; ++lane)
                emitter->boundsMin[lane] = ee::min(at[lane], emitter->boundsMin[lane]);
            for (uint32_t lane = 0; lane < 3; ++lane)
                emitter->boundsMax[lane] = ee::max(at[lane], emitter->boundsMax[lane]);
            if (p.flags & kParticleSpins)
                zge::vec3RotateEuler(&p.axis.xyz(), &p.axis.xyz(), &p.spin);
            particle = next(*emitter->particles);
        }
        for (ee::Ptr<AffectorLink> link = ee::Ptr<ClassAffectors>(effect)->affectors; link; link = link->next)
            link->affector->apply(link->affector, emitter, link->argument);
        emitter->lastMoved = now;
        return looked;
    }
    }

    int32_t moveParticles(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        return moveEmitter(effect, emitter, *emptyBoundsMin, *emptyBoundsMax);
    }
    READABLE_FUNCTION_WAITS(moveParticles, Verified);

    // moveSleep ("sleep" +0x1C): the same move, with the class's own copies
    // of the empty box's two numbers.
    int32_t moveSleep(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        return moveEmitter(effect, emitter, *sleepEmptyBoundsMin, *sleepEmptyBoundsMax);
    }
    READABLE_FUNCTION_WAITS(moveSleep, Verified);

    namespace
    {
        // Each class's particle record: the common start, then the class's own.
        constexpr uint32_t kSparkBytes = 0x70, kTracerBytes = 0xD0, kShardBytes = 0x120, kPosterBytes = 0xB0, kSleepBytes = 0x80;
        // Every class's emitter: a list for `capacity` particles; the
        // particles themselves come from the pools by size as they spawn.
        ee::Ptr<Emitter> newEmitter(uint32_t effectClass, uint16_t capacity, uint32_t particleBytes)
        {
            const ee::Ptr<Emitter> emitter(zge::heapAllocate(sizeof(Emitter)));
            if (emitter)
            {
                emitter->particles = zge::newCursorArray(capacity);
                emitter->effectClass = effectClass;
                emitter->particleBytes = particleBytes;
                emitter->lastMoved = static_cast<uint32_t>(effectsClock());
            }
            return emitter;
        }
    }

    ee::Ptr<Emitter> newSparkEmitter(uint32_t effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kSparkBytes);
    }
    READABLE_FUNCTION_WAITS(newSparkEmitter, Verified);

    ee::Ptr<Emitter> newTracerEmitter(uint32_t effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kTracerBytes);
    }
    READABLE_FUNCTION_WAITS(newTracerEmitter, Draft);

    ee::Ptr<Emitter> newShardEmitter(uint32_t effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kShardBytes);
    }
    READABLE_FUNCTION_WAITS(newShardEmitter, Verified);

    ee::Ptr<Emitter> newPosterEmitter(uint32_t effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kPosterBytes);
    }
    READABLE_FUNCTION_WAITS(newPosterEmitter, Verified);

    ee::Ptr<Emitter> newSleepEmitter(uint32_t effectClass, uint16_t capacity)
    {
        return newEmitter(effectClass, capacity, kSleepBytes);
    }
    READABLE_FUNCTION_WAITS(newSleepEmitter, Verified);

    // freeEmitter (the classes' free): every particle the emitter still has
    // goes back to its pool, then its list and the emitter itself are freed.
    void freeEmitter(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        if (emitter->particles)
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = zge::current(emitter->particles))
            {
                zge::pointerArrayRemoveCurrent(emitter->particles);
                freeParticle(emitter, particle);
            }
            zge::freePointerArray(emitter->particles);
        }
        emitter->particleBytes = 0;
        game::tail(zge::heapFree, emitter);
    }
    READABLE_FUNCTION_WAITS(freeEmitter, Verified);

    namespace
    {
        // The effects' budget of particles (every emitter's together).
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;
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
    }

    // spawnSparks ("spark" +0x18): `count` sparks at the parameters'
    // position, each with the class's material, the parameters' size and
    // colour, a lifetime up to a half off the parameters', and a velocity up
    // to the spread off theirs. DM2's, with BMX XXX's budget. Kept bug: with
    // the effects' budget of particles used up no spark is allocated, and
    // the spark is written at address 0 and added to the emitter.
    void spawnSparks(uint32_t effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<SparkParams> params)
    {
        uint32_t left = count & 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<SparkParams>(kSparkEffectDefaults);
        if (!effectClass || !emitter)
            return;
        while (left != 0)
        {
            uint32_t made = 0;
            if (*g_particlesAlive < *g_particleBudget)
            {
                *g_particlesAlive = *g_particlesAlive + 1;
                made = zge::allocateSized(emitter->particleBytes);
            }
            const ee::Ptr<Particle> spark(made);
            spark->born = now;
            spark->flags = kParticleMoves;
            spark->texture = *g_sparkMaterial;
            spark->size = params->size;
            std::memcpy(spark->colour, params->colour, 4);
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

    namespace
    {
        // A spark's colour by how much of its life is gone, in sixteenths.
        constexpr uint32_t kSparkShades = 16;
        inline ee::Global<uint8_t[kSparkShades][4], sym::g_sparkColours> g_sparkColours;
        // How long a spark's streak is drawn against the way it moved in a tick.
        constexpr ee::Float kStreakStretch = 1.75f;
        // drawPrimitiveQuads' flags for a quad in the world, through a matrix.
        constexpr uint32_t kWorldQuads = 2;

        // What drawSparks keeps in its frame, as the original lays it out.
        struct SparkDraw
        {
            QuadVertex corners[4];                // +0x00
            Quad quad;                            // +0x80 the corners from the last back to the first
            uint8_t _94[0x0C];
            zge::Matrix camera;                   // +0xA0 the camera's matrix, moved to each spark's head
            zge::Matrix toCamera;                 // +0xE0 the world's directions in the camera's frame
            uint8_t colour[4];                    // +0x120
            uint8_t _124[0x0C];
            zge::Vec4 streak;                     // +0x130 from the head to the far end, in the camera's frame
            zge::Vec4 along;                      // +0x140 the streak's direction on screen
            zge::Vec4 across;                     // +0x150 the direction across it
            zge::Vec4 head;                       // +0x160 the spark's position in the camera's frame
            zge::Vec4 tail;                       // +0x170 last tick's
            zge::Vec4 alongOffset;                // +0x180
            zge::Vec4 acrossOffset;               // +0x190
            ee::Ptr<Emitter> emitter;             // +0x1A0
            int32_t now;                          // +0x1A4
            ee::Ptr<zge::Matrix> cameraPlace;     // +0x1A8 (the original keeps these four addresses)
            ee::Ptr<zge::Vec4> alongOffsetPlace;  // +0x1AC
            ee::Ptr<zge::Vec4> headInMatrix;      // +0x1B0
            ee::Ptr<zge::Vec4> acrossOffsetPlace; // +0x1B4
        };
        static_assert(offsetof(SparkDraw, quad) == 0x80 && offsetof(SparkDraw, camera) == 0xA0 && offsetof(SparkDraw, colour) == 0x120);
        static_assert(offsetof(SparkDraw, streak) == 0x130 && offsetof(SparkDraw, emitter) == 0x1A0 && offsetof(SparkDraw, acrossOffsetPlace) == 0x1B4);
    }

    // drawSparks ("spark" +0x20): each spark a streak from where it is back
    // along the way it moved in the last tick (1.75 of it), as wide as its
    // size, in the colour of its age, fading to nothing at the far end.
    // Returns how many it drew. Aggressive Inline's without the cap on a
    // streak's length.
    int32_t drawSparks(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<SparkDraw> draw;
        draw->emitter = emitter;
        int32_t drawn = 0;
        for (uint32_t i = 0; i < 4; ++i)
            draw->quad.corners[i] = &draw->corners[3 - i];
        const ee::Ptr<zge::SceneObject> cameraObject(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)));
        const ee::Ptr<zge::Matrix> cameraMatrix = zge::objectMatrix(cameraObject, 1);
        draw->cameraPlace = &draw->camera;
        draw->camera = *cameraMatrix;
        draw->toCamera = draw->camera;
        zge::matrixTranspose3(&draw->toCamera, &draw->toCamera);
        draw->toCamera.translation() = zge::kZero;
        const int32_t now = effectsClock();
        draw->now = now;
        uint32_t particle = first(*emitter->particles);
        if (!particle)
            return drawn;
        draw->headInMatrix = &draw->camera.translation();
        draw->alongOffsetPlace = &draw->alongOffset;
        draw->acrossOffsetPlace = &draw->acrossOffset;
        do
        {
            const Particle &spark = *ee::Ptr<Particle>(particle);
            const int32_t shade = ee::toInt(ee::toFloat(now - static_cast<int32_t>(spark.born)) * ee::Float(15.0f) /
                                            ee::toFloat(static_cast<int32_t>(spark.lifetime)));
            std::memcpy(draw->colour, (*g_sparkColours)[static_cast<uint32_t>(shade) < kSparkShades ? shade : kSparkShades - 1], 4);
            // The quad's matrix moved to the head; the head and tail in the
            // camera's frame, and the streak between them.
            draw->camera.translation() = spark.position;
            draw->head = zge::transformed(draw->toCamera, spark.position);
            draw->tail = zge::transformed(draw->toCamera, spark.tail);
            draw->streak = draw->tail - draw->head;
            draw->streak = draw->streak * kStreakStretch;
            // Its direction on screen (x, y), and the one across it.
            draw->along = draw->streak;
            draw->along.z = ee::Float(0.0f);
            const ee::Float length = zge::vec3Length(&draw->along.xyz());
            if (ee::Float(0.0f) < length)
                draw->along = draw->along * (ee::Float(1.0f) / length);
            else
            {
                draw->along.z = ee::Float(0.0f);
                draw->along.x = ee::Float(1.0f);
                draw->along.y = ee::Float(0.0f);
            }
            ++drawn;
            draw->across.z = ee::Float(0.0f);
            draw->across.x = draw->along.y;
            draw->across.y = -draw->along.x;
            // The corners: 0 and 1 at the head, 2 and 3 at the far end, the
            // streak's width either side.
            for (uint32_t i = 0; i < 4; ++i)
            {
                const bool farEnd = (i & 2u) != 0;
                const bool otherSide = ((i + 1) & 2u) != 0;
                QuadVertex &corner = draw->corners[i];
                std::memcpy(corner.colour, draw->colour, 4);
                corner.u = farEnd ? ee::Float(1.0f) : ee::Float(0.0f);
                corner.v = otherSide ? ee::Float(1.0f) : ee::Float(0.0f);
                draw->alongOffset = draw->along * (farEnd ? -spark.size : spark.size);
                draw->acrossOffset = draw->across * (otherSide ? -spark.size : spark.size);
                corner.position = draw->alongOffset + draw->acrossOffset;
                if (farEnd)
                {
                    // The far end: along the streak, transparent.
                    corner.position = corner.position + draw->streak;
                    corner.colour[3] = 0;
                }
            }
            draw->quad.texture = spark.texture;
            zge::drawPrimitiveQuads(kWorldQuads, &draw->quad, 1, &draw->camera);
            particle = next(*emitter->particles);
        } while (particle);
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSparks, Verified);
}
