#pragma once

// DM2's effect spawns (fx.c; spawns.cpp): what the rider's code asks for.
// Each fills the effect's spawn parameters from its defaults and spawns the
// named effect at the game's effects object.

#include "ee/ee.h"
#include "particles.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
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

    // The spawn parameters of "dust".
    struct DustParams
    {
        zge::Vec4 position; // +0x00
        zge::Vec4 velocity; // +0x10
        ee::Float size;     // +0x20 the puff's size (its particle's); dust() starts it this far above the point
        int32_t lifetime;   // +0x24 milliseconds
        uint8_t _28[0x08];
    };
    static_assert(offsetof(DustParams, size) == 0x20 && sizeof(DustParams) == 0x30);

    // `count` sparks from a point, thrown along 0.65 of the motion; none
    // for a count of 0.
    void sparks(const ee::Float *position, const ee::Float *motion, int32_t count);
    // A puff of dust above a point, drifting along half the motion.
    void dust(const ee::Float *position, const ee::Float *motion);

    // A skid mark: the "skid" poster on the strip from `to` up to `from`,
    // lying flat on the ground whose normal is `normal`. Nothing for a step
    // shorter than 0.02.
    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec3> normal);

    // A triangle to break off as a shard, in the world.
    struct ShardTriangle
    {
        uint32_t flags;         // +0x00 the shard's (ShardFlags); kShardOnScreen: a "screenshard"
        uint8_t _04[0x0C];
        QuadVertex corners[3];  // +0x10 positions in the world, and texture coordinates
        ee::Float velocity[3];  // +0x70 each axis within 30 % of this, at random
        uint32_t _7c;
        uint32_t material;      // +0x80
        uint8_t colour[4];      // +0x84
        int32_t lifetime;       // +0x88 milliseconds, within 30 % of it at random
    };
    static_assert(offsetof(ShardTriangle, corners) == 0x10 && offsetof(ShardTriangle, velocity) == 0x70);
    static_assert(offsetof(ShardTriangle, material) == 0x80 && offsetof(ShardTriangle, lifetime) == 0x88);

    // The triangle broken off as one "shard" (or "screenshard"): the
    // shard's frame is laid on the triangle at its centre, with a random
    // spin, and its lifetime and velocity stray at random.
    void spawnShardFromTriangle(ee::Ptr<ShardTriangle> triangle);

    // A splash at a point on the water: a "ripple" ring and "splash"'s drops.
    void spawnSplash(const ee::Float *point);

    // Debris from a point: eight pale shards strewn about `centre` in the
    // plane whose normal is `normal`, thrown along `velocity`.
    void debris(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec3> velocity, ee::Ptr<zge::Vec4> normal);

    // A pock mark on a face: the "pockmark" poster flat on the face whose
    // normal is `normal`, turned at random, with the first or the second
    // pock mark texture. (Nothing in this build calls it.)
    void spawnPockMark(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec3> normal, uint32_t which);

    // The named effects' starters: the effect at the object with the
    // parameters (0: its defaults).
    uint32_t startSparkEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<SparkParams> params);
    uint32_t startShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startScreenShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startSplashEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startDustEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<DustParams> params);
    uint32_t startPosterEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
    uint32_t startSleepEffect(ee::Ptr<zge::SceneObject> object, ee::Address params);
}
