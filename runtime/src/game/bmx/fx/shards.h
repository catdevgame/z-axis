#pragma once
// BMX XXX's shards (shards.cpp): the "shard" effect's pieces, particles with a
// frame and a spin. Aggressive Inline's Shard (game/aggressive/fx/billboard.h)
// at BMX XXX's offsets, the fields its code reads; the rest is not laid out
// yet.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A shard's corner: a vertex of the primitive API (particles.h).
    using ShardVertex = QuadVertex;

    // A shard: a particle with a frame and a triangle. Aggressive Inline's
    // without its floor height (BMX XXX's flags stand where that is).
    struct Shard
    {
        uint32_t flags;          // +0x00 the particle's (kParticleMoves)
        uint8_t _04[0x0C];
        zge::Vec4 position;      // +0x10
        zge::Vec4 tail;          // +0x20 last tick's position
        zge::Vec4 velocity;      // +0x30
        uint8_t _40[0x10];
        zge::EulerAngles spin;   // +0x50 a tick's turn
        uint8_t colour[4];       // +0x56 RGBA (unaligned)
        uint8_t _5a[2];
        ee::Float size;          // +0x5C
        uint32_t texture;        // +0x60
        uint32_t lifetime;       // +0x64 milliseconds
        uint32_t born;           // +0x68 the effects' clock
        uint32_t _6c;
        zge::Matrix frame;       // +0x70 its orientation and position (turned by its spin)
        ShardVertex corners[3];  // +0xB0
        uint32_t shardFlags;     // +0x110 the parameters'
    };
    static_assert(offsetof(Shard, spin) == 0x50 && offsetof(Shard, colour) == 0x56 && offsetof(Shard, size) == 0x5C);
    static_assert(offsetof(Shard, texture) == 0x60 && offsetof(Shard, born) == 0x68 && offsetof(Shard, frame) == 0x70);
    static_assert(offsetof(Shard, corners) == 0xB0 && offsetof(Shard, shardFlags) == 0x110);

    // What a shard starts from (Aggressive Inline's ShardParameters without
    // the floor height); with none given, the game's defaults.
    struct ShardParameters
    {
        uint32_t flags;          // +0x00 (Shard::shardFlags)
        uint8_t _04[0x0C];
        zge::Vec4 frame[4];      // +0x10 its orientation; row 3 its position
        ShardVertex corners[3];  // +0x50 the triangle in its own frame
        zge::Vec4 velocity;      // +0xB0
        zge::EulerAngles spin;   // +0xC0 a tick's turn
        uint8_t _c6[2];
        uint32_t lifetime;       // +0xC8 milliseconds
        uint32_t texture;        // +0xCC
        uint8_t colour[4];       // +0xD0 RGBA
    };
    static_assert(offsetof(ShardParameters, corners) == 0x50 && offsetof(ShardParameters, velocity) == 0xB0);
    static_assert(offsetof(ShardParameters, spin) == 0xC0 && offsetof(ShardParameters, lifetime) == 0xC8);
    static_assert(offsetof(ShardParameters, colour) == 0xD0);

    // The shard class's spawn: `count` shards from the parameters.
    void spawnShards(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<ShardParameters> given);
    // The shard class's move: each shard's frame turned by its spin, then
    // the move the particle classes share.
    int32_t moveShards(uint32_t effect, ee::Ptr<Emitter> emitter);
    // The shard class's draw: each shard's triangle; returns how many it drew.
    int32_t drawShards(uint32_t effect, ee::Ptr<Emitter> emitter);
}
