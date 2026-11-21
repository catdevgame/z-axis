// DM2's "shard" and "screenshard" effect classes (shards.h). DM2's own code (fx.c).

#include "shards.h"

#include "effects_clock.h"

#include "dm2/rider/physics/physics.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/render/primitives.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/fx/shards.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kShardEffectDefaults = sym::shardEffectDefaults;

        // A shard's colour by its facing, in ten steps from flat to on edge:
        // the colour itself, and what is added to a shard's own red, green and blue.
        inline ee::Global<uint8_t[10][4], sym::g_shardColours> g_shardColours;
        inline ee::Global<int16_t[10][3], sym::g_shardShades> g_shardShades;

        // The primitive API's flag for screen space.
        constexpr uint32_t kScreenSpace = 0x2000000u;

        // The triangle the draw hands the primitive API: its material and corners.
        struct TriangleCorners
        {
            uint32_t material;
            ee::Ptr<QuadVertex> corners[3];
        };

        // A colour channel with a shade added, kept within a byte.
        uint8_t shaded(uint8_t channel, int16_t shade)
        {
            int32_t value = channel + shade;
            if (!(value < 0x100))
                value = 0xFF;
            if (!(value > -1))
                value = 0;
            return static_cast<uint8_t>(value);
        }

        // How far a shard is turned from facing up or down, 0 to 4 in sixteenths of a turn.
        int16_t facingOf(const Shard &shard)
        {
            return static_cast<int16_t>(zge::angleAcos(ee::abs(shard.orientation.row(2).y)) / 4096);
        }
    }

    void spawnShards(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<ShardParams> params)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<ShardParams>(kShardEffectDefaults);
        if (!effectClass || !emitter)
            return;
        for (uint32_t made = 0; made < (count & 0xFFu); ++made)
        {
            const ee::Ptr<Shard> shard(zge::poolAlloc(emitter->pool));
            shard->flags = zge::kParticleMoves;
            shard->born = now;
            for (int i = 0; i < 4; ++i)
                shard->colour[i] = params->colour[i];
            shard->size = 0.0f;
            shard->lifetime = static_cast<uint32_t>(params->lifetime);
            shard->material = params->material;
            shard->position = params->orientation.translation();
            shard->tail = shard->position;
            shard->velocity = params->velocity;
            shard->spin = params->spin;
            zge::copyMatrixRotationAndTranslation(ee::Ptr<zge::Matrix>(&shard->orientation), ee::Ptr<const zge::Matrix>(&params->orientation));
            for (int corner = 0; corner < 3; ++corner)
                zge::memoryCopy(ee::Ptr<QuadVertex>(&shard->triangle[corner]), ee::Ptr<QuadVertex>(&params->triangle[corner]), sizeof(QuadVertex));
            shard->shardFlags = params->flags;
            zge::pointerArrayAdd(emitter->particles, shard);
        }
    }
    READABLE_FUNCTION_WAITS(spawnShards, Verified);

    int32_t moveShards(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter)
    {
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (ee::Ptr<Shard> shard(zge::arrayFirst(emitter->particles)); shard; shard = ee::Ptr<Shard>(zge::arrayNext(emitter->particles)))
            {
                ee::Local<zge::Matrix> turn;
                zge::matrixIdentity(turn);
                zge::matrixRotateXYZ(turn, turn, &shard->spin);
                const ee::Ptr<zge::Matrix> orientation(&shard->orientation);
                rider::physics::engine::matrixProduct(orientation, orientation, turn);
            }
        }
        return moveParticles(effectClass, emitter);
    }
    READABLE_FUNCTION_WAITS(moveShards, Verified);

    int32_t drawShards(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<TriangleCorners> triangle;
        ee::Local<QuadVertex[3]> vertices;
        ee::Local<uint8_t[4]> colour;
        ee::Local<int32_t> clockKept; // (the original keeps the clock in its frame)
        int32_t drawn = 0;
        const int32_t clock = effectsClock();
        *clockKept = clock;
        triangle->corners[1] = ee::Ptr<QuadVertex>(vertices) + 1;
        triangle->corners[2] = ee::Ptr<QuadVertex>(vertices) + 2;
        triangle->corners[0] = ee::Ptr<QuadVertex>(vertices);
        for (ee::Ptr<Shard> at(zge::arrayFirst(emitter->particles)); at; at = ee::Ptr<Shard>(zge::arrayNext(emitter->particles)))
        {
            Shard &shard = *at;
            const uint32_t left = shard.lifetime - (static_cast<uint32_t>(clock) - shard.born);
            const int16_t facing = facingOf(shard);
            if (shard.shardFlags & kShardKindTwo)
            {
                for (int i = 0; i < 4; ++i)
                    colour[i] = (*g_shardColours)[facing][i];
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                    colour[i] = shard.colour[i];
                for (int channel = 0; channel < 3; ++channel)
                    colour[channel] = shaded(colour[channel], (*g_shardShades)[facing][channel]);
            }
            if (left < 0x200u)
                colour[3] = static_cast<uint8_t>(colour[3] * left >> 9);
            shard.orientation.translation() = shard.position;
            for (uint32_t i = 0; i < 3; ++i)
            {
                QuadVertex &vertex = vertices[i];
                for (int byte = 0; byte < 4; ++byte)
                    vertex.colour[byte] = colour[byte];
                vertex.u = shard.triangle[i].u;
                vertex.v = shard.triangle[i].v;
                vertex.position = shard.triangle[i].position;
            }
            triangle->material = shard.material;
            uint32_t kind = shard.shardFlags & kShardKindTwo;
            if (shard.shardFlags & kShardOnScreen)
                kind |= kScreenSpace;
            const ee::Ptr<zge::Matrix> orientation(&shard.orientation);
            zge::drawPrimitiveStrips(kind, triangle, 1, orientation);
            if (shard.shardFlags & kShardSecondDraw)
            {
                triangle->material = 0;
                const int16_t facingAgain = facingOf(shard);
                for (int i = 0; i < 4; ++i)
                    colour[i] = (*g_shardColours)[facingAgain][i];
                if (left < 0x100u)
                    colour[3] = static_cast<uint8_t>(colour[3] * left >> 8);
                for (uint32_t i = 0; i < 3; ++i)
                    for (int byte = 0; byte < 4; ++byte)
                        vertices[i].colour[byte] = colour[byte];
                zge::drawPrimitiveStrips(kind | kShardKindTwo, triangle, 1, orientation);
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawShards, Verified);
}
