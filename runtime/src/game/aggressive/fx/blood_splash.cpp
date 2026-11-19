// The blood's and the splashes' effect classes (inlineFx.c; were the
// sweep's drafts of spawnBloodSquirt .. moveSplashRing; fx/particles.h has the class
// table and the particles): "bloodsquirt" (drops that fall to their floor),
// "bloodcloud" (a puff that grows), "splashroot" (thrown up from the water,
// it leaves the splash's rings as it falls back, and a ring of drops where
// it goes through the water) and "splashring" (the band between two of
// those rings). Each class's spawn (+0x18), move (+0x1C) and draw (+0x20).
// Also "bloodpool" (a flat pool that spreads) and the "splash" class's
// start and draw (grey drops facing the camera).
// The y axis points down: falling is y growing.
//

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"

#include <cstring>
#include "sym/aggressive/fx/blood_splash.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        // The classes' colours (RGBA) and constants.
        inline ee::Global<uint8_t[4], sym::bloodSquirtColour> g_bloodSquirtColour;
        inline ee::Global<uint8_t[4], sym::bloodCloudColour> g_bloodCloudColour;
        inline ee::Global<uint8_t[4], sym::splashRootColour> g_splashRootColour;
        inline ee::Global<uint8_t[4], sym::splashRingColour> g_splashRingColour;
        // Alpha a millisecond of the time a particle has left, the first
        // read to test against the cap and the second for the alpha (two
        // words of the same value).
        inline ee::Global<ee::Float, sym::g_bloodSquirtFadeTest> g_bloodSquirtFadeTest;
        inline ee::Global<ee::Float, sym::g_bloodSquirtFade> g_bloodSquirtFade;
        inline ee::Global<ee::Float, sym::g_bloodCloudFadeTest> g_bloodCloudFadeTest;
        inline ee::Global<ee::Float, sym::g_bloodCloudFade> g_bloodCloudFade;
        inline ee::Global<ee::Float, sym::g_splashRootSize> g_splashRootSize;
        inline ee::Global<ee::Float, sym::g_splashRootBounce> g_splashRootBounce; // a falling root's speed times this as it goes through the water
        inline constexpr ee::Float kBloodGravity = 15.0f;  // a drop's fall speeds up by this a second
        inline constexpr ee::Float kSplashGravity = 60.0f; // a root's
        inline constexpr ee::Float kSettledSpeed = 1.0f;   // a root under the water slower than this is gone
        constexpr uint32_t kBloodTextures = 3;             // small, medium, large
        constexpr uint32_t kSplashRootLifetime = 2000;     // milliseconds
        constexpr uint32_t kSplashRingLifetime = 500;
        constexpr uint32_t kTicksPerRing = 3;
        constexpr uint32_t kRingPoints = 16;

        // A random number below n.
        uint32_t randomBelow(uint32_t n)
        {
            return zge::randomNumber() % n;
        }
    }

    // spawnBloodSquirt ("bloodsquirt" +0x18): `count` drops at the squirt's
    // position with its velocity, size, lifetime and floor, each with one
    // of the three blood textures at random.
    void spawnBloodSquirt(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<BloodParameters> squirt)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<BloodDrop> drop = newParticle<BloodDrop>(*emitter);
            if (!drop)
                break;
            drop->born = static_cast<uint32_t>(clock);
            drop->flags = kParticleMoves;
            drop->texture = (&*g_bloodSmallTexture)[randomBelow(kBloodTextures)];
            drop->size = squirt->size;
            std::memcpy(drop->colour, *g_bloodSquirtColour, 4);
            drop->lifetime = squirt->lifetime;
            drop->position = squirt->position;
            drop->tail = drop->position;
            drop->velocity = squirt->velocity;
            drop->landed = 0;
            drop->floor = squirt->floorOrGrowth;
            zge::pointerArrayAdd(emitter->particles, drop);
        }
    }
    READABLE_FUNCTION_WAITS(spawnBloodSquirt, Verified);

    // moveBloodSquirt ("bloodsquirt" +0x1C): once a tick, drops whose life is
    // over are freed; the others fall (gravity on their velocity, then the
    // step over the tick, the tail left where they were) until they reach
    // their floor, where they stay. Returns how many it looked at.
    int32_t moveBloodSquirt(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t looked = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++looked)
        {
            BloodDrop &drop = *ee::Ptr<BloodDrop>(particle);
            if (clock - drop.born >= drop.lifetime)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            if (!drop.landed)
            {
                drop.velocity.y = drop.velocity.y + *g_tickTime * kBloodGravity;
                drop.tail = drop.position;
                step = drop.velocity * *g_tickTime;
                drop.position = drop.position + *step;
                const ee::Float floor = drop.floor;
                if (floor <= drop.position.y)
                {
                    drop.position.y = floor;
                    drop.landed = 1;
                }
            }
            particle = next(*emitter->particles);
        }
        emitter->lastMoved = clock;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveBloodSquirt, Verified);

    // drawBloodSquirt ("bloodsquirt" +0x20): each drop a billboard of its
    // size, texture and colour, its alpha fading out over its last second.
    // Returns how many it drew.
    int32_t drawBloodSquirt(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        int32_t drawn = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        listCorners(billboard->quad);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const BloodDrop &drop = *ee::Ptr<BloodDrop>(particle);
            std::memcpy(billboard->colour, drop.colour, 4);
            const uint32_t left = drop.lifetime - (clock - drop.born);
            billboard->colour[3] = fadedAlpha(left, *g_bloodSquirtFadeTest, *g_bloodSquirtFade, 255);
            billboard->quad.record.texture = drop.texture;
            placeBillboard(billboard, currentCameraMatrix(), drop.position);
            ++drawn;
            placeCorners(billboard, drop.size);
            drawBillboard(billboard);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawBloodSquirt, Verified);

    // spawnBloodCloud ("bloodcloud" +0x18): `count` clouds at the effect's
    // position with its velocity, size, growth and lifetime.
    void spawnBloodCloud(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<BloodParameters> cloud)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<BloodCloud> puff = newParticle<BloodCloud>(*emitter);
            if (!puff)
                return;
            puff->born = static_cast<uint32_t>(clock);
            puff->flags = 0;
            puff->texture = *g_bloodCloudTexture;
            puff->size = cloud->size;
            std::memcpy(puff->colour, *g_bloodCloudColour, 4);
            puff->lifetime = cloud->lifetime;
            puff->position = cloud->position;
            puff->tail = puff->position;
            puff->velocity = cloud->velocity;
            puff->growth = cloud->floorOrGrowth;
            zge::pointerArrayAdd(emitter->particles, puff);
        }
    }
    READABLE_FUNCTION_WAITS(spawnBloodCloud, Verified);

    // moveBloodCloud ("bloodcloud" +0x1C): once a tick, clouds whose life is
    // over are freed and the others step by their velocity over the tick.
    // Returns how many it looked at.
    int32_t moveBloodCloud(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t looked = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        for (uint32_t particle = first(*emitter->particles); particle; ++looked)
        {
            BloodCloud &puff = *ee::Ptr<BloodCloud>(particle);
            if (clock - puff.born >= puff.lifetime)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            puff.tail = puff.position;
            step = puff.velocity * *g_tickTime;
            puff.position = puff.position + *step;
            particle = next(*emitter->particles);
        }
        emitter->lastMoved = clock;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveBloodCloud, Verified);

    // drawBloodCloud ("bloodcloud" +0x20): each cloud a billboard of its size
    // grown with age (never below it), its alpha at most half, fading out
    // over its last 300 milliseconds. Returns how many it drew.
    int32_t drawBloodCloud(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<Billboard> billboard;
        int32_t drawn = 0;
        const int32_t clock = effectsClock();
        listCorners(billboard->quad);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const BloodCloud &puff = *ee::Ptr<BloodCloud>(particle);
            std::memcpy(billboard->colour, puff.colour, 4);
            const uint32_t age = static_cast<uint32_t>(clock) - puff.born;
            billboard->colour[3] = fadedAlpha(puff.lifetime - age, *g_bloodCloudFadeTest, *g_bloodCloudFade, 0x80);
            const ee::Float size = puff.size;
            ee::Float grown = size + size * (puff.growth * ee::toFloat(static_cast<int32_t>(clock - static_cast<int32_t>(puff.born))));
            if (grown < size)
                grown = size;
            billboard->quad.record.texture = puff.texture;
            placeBillboard(billboard, currentCameraMatrix(), puff.position);
            placeCorners(billboard, grown);
            drawBillboard(billboard);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawBloodCloud, Verified);

    // spawnSplashRoot ("splashroot" +0x18): `count` roots at the splash's
    // position with its velocity and its rings' radius, each living two
    // seconds at most.
    void spawnSplashRoot(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<SplashParameters> splash)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        const ee::Float size = *g_splashRootSize;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<SplashRoot> root = newParticle<SplashRoot>(*emitter);
            if (!root)
                break;
            root->born = static_cast<uint32_t>(clock);
            root->flags = 0;
            root->size = size;
            root->texture = *g_splashRingTexture;
            std::memcpy(root->colour, *g_splashRootColour, 4);
            root->lifetime = kSplashRootLifetime;
            root->position = splash->position;
            root->tail = root->position;
            root->velocity = splash->velocity;
            root->ticks = 0;
            root->radius = splash->radius;
            root->dropsMade = 0;
            root->origin = splash->position;
            root->origin2 = splash->position;
            root->lastRing = splash->position;
            zge::pointerArrayAdd(emitter->particles, root);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSplashRoot, Verified);

    // moveSplashRoot ("splashroot" +0x1C): once a tick each root falls
    // (gravity, then the step over the tick); one that goes through the
    // water bounces back up (its speed down times the bounce, -0.6) and, the
    // first time, leaves a ring of drops; every third tick it leaves a ring
    // between where it was at the last one and where it is. A root whose
    // life is over, or one under the water that has all but stopped, is
    // freed. Returns how many it looked at.
    int32_t moveSplashRoot(uint32_t, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        int32_t looked = 0;
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        if (!emitter)
            return 0;
        if (!emitter->particles || !(emitter->lastMoved < clock))
            return 0;
        uint32_t particle = first(*emitter->particles);
        // The game reads the bounce only once there is a root.
        const ee::Float bounce = particle ? *g_splashRootBounce : ee::Float(0.0f);
        for (; particle; ++looked)
        {
            SplashRoot &root = *ee::Ptr<SplashRoot>(particle);
            bool gone = true;
            if (clock - root.born < root.lifetime)
                gone = ee::abs(root.velocity.y) < kSettledSpeed && root.origin.y <= root.position.y;
            if (gone)
            {
                particle = freeCurrent(emitter, particle);
                continue;
            }
            root.velocity.y = root.velocity.y + *g_tickTime * kSplashGravity;
            root.tail = root.position;
            step = root.velocity * *g_tickTime;
            root.position = root.position + *step;
            if (root.origin.y <= root.position.y)
            {
                const ee::Float fall = root.velocity.y;
                if (ee::Float(0.0f) < fall)
                    root.velocity.y = fall * bounce;
                if (!root.dropsMade)
                {
                    spawnSplashDrops(&root.origin, &root.position, root.radius);
                    root.dropsMade = 1;
                }
            }
            root.ticks = root.ticks + 1;
            if (root.ticks == kTicksPerRing)
            {
                spawnSplashRing(&root.origin, &root.lastRing, &root.position, root.radius);
                root.lastRing = root.position;
                root.ticks = 0;
            }
            particle = next(*emitter->particles);
        }
        emitter->lastMoved = clock;
        return looked;
    }
    READABLE_FUNCTION_WAITS(moveSplashRoot, Verified);

    // drawSplashRoot ("splashroot" +0x20): a root is not drawn (its rings are).
    int32_t drawSplashRoot()
    {
        return 0;
    }
    READABLE_FUNCTION(drawSplashRoot, Verified);

    // spawnSplashRing2 ("splashring" +0x18): `count` rings, each the band
    // between the two rings of 16 points the splash's root made (`rings`:
    // the root's ring, then the ring before), living half a second.
    void spawnSplashRing2(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<zge::Vec4> rings)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<SplashRing> ring = newParticle<SplashRing>(*emitter);
            if (!ring)
                break;
            ring->born = static_cast<uint32_t>(clock);
            ring->flags = 0;
            ring->size = 0.0f;
            ring->texture = *g_splashRingTexture;
            std::memcpy(ring->colour, *g_splashRingColour, 4);
            ring->lifetime = kSplashRingLifetime;
            ring->position = zge::kZero;
            ring->tail = zge::kZero;
            ring->velocity = zge::kZero;
            ring->ticks = 0;
            zge::memoryCopy(ring->ring, rings, sizeof(ring->ring));
            zge::memoryCopy(ring->ringBefore, rings + kRingPoints, sizeof(ring->ringBefore));
            zge::pointerArrayAdd(emitter->particles, ring);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSplashRing2, Verified);

    // moveSplashRing ("splashring" +0x1C): each ring counts its ticks and
    // takes the second ring texture at its third; then the move the classes
    // share.
    int32_t moveSplashRing(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            SplashRing &ring = *ee::Ptr<SplashRing>(particle);
            ring.ticks = ring.ticks + 1;
            if (ring.ticks == kTicksPerRing)
                ring.texture = *g_splashRing2Texture;
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveSplashRing, Verified);
    namespace
    {
        // The splash ring's draw: alpha a millisecond of the time it has
        // left (0.512: half alpha from its last quarter second), and the
        // texture's u a band of the ring (half the texture's width).
        inline ee::Global<ee::Float, sym::g_splashRingFade> g_splashRingFade;
        inline ee::Global<ee::Float, sym::g_splashRingUStep> g_splashRingUStep;
    }

    // drawSplashRings ("splashring" +0x20): each ring the band between its
    // two rings of 16 points, a quad a step round (the texture's u running
    // half its width a step, over again every two), at most half alpha,
    // fading over its last quarter second. Returns how many it drew.
    int32_t drawSplashRings(uint32_t, ee::Ptr<Emitter> emitter)
    {
        // The band's quad: the ring's point, the ring before's, the ring
        // before's next and the ring's next.
        ee::Local<Quad> band;
        ee::Local<uint8_t[4]> colour;
        QuadVertex &here = band->corners[0];
        QuadVertex &hereBefore = band->corners[1];
        QuadVertex &nextBefore = band->corners[2];
        QuadVertex &next = band->corners[3];
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        listCorners(band);
        int32_t drawn = 0;
        uint32_t particle = first(*emitter->particles);
        // The game reads the fade time only once there is a ring.
        const ee::Float fade = particle ? *g_splashRingFade : ee::Float(0.0f);
        for (; particle; particle = fx::next(*emitter->particles))
        {
            const SplashRing &ring = *ee::Ptr<SplashRing>(particle);
            std::memcpy(&colour[0], ring.colour, 4);
            const uint32_t left = ring.lifetime - (clock - ring.born);
            colour[3] = fadedAlpha(left, fade, fade, 0x80);
            uint32_t step = 0; // the texture's u runs 0, 1, 2 = 0, 1, 2 ...
            band->record.texture = ring.texture;
            for (uint32_t i = 0; i < kRingPoints; ++i)
            {
                if (step == 2)
                    step = 0;
                const uint32_t stepNext = step + 1;
                const ee::Float uStep = *g_splashRingUStep;
                std::memcpy(here.colour, &colour[0], 4);
                here.u = unsignedToFloat(step) * uStep;
                here.v = 0.0f;
                std::memcpy(hereBefore.colour, &colour[0], 4);
                hereBefore.u = unsignedToFloat(step) * uStep;
                hereBefore.v = 1.0f;
                std::memcpy(nextBefore.colour, &colour[0], 4);
                nextBefore.u = unsignedToFloat(stepNext) * uStep;
                nextBefore.v = 1.0f;
                std::memcpy(next.colour, &colour[0], 4);
                next.u = unsignedToFloat(stepNext) * uStep;
                next.v = 0.0f;
                const uint32_t after = i + 1 < kRingPoints ? i + 1 : 0; // round the ring
                here.position = ring.ring[i];
                hereBefore.position = ring.ringBefore[i];
                nextBefore.position = ring.ringBefore[after];
                next.position = ring.ring[after];
                zge::drawPrimitives(kWorldPrimitive, &band->record, 1, 0);
                step = stepNext;
            }
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSplashRings, Verified);

    // startBloodPool: a "bloodpool" effect on the object with the parameters.
    uint32_t startBloodPool(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_bloodPoolClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startBloodPool, Verified);

    namespace
    {
        // The pool's colour, and its alpha a millisecond of the time it has
        // left (two words of the same value, as the squirt's).
        inline ee::Global<uint8_t[4], sym::g_bloodPoolColour> g_bloodPoolColour;
        inline ee::Global<ee::Float, sym::g_bloodPoolFadeTest> g_bloodPoolFadeTest;
        inline ee::Global<ee::Float, sym::g_bloodPoolFade> g_bloodPoolFade;
    }

    // newBloodPoolEmitter ("bloodpool" +0x10): the class's emitter.
    uint32_t newBloodPoolEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, sizeof(BloodPool), sym::newBloodPoolEmitter);
    }
    READABLE_FUNCTION_WAITS(newBloodPoolEmitter, Verified);

    // spawnBloodPool ("bloodpool" +0x18): one pool, whatever the count asked
    // for, at the parameters' position, lying in the plane their orientation
    // turns to, in the pool colour, with their size, growth, largest size and
    // lifetime. It does not move.
    void spawnBloodPool(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t /*count*/, uint32_t,
                        ee::Ptr<BloodPoolParameters> parameters)
    {
        const int32_t clock = effectsClock();
        ee::Local<uint8_t[4]> colour;
        std::memcpy(&colour[0], *g_bloodPoolColour, 4);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<BloodPool> pool = newParticle<BloodPool>(*emitter);
        if (!pool)
            return;
        pool->born = static_cast<uint32_t>(clock);
        pool->flags = 0;
        pool->texture = *g_bloodPoolTexture;
        pool->size = parameters->size;
        std::memcpy(pool->colour, &colour[0], 4);
        pool->lifetime = parameters->lifetime;
        pool->tail = parameters->position;
        pool->position = pool->tail;
        pool->velocity = zge::kZero;
        zge::matrixIdentity(&pool->frame);
        zge::quaternionToMatrix(&pool->frame, &parameters->orientation);
        zge::matrixClearTranslation(&pool->frame);
        zge::matrixRow(pool->frame, 3) = pool->position;
        pool->growth = parameters->growth;
        pool->largest = parameters->largest;
        zge::pointerArrayAdd(emitter->particles, pool);
    }
    READABLE_FUNCTION_WAITS(spawnBloodPool, Verified);

    // drawBloodPool ("bloodpool" +0x20): each pool a square in its frame's
    // x-z plane, growing with its age up to its largest size, its alpha
    // fading over the time it has left. Returns how many it drew.
    int32_t drawBloodPool(uint32_t, ee::Ptr<Emitter> emitter)
    {
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        ee::Local<Quad> quad;
        ee::Local<zge::Vec4> corner; // in the pool's frame
        ee::Local<uint8_t[4]> colour;
        listCorners(*quad);
        int32_t drawn = 0;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            BloodPool &pool = *ee::Ptr<BloodPool>(particle);
            std::memcpy(&colour[0], pool.colour, 4);
            const uint32_t age = clock - pool.born;
            colour[3] = fadedAlpha(pool.lifetime - age, *g_bloodPoolFadeTest, *g_bloodPoolFade, 255);
            ee::Float half = pool.size + pool.size * (pool.growth * ee::toFloat(static_cast<int32_t>(age)));
            if (pool.largest < half)
                half = pool.largest;
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &vertex = quad->corners[i];
                std::memcpy(vertex.colour, &colour[0], 4);
                const bool left = ((i + 1) & 2u) != 0; // corners 1 and 2
                const bool far = (i & 2u) != 0;        // corners 2 and 3
                vertex.u = left ? 1.0f : 0.0f;
                vertex.v = far ? 1.0f : 0.0f;
                corner->x = left ? -half : half;
                corner->z = far ? half : -half;
                corner->y = 0.0f;
                vertex.position = zge::transformed(pool.frame, *corner);
            }
            quad->record.texture = pool.texture;
            zge::drawPrimitives(kWorldPrimitive, &quad->record, 1, 0);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawBloodPool, Draft);

    namespace
    {
        // What a splash starts from when none is given.
        constexpr uint32_t kSplashDefaults = sym::g_splashParameters;
        constexpr uint32_t kSplashDrops = 32;

    }

    // startSplash ("splash", the named effect's start): 32 of the "splash"
    // class's drops on the object, from the parameters (the defaults when
    // none).
    uint32_t startSplash(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startNamedEffect, object, sym::g_textSplash, kSplashDrops, 0, parameters ? parameters : kSplashDefaults);
    }
    READABLE_FUNCTION_WAITS(startSplash, Verified);

    namespace
    {
        // A quad for the primitive API's quad draw: its texture and its four
        // corners.
        struct QuadCorners
        {
            uint32_t texture;
            ee::Ptr<QuadVertex> corners[4];
        };
        static_assert(sizeof(QuadCorners) == 0x14);

        // A splash drop's alpha: an eighth of the milliseconds it has left, at
        // most 0x80 (none when it has more than 30 seconds left).
        uint8_t splashAlpha(uint32_t left)
        {
            if (left >= 30001u)
                left = 0;
            return static_cast<uint8_t>(left < 0x400u ? left >> 3 : 0x80u);
        }
    }

    // drawSplash ("splash" +0x20): each drop a grey camera-facing square of
    // its size with the splash texture, as light as its alpha. Returns how
    // many it drew.
    int32_t drawSplash(uint32_t, ee::Ptr<Emitter> emitter)
    {
        const uint32_t clock = static_cast<uint32_t>(effectsClock());
        ee::Local<QuadCorners> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> camera;
        ee::Local<uint8_t[4]> colour;
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &vertices[i];
        *camera = currentCameraMatrix();
        int32_t drawn = 0;
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
        {
            const Particle &drop = *ee::Ptr<Particle>(particle);
            zge::matrixRow(*camera, 3) = drop.position;
            std::memcpy(&colour[0], drop.colour, 4);
            colour[3] = splashAlpha(drop.lifetime - (clock - drop.born));
            colour[0] = colour[3];
            colour[2] = colour[3];
            colour[1] = colour[3];
            // The corners from the last to the first: (-, -), (-, +), (+, +),
            // (+, -) in the camera's x and y, each with its corner of the
            // texture.
            for (int32_t i = 3; i >= 0; --i)
            {
                QuadVertex &vertex = vertices[i];
                const bool left = i >= 2;              // corners 2 and 3
                const bool low = i == 0 || i == 3;     // corners 0 and 3
                std::memcpy(vertex.colour, &colour[0], 4);
                vertex.position.x = left ? -drop.size : drop.size;
                vertex.position.z = 0.0f;
                vertex.position.y = low ? -drop.size : drop.size;
                vertex.position = zge::transformed(*camera, vertex.position);
                vertex.u = left ? 0.0f : 1.0f;
                vertex.v = low ? 0.0f : 1.0f;
            }
            quad->texture = *g_splashTexture;
            zge::drawPrimitiveQuads(kWorldPrimitive, quad, 1, 0);
            ++drawn;
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSplash, Draft);
}
