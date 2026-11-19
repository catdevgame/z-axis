#pragma once
// BMX XXX's effect starters (fx.c; spawns.cpp): what the rider's and the
// weapons' code asks for, and the named effects' starts. DM2's
// (game/dm2/fx/spawns.h) with the tracer added.

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // The spawn parameters of "spark".
    struct SparkParams
    {
        zge::Vec4 position; // +0x00
        zge::Vec4 velocity; // +0x10
        int32_t count;      // +0x20 sparks a spawn
        ee::Float size;     // +0x24
        ee::Float spread;   // +0x28
        int32_t lifetime;   // +0x2C milliseconds
        uint8_t colour[4];  // +0x30
        uint8_t _34[0x0C];
    };
    static_assert(offsetof(SparkParams, count) == 0x20 && offsetof(SparkParams, colour) == 0x30 && sizeof(SparkParams) == 0x40);

    // `count` sparks from a point, thrown along 0.65 of the motion; none
    // for a count of 0.
    void sparks(const ee::Float *position, const ee::Float *motion, int32_t count);
    // Empty in this build: an effect at the point with its velocity, colour
    // (RGBA) and size (the beaten record's sparkle hands it these).
    void unusedSpawnHook(ee::Ptr<const zge::Vec4> point, ee::Ptr<const zge::Vec4> velocity, ee::Ptr<const uint8_t> colour,
                         ee::Float size);
    // A tracer at `position` flying at `velocity` (the weapons' rounds).
    void spawnTracer(const zge::Vec4 *position, const zge::Vec4 *velocity);

    // The named effects' starters: the effect at the object with the
    // parameters (0: its defaults). Each returns startNamedEffect's result.
    uint32_t startSparkEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<SparkParams> params);
    uint32_t startTracerEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startScreenShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startSplashEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startDustEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startPosterEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startSleepEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
}
