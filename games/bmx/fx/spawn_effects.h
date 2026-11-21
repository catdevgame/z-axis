#pragma once
// BMX XXX's marks and pieces (fx.c; spawn_effects.cpp): what the rider's,
// the weapons' and the level's code asks the effects for by name: skid and
// bullet marks (posters), a sleeper's "z", a shard broken off a triangle.

#include "particles.h"
#include "shards.h"

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/render/part.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A triangle a shard is broken from (the exploders' and the weapons' broken faces, the health bar's falling
    // pieces).
    struct ShardTriangle
    {
        uint32_t flags;          // +0x00 bit 0: a "screenshard"
        uint8_t _04[0x0C];
        ShardVertex corners[3];  // +0x10 in the world: each one's position and texture coordinates
        zge::Vec4 velocity;      // +0x70 (its w unread)
        ee::Ptr<zge::Material> material; // +0x80 the face's (0: untextured)
        zge::VertexColour colour; // +0x84
        int32_t lifetime;        // +0x88 milliseconds
    };
    static_assert(offsetof(ShardTriangle, corners) == 0x10 && offsetof(ShardTriangle, velocity) == 0x70);
    static_assert(offsetof(ShardTriangle, material) == 0x80 && offsetof(ShardTriangle, lifetime) == 0x88 && sizeof(ShardTriangle) == 0x90);

    // A skid mark: the "skid" poster on the strip from `to` up to `from`,
    // lying on the ground whose normal is `normal`. Nothing for a step
    // shorter than 0.02.
    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec4> normal);
    // A bullet mark: a "pockmark" poster 0.07 square at the point, flat on
    // the face whose normal is given, turned at random, with the pock mark
    // texture of that number and the colour.
    void spawnBulletMark(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec3> normal, uint32_t texture, ee::Ptr<uint8_t> colour);
    // A sleeper's "z" at the point, living `lifetime` milliseconds.
    void sleep(ee::Ptr<zge::Vec4> position, uint32_t lifetime);
    // A shard broken off a triangle: a "shard" (a "screenshard" with the
    // triangle's flag 1) at the triangle's centre.
    void spawnShardFromTriangle(ee::Ptr<ShardTriangle> triangle);
    // A breaking face's debris: eight shards about the centre, in the plane
    // whose normal is given, thrown with the velocity.
    void faceDebris(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> normal);
}
