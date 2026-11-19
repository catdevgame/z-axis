// The shards ("shard" and "screenshard"; inlineFx.c, were the sweep's
// drafts of startShard .. moveShards): pieces of something broken, each a
// triangle with its own frame, flying off and spinning. The start, the
// class's emitter, spawn, move and draw.
//

#include "zge/scene/camera.h"
#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"

#include <cstring>
#include "sym/aggressive/fx/shards.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        // (out, m, angles *): m turned by the three angles (the engine's
        // matrix code after its turns about x, y and z) [inferred].
        constexpr uint32_t kMatrixRotateEuler = sym::matrixRotateXYZ;

        // A shard's defaults (ShardParameters, billboard.h; with none given:
        // a small triangle, living 2 seconds).
        constexpr uint32_t kShardDefaults = sym::shardDefaults;

    }

    // startShard: a shard (the named effect's start).
    uint32_t startShard(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_shardClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startShard, Verified);

    // newShardEmitter ("shard", "screenshard" +0x10): the class's emitter.
    uint32_t newShardEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, sizeof(Shard), sym::newShardEmitter);
    }
    READABLE_FUNCTION_WAITS(newShardEmitter, Verified);

    // spawnShards ("shard", "screenshard" +0x18): `count` shards from the
    // parameters (the defaults when none): at the frame's position, with
    // its velocity, spin, frame, triangle, lifetime, texture, colour.
    void spawnShards(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<ShardParameters> given)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        const ee::Ptr<ShardParameters> shard = given ? given : ee::Ptr<ShardParameters>(kShardDefaults);
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Shard> piece = newParticle<Shard>(*emitter);
            if (!piece)
                break;
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
                zge::memoryCopy(&piece->corners[corner], &shard->corners[corner], sizeof(QuadVertex));
            piece->shardFlags = shard->flags;
            piece->floorHeight = shard->floorHeight;
            zge::pointerArrayAdd(emitter->particles, piece);
        }
    }
    READABLE_FUNCTION_WAITS(spawnShards, Verified);

    // moveShards ("shard", "screenshard" +0x1C): once a tick each shard
    // not still has its frame turned by its spin; then the move the classes
    // share.
    int32_t moveShards(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Matrix> turn;
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                Shard &piece = *ee::Ptr<Shard>(particle);
                if (piece.shardFlags & kShardStill)
                    continue;
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
        // A shard's colours by how far it is turned from facing up or down
        // (its frame's z axis from the y axis, in sixteenths of a turn): the
        // colour whole (flag 2's draw and the second draw), and the shade added
        // to a shard's own red, green and blue otherwise.
        inline ee::Global<uint8_t[10][4], sym::g_shardColours> g_shardColours;
        inline ee::Global<int16_t[9][3], sym::g_shardShades> g_shardShades;

        // The primitive API's flag for screen-space primitives (2D).
        constexpr uint32_t kScreenSpace = 0x02000000u;
        // The shard's flags (ShardParameters::flags, billboard.h).
        constexpr uint32_t kShardOnScreen = 0x1;  // drawn in screen space
        constexpr uint32_t kShardKindTwo = 0x2;   // drawn as the API's kind 2, in its facing's colour
        constexpr uint32_t kShardSecondDraw = 0x4;

        // A triangle for the primitive API's strip draw: its texture and its
        // three corners.
        struct TriangleCorners
        {
            uint32_t texture;
            ee::Ptr<QuadVertex> corners[3];
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
    }

    // drawShards ("shard", "screenshard" +0x20): each shard its triangle in
    // its frame, coloured by how far it is turned from facing up or down,
    // fading over its last half second; a shard with flag 4 is drawn a
    // second time untextured in its facing's colour, fading over its last
    // quarter second. Returns how many it drew.
    int32_t drawShards(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<TriangleCorners> triangle;
        ee::Local<QuadVertex[3]> vertices;
        ee::Local<uint8_t[4]> colour;
        // (The original keeps these two in its frame.)
        ee::Local<ee::Ptr<Emitter>> emitterKept;
        ee::Local<int32_t> clockKept;
        *emitterKept = emitter;
        int32_t drawn = 0;
        const int32_t clock = effectsClock();
        *clockKept = clock;
        triangle->corners[2] = &vertices[2];
        triangle->corners[1] = &vertices[1];
        triangle->corners[0] = &vertices[0];
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            Shard &shard = *ee::Ptr<Shard>(particle);
            const uint32_t left = shard.lifetime - (static_cast<uint32_t>(clock) - shard.born);
            const int16_t facing = static_cast<int16_t>(zge::angleAcos(ee::abs(zge::matrixRow(shard.frame, 2).y)) / 4096);
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
                QuadVertex &vertex = vertices[i];
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
                zge::angleAcos(ee::abs(zge::matrixRow(shard.frame, 2).y)); // asked again; the answer goes unused
                std::memcpy(&colour[0], (*g_shardColours)[facing], 4);
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

    namespace
    {
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_screenShardClass> g_screenShardClassWord; // g_screenShardClassWord (g_screenShardClassWord)
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
    }

    uint32_t startScreenShards(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_screenShardClassWord, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startScreenShards, Verified);
}
