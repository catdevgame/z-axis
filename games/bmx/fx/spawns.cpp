// BMX XXX's effect starters and the spawn hooks that do nothing: BMX XXX's
// own code (fx.c's block); the hooks word for word Aggressive Inline's
// (games/aggressive/fx/spawns.cpp).

#include "spawns.h"

#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/spawns.cpp.h"

namespace game::bmx
{
    // animationEventHook (the rider, the hook's number): the animation event
    // hook; nothing.
    void animationEventHook(ee::Address /*rider*/, uint32_t /*hook*/)
    {
    }
    READABLE_FUNCTION(animationEventHook, Draft);

    // unusedSpawnHook: empty in this build (fx.c's effect spawn; the beaten
    // record's sparkle, recordEffectFrame, hands it its place, motion, colour
    // and size, and so shows nothing).
    void unusedSpawnHook(ee::Ptr<const zge::Vec4> /*point*/, ee::Ptr<const zge::Vec4> /*velocity*/,
                         ee::Ptr<const uint8_t> /*colour*/, ee::Float /*size*/)
    {
    }
    READABLE_FUNCTION(unusedSpawnHook, Verified);

    namespace
    {
        // The object the level's effects start on (0 before a level has one).
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject;

        // What a tracer starts from: where, and how fast; the other words are
        // the class's own, taken from the game's defaults.
        struct TracerParameters
        {
            zge::Vec4 position; // +0x00
            zge::Vec4 velocity; // +0x10
            uint64_t rest[2];   // +0x20
        };
        static_assert(sizeof(TracerParameters) == 0x30);
        inline ee::Global<TracerParameters, sym::tracerDefaults> tracerDefaults;
        // The effect's name hash ("tracer").
        constexpr uint32_t kTracerEffect = 0x059657A2;

        // Scaffolding (address-call): the engine's start of a named effect on an object.
        namespace calls
        {
        void spawnNamedEffect(uint32_t name, uint32_t object, ee::Address parameters)
        {
            ee::game(sym::spawnNamedEffect, name, object, parameters);
        }
        }
    }

    // spawnTracer: a tracer at `position` flying at `velocity` (the weapons'
    // rounds). Nothing before the level's effects object is there.
    void spawnTracer(const zge::Vec4 *position, const zge::Vec4 *velocity)
    {
        ee::Local<TracerParameters> tracer;
        std::memcpy(&*tracer, &*tracerDefaults, sizeof(TracerParameters));
        if (*g_effectsObject == 0)
            return;
        tracer->position = *position;
        tracer->velocity = *velocity;
        calls::spawnNamedEffect(kTracerEffect, *g_effectsObject, tracer.address());
    }
    READABLE_FUNCTION_WAITS(spawnTracer, Draft);

    namespace
    {
        inline ee::Global<SparkParams, sym::sparksDefaults> g_sparkDefaults;
        // The effect's name hash ("spark").
        constexpr uint32_t kSparkEffect = zge::nameHashOf("spark");
        constexpr ee::Float kSparkThrow = 0.65f;
    }

    // sparks: `count` sparks from a point, thrown along 0.65 of the motion
    // (a grind's). None for a count of 0 or before the level's effects
    // object is there.
    void sparks(const ee::Float *position, const ee::Float *motion, int32_t count)
    {
        ee::Local<SparkParams> spawn;
        *spawn = *g_sparkDefaults;
        if (count == 0)
            return;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        spawn->position = *rider::vec4(position);
        spawn->velocity = *rider::vec4(motion) * kSparkThrow;
        spawn->count = count;
        calls::spawnNamedEffect(kSparkEffect, object, spawn.address());
    }
    READABLE_FUNCTION_WAITS(sparks, Verified);

    namespace
    {
        // The parameters a named effect starts with when the caller gives none.
        constexpr uint32_t kSparkEffectDefaults = sym::sparkEffectDefaults;
        constexpr uint32_t kTracerEffectDefaults = sym::tracerEffectDefaults;
        constexpr uint32_t kDustEffectDefaults = sym::dustEffectDefaults;
        constexpr uint32_t kPosterEffectDefaults = sym::posterDefaults;
        constexpr uint32_t kSleepEffectDefaults = sym::sleepEffectDefaults;
        constexpr uint32_t kSplashEffectDefaults = sym::splashEffectDefaults;
        constexpr uint32_t kSparkName = sym::sparkName;             // "spark"
        constexpr uint32_t kTracerName = sym::tracerName;           // "tracer"
        constexpr uint32_t kShardName = sym::shardName;             // "shard"
        constexpr uint32_t kScreenShardName = sym::screenShardName; // "screenshard"
        constexpr uint32_t kSplashName = sym::splashName;           // "splash"
        constexpr uint32_t kDustName = sym::dustName;               // "dust"
        constexpr uint32_t kPosterName = sym::posterName;           // "poster"
        constexpr uint32_t kSleepName = sym::sleepName;             // "sleep"
        constexpr uint32_t kSplashParticles = 0x20;

        // The effect of that name started at the object with `particles`
        // particles, from the parameters or, with none, the defaults.
        uint32_t startNamed(ee::Ptr<zge::SceneObject> object, uint32_t name, uint32_t particles, ee::Address params, uint32_t defaults)
        {
            if (params == 0)
                params = ee::Address(defaults);
            return game::tail(zge::startNamedEffect, object, name, particles, 0, params);
        }
    }

    // The named effects' starters (the effects' set-up registers each under
    // its effect's name): the effect started at the object with the
    // parameters, or with its defaults.

    // "spark" starts with as many particles as its parameters' count (its low byte).
    uint32_t startSparkEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<SparkParams> params)
    {
        if (!params)
            params = ee::Ptr<SparkParams>(kSparkEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kSparkName, static_cast<uint8_t>(params->count), 0, params);
    }
    READABLE_FUNCTION_WAITS(startSparkEffect, Verified);

    uint32_t startTracerEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return startNamed(object, kTracerName, 1, params, kTracerEffectDefaults);
    }
    READABLE_FUNCTION_WAITS(startTracerEffect, Draft);

    // "shard" and "screenshard" have no defaults here (their spawn has its own).
    uint32_t startShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return game::tail(zge::startNamedEffect, object, kShardName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startShardEffect, Verified);

    uint32_t startScreenShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return game::tail(zge::startNamedEffect, object, kScreenShardName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startScreenShardEffect, Verified);

    // "splash" starts with 32 particles.
    uint32_t startSplashEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return startNamed(object, kSplashName, kSplashParticles, params, kSplashEffectDefaults);
    }
    READABLE_FUNCTION_WAITS(startSplashEffect, Draft);

    uint32_t startDustEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return startNamed(object, kDustName, 1, params, kDustEffectDefaults);
    }
    READABLE_FUNCTION_WAITS(startDustEffect, Draft);

    uint32_t startPosterEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return startNamed(object, kPosterName, 1, params, kPosterEffectDefaults);
    }
    READABLE_FUNCTION_WAITS(startPosterEffect, Verified);

    uint32_t startSleepEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return startNamed(object, kSleepName, 1, params, kSleepEffectDefaults);
    }
    READABLE_FUNCTION_WAITS(startSleepEffect, Verified);
}
