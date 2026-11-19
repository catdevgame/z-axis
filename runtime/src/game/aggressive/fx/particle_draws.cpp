// The particle effects' draws (inlineFx.c; readable round gamehand2): the
// "spark", "smoke" and "firetrail" classes' draw methods (their class
// tables' +0x20; the results, particles drawn, are summed by the engine's
// emitter object), the cull in front of the spark and smoke draws, and the
// fire trail's spawn. The particles are camera-facing squares: the
// camera's matrix with the particle's position as its translation takes a
// corner offset to the world (sparks: a streak in the camera's frame, from
// the head along the way the tail moved on screen). See particles.h for
// the particles' layouts.

#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/scene/scene.h"
#include "aggressive/fx/billboard.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/collision/collision.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstring>
#include "sym/aggressive/fx/particle_draws.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        constexpr uint32_t kAllocateSized = sym::allocateSized;    // (bytes) -> a block of the smallest size-class pool that fits
        constexpr uint32_t kRandom = sym::randomNumber;           // () -> a random number
        constexpr uint64_t kDoubleOne = 0x3FF0000000000000ull;

        // A spark's streak is at most this long on screen.
        inline ee::Global<ee::Float, sym::g_longestStreak> g_longestStreak;
        // "smoke": the last puff's growth and fade (written as each is drawn).
        inline ee::Global<ee::Float, sym::g_puffGrowth> g_puffGrowth;
        inline ee::Global<uint32_t, sym::g_puffFade> g_puffFade;
        // "firetrail": the flames' colour, spacing along the trail, the
        // frames' time, and the class an effect starts with.
        inline ee::Global<uint8_t[4], sym::flameColour> g_flameColour;
        inline ee::Global<ee::Float, sym::g_flameSpacing> g_flameSpacing;
        inline ee::Global<ee::Float, sym::g_fireFrameTime> g_fireFrameTime;
        inline ee::Global<uint32_t[10], sym::g_fireFrames> g_fireFrames;
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_firetrailClass> g_firetrailClass;
    }

    // drawSmoke: "smoke"'s draw: each puff a camera-facing square, its
    // colour stepped through the puff's keys at their times (the rate to
    // the next key worked out at each step) and moved toward the next one
    // since, its alpha faded by its rate and its size grown with age (never
    // below its own). Returns how many it drew.
    int32_t drawSmoke(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        int32_t drawn = 0;
        const int32_t now = effectsClock();
        listCorners(billboard->quad);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            Puff &puff = *ee::Ptr<Puff>(particle);
            *g_puffFade = puff.fade;
            *g_puffGrowth = puff.growth;
            const int32_t age = now - static_cast<int32_t>(puff.born);
            // The keys by index (the original does not bound it).
            const ee::Ptr<uint32_t> keyTime(&puff.keyTime[0]);
            const ee::Ptr<uint8_t[4]> keyColour(&puff.keyColour[0]);
            uint32_t key = puff.key;
            if (static_cast<uint32_t>(age) >= keyTime[key])
            {
                // On to the next key: its colour, and the rate to the one after.
                puff.key = static_cast<uint8_t>(key + 1);
                std::memcpy(puff.colour, keyColour[(key + 1) & 0xFFu], 4);
                key = puff.key;
                const ee::Float span = ee::toFloat(static_cast<int32_t>(keyTime[key] - keyTime[key - 1]));
                for (int c = 0; c < 4; ++c)
                    puff.colourRate[c] = ee::toFloat(static_cast<int32_t>(keyColour[key][c]) - static_cast<int32_t>(keyColour[key - 1][c])) / span;
            }
            const int32_t since = age - static_cast<int32_t>(keyTime[key]);
            uint8_t(&colour)[4] = billboard->colour;
            std::memcpy(colour, puff.colour, 4);
            for (int c = 0; c < 3; ++c)
                colour[c] = static_cast<uint8_t>(colour[c] + ee::toInt(puff.colourRate[c] * ee::toFloat(since)));
            const uint32_t faded = static_cast<uint32_t>(age) * *g_puffFade / 1000u;
            if (faded < colour[3])
                colour[3] = static_cast<uint8_t>(colour[3] - faded);
            else
                colour[3] = 0;
            ee::Float size = puff.size * (*g_puffGrowth * ee::toFloat(age));
            if (size < puff.size)
                size = puff.size;
            billboard->quad.record.texture = puff.texture;
            placeBillboard(billboard, currentCameraMatrix(), puff.position);
            ++drawn;
            placeCorners(billboard, size);
            drawBillboard(billboard);
        }
        return drawn;
    }
    READABLE_FUNCTION(drawSmoke, Verified);

    // drawSmokeInView: "smoke"'s draw method: drawSmoke unless the effect
    // it belongs to is hidden.
    int32_t drawSmokeInView(uint32_t effectClass, ee::Ptr<Emitter> emitter)
    {
        return drawUnlessHidden(effectClass, emitter, EmitterDraw(sym::drawSmoke));
    }
    READABLE_FUNCTION(drawSmokeInView, Verified);

    namespace
    {
        // "spark": the colours a spark goes through over its life.
        constexpr uint32_t kSparkShades = 16;
        inline ee::Global<uint8_t[kSparkShades][4], sym::sparkColours> g_sparkColours;

        // drawSparks's locals, in its frame's order.
        struct SparkDraw
        {
            Quad quad;                   // +0x000 the spark's quad
            // +0x0A0 the camera's world matrix; its translation is set to each
            // spark's head: the quad's matrix.
            zge::Matrix camera;
            // +0x0E0 the camera's rotation inverted (its translation (0, 0, 0,
            // 1)): takes a point to the camera's frame.
            zge::Matrix toCamera;
            uint8_t colour[4];           // +0x120 the spark's
            uint8_t _124[0xC];
            zge::Vec4 streak;            // +0x130 head to tail in the camera's frame
            zge::Vec4 along;             // +0x140 the streak's direction on screen
            zge::Vec4 across;            // +0x150 the one across it
            zge::Vec4 head;              // +0x160 in the camera's frame
            zge::Vec4 tail;              // +0x170
            zge::Vec4 alongOffset;       // +0x180 a corner's offset along the streak
            zge::Vec4 acrossOffset;      // +0x190 and across it
            // +0x1A0 what the original keeps in the frame across the loop's
            // calls: the emitter, the clock, and where the first corner, the
            // head's place in the quad's matrix and the two offsets are.
            ee::Ptr<Emitter> emitter;
            int32_t now;                 // +0x1A4
            ee::Ptr<QuadVertex> firstCorner;       // +0x1A8
            ee::Ptr<zge::Vec4> headInMatrix;       // +0x1AC
            ee::Ptr<zge::Vec4> alongOffsetPlace;   // +0x1B0
            ee::Ptr<zge::Vec4> acrossOffsetPlace;  // +0x1B4
        };
        static_assert(offsetof(SparkDraw, camera) == 0xA0 && offsetof(SparkDraw, toCamera) == 0xE0);
        static_assert(offsetof(SparkDraw, colour) == 0x120 && offsetof(SparkDraw, streak) == 0x130);
        static_assert(offsetof(SparkDraw, alongOffset) == 0x180 && offsetof(SparkDraw, emitter) == 0x1A0);
        static_assert(offsetof(SparkDraw, acrossOffsetPlace) == 0x1B4);
    }

    // drawSparks: "spark"'s draw: each spark a streak in the camera's frame,
    // from its head along 1.75 times the way its tail lies on screen (made
    // shorter when [a stale vector's length: the original measures `along`
    // before it is written for this spark] passes the longest streak), its
    // colour from the sparks' 16 by age, the far end's alpha 0. Returns how
    // many it drew.
    int32_t drawSparks(uint32_t /*effect*/, ee::Ptr<Emitter> emitter)
    {
        ee::Local<SparkDraw> draw;
        draw->firstCorner = &draw->quad.corners[0];
        listCorners(draw->quad);
        draw->emitter = emitter;
        int32_t drawn = 0;
        draw->camera = currentCameraMatrix();
        draw->toCamera = draw->camera;
        zge::matrixTranspose3(&draw->toCamera, &draw->toCamera);
        draw->toCamera.translation() = zge::kZero;
        const int32_t now = effectsClock();
        draw->now = now;
        uint32_t particle = first(*emitter->particles);
        if (!particle)
            return drawn;
        draw->headInMatrix = &draw->camera.translation();
        draw->alongOffsetPlace = &draw->alongOffset;
        draw->acrossOffsetPlace = &draw->acrossOffset;
        do
        {
            const Particle &spark = *ee::Ptr<Particle>(particle);
            const int32_t shade = ee::toInt(ee::toFloat(now - static_cast<int32_t>(spark.born)) * ee::Float(15.0f) /
                                            ee::toFloat(static_cast<int32_t>(spark.lifetime)));
            std::memcpy(draw->colour, (*g_sparkColours)[static_cast<uint32_t>(shade) < kSparkShades ? shade : kSparkShades - 1], 4);
            // The quad's matrix moved to the head; the head and tail in the
            // camera's frame, and the streak between them.
            draw->camera.translation() = zge::quadwordOf(ee::Ptr<const zge::Vec4>(&spark.position));
            draw->head = zge::transformed(draw->toCamera, spark.position);
            draw->tail = zge::transformed(draw->toCamera, spark.tail);
            draw->streak = draw->tail - draw->head;
            draw->streak = draw->streak * ee::Float(1.75f);
            // The original measures `along`, not the streak it then shortens: after
            // the first spark that is the last spark's unit direction, never over
            // the longest. On a draw's first spark it is what the last function
            // this deep left on the stack (a smoke's or the scene's box corner, the
            // last draw's direction, lengths 1 to 31; drawWorldPrimitives' place in
            // its strips after drawParticles, past any length), and past the longest
            // (3.3) that one streak comes out shorter, or with no length at all.
            // This frame's place on the stack keeps it.
            const ee::Float measured = zge::vec3Length(&draw->along.xyz());
            const ee::Float longest = *g_longestStreak;
            if (longest < measured)
                draw->streak = draw->streak * (longest / measured);
            // Its direction on screen (x, y), and the one across it.
            draw->along = draw->streak;
            draw->along.z = ee::Float(0.0f);
            const ee::Float length = zge::vec3Length(&draw->along.xyz());
            if (ee::Float(0.0f) < length)
                draw->along = draw->along * (ee::Float(1.0f) / length);
            else
            {
                draw->along.z = ee::Float(0.0f);
                draw->along.x = ee::Float(1.0f);
                draw->along.y = ee::Float(0.0f);
            }
            ++drawn;
            draw->across.z = ee::Float(0.0f);
            draw->across.x = draw->along.y;
            draw->across.y = -draw->along.x;
            // The corners, in the quad's order: 0 and 1 at the head, 2 and 3 at
            // the far end, the streak's width either side.
            for (uint32_t i = 0; i < 4; ++i)
            {
                const bool farEnd = (i & 2u) != 0;
                const bool otherSide = ((i + 1) & 2u) != 0;
                QuadVertex &corner = draw->quad.corners[i];
                std::memcpy(corner.colour, draw->colour, 4);
                corner.u = farEnd ? ee::Float(1.0f) : ee::Float(0.0f);
                corner.v = otherSide ? ee::Float(1.0f) : ee::Float(0.0f);
                draw->alongOffset = draw->along * (farEnd ? -spark.size : spark.size);
                draw->acrossOffset = draw->across * (otherSide ? -spark.size : spark.size);
                corner.position = draw->alongOffset + draw->acrossOffset;
                if (farEnd)
                {
                    // The far end: along the streak, transparent.
                    corner.position = corner.position + draw->streak;
                    corner.colour[3] = 0;
                }
            }
            draw->quad.record.texture = spark.texture;
            zge::drawPrimitives(kWorldPrimitive, &draw->quad.record, 1, &draw->camera);
            particle = next(*emitter->particles);
        } while (particle);
        return drawn;
    }
    READABLE_FUNCTION(drawSparks, Verified);

    // drawSparksInView: "spark"'s draw method: drawSparks unless the effect
    // it belongs to is hidden.
    int32_t drawSparksInView(uint32_t effectClass, ee::Ptr<Emitter> emitter)
    {
        return drawUnlessHidden(effectClass, emitter, EmitterDraw(sym::drawSparks));
    }
    READABLE_FUNCTION(drawSparksInView, Verified);

    // drawFiretrail: "firetrail"'s draw: each flame a camera-facing square,
    // shrinking and fading with age. Returns how many it drew.
    int32_t drawFiretrail(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        int32_t drawn = 0;
        const int32_t now = effectsClock();
        listCorners(billboard->quad);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const Flame &flame = *ee::Ptr<Flame>(particle);
            std::memcpy(billboard->colour, flame.colour, 4);
            const int32_t age = now - static_cast<int32_t>(flame.born);
            const ee::Float size = flame.size - ee::toFloat(age) * flame.shrink;
            billboard->colour[3] = static_cast<uint8_t>(billboard->colour[3] - ee::toInt(ee::toFloat(age) * flame.fade));
            billboard->quad.record.texture = flame.texture;
            placeBillboard(billboard, currentCameraMatrix(), flame.position);
            ++drawn;
            placeCorners(billboard, size);
            drawBillboard(billboard);
        }
        return drawn;
    }
    READABLE_FUNCTION(drawFiretrail, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // libgcc's soft-float double conversions and sum.
    }

    // spawnFiretrail: "firetrail"'s spawn: flames one spacing apart from the
    // tail to the head (one more than fit), each a random flame frame,
    // while the particle budget lasts.
    void spawnFiretrail(uint32_t effect, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t /*count*/, uint32_t,
                        ee::Ptr<FiretrailParams> params)
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> along; // the tail to the head
        ee::Local<zge::Vec4> step;  // one flame to the next
        ee::Local<zge::Vec4> at;    // a flame's place
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        std::memcpy(&colour[0], *g_flameColour, 4);
        *along = params->head - params->tail;
        const ee::Float length = zge::vec3Length(asVec3(*along));
        const uint32_t count = static_cast<uint32_t>(ee::toInt(length / *g_flameSpacing) + 1) & 0xFFu;
        const ee::Float flames = ee::toFloat(static_cast<int32_t>(count));
        const ee::Float stepY = (params->head.y - params->tail.y) / flames;
        const ee::Float stepZ = (params->head.z - params->tail.z) / flames;
        ee::Float n = 0.0f; // the flame's number along the trail
        step->x = (params->head.x - params->tail.x) / flames;
        step->y = stepY;
        step->z = stepZ;
        if (!effect || !emitter || !count)
            return;
        for (uint32_t made = 0; made < count; ++made)
        {
            const ee::Ptr<Flame> flame = newParticle<Flame>(*emitter);
            if (!flame)
                return;
            flame->born = now;
            flame->flags = 0;
            const uint32_t frame = zge::randomNumber() % 10u;
            flame->texture = (*g_fireFrames)[frame];
            flame->size = params->size;
            std::memcpy(flame->colour, &colour[0], 4);
            flame->lifetime = params->lifetime;
            const ee::Float y = params->tail.y + step->y * n;
            const ee::Float z = params->tail.z + step->z * n;
            at->x = params->tail.x + step->x * n;
            at->y = y;
            at->z = z;
            flame->position = *at;
            flame->tail = flame->position;
            flame->velocity = zge::kZero;
            flame->frame = frame;
            flame->frameLeft = *g_fireFrameTime;
            flame->shrink = params->size / ee::toFloat(static_cast<int32_t>(params->lifetime << 1));
            flame->fade = ee::toFloat(static_cast<int32_t>(colour[3])) / ee::toFloat(static_cast<int32_t>(params->lifetime));
            n = sce::libgcc::dptofp(sce::libgcc::dpadd(sce::libgcc::fptodp(n), kDoubleOne)); // n + 1, in double precision as the C has it
            zge::pointerArrayAdd(emitter->particles, flame);
        }
    }
    READABLE_FUNCTION_WAITS(spawnFiretrail, Verified);

    // startFiretrail: a fire trail effect started (the engine's effect start
    // with the "firetrail" class).
    int32_t startFiretrail(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_firetrailClass, 1, 0, parameters);
    }
    READABLE_FUNCTION(startFiretrail, Verified);

    namespace
    {
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_impactRingClass> g_impactRingClassWord; // g_impactRingClassWord (g_impactRingClassWord)
        inline ee::Global<uint64_t, sym::g_splashSparks> g_spawnSplashTemplateInitializer0; // g_spawnSplashTemplateInitializer0; its name is a low-confidence guess (readable_names.tsv)
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_explosionClass> g_explosionClassWord; // g_explosionClassWord (g_explosionClassWord)
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_debrisClass> g_debrisClassWord; // g_debrisClassWord (g_debrisClassWord)
        inline ee::Global<int32_t, sym::g_debrisObject> g_debrisObject; // g_debrisObject ((anonymous)::g_debrisObject); its name is a low-confidence guess (readable_names.tsv)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(randomint32_t, ee::game<int32_t>, kRandom);
        }
    }

    uint32_t startImpactRings(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_impactRingClassWord, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startImpactRings, Verified);

    uint32_t startExplosion(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_explosionClassWord, 50, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startExplosion, Verified);

    uint32_t startDebris(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_debrisClassWord, 6, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startDebris, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t setNodeShown(ee::Ptr<zge::Node> appInstanceNode_, uint32_t value, uint32_t value2) { return zge::setNodeShown(appInstanceNode_, value, value2); }
        uint32_t setNodeCollides(ee::Ptr<zge::Node> x70, uint32_t value, uint32_t value2) { return zge::setNodeCollides(x70, value, value2); }
    }

    uint32_t drawDebris()
    {
        return 0;
    }
    READABLE_FUNCTION(drawDebris, Verified);
}
