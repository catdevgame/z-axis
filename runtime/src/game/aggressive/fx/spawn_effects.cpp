// Named effects the game spawns with their parameters filled in here
// (inlineFx.c): a skid mark (the "skid" poster on the strip a skate or wheel
// left), the ring a wall tap leaves, a splash's drops (a ripple where each
// lands), a whole splash (a ripple and the "splash" sparks), a shard broken
// off a triangle, and the starts of the "spark" and "userspark" classes.
// Each is spawned on the effects' object (g_effectsObject), nothing when
// there is none yet.

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/skater/skater.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/scene/particle_types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/fx/spawn_effects.cpp.h"

namespace zge
{
}

namespace aggressive::fx
{
    namespace
    {
        // The scene object the named effects are spawned on (initInlineFx makes
        // it). 0: none yet.
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_effectsObject> g_effectsObject;

        // A triangle to break off as a shard (the shard's own layout before it:
        // flags, then its corners, in the world).
        struct ShardTriangle
        {
            uint32_t flags;           // +0x00 the shard's (ShardParameters::flags); 1: a "screenshard"
            uint8_t _04[0x0C];
            QuadVertex corners[3];    // +0x10 positions in the world, and texture coordinates
            ee::Float velocity[3];    // +0x70 each axis within 30 % of this, at random
            uint32_t _7c;
            uint32_t texture;         // +0x80
            uint8_t colour[4];        // +0x84 RGBA
            int32_t lifetime;         // +0x88 milliseconds, within 30 % of it at random
        };
        static_assert(offsetof(ShardTriangle, corners) == 0x10 && offsetof(ShardTriangle, velocity) == 0x70);
        static_assert(offsetof(ShardTriangle, texture) == 0x80 && offsetof(ShardTriangle, lifetime) == 0x88);

        // A skid mark's look: dark grey at half alpha, for ten seconds, lifted a
        // tenth off the ground and a hundredth past each end.
        constexpr uint8_t kSkidColour[4] = {0x10, 0x10, 0x10, 0x80};
        constexpr uint32_t kSkidLifetime = 10000;
        constexpr float kSkidLift = 0.1f;
        constexpr float kSkidOverlap = 0.01f;
        constexpr float kShortestSkid = 0.02f;

        // A splash's drops: five, round it.
        constexpr uint32_t kSplashDrops = 5;
        inline ee::Global<zge::Angle, sym::g_splashDropAngleStep> g_splashDropAngleStep;

        // The "splash" sparks' parameters as spawnSplash starts them.
        constexpr uint32_t kSplashSparks = sym::g_splashSparks;
        constexpr uint32_t kTextSplash = sym::g_textSplash; // "splash"

        // The shard's parameters, then its centre and frame, as the original's frame keeps them.
        static_assert(sizeof(ShardParameters) == 0xE0);
    }

    namespace
    {

        // A random part of a range: (random 0 .. 31, less 16) times `step`,
        // added to 1 (a factor within half of 32 steps of 1).
        ee::Float randomAroundOne(ee::Float step)
        {
            return ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1F) - 0x10)) * step + ee::Float(1.0f);
        }
    }

    // A skid mark: the "skid" poster on the strip from `to` up to `from`, lying
    // flat on the ground whose normal is `normal`, `fromWidth` across at the
    // `from` end and `toWidth` at the `to` end. Nothing for a step shorter than
    // 0.02. (The skid marks of the skates and wheels, and a wall ride's.)
    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec3> normal, ee::Float fromWidth, ee::Float toWidth)
    {
        ee::Local<PosterParameters> mark;
        ee::Local<zge::Matrix> strip;  // the mark's frame: across, the ground's normal, along, the `to` end
        ee::Local<zge::Vec4> corner;   // first the step from `to` to `from`, then each corner in the strip's frame
        *corner = *from - *to;
        const ee::Float length = zge::vec3Length(&corner->xyz());
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (length < ee::Float(kShortestSkid) || !object)
            return;

        std::memcpy(mark->colour, kSkidColour, sizeof(mark->colour));
        mark->texture = *g_skidTexture;
        mark->lifetime = kSkidLifetime;
        strip->row(2) = *corner * (ee::Float(1.0f) / length);
        strip->row(1).xyz() = *normal;
        strip->row(0) = zge::cross(strip->row(1), strip->row(2));
        strip->translation() = zge::aligned(to);

        // The corners: two at the `from` end, then two at the `to` end, round
        // the strip.
        const ee::Float halfToWidth = toWidth / ee::Float(2.0f);
        const ee::Float halfFromWidth = fromWidth / ee::Float(2.0f);
        corner->y = ee::Float(kSkidLift);
        for (uint32_t i = 0; i < 4; ++i)
        {
            const bool fromEnd = i < 2;
            const ee::Float halfWidth = fromEnd ? halfFromWidth : halfToWidth;
            corner->x = (i == 1 || i == 2) ? -halfWidth : halfWidth;
            corner->z = fromEnd ? length + ee::Float(kSkidOverlap) : ee::Float(-kSkidOverlap);
            mark->corners[i] = zge::transformed(*strip, *corner);
        }
        zge::spawnNamedEffect(*g_skidHash, object, mark);
    }
    READABLE_FUNCTION_WAITS(spawnSkidMark, Verified);

    // The ring a wall tap leaves at a skate: a grey ripple at `at` that
    // follows the player, moves with `velocity` and grows the faster that is.
    void spawnWallTapRipple(ee::Ptr<skater::Player> player, ee::Ptr<zge::Vec4> at, ee::Ptr<zge::Vec4> velocity)
    {
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        ee::Local<RippleParameters> ripple;
        ripple->x = at->x;
        ripple->y = at->y;
        ripple->z = at->z;
        const uint8_t colour[4] = {0x80, 0x80, 0x80, 0x20};
        std::memcpy(ripple->colour, colour, sizeof(colour));
        ripple->texture = *g_rippleTexture;
        ripple->lifetime = 1000;
        ripple->size = ee::Float(0.05f);
        ripple->follows = player;
        ripple->growth = zge::vec3Length(&velocity->xyz()) * ee::Float(0.35f);
        ripple->velocity = zge::aligned(velocity);
        zge::startEffect(object, *g_rippleClass, 1, 0, ripple);
    }
    READABLE_FUNCTION_WAITS(spawnWallTapRipple, Verified);

    // Where a drop of a splash lands: a white ripple, still, half a unit
    // across and growing fast, for a second and a half.
    void spawnSplashDrop(ee::Ptr<zge::Vec4> at)
    {
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        ee::Local<RippleParameters> ripple;
        ripple->x = at->x;
        ripple->y = at->y;
        ripple->z = at->z;
        const uint8_t colour[4] = {0xFF, 0xFF, 0xFF, 0x40};
        std::memcpy(ripple->colour, colour, sizeof(colour));
        ripple->lifetime = 1500;
        ripple->size = ee::Float(0.5f);
        ripple->growth = ee::Float(2.5f);
        ripple->texture = 0;
        ripple->velocity = zge::kZero;
        ripple->follows = ee::Ptr<skater::Player>();
        zge::spawnNamedEffect(*g_rippleHash, object, ripple);
    }
    READABLE_FUNCTION_WAITS(spawnSplashDrop, Verified);

    // A splash's drops as its root falls back through the water: five round
    // the water's point (`origin`), at the distance of `position` moved
    // `radius` along x, each turned a step further round and scattered up to a
    // half either way; then one at the point itself.
    void spawnSplashDrops(ee::Ptr<zge::Vec4> origin, ee::Ptr<zge::Vec4> position, ee::Float radius)
    {
        ee::Local<zge::Vec4> spoke; // from the origin to the first drop
        ee::Local<zge::Vec4> drop;
        *spoke = zge::aligned(position);
        spoke->x = spoke->x + radius;
        *spoke = *spoke - *origin;
        const auto scatter = []
        {
            return ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0xFF) - 0x80)) * ee::Float(1.0f / 128.0f) * ee::Float(0.5f);
        };
        zge::Angle angle = 0;
        for (uint32_t made = 0; made < kSplashDrops; ++made)
        {
            zge::vec3RotateY(&drop->xyz(), &spoke->xyz(), angle);
            drop->y = ee::Float(0.0f); // on the water
            *drop = *drop + *origin;
            drop->x = drop->x + scatter();
            drop->z = drop->z + scatter();
            spawnSplashDrop(drop);
            angle = static_cast<zge::Angle>(angle + *g_splashDropAngleStep);
        }
        spawnSplashDrop(origin);
    }
    READABLE_FUNCTION_WAITS(spawnSplashDrops, Verified);

    // A splash at `at`: a white ripple and the "splash" class's sparks,
    // thrown up. Nothing calls it.
    void spawnSplash(ee::Ptr<zge::Vec4> at)
    {
        ee::Local<SparkParams> sparks;
        ee::Local<RippleParameters> ripple;
        std::memcpy(&*sparks, &*ee::Ptr<SparkParams>(kSplashSparks), sizeof(SparkParams)); // all 0x40 bytes, as the original copies them
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        ripple->x = at->x;
        ripple->y = at->y;
        ripple->z = at->z;
        const uint8_t colour[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        std::memcpy(ripple->colour, colour, sizeof(colour));
        ripple->lifetime = 1000;
        ripple->size = ee::Float(0.3f);
        ripple->growth = ee::Float(2.5f);
        ripple->texture = 0;
        ripple->follows = ee::Ptr<skater::Player>();
        ripple->velocity = zge::kZero;
        sparks->position.x = ripple->x;
        sparks->position.y = ripple->y;
        sparks->position.z = ripple->z;
        sparks->velocity.x = ee::Float(0.0f);
        sparks->velocity.y = ee::Float(-0.07f); // up (y down)
        sparks->velocity.z = ee::Float(0.0f);
        sparks->size = ee::Float(0.2f);
        sparks->spread = ee::Float(4.0f);
        zge::spawnNamedEffect(*g_rippleHash, object, ripple);
        zge::spawnNamedEffect(zge::nameHash(ee::Ptr<const char>(kTextSplash)), object, sparks);
    }
    READABLE_FUNCTION_WAITS(spawnSplash, Draft);

    // A shard broken off a triangle: a "shard" (a "screenshard" with the
    // triangle's flag 1) whose frame sits at the triangle's centre, facing
    // along its normal with x toward the first corner, the corners put in
    // that frame; spinning at random, its velocity and lifetime the
    // triangle's within 30 % at random. Nothing calls it.
    void spawnShardFromTriangle(ee::Ptr<ShardTriangle> triangle)
    {
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        ee::Local<ShardParameters> shard;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Matrix> toShard; // the world to the shard's frame
        shard->texture = triangle->texture;
        shard->flags = triangle->flags;
        std::memcpy(shard->colour, triangle->colour, sizeof(shard->colour));
        const ee::Float lifetime = ee::toFloat(triangle->lifetime);
        const ee::Float lifetimeStep = ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1F) - 0x10)) * ee::Float(0.01875f);
        shard->lifetime = static_cast<uint32_t>(ee::toInt(lifetimeStep * lifetime + lifetime));

        *centre = zge::kZero;
        for (uint32_t i = 0; i < 3; ++i)
        {
            QuadVertex &corner = shard->corners[i];
            corner.u = triangle->corners[i].u;
            corner.v = triangle->corners[i].v;
            corner.position = zge::aligned(ee::Ptr<zge::Vec4>(&triangle->corners[i].position));
            *centre = *centre + corner.position;
        }
        *centre = *centre * ee::Float(1.0f / 3.0f);

        shard->spin.x = static_cast<zge::Angle>((zge::randomNumber() & 0xFFF) - 0x800);
        shard->spin.y = static_cast<zge::Angle>((zge::randomNumber() & 0xFFF) - 0x800);
        shard->spin.z = static_cast<zge::Angle>((zge::randomNumber() & 0xFFF) - 0x800);

        // The frame: z the triangle's normal, x toward the first corner, y
        // across them; at the centre.
        const zge::Vec4 edge = shard->corners[1].position - shard->corners[2].position;
        const zge::Vec4 edge2 = shard->corners[0].position - shard->corners[1].position;
        shard->frame[2] = zge::cross(edge, edge2);
        shard->frame[2] = zge::unit(shard->frame[2]);
        shard->frame[0] = shard->corners[0].position - *centre;
        shard->frame[0] = zge::unit(shard->frame[0]);
        shard->frame[1] = zge::cross(shard->frame[2], shard->frame[0]);
        shard->frame[3] = *centre;

        // Its inverse turn (no translation) takes the corners into it.
        for (int row = 0; row < 4; ++row)
            toShard->row(row) = shard->frame[row];
        toShard->translation() = zge::kZero;
        zge::matrixTranspose3(toShard, toShard);
        for (uint32_t i = 0; i < 3; ++i)
        {
            zge::Vec4 &position = shard->corners[i].position;
            position = position - shard->frame[3];
            position = zge::transformed(*toShard, position);
        }

        const ee::Float step = ee::Float(0.01875f);
        shard->velocity.x = triangle->velocity[0] * randomAroundOne(step);
        shard->velocity.y = triangle->velocity[1] * randomAroundOne(step);
        shard->velocity.z = triangle->velocity[2] * randomAroundOne(step);
        const uint32_t kind = (shard->flags & 1) ? *g_screenShardHash : *g_shardHash;
        zge::spawnNamedEffect(kind, object, shard);
    }
    READABLE_FUNCTION_WAITS(spawnShardFromTriangle, Draft);

    // "userspark"'s start: the placed effect's sparks due this frame.
    uint32_t startUserSparks(ee::Ptr<zge::SceneObject> object, ee::Ptr<UserEffect> effect)
    {
        return game::tail(zge::startEffect, object, *g_userSparkClass, effect->due, 0, effect);
    }
    READABLE_FUNCTION_WAITS(startUserSparks, Verified);

    // "spark"'s start: the parameters' count of sparks (the class's defaults
    // when none are given).
    uint32_t startSparks(ee::Ptr<zge::SceneObject> object, ee::Ptr<SparkParams> parameters)
    {
        const ee::Ptr<zge::EffectClass> sparkClass = *g_sparkClass;
        if (!parameters)
            parameters = ee::Ptr<SparkParams>(sym::sparkDefaults);
        return game::tail(zge::startEffect, object, sparkClass, parameters->count & 0xFF, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startSparks, Verified);
}
