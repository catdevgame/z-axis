// DM2's effect spawns (spawns.h). DM2's own code (fx.c); Aggressive Inline's
// grindSparks (game/aggressive/fx/spawns.cpp) is the same shape.

#include "spawns.h"

#include "dm2/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "posters.h"
#include "ripples.h"
#include "sce/libgcc.h"
#include "shards.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/platform/platform.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/dm2/fx/spawns.cpp.h"

namespace game::dm2
{
    using rider::vec4;

    namespace
    {
        // The object the game's effects are spawned at (the effects' set-up
        // makes it; 0 before).
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject;

        inline ee::Global<SparkParams, sym::sparkDefaults> g_sparkDefaults;
        inline ee::Global<DustParams, sym::dustDefaults> g_dustDefaults;
        // The effects' names (spawnNamedEffect takes the name in DM2).
        constexpr uint32_t kSparkName = sym::sparkName; // "spark"
        constexpr uint32_t kDustName = sym::dustName;   // "dust"

        constexpr ee::Float kSparkThrow = 0.65f;
        constexpr ee::Float kDustDrift = 0.5f;
    }

    void sparks(const ee::Float *position, const ee::Float *motion, int32_t count)
    {
        ee::Local<SparkParams> spawn;
        *spawn = *g_sparkDefaults;
        if (count == 0)
            return;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        spawn->position = *vec4(position);
        spawn->velocity = *vec4(motion) * kSparkThrow;
        spawn->count = count;
        zge::spawnNamedEffect(kSparkName, object, spawn);
    }
    READABLE_FUNCTION_WAITS(sparks, Verified);

    void dust(const ee::Float *position, const ee::Float *motion)
    {
        ee::Local<DustParams> spawn;
        *spawn = *g_dustDefaults;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        spawn->position = *vec4(position);
        spawn->position.y = spawn->position.y - spawn->size;
        spawn->velocity = *vec4(motion) * kDustDrift;
        zge::spawnNamedEffect(kDustName, object, spawn);
    }
    READABLE_FUNCTION_WAITS(dust, Verified);

    namespace
    {
        // A skid mark's look: dark grey at half alpha, for thirty seconds, a
        // twentieth wide, lifted a tenth off the ground and a hundredth past
        // each end. Nothing for a step shorter than a fiftieth.
        constexpr uint8_t kSkidColour[4] = {0x10, 0x10, 0x10, 0x80};
        constexpr int32_t kSkidLifetime = 30000;
        constexpr ee::Float kShortestSkid = 0.02f;
        constexpr ee::Float kSkidLift = 0.1f;
        constexpr ee::Float kSkidHalfWidth = 0.025f;
        constexpr ee::Float kSkidOverlap = 0.01f;
        inline ee::Global<uint32_t, sym::g_skidMaterial> g_skidMaterial;
        constexpr uint32_t kSkidName = sym::skidName; // "skid"
    }

    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec3> normal)
    {
        ee::Local<PosterParams> mark;
        ee::Local<zge::Matrix> strip; // the mark's frame: across, the ground's normal, along, the `to` end
        ee::Local<zge::Vec4> corner;  // first the step from `to` to `from`, then each corner in the strip's frame
        *corner = *from - *to;
        const ee::Float length = zge::vec3Length(&corner->xyz());
        const uint32_t object = *g_effectsObject;
        if (!(kShortestSkid <= length) || object == 0)
            return;

        std::memcpy(mark->colour, kSkidColour, sizeof(mark->colour));
        mark->material = *g_skidMaterial;
        mark->lifetime = kSkidLifetime;
        // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose
        // upper half the length's call left.
        strip->row(2) = zge::scaledCarrying(*corner, ee::Float(1.0f) / length, zge::scaleLeftover(zge::LeftBy::lastCall));
        strip->row(1).xyz() = *normal;
        strip->row(0) = zge::cross(strip->row(1), strip->row(2));
        strip->translation() = zge::aligned(to);

        // The corners: two at the `from` end, then two at the `to` end, round
        // the strip.
        corner->y = kSkidLift;
        for (uint32_t i = 0; i < 4; ++i)
        {
            corner->x = ((i + 1) & 2) != 0 ? -kSkidHalfWidth : kSkidHalfWidth;
            corner->z = (i & 2) != 0 ? -kSkidOverlap : length + kSkidOverlap;
            zge::transformPoint(&mark->corners[i], strip, corner);
        }
        zge::spawnNamedEffect(kSkidName, object, mark);
    }
    READABLE_FUNCTION_WAITS(spawnSkidMark, Verified);

    namespace
    {
        // How far a shard's lifetime and each axis of its velocity stray, at
        // random: 16 steps of this either side (30 %).
        constexpr ee::Float kShardStrayStep = 0.01875f;
        constexpr ee::Float kThird = 0.33333334f;
        constexpr uint32_t kShardName = sym::shardName;             // "shard"
        constexpr uint32_t kScreenShardName = sym::screenShardName; // "screenshard"

        // A random step count: -16 to 15.
        ee::Float randomSteps()
        {
            return ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1F) - 0x10));
        }
        // A random turn rate about one axis.
        zge::Angle randomSpin()
        {
            return static_cast<zge::Angle>((zge::randomNumber() & 0xFFF) - 0x800);
        }
    }

    void spawnShardFromTriangle(ee::Ptr<ShardTriangle> triangle)
    {
        ee::Local<ShardParams> shard;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Matrix> toShard; // the world to the shard's frame
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        shard->material = triangle->material;
        shard->flags = triangle->flags;
        std::memcpy(shard->colour, triangle->colour, sizeof(shard->colour));
        const ee::Float lifetime = ee::toFloat(triangle->lifetime);
        shard->lifetime = ee::toInt(randomSteps() * kShardStrayStep * lifetime + lifetime);

        *centre = zge::kZero;
        for (uint32_t i = 0; i < 3; ++i)
        {
            QuadVertex &corner = shard->triangle[i];
            corner.u = triangle->corners[i].u;
            corner.v = triangle->corners[i].v;
            corner.position = zge::aligned(ee::Ptr<zge::Vec4>(&triangle->corners[i].position));
            *centre = *centre + corner.position;
        }
        // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose
        // upper half the last call left.
        *centre = zge::scaledCarrying(*centre, kThird, zge::scaleLeftover(zge::LeftBy::lastCall));

        shard->spin.x = randomSpin();
        shard->spin.y = randomSpin();
        shard->spin.z = randomSpin();

        // The shard's frame: z the triangle's normal, x toward the first
        // corner, y across them; at the centre.
        zge::Matrix &frame = shard->orientation;
        const zge::Vec4 edge = shard->triangle[1].position - shard->triangle[2].position;
        const zge::Vec4 edge2 = shard->triangle[0].position - shard->triangle[1].position;
        frame.row(2) = zge::cross(edge, edge2);
        frame.row(2) = zge::unit(frame.row(2));
        frame.row(0) = shard->triangle[0].position - *centre;
        frame.row(0) = zge::unit(frame.row(0));
        frame.row(1) = zge::cross(frame.row(2), frame.row(0));
        frame.translation() = *centre;

        // Its inverse turn (no translation) takes the corners into it.
        zge::copyRotationAndTranslation(toShard, ee::Ptr<zge::Matrix>(&frame));
        toShard->translation() = zge::kZero;
        zge::matrixTranspose3(toShard, toShard);
        for (uint32_t i = 0; i < 3; ++i)
        {
            const ee::Ptr<zge::Vec4> position(&shard->triangle[i].position);
            *position = *position - frame.translation();
            zge::transformPoint(position, toShard, position);
        }

        shard->velocity.x = triangle->velocity[0] * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
        shard->velocity.y = triangle->velocity[1] * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
        shard->velocity.z = triangle->velocity[2] * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
        zge::spawnNamedEffect((shard->flags & kShardOnScreen) ? kScreenShardName : kShardName, object, shard);
    }
    READABLE_FUNCTION_WAITS(spawnShardFromTriangle, Verified);

    namespace
    {
        // A splash: its ring (white, a second, 0.3 across growing 2.5 a
        // second) and its drops ("splash"'s sparks thrown up at 0.07 a move,
        // 0.2 big, spread 4).
        inline ee::Global<SparkParams, sym::g_splashSparks> g_splashSparks;
        constexpr uint32_t kRippleName = sym::rippleName; // "ripple"
        constexpr uint32_t kSplashName = sym::splashName; // "splash"
        constexpr int32_t kSplashRingLifetime = 1000;
        constexpr ee::Float kSplashRingSize = 0.3f, kSplashRingGrowth = 2.5f;
        constexpr ee::Float kSplashDropRise = -0.07f, kSplashDropSize = 0.2f, kSplashDropSpread = 4.0f;
    }

    void spawnSplash(const ee::Float *point)
    {
        ee::Local<SparkParams> drops;
        ee::Local<RippleParams> ring;
        *drops = *g_splashSparks;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        ring->colour[2] = 0xFF;
        ring->lifetime = kSplashRingLifetime;
        ring->size = kSplashRingSize;
        ring->growth = kSplashRingGrowth;
        drops->position.x = point[0];
        drops->position.y = point[1];
        drops->position.z = point[2];
        drops->velocity.y = kSplashDropRise;
        drops->size = kSplashDropSize;
        drops->spread = kSplashDropSpread;
        ring->position.x = point[0];
        ring->position.y = point[1];
        ring->position.z = point[2];
        ring->colour[3] = 0xFF;
        ring->colour[0] = 0xFF;
        ring->colour[1] = 0xFF;
        ring->material = 0;
        drops->velocity.x = 0.0f;
        drops->velocity.z = 0.0f;
        zge::spawnNamedEffect(kRippleName, object, ring);
        zge::spawnNamedEffect(kSplashName, object, drops);
    }
    READABLE_FUNCTION_WAITS(spawnSplash, Draft);

    namespace
    {
        // A pock mark's look: white, for thirty seconds, 0.07 across, lifted
        // a hundredth off the face.
        constexpr int32_t kPockMarkLifetime = 30000;
        constexpr ee::Float kPockMarkLift = 0.01f;
        constexpr ee::Float kPockMarkNearSide = 0.035f, kPockMarkFarSide = -0.035f;
        inline ee::Global<uint32_t[2], sym::g_pockMarkMaterials> g_pockMarkMaterials;
        constexpr uint32_t kPockMarkName = sym::pockmarkName; // "pockmark"
    }

    void spawnPockMark(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec3> normal, uint32_t which)
    {
        ee::Local<PosterParams> mark;
        ee::Local<zge::Matrix> spot;  // the mark's frame: across, the face's normal, along, the position
        ee::Local<zge::Vec4> corner;  // each corner in the spot's frame
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        mark->colour[3] = 0xFF;
        mark->material = (*g_pockMarkMaterials)[which];
        mark->lifetime = kPockMarkLifetime;
        spot->row(1).xyz() = *normal;
        mark->colour[0] = 0xFF;
        mark->colour[1] = 0xFF;
        mark->colour[2] = 0xFF;
        // Turned any way round the normal: a level direction turned at random,
        // then squared up against the normal.
        spot->row(2).x = 0.0f;
        spot->row(2).y = 0.0f;
        spot->row(2).z = 1.0f;
        zge::vec3RotateY(&spot->row(2).xyz(), &spot->row(2).xyz(), static_cast<zge::Angle>(zge::randomNumber()));
        spot->row(0) = zge::cross(spot->row(1), spot->row(2));
        spot->row(0) = zge::unit(spot->row(0));
        spot->row(2) = zge::cross(spot->row(0), spot->row(1));
        spot->translation() = zge::aligned(position);

        corner->y = kPockMarkLift;
        for (uint32_t i = 0; i < 4; ++i)
        {
            corner->x = ((i + 1) & 2) != 0 ? kPockMarkFarSide : kPockMarkNearSide;
            corner->z = (i & 2) != 0 ? kPockMarkFarSide : kPockMarkNearSide;
            zge::transformPoint(&mark->corners[i], spot, corner);
        }
        zge::spawnNamedEffect(kPockMarkName, object, mark);
    }
    READABLE_FUNCTION_WAITS(spawnPockMark, Draft);

    namespace
    {
        // Debris: eight pale blue shards at a time, each one of four
        // triangles, living 1 to 3 seconds, a quarter to 0.85 big, strewn
        // within 0.75 of the centre, each axis of the velocity within 30 %.
        constexpr uint32_t kDebrisShards = 8;
        constexpr uint8_t kDebrisColour[4] = {0x80, 0x80, 0xD0, 0x30};
        constexpr ee::Float kDebrisShortestLife = 1000.0f, kDebrisLifeStep = 133.33334f;
        constexpr ee::Float kDebrisSmallest = 0.25f, kDebrisSizeStep = 0.040000003f;
        constexpr ee::Float kDebrisStrewStep = 0.0029296875f;
        constexpr ee::Ptr<zge::Vec4[3]> g_shardTriangles{sym::g_debrisTriangles};
    }

    // Kept from the original: a shard's distance off the plane through the
    // centre is taken before its new z is put in its place, so with the z
    // the shard before it ended with (for the first, whatever the stack
    // held); the new z is then moved by that distance like x and y.
    void debris(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec3> velocity, ee::Ptr<zge::Vec4> normal)
    {
        ee::Local<ShardParams> shard;
        ee::Local<zge::Vec4> offset; // from the centre to the shard's place
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        shard->flags = kShardKindTwo;
        std::memcpy(shard->colour, kDebrisColour, sizeof(shard->colour));
        shard->material = 0;
        zge::Matrix &frame = shard->orientation;
        for (uint32_t made = 0; made < kDebrisShards; ++made)
        {
            // Its life and size.
            shard->lifetime = static_cast<int32_t>(
                sce::libgcc::fptoui(ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xF)) * kDebrisLifeStep + kDebrisShortestLife));
            const ee::Float size = ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xF)) * kDebrisSizeStep + kDebrisSmallest;

            // Its triangle: one of four, scaled; untextured.
            const zge::Vec4 (&triangle)[3] = g_shardTriangles[zge::randomNumber() & 3u];
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                QuadVertex &vertex = shard->triangle[corner];
                // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose
                // upper half the last random number's call left.
                vertex.position = zge::scaledCarrying(triangle[corner], size, zge::scaleLeftover(zge::LeftBy::lastCall));
                vertex.u = 0.0f;
                vertex.v = 0.0f;
            }

            shard->spin.x = randomSpin();
            shard->spin.y = randomSpin();
            shard->spin.z = randomSpin();

            // Its frame: the normal, turned about y and x at random, made square.
            frame.row(2) = zge::aligned(normal);
            const uint32_t yaw = zge::randomNumber();
            zge::vec3RotateY(&frame.row(1).xyz(), &frame.row(2).xyz(), static_cast<zge::Angle>(yaw));
            const uint32_t pitch = zge::randomNumber();
            zge::vec3RotateX(&frame.row(1).xyz(), &frame.row(1).xyz(), static_cast<zge::Angle>(pitch));
            frame.row(0) = zge::cross(frame.row(1), frame.row(2));
            frame.row(0) = zge::unit(frame.row(0));
            frame.row(1) = zge::cross(frame.row(2), frame.row(0));

            // Its place: about the centre, moved into the plane of the normal.
            zge::Vec4 &place = frame.translation();
            place.x = centre->x + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FF) - 0x100)) * kDebrisStrewStep;
            place.y = centre->y + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FF) - 0x100)) * kDebrisStrewStep;
            const ee::Float z = centre->z + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FF) - 0x100)) * kDebrisStrewStep;
            *offset = place - *centre;
            const ee::Float along = zge::dot(*offset, *normal);
            const ee::Float alongX = normal->x * along, alongY = normal->y * along, alongZ = normal->z * along;
            place.x = place.x - alongX;
            place.y = place.y - alongY;
            place.z = z - alongZ;

            shard->velocity.x = velocity->x * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
            shard->velocity.y = velocity->y * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
            shard->velocity.z = velocity->z * (randomSteps() * kShardStrayStep + ee::Float(1.0f));
            zge::spawnNamedEffect(kShardName, object, shard);
        }
    }
    READABLE_FUNCTION_WAITS(debris, Verified);

    namespace
    {
        // The parameters a named effect starts with when the caller gives none.
        constexpr uint32_t kSparkEffectDefaults = sym::sparkEffectDefaults;
        constexpr uint32_t kDustEffectDefaults = sym::dustEffectDefaults;
        constexpr uint32_t kPosterEffectDefaults = sym::posterEffectDefaults;
        constexpr uint32_t kSleepEffectDefaults = sym::sleepEffectDefaults;
        constexpr uint32_t kPosterName = sym::posterName;           // "poster"
        constexpr uint32_t kSleepName = sym::sleepName;             // "sleep"
        constexpr uint32_t kSplashEffectDefaults = sym::splashEffectDefaults;
        constexpr uint32_t kSplashParticles = 0x20;
    }

    // The named effects' starters (the effects' set-up registers each under
    // its effect's name; the poster's also under "skid" and "pockmark"):
    // the effect started at the object with the parameters, or with its
    // defaults. Each returns startNamedEffect's result.

    // "spark" starts with as many particles as its parameters' count (its low byte).
    uint32_t startSparkEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<SparkParams> params)
    {
        if (!params)
            params = ee::Ptr<SparkParams>(kSparkEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kSparkName, static_cast<uint8_t>(params->count), 0, params);
    }
    READABLE_FUNCTION_WAITS(startSparkEffect, Verified);

    uint32_t startShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return game::tail(zge::startNamedEffect, object, kShardName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startShardEffect, Verified);

    uint32_t startScreenShardEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        return game::tail(zge::startNamedEffect, object, kScreenShardName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startScreenShardEffect, Draft);

    // "splash" starts with 32 particles.
    uint32_t startSplashEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        if (params == 0)
            params = ee::Address(kSplashEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kSplashName, kSplashParticles, 0, params);
    }
    READABLE_FUNCTION_WAITS(startSplashEffect, Draft);

    uint32_t startDustEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<DustParams> params)
    {
        if (!params)
            params = ee::Ptr<DustParams>(kDustEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kDustName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startDustEffect, Verified);

    uint32_t startPosterEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        if (params == 0)
            params = ee::Address(kPosterEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kPosterName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startPosterEffect, Verified);

    uint32_t startSleepEffect(ee::Ptr<zge::SceneObject> object, ee::Address params)
    {
        if (params == 0)
            params = ee::Address(kSleepEffectDefaults);
        return game::tail(zge::startNamedEffect, object, kSleepName, 1, 0, params);
    }
    READABLE_FUNCTION_WAITS(startSleepEffect, Verified);
}
