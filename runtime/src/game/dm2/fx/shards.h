#pragma once

// DM2's "shard" and "screenshard" effect classes (fx.c; shards.cpp):
// tumbling pieces of what broke.

#include "particles.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // How a shard is drawn.
    enum ShardFlags : uint32_t
    {
        kShardOnScreen = 1u << 0,   // in screen space
        kShardKindTwo = 1u << 1,    // primitive kind 2, in its facing's colour
        kShardSecondDraw = 1u << 2, // drawn a second time, untextured, in its facing's colour
    };

    // The spawn parameters of "shard" and "screenshard".
    struct ShardParams
    {
        uint32_t flags;            // +0x00 ShardFlags
        uint8_t _04[0x0C];
        zge::Matrix orientation;   // +0x10 its translation is where the shard starts
        QuadVertex triangle[3];    // +0x50 the piece's three corners
        zge::Vec4 velocity;        // +0xB0
        zge::EulerAngles spin;     // +0xC0
        uint8_t _c6[2];
        int32_t lifetime;          // +0xC8 milliseconds
        uint32_t material;         // +0xCC
        uint8_t colour[4];         // +0xD0
        uint8_t _d4[0x0C];
    };
    static_assert(offsetof(ShardParams, triangle) == 0x50 && offsetof(ShardParams, velocity) == 0xB0 && offsetof(ShardParams, spin) == 0xC0);
    static_assert(offsetof(ShardParams, lifetime) == 0xC8 && offsetof(ShardParams, colour) == 0xD0 && sizeof(ShardParams) == 0xE0);

    // A shard: a particle that tumbles, a triangle of what broke.
    struct Shard : Particle
    {
        zge::Matrix orientation; // +0x70 turned by the particle's spin at each move
        QuadVertex triangle[3];  // +0xB0
        uint32_t shardFlags;     // +0x110 ShardFlags, the parameters'
        uint8_t _114[0x0C];
    };
    static_assert(offsetof(Shard, orientation) == 0x70 && offsetof(Shard, triangle) == 0xB0 && sizeof(Shard) == 0x120);

    // The shards' spawn: `count` (its low byte) shards, each a copy of the
    // parameters (the effect's defaults when none).
    void spawnShards(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<ShardParams> params);
    // The shards' move, once per tick of the effects' clock: every shard's
    // orientation turned by its spin, then the particles' move. Returns
    // what that returns.
    int32_t moveShards(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // The shards' draw: each shard its triangle in its orientation, coloured
    // by how far it is turned from facing up or down, fading over its last
    // half second; a shard with kShardSecondDraw again untextured in its
    // facing's colour, fading over its last quarter second. Returns how
    // many it drew.
    int32_t drawShards(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
