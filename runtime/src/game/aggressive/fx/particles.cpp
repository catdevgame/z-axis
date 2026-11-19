// The particle effects' classes, their emitters' creation, spawning and
// moving (inlineFx.c; readable round gamehand). The classes' names are the
// ones the effects library registers them by (initInlineFx); see
// particles.h for the class table and the particle's layout. The draws of
// "vapor" are in effects.cpp (round 13).

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/particles.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/math/random.h"
#include "zge/container/cursor_array.h"
#include "zge/memory/regions.h"

#include <cstring>
#include "sym/aggressive/fx/particles.cpp.h"

namespace aggressive::fx
{

    namespace
    {
        constexpr uint32_t kEffectsClock = sym::effectsClock;   // effectsClock below
        constexpr uint32_t kAllocateSized = sym::allocateSized;  // (bytes) -> a block of the smallest size-class pool that fits
        constexpr uint32_t kFreeSized = sym::freeSized;      // (block, bytes) back to its size-class pool
        constexpr uint32_t kListAppend = sym::pointerArrayAdd;     // (CursorArray *, item)
        constexpr uint32_t kRemoveCurrent = sym::pointerArrayRemoveCurrent;  // (CursorArray *) removes the item at the cursor (the last moves there)

        // The affectors' constants (.sdata).
        inline ee::Global<ee::Float, sym::g_screenGravityStep> g_screenGravityStep; // 0.2 a tick
        inline ee::Global<ee::Float, sym::g_smokeFloorBounce> g_smokeFloorBounce;  // -0.3
        inline ee::Global<ee::Float, sym::g_smokeFloor> g_smokeFloor;        // -2 (y down)
        // "spark": the texture and the spawn's defaults (no parameters given).
        inline ee::Global<uint32_t, sym::g_sparkTexture> g_sparkTexture;
        constexpr uint32_t kSparkDefaults = sym::sparkDefaults;
        // "firetrail": set to have every flame freed at the next move; the
        // flame's ten frames and how long each shows.
        inline ee::Global<uint32_t, sym::g_clearFiretrails> g_clearFiretrails;
        inline ee::Global<uint32_t[10], sym::g_fireFrames> g_fireFrames;
        inline ee::Global<ee::Float, sym::g_fireFrameTime2> g_fireFrameTime;
        // "smoke": the wind every puff drifts with (units a second).
        inline ee::Global<zge::Vec4, sym::g_smokeWind> g_smokeWind;

        // Each class's particle size (its pool).
        constexpr uint32_t kUserSparkBytes = 0x70;
        constexpr uint32_t kSparkBytes = 0x90;
        constexpr uint32_t kFiretrailBytes = 0x80;
        constexpr uint32_t kSmokeBytes = 0xC0;

        // A spark follows the object it came off (a skater's wheel, a
        // grind's contact point): what it was spawned with.
        struct Spark : Particle
        {
            ee::Ptr<skater::Player> source; // +0x70 the skater it came off
            uint8_t _74[0x0C];
            zge::Vec4 sourceAt;             // +0x80 the skater's position the last tick
        };
        static_assert(offsetof(Spark, source) == 0x70 && offsetof(Spark, sourceAt) == 0x80 && sizeof(Spark) == kSparkBytes);
        // Sparks move with a skater while it grinds (on its feet, a hand or both).
        bool dragsSparks(skater::State state)
        {
            return state >= skater::State::Grind && state <= skater::State::DoubleGrind;
        }

        // "userspark": position, velocity and lifetime each with their own spread.
        struct UserSparkParams
        {
            uint8_t _00[0x10];
            zge::Vec4 velocity;       // +0x10
            zge::Vec4 velocitySpread; // +0x20
            zge::Vec4 position;       // +0x30
            zge::Vec4 positionSpread; // +0x40
            uint8_t _50[0x10];
            uint32_t ownTexture;      // +0x60 nonzero: the texture below, else the spark texture
            uint32_t texture;         // +0x64
            ee::Float size;           // +0x68
            uint32_t _6c;
            ee::Float lifetime;       // +0x70 milliseconds
            ee::Float lifetimeSpread; // +0x74
        };
        static_assert(offsetof(UserSparkParams, positionSpread) == 0x40 && offsetof(UserSparkParams, lifetimeSpread) == 0x74);
        // "spark"'s parameters when a spawn is given none; "userspark" takes
        // its colour from them.
        inline ee::Global<SparkParams, sym::sparkDefaults> g_sparkDefaults;

        // A random number in [-1, 1) in steps of 1/128.
        ee::Float randomUnit()
        {
            const uint32_t r = zge::randomNumber();
            return ee::toFloat(static_cast<int32_t>((r & 0xFFu) - 0x80)) * ee::Float(0.0078125f);
        }

        // Every class's emitter: room for `capacity` particles of its size
        // (`returnLeader`: the original's epilogue block).
        uint32_t newEmitter(uint32_t effectClass, uint32_t capacity, uint32_t particleBytes, uint32_t returnLeader)
        {
            const uint32_t address = zge::heapAllocate(static_cast<uint32_t>(sizeof(Emitter)));
            if (address)
            {
                Emitter &emitter = *ee::at<Emitter>(address);
                const ee::Ptr<CursorArray> particles = zge::newCursorArray(capacity & 0xFFFFu);
                emitter.effectClass = effectClass;
                emitter.particles = particles;
                emitter.particleBytes = particleBytes;
                emitter.lastMoved = static_cast<uint32_t>(effectsClock());
            }
            return address;
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void freeSized(ee::Address memory, uint32_t bytes) { ee::tailCall(kFreeSized, memory, bytes); }
        }
    }

    // freeParticle: a particle back to its emitter's pool.
    void freeParticle(ee::Ptr<Emitter> emitter, ee::Address particle)
    {
        const uint32_t bytes = emitter->particleBytes;
        *g_particlesAlive = *g_particlesAlive - 1;
        tail::freeSized(particle, bytes);
    }
    READABLE_FUNCTION_WAITS(freeParticle, Verified);

    // The classes' "make an emitter" methods (+0x10): (class, capacity).
    // newUserSparkEmitter: "userspark" (also "snow", "rain", "vapor").
    uint32_t newUserSparkEmitter(uint32_t effectClass, uint32_t capacity)
    {
        namespace here = points::newUserSparkEmitter;
        return newEmitter(effectClass, capacity, kUserSparkBytes, here::taken);
    }
    READABLE_FUNCTION_WAITS(newUserSparkEmitter, Verified);

    // newSparkEmitter: "spark".
    uint32_t newSparkEmitter(uint32_t effectClass, uint32_t capacity)
    {
        namespace here = points::newSparkEmitter;
        return newEmitter(effectClass, capacity, kSparkBytes, here::taken);
    }
    READABLE_FUNCTION_WAITS(newSparkEmitter, Verified);

    // newFiretrailEmitter: "firetrail".
    uint32_t newFiretrailEmitter(uint32_t effectClass, uint32_t capacity)
    {
        namespace here = points::newFiretrailEmitter;
        return newEmitter(effectClass, capacity, kFiretrailBytes, here::taken);
    }
    READABLE_FUNCTION_WAITS(newFiretrailEmitter, Verified);

    // newSmokeEmitter: "smoke" and "usersmoke".
    uint32_t newSmokeEmitter(uint32_t effectClass, uint32_t capacity)
    {
        namespace here = points::newSmokeEmitter;
        return newEmitter(effectClass, capacity, kSmokeBytes, here::taken);
    }
    READABLE_FUNCTION_WAITS(newSmokeEmitter, Verified);

    // moveParticles: the move most classes share (their +0x1C, or called at
    // the end of their own), once a clock tick at most: particles whose
    // life is over are freed, moving ones step by their velocity over the
    // tick (the tail left at the old position), spinning ones turn their
    // axis; then the effect's affectors (gravity, floor collision ...) each
    // run on the emitter. Returns how many particles it looked at.
    int32_t moveParticles(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        int32_t looked = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < now))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++looked)
        {
            Particle &p = *ee::Ptr<Particle>(particle);
            if (now - p.born >= p.lifetime)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            if (p.flags & kParticleMoves)
            {
                p.tail = p.position;
                // The tick reaches VU0 in a register whose upper half is the
                // tail's z and w, which stay in VU0's upper lanes.
                const zge::Vec4 step = zge::scaledCarrying(p.velocity, *g_tickTime, upperHalf(p.tail));
                p.position = p.position + step;
            }
            if (p.flags & kParticleSpins)
                zge::vec3RotateEuler(asVec3(p.axis), asVec3(p.axis), &p.spin);
            particle = next(*emitter->particles);
        }
        runAffectors(effect, emitter);
        emitter->lastMoved = now;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveParticles, Verified);

    // applyGravity: the "gravity" affector (its object at g_gravityAffectorClass): every
    // particle's fall speeds up by Physics / Gravity over the tick, once a
    // tick (before the move marks it).
    void applyGravity(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        namespace here = points::applyGravity;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
            while (particle)
            {
                Particle &p = *ee::Ptr<Particle>(particle);
                const ee::Float gravity = (*g_tuningCategories)[3].entries->value; // Physics / Gravity
                p.velocity.y = p.velocity.y + gravity * *g_tickTime;
                particle = next(*emitter->particles, here::join);
                if (!particle)
                    break;
            }
        }
    }
    READABLE_FUNCTION(applyGravity, Verified);

    // applyScreenGravity: the "gravity_screen" affector (its object at g_gravityScreenAffectorClass):
    // once a tick every particle falls faster by a fixed 0.2 (screen units,
    // y down).
    void applyScreenGravity(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        namespace here = points::applyScreenGravity;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
            if (particle)
            {
                const ee::Float step = *g_screenGravityStep;
                for (;;)
                {
                    Particle &p = *ee::Ptr<Particle>(particle);
                    p.velocity.y = p.velocity.y + step;
                    particle = next(*emitter->particles, here::join);
                    if (!particle)
                        break;
                }
            }
        }
    }
    READABLE_FUNCTION(applyScreenGravity, Draft);

    // applyShardGravity: the "shard_gravity" affector (g_shardGravityAffectorClass): once a tick
    // every shard not yet still falls faster by half of Physics / Gravity
    // over the tick.
    void applyShardGravity(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        namespace here = points::applyShardGravity;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
            if (particle)
            {
                const ee::Float tick = *g_tickTime;
                for (;;)
                {
                    Shard &shard = *ee::Ptr<Shard>(particle);
                    if (!(shard.shardFlags & kShardStill))
                    {
                        const ee::Float gravity = (*g_tuningCategories)[kTuningPhysics].entries->value; // Physics / Gravity
                        shard.velocity.y = shard.velocity.y + gravity * (tick * ee::Float(0.5f));
                    }
                    particle = next(*emitter->particles, here::join);
                    if (!particle)
                        break;
                }
            }
        }
    }
    READABLE_FUNCTION(applyShardGravity, Verified);

    // The "floorcoll" affector's argument.
    struct FloorBounce
    {
        uint32_t _00;
        ee::Float bounce; // +0x04 the velocity kept by a bounce
    };

    // bounceOffFloor: the "floorcoll" affector (g_floorCollisionAffectorClass; argument a
    // FloorBounce), every move (no tick test): a shard that reached its
    // floor (y down: at or past it) is put on it and bounces, its velocity
    // times the bounce, the fall's turned up; under 1 a second up, it stays
    // still on the floor.
    void bounceOffFloor(uint32_t affector, ee::Ptr<Emitter> emitter, ee::Ptr<FloorBounce> argument)
    {
        namespace here = points::bounceOffFloor;
        const ee::Float bounce = argument->bounce;
        if (!affector || !emitter)
            return;
        uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
        if (!particle)
            return;
        for (;;)
        {
            Shard &shard = *ee::Ptr<Shard>(particle);
            const uint32_t flags = shard.shardFlags;
            if (!(flags & kShardStill))
            {
                const ee::Float floor = shard.floorHeight;
                if (floor <= shard.position.y)
                {
                    const ee::Float fall = bounce * shard.velocity.y;
                    const ee::Float z = shard.velocity.z;
                    const ee::Float x = bounce * shard.velocity.x;
                    shard.position.y = floor;
                    const ee::Float up = -fall;
                    shard.velocity.x = x;
                    shard.velocity.z = bounce * z;
                    shard.velocity.y = up;
                    if (ee::abs(up) < ee::Float(1.0f))
                    {
                        shard.velocity.y = ee::Float(0.0f);
                        shard.shardFlags = flags | kShardStill;
                    }
                }
            }
            particle = next(*emitter->particles, here::join2);
            if (!particle)
                break;
        }
    }
    READABLE_FUNCTION(bounceOffFloor, Verified);

    // bounceSmokeOffFloor: the "smoke_floor_coll" affector (g_smokeFloorCollisionAffectorClass): once a
    // tick every puff at or below the smoke's floor (y -2) has its rise
    // turned back at 0.3 (dropped under 1).
    void bounceSmokeOffFloor(uint32_t affector, ee::Ptr<Emitter> emitter)
    {
        namespace here = points::bounceSmokeOffFloor;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (now != emitter->lastMoved)
        {
            if (!affector || !emitter)
                return;
            uint32_t particle = first(*emitter->particles, here::taken, here::taken2);
            if (particle)
            {
                const ee::Float floor = *g_smokeFloor;
                const ee::Float turn = *g_smokeFloorBounce;
                for (;;)
                {
                    Particle &p = *ee::Ptr<Particle>(particle);
                    if (floor <= p.position.y)
                    {
                        const ee::Float rise = p.velocity.y * turn;
                        p.velocity.y = rise;
                        if (ee::abs(rise) < ee::Float(1.0f))
                            p.velocity.y = ee::Float(0.0f);
                    }
                    particle = next(*emitter->particles, here::join);
                    if (!particle)
                        break;
                }
            }
        }
    }
    READABLE_FUNCTION(bounceSmokeOffFloor, Verified);

    // moveSparks: "spark"'s move: sparks off a grinding skater move with
    // it (by how far it went since the last tick), then the common move.
    int32_t moveSparks(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            Spark &spark = *ee::Ptr<Spark>(particle);
            if (dragsSparks(spark.source->state))
            {
                spark.position = spark.position - spark.sourceAt;
                spark.sourceAt = spark.source->position;
                spark.position = spark.position + spark.sourceAt;
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveSparks, Verified);

    // moveFiretrail: "firetrail"'s move: all flames freed if asked
    // (g_clearFiretrails), each flame's animation stepped (ten frames, each
    // g_fireFrameTime long), then the common move.
    int32_t moveFiretrail(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        if (*g_clearFiretrails)
        {
            if (emitter && emitter->particles)
            {
                for (uint32_t particle = first(*emitter->particles); particle;)
                    particle = freeCurrent(emitter, particle);
                emitter->lastMoved = static_cast<uint32_t>(effectsClock());
            }
            *g_clearFiretrails = 0;
        }
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                const ee::Float tick = *g_tickTime;
                Flame &flame = *ee::Ptr<Flame>(particle);
                if (flame.frameLeft <= tick)
                {
                    flame.frame = flame.frame + 1;
                    if (flame.frame == 10)
                        flame.frame = 0;
                    flame.texture = (*g_fireFrames)[flame.frame];
                    flame.frameLeft = *g_fireFrameTime;
                }
                else
                    flame.frameLeft = flame.frameLeft - tick;
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveFiretrail, Verified);

    // moveSmoke: "smoke"'s and "usersmoke"'s move: each puff drifts by
    // its own velocity and the wind over the tick, then the common move.
    int32_t moveSmoke(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                Puff &puff = *ee::Ptr<Puff>(particle);
                puff.position = puff.position + puff.drift * *g_tickTime;
                puff.position = puff.position + *g_smokeWind * *g_tickTime;
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveSmoke, Verified);

    // The classes' spawns (+0x18): (effect, -, emitter, count, -, parameters).
    // spawnSparks: "spark": `count` sparks at the parameters' position
    // (the defaults if none), velocity spread at random on each axis,
    // lifetime +-50 %, following the source object.
    void spawnSparks(uint32_t effect, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<SparkParams> parameters)
    {
        count &= 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        const SparkParams &params = parameters ? *parameters : *g_sparkDefaults;
        if (!effect || !emitter || !count)
            return;
        for (uint32_t spawned = 0; spawned < count; ++spawned)
        {
            const ee::Ptr<Spark> spark = newParticle<Spark>(*emitter);
            if (!spark)
                break;
            spark->born = now;
            spark->flags = kParticleMoves;
            spark->texture = *g_sparkTexture;
            spark->size = params.size;
            std::memcpy(spark->colour, params.colour, 4);
            const ee::Float lifetime = ee::toFloat(params.lifetime);
            // -8 .. 7 sixteenths of the lifetime added.
            const int32_t sixteenths = static_cast<int32_t>((zge::randomNumber() & 0xFu) - 8);
            spark->lifetime = static_cast<uint32_t>(ee::toInt(ee::toFloat(sixteenths) * ee::Float(0.0625f) * lifetime + lifetime));
            spark->tail = params.position;
            spark->position = spark->tail;
            spark->velocity.x = params.velocity.x + params.spread * randomUnit();
            spark->velocity.y = params.velocity.y + params.spread * randomUnit();
            spark->velocity.z = params.velocity.z + params.spread * randomUnit();
            spark->source = params.source;
            spark->sourceAt = params.source->position;
            zge::pointerArrayAdd(emitter->particles, spark);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSparks, Verified);

    // spawnUserSparks: "userspark": the effect's own position, velocity and
    // lifetime, each with its spread; the spark colour of the defaults.
    void spawnUserSparks(uint32_t effect, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<UserSparkParams> params)
    {
        count &= 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!effect || !emitter || !count)
            return;
        for (uint32_t spawned = 0; spawned < count; ++spawned)
        {
            const ee::Ptr<Particle> spark = newParticle(*emitter);
            if (!spark)
                break;
            spark->born = now;
            spark->flags = kParticleMoves;
            spark->texture = params->ownTexture ? params->texture : *g_sparkTexture;
            spark->size = params->size;
            std::memcpy(spark->colour, g_sparkDefaults->colour, 4);
            spark->lifetime = static_cast<uint32_t>(ee::toInt(params->lifetime + params->lifetimeSpread * randomUnit()));
            spark->tail.x = params->position.x + params->positionSpread.x * randomUnit();
            spark->tail.y = params->position.y + params->positionSpread.y * randomUnit();
            spark->tail.z = params->position.z + params->positionSpread.z * randomUnit();
            spark->position = spark->tail;
            spark->velocity.x = params->velocity.x + params->velocitySpread.x * randomUnit();
            spark->velocity.y = params->velocity.y + params->velocitySpread.y * randomUnit();
            spark->velocity.z = params->velocity.z + params->velocitySpread.z * randomUnit();
            zge::pointerArrayAdd(emitter->particles, spark);
        }
    }
    READABLE_FUNCTION_WAITS(spawnUserSparks, Verified);

    // spawnVapor: "vapor": `count` still streaks between the parameters'
    // two points (drawn by drawParticles, expired by expireParticles).
    void spawnVapor(uint32_t effect, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<VaporParams> params)
    {
        count &= 0xFFu;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!effect || !emitter || !count)
            return;
        for (uint32_t spawned = 0; spawned < count; ++spawned)
        {
            const ee::Ptr<Particle> streak = newParticle(*emitter);
            if (!streak)
                break;
            streak->born = now;
            streak->flags = params->flags;
            streak->texture = params->texture;
            streak->size = params->size;
            std::memcpy(streak->colour, params->colour, 4);
            streak->lifetime = params->lifetime;
            streak->position = params->head;
            streak->tail = params->tail;
            streak->velocity = zge::kZero;
            zge::pointerArrayAdd(emitter->particles, streak);
        }
    }
    READABLE_FUNCTION_WAITS(spawnVapor, Verified);

    // Empty; registerDebugMenuFunctions.
    void debugMenuSecondHook()
    {
    }
    READABLE_FUNCTION(debugMenuSecondHook, Verified);

    uint32_t alwaysZero()
    {
        return 0;
    }
    READABLE_FUNCTION(alwaysZero, Verified);
}
