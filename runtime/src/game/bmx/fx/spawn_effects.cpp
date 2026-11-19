// BMX XXX's marks and pieces (spawn_effects.h). BMX XXX's own code (fx.c's
// block); the skid mark and the shard follow Aggressive Inline's
// (game/aggressive/fx/spawn_effects.cpp), DM2 has the same four.

#include "spawn_effects.h"
#include "sleep.h"
#include "posters.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/particle_types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/spawn_effects.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The object the level's effects start on (0 before a level has one).
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_effectsObject> g_effectsObject;
        // The skid mark's material and the pock marks' three.
        inline ee::Global<uint32_t, sym::g_skidMaterial> g_skidMaterial;
        inline const ee::Ptr<uint32_t> kPockMarkMaterials(sym::g_pockMarkMaterials);

        // The effects' name hashes.
        constexpr uint32_t kSkidEffect = 0x00057FD4;        // "skid"
        constexpr uint32_t kPockMarkEffect = 0x0380133B;    // "pockmark"
        constexpr uint32_t kSleepEffect = zge::nameHashOf("sleep");
        constexpr uint32_t kShardEffect = zge::nameHashOf("shard");
        constexpr uint32_t kScreenShardEffect = 0x0A6040F4; // "screenshard"

        // A mark stays 30 seconds.
        constexpr uint32_t kMarkLifetime = 30000;

        // A skid mark: dark grey, half see-through; 0.05 wide, lifted 0.08
        // off the ground at the `from` end and 0.1 at the `to` end, and 0.01
        // longer than the step at each end.
        constexpr uint8_t kSkidColour[4] = {0x10, 0x10, 0x10, 0x80};
        constexpr ee::Float kShortestSkid = 0.02f;
        constexpr ee::Float kSkidHalfWidth = 0.025f;
        constexpr ee::Float kSkidLiftFrom = 0.08f, kSkidLiftTo = 0.1f;
        constexpr ee::Float kSkidOverlap = 0.01f;

        // A bullet mark: 0.07 square, 0.01 off its face.
        constexpr ee::Float kPockMarkLift = 0.01f;
        constexpr ee::Float kPockMarkHalf = 0.035f;

        // A random part of a range: (random 0 .. 31, less 16) times `step`,
        // added to 1.
        ee::Float randomAroundOne(ee::Float step)
        {
            return ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1F) - 0x10)) * step + ee::Float(1.0f);
        }
        constexpr ee::Float kShardSpread = 0.01875f;

        // Scaffolding (address-call): the engine's start of a named effect on an object.
        namespace calls
        {
        void spawnNamedEffect(uint32_t name, ee::Ptr<zge::SceneObject> object, ee::Address parameters)
        {
            ee::game(sym::spawnNamedEffect, name, object, parameters);
        }
        }
    }

    // spawnSkidMark: see spawn_effects.h. The mark's frame: across, the
    // ground's normal, along the step, at the `to` end; its corners go round
    // the strip, two at the `from` end, then two at the `to` end.
    void spawnSkidMark(ee::Ptr<zge::Vec4> from, ee::Ptr<zge::Vec4> to, ee::Ptr<zge::Vec4> normal)
    {
        ee::Local<PosterParameters> mark;
        ee::Local<zge::Matrix> strip;
        ee::Local<zge::Vec4> corner; // first the step from `to` to `from`, then each corner in the strip's frame
        zge::matrixIdentity(strip);
        *corner = *from - *to;
        const ee::Float length = zge::vec3Length(&corner->xyz());
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (length < kShortestSkid || !object)
            return;

        std::memcpy(mark->colour, kSkidColour, sizeof(mark->colour));
        mark->texture = *g_skidMaterial;
        mark->lifetime = kMarkLifetime;
        strip->row(2) = *corner * (ee::Float(1.0f) / length);
        strip->row(1) = zge::aligned(normal);
        strip->row(0) = zge::cross(strip->row(1), strip->row(2));
        strip->translation() = zge::aligned(to);
        strip->translation().w = 0.0f;

        for (uint32_t i = 0; i < 4; ++i)
        {
            const bool toEnd = (i & 2) != 0;
            corner->x = ((i + 1) & 2) ? -kSkidHalfWidth : kSkidHalfWidth;
            corner->y = toEnd ? kSkidLiftTo : kSkidLiftFrom;
            corner->z = toEnd ? -kSkidOverlap : length + kSkidOverlap;
            mark->corners[i] = zge::transformed(*strip, *corner);
        }
        calls::spawnNamedEffect(kSkidEffect, object, mark.address());
    }
    READABLE_FUNCTION_WAITS(spawnSkidMark, Verified);

    // spawnBulletMark: see spawn_effects.h. The mark's frame has the face's
    // normal as its up; its forward is turned about the world's up by a
    // random angle before the frame is squared to the normal. Nothing before
    // the level's effects object is there.
    void spawnBulletMark(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Vec3> normal, uint32_t texture, ee::Ptr<uint8_t> colour)
    {
        ee::Local<PosterParameters> mark;
        ee::Local<zge::Matrix> frame;
        ee::Local<zge::Vec4> corner;
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        mark->lifetime = kMarkLifetime;
        mark->texture = kPockMarkMaterials[texture];
        std::memcpy(mark->colour, colour.get(), sizeof(mark->colour));
        frame->row(1).x = normal->x;
        frame->row(1).y = normal->y;
        frame->row(1).z = normal->z;
        frame->row(2).x = 0.0f;
        frame->row(2).y = 0.0f;
        frame->row(2).z = 1.0f;
        const zge::Angle turn = static_cast<zge::Angle>(zge::randomNumber());
        zge::vec3RotateY(&frame->row(2).xyz(), &frame->row(2).xyz(), turn);
        frame->row(0) = zge::cross(frame->row(1), frame->row(2));
        frame->row(0) = zge::unit(frame->row(0));
        frame->row(2) = zge::cross(frame->row(0), frame->row(1));
        frame->translation() = zge::aligned(position);

        corner->y = kPockMarkLift;
        for (uint32_t i = 0; i < 4; ++i)
        {
            corner->x = ((i + 1) & 2) ? -kPockMarkHalf : kPockMarkHalf;
            corner->z = (i & 2) ? -kPockMarkHalf : kPockMarkHalf;
            mark->corners[i] = zge::transformed(*frame, *corner);
        }
        calls::spawnNamedEffect(kPockMarkEffect, object, mark.address());
    }
    READABLE_FUNCTION_WAITS(spawnBulletMark, Verified);

    // sleep: a "sleep" effect at the point. Nothing before the level's
    // effects object is there.
    void sleep(ee::Ptr<zge::Vec4> position, uint32_t lifetime)
    {
        ee::Local<SleepParameters> parameters;
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        parameters->lifetime = lifetime;
        parameters->position = zge::aligned(position);
        calls::spawnNamedEffect(kSleepEffect, object, parameters.address());
    }
    READABLE_FUNCTION_WAITS(sleep, Verified);

    // spawnShardFromTriangle: see spawn_effects.h. The shard's frame sits at
    // the triangle's centre, facing along its normal with x toward the first
    // corner, the corners put in that frame; spinning at random, its
    // velocity and lifetime the triangle's within 30 % at random.
    void spawnShardFromTriangle(ee::Ptr<ShardTriangle> triangle)
    {
        ee::Local<ShardParameters> shard;
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Matrix> toShard; // the world to the shard's frame
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        shard->texture = triangle->material.address;
        shard->flags = triangle->flags;
        std::memcpy(shard->colour, &triangle->colour, sizeof(shard->colour));
        const ee::Float lifetime = ee::toFloat(triangle->lifetime);
        const ee::Float lifetimeStep = ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1F) - 0x10)) * kShardSpread;
        shard->lifetime = static_cast<uint32_t>(ee::toInt(lifetimeStep * lifetime + lifetime));

        *centre = zge::kZero;
        for (uint32_t i = 0; i < 3; ++i)
        {
            ShardVertex &corner = shard->corners[i];
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

        shard->velocity.x = triangle->velocity.x * randomAroundOne(kShardSpread);
        shard->velocity.y = triangle->velocity.y * randomAroundOne(kShardSpread);
        shard->velocity.z = triangle->velocity.z * randomAroundOne(kShardSpread);
        const uint32_t effect = (shard->flags & 1) ? kScreenShardEffect : kShardEffect;
        calls::spawnNamedEffect(effect, object, shard.address());
    }
    READABLE_FUNCTION_WAITS(spawnShardFromTriangle, Verified);

    namespace
    {
        // One of four triangles a piece of debris is scaled from.
        inline const ee::Ptr<zge::Vec4[3]> kDebrisTriangles(sym::debrisTriangles);
        constexpr uint32_t kDebrisPieces = 8;
        constexpr uint32_t kShardDraw = 2; // ShardParameters::flags
        // A piece lives 1 to 3 seconds and is 0.25 to 0.85 of its triangle.
        constexpr ee::Float kDebrisLifeMin = 1000.0f, kDebrisLifeStep = 133.33334f;
        constexpr ee::Float kDebrisSizeMin = 0.25f, kDebrisSizeStep = 0.04f;
        // (random 0 .. 511, less 256) times this: about 0.75 either way.
        constexpr ee::Float kDebrisPlaceStep = 3.0f / 1024.0f;
    }

    // faceDebris: a breaking face's debris: eight untextured shards
    // ("shard") about the centre in the face's plane, each a random triangle
    // of a random size, life and spin, turned at random about the normal
    // and tipped, thrown with the velocity a little spread (each axis by 0.7
    // to 1.28). Aggressive Inline's debris with one kind. Nothing before
    // the level's effects object is there.
    void faceDebris(ee::Ptr<zge::Vec4> centre, ee::Ptr<zge::Vec4> velocity, ee::Ptr<zge::Vec4> normal)
    {
        ee::Local<ShardParameters> shard;
        ee::Local<zge::Vec4> offset;
        const ee::Ptr<zge::SceneObject> object = *g_effectsObject;
        if (!object)
            return;
        shard->flags = kShardDraw;
        shard->colour[0] = 0x80;
        shard->colour[1] = 0x80;
        shard->colour[2] = 0xD0;
        shard->colour[3] = 0x30;
        shard->texture = 0;
        for (uint32_t made = 0; made < kDebrisPieces; ++made)
        {
            // Its life and size.
            shard->lifetime = static_cast<uint32_t>(
                ee::toInt(ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFu)) * kDebrisLifeStep + kDebrisLifeMin));
            const ee::Float size = ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFu)) * kDebrisSizeStep + kDebrisSizeMin;

            // Its triangle: one of four, scaled.
            const zge::Vec4 (&triangle)[3] = kDebrisTriangles[zge::randomNumber() & 3u];
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                ShardVertex &vertex = shard->corners[corner];
                vertex.position = triangle[corner] * size;
                vertex.u = 0.0f;
                vertex.v = 0.0f;
            }

            // Its spin.
            shard->spin.x = static_cast<zge::Angle>((zge::randomNumber() & 0xFFFu) - 0x800);
            shard->spin.y = static_cast<zge::Angle>((zge::randomNumber() & 0xFFFu) - 0x800);
            shard->spin.z = static_cast<zge::Angle>((zge::randomNumber() & 0xFFFu) - 0x800);

            // Its frame: the normal, turned about y and x at random, made square.
            shard->frame[2] = zge::aligned(normal);
            const zge::Angle yaw = static_cast<zge::Angle>(zge::randomNumber());
            zge::vec3RotateY(&shard->frame[1].xyz(), &shard->frame[2].xyz(), yaw);
            const zge::Angle pitch = static_cast<zge::Angle>(zge::randomNumber());
            zge::vec3RotateX(&shard->frame[1].xyz(), &shard->frame[1].xyz(), pitch);
            shard->frame[0] = zge::cross(shard->frame[1], shard->frame[2]);
            shard->frame[0] = zge::unit(shard->frame[0]);
            shard->frame[1] = zge::cross(shard->frame[2], shard->frame[0]);

            // Its position: about the centre, in the plane of the normal.
            zge::Vec4 &place = shard->frame[3];
            place.x = centre->x + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FFu) - 0x100)) * kDebrisPlaceStep;
            place.y = centre->y + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FFu) - 0x100)) * kDebrisPlaceStep;
            place.z = centre->z + ee::toFloat(static_cast<int32_t>((zge::randomNumber() & 0x1FFu) - 0x100)) * kDebrisPlaceStep;
            *offset = place - *centre;
            const ee::Float along = zge::dot(*offset, *normal);
            const ee::Float alongX = normal->x * along, alongY = normal->y * along, alongZ = normal->z * along;
            place.x = place.x - alongX;
            place.y = place.y - alongY;
            place.z = place.z - alongZ;

            // Its velocity: the given one, each axis by 0.7 .. 1.28.
            shard->velocity.x = velocity->x * randomAroundOne(kShardSpread);
            shard->velocity.y = velocity->y * randomAroundOne(kShardSpread);
            shard->velocity.z = velocity->z * randomAroundOne(kShardSpread);
            calls::spawnNamedEffect(kShardEffect, object, shard.address());
        }
    }
    READABLE_FUNCTION_WAITS(faceDebris, Verified);
}
