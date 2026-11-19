// BMX XXX's shards (shards.h): their move. BMX XXX's own code (fx.c's block),
// Aggressive Inline's logic (game/aggressive/fx/shards.cpp) over BMX XXX's
// shard.

#include "shards.h"

#include "effects_clock.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include "zge/container/array.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include <cstring>
#include "sym/bmx/fx/shards.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The effects' budget of particles and how many are alive (every emitter's together).
        inline ee::Global<uint32_t, sym::g_particleBudget> g_particleBudget;
        inline ee::Global<uint32_t, sym::g_particlesAlive> g_particlesAlive;
        // A shard's parameters when none are given.
        constexpr uint32_t kShardDefaults = sym::shardDefaults;
    }

    // spawnShards ("shard", "screenshard" +0x18): `count` shards from the
    // parameters (the defaults when none): at the frame's position, with its
    // velocity, spin, frame, triangle, lifetime, texture, colour and flags.
    // Kept bug: with the effects' budget of particles used up no shard is
    // allocated, and BMX XXX goes on to write the shard at address 0 and to
    // add that to the emitter (Aggressive Inline stops there).
    void spawnShards(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                     ee::Ptr<ShardParameters> given)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        const ee::Ptr<ShardParameters> shard = given ? given : ee::Ptr<ShardParameters>(kShardDefaults);
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            uint32_t particle = 0;
            if (*g_particlesAlive < *g_particleBudget)
            {
                *g_particlesAlive = *g_particlesAlive + 1;
                particle = zge::allocateSized(emitter->particleBytes);
            }
            const ee::Ptr<Shard> piece(particle);
            piece->flags = kParticleMoves;
            piece->born = static_cast<uint32_t>(clock);
            std::memcpy(piece->colour, shard->colour, 4);
            piece->size = 0.0f;
            piece->lifetime = shard->lifetime;
            piece->texture = shard->texture;
            piece->position = shard->frame[3];
            piece->tail = piece->position;
            piece->velocity = shard->velocity;
            piece->spin = shard->spin;
            std::memcpy(&piece->frame, shard->frame, sizeof(piece->frame));
            for (uint32_t corner = 0; corner < 3; ++corner)
                zge::memoryCopy(&piece->corners[corner], &shard->corners[corner], sizeof(ShardVertex));
            piece->shardFlags = shard->flags;
            zge::pointerArrayAdd(emitter->particles, piece);
        }
    }
    READABLE_FUNCTION_WAITS(spawnShards, Verified);

    // moveShards ("shard", "screenshard" +0x1C): once a tick each shard has
    // its frame turned by its spin; then the move the classes share.
    // Aggressive Inline's skips a shard lying still; BMX XXX's turns every one.
    int32_t moveShards(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Matrix> turn;
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                Shard &piece = *ee::Ptr<Shard>(particle);
                zge::matrixIdentity(&*turn);
                zge::matrixRotateXYZ(turn, turn, &piece.spin);
                zge::matrixMultiply(&piece.frame, &piece.frame, turn);
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveShards, Verified);

    namespace
    {
        // A shard's colour by how far its face is turned from level (a
        // sixteenth of a turn a step), and what a lit shard's own colour
        // gets added (red, green, blue).
        inline ee::Global<uint8_t[10][4], sym::g_shardColours> g_shardColours;
        inline ee::Global<int16_t[9][3], sym::g_shardShades> g_shardShades;

        // drawPrimitiveStrips' flag for screen space.
        constexpr uint32_t kScreenSpace = 0x02000000u;
        // The shard's flags (ShardParameters::flags).
        constexpr uint32_t kShardOnScreen = 0x1;  // drawn in screen space
        constexpr uint32_t kShardKindTwo = 0x2;   // drawn as the API's kind 2, in its facing's colour
        constexpr uint32_t kShardSecondDraw = 0x4;

        // A triangle for the primitive API's strip draw: its texture and its
        // three corners.
        struct TriangleCorners
        {
            uint32_t texture;
            ee::Ptr<ShardVertex> corners[3];
        };
        static_assert(sizeof(TriangleCorners) == 0x10);

        // A channel with a shade added, kept to 0 .. 255.
        uint8_t shaded(uint8_t channel, int16_t shade)
        {
            int32_t value = channel + shade;
            if (!(value < 0x100))
                value = 0xFF;
            if (!(value > -1))
                value = 0;
            return static_cast<uint8_t>(value);
        }

        // How far the shard's face is turned from level, in sixteenths of a turn.
        int16_t shardFacing(const Shard &shard)
        {
            return static_cast<int16_t>(zge::angleAcos(ee::abs(zge::matrixRow(shard.frame, 2).y)) / 4096);
        }
    }

    // drawShards ("shard", "screenshard" +0x20): each shard its triangle in
    // its own frame, in its facing's colour (kind two) or its own colour
    // shaded by its facing, fading out over its last half second; a shard
    // with the second draw gets its triangle again untextured in the
    // facing's colour, fading over its last quarter second. Returns how
    // many it drew. Aggressive Inline's, but the second draw takes the
    // facing it asks for again.
    int32_t drawShards(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<TriangleCorners> triangle;
        ee::Local<ShardVertex[3]> vertices;
        ee::Local<uint8_t[4]> colour;
        // (The original keeps the clock in its frame.)
        ee::Local<int32_t> clockKept;
        int32_t drawn = 0;
        const int32_t clock = effectsClock();
        *clockKept = clock;
        triangle->corners[1] = &vertices[1];
        triangle->corners[2] = &vertices[2];
        triangle->corners[0] = &vertices[0];
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            Shard &shard = *ee::Ptr<Shard>(particle);
            const uint32_t left = shard.lifetime - (static_cast<uint32_t>(clock) - shard.born);
            const int16_t facing = shardFacing(shard);
            if (shard.shardFlags & kShardKindTwo)
                std::memcpy(&colour[0], (*g_shardColours)[facing], 4);
            else
            {
                std::memcpy(&colour[0], shard.colour, 4);
                for (uint32_t channel = 0; channel < 3; ++channel)
                    colour[channel] = shaded(colour[channel], (*g_shardShades)[facing][channel]);
            }
            if (left < 0x200u)
                colour[3] = static_cast<uint8_t>(colour[3] * left >> 9);
            zge::matrixRow(shard.frame, 3) = shard.position;
            for (uint32_t i = 0; i < 3; ++i)
            {
                ShardVertex &vertex = vertices[i];
                std::memcpy(vertex.colour, &colour[0], 4);
                vertex.u = shard.corners[i].u;
                vertex.v = shard.corners[i].v;
                vertex.position = shard.corners[i].position;
            }
            triangle->texture = shard.texture;
            uint32_t kind = shard.shardFlags & kShardKindTwo;
            if (shard.shardFlags & kShardOnScreen)
                kind |= kScreenSpace;
            zge::drawPrimitiveStrips(kind, triangle, 1, &shard.frame);
            if (shard.shardFlags & kShardSecondDraw)
            {
                triangle->texture = 0;
                std::memcpy(&colour[0], (*g_shardColours)[shardFacing(shard)], 4);
                if (left < 0x100u)
                    colour[3] = static_cast<uint8_t>(colour[3] * left >> 8);
                for (uint32_t i = 0; i < 3; ++i)
                    std::memcpy(vertices[i].colour, &colour[0], 4);
                zge::drawPrimitiveStrips(kind | kShardKindTwo, triangle, 1, &shard.frame);
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawShards, Verified);
}
