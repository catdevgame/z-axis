// More of the effect classes' methods (inlineFx.c; were the sweep's
// drafts of newBubbleEmitter .. startSplashRing; fx/particles.h has the class table):
// the bubbles', fireflies', blood's and splashes' emitters, the starts the
// level and the placed effects use (startEffect with the class and its
// parameters), the bubbles' and fireflies' moves, their spawns (placed and
// not), and the bubbles' and fireflies' draws. The blood's and
// the splashes' spawns, moves and draws are in blood_splash.cpp.
//

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"
#include "aggressive/globals.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"
#include "zge/container/cursor_array.h"
#include "zge/memory/regions.h"

#include <cstring>
#include "sym/aggressive/fx/effect_classes.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        constexpr uint32_t kRandom = sym::randomNumber;         // () -> a random number
        // (object, class, count, 0, parameters): count particles of the class
        // spawned in the object's emitter of it (zge/scene/particle_emitter.cpp).

        // .sdata: how far around the spawn point a placed bubble starts.
        inline ee::Global<ee::Float, sym::g_bubbleScatter> g_bubbleScatter;
        inline constexpr ee::Float kRandomStep = 0.0078125f; // 1/128: a random byte's step in [-1, 1)
        inline constexpr ee::Float kBubbleSizeSpread = 0.75f; // of the size, at random either way

        // Each class's particle size (its pool).
        constexpr uint32_t kBubbleBytes = 0x80;
        constexpr uint32_t kFireflyBytes = 0xA0;
        constexpr uint32_t kBloodBytes = 0x80;
        constexpr uint32_t kSplashRootBytes = 0xC0;
        constexpr uint32_t kSplashRingBytes = 0x280;

        // A random number in [-1, 1) in steps of 1/128, the step as the
        // original keeps it in a register (`step`).
        ee::Float randomUnit(ee::Float step)
        {
            const uint32_t r = zge::randomNumber();
            return ee::toFloat(static_cast<int32_t>((r & 0xFFu) - 0x80)) * step;
        }
    }

    // A class's emitter: room for `capacity` particles of its size; `copy`
    // the copy's address (the effects clock's return at +0x4C, the
    // return's block at +0x54: the copies are the same code).
    uint32_t newClassEmitter(uint32_t effectClass, uint32_t capacity, uint32_t particleBytes, uint32_t copy)
    {
        const uint32_t slots = capacity & 0xFFFFu;
        const uint32_t address = zge::heapAllocate(static_cast<uint32_t>(sizeof(Emitter)));
        if (address)
        {
            const ee::Ptr<CursorArray> particles = zge::newCursorArray(slots);
            Emitter &emitter = *ee::at<Emitter>(address);
            emitter.effectClass = effectClass;
            emitter.particles = particles;
            emitter.particleBytes = particleBytes;
            emitter.lastMoved = static_cast<uint32_t>(effectsClock());
        }
        return address;
    }

    // The classes' "make an emitter" methods (+0x10): (class, capacity).
    // newBubbleEmitter: "bubble" and "userbubble".
    uint32_t newBubbleEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kBubbleBytes, sym::newBubbleEmitter);
    }
    READABLE_FUNCTION_WAITS(newBubbleEmitter, Verified);

    // newFireflyEmitter: "firefly" and "userfirefly".
    uint32_t newFireflyEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kFireflyBytes, sym::newFireflyEmitter);
    }
    READABLE_FUNCTION_WAITS(newFireflyEmitter, Verified);

    // newBloodSquirtEmitter: "bloodsquirt".
    uint32_t newBloodSquirtEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kBloodBytes, sym::newBloodSquirtEmitter);
    }
    READABLE_FUNCTION_WAITS(newBloodSquirtEmitter, Verified);

    // newBloodCloudEmitter: "bloodcloud".
    uint32_t newBloodCloudEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kBloodBytes, sym::newBloodCloudEmitter);
    }
    READABLE_FUNCTION_WAITS(newBloodCloudEmitter, Verified);

    // newSplashRootEmitter: "splashroot".
    uint32_t newSplashRootEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kSplashRootBytes, sym::newSplashRootEmitter);
    }
    READABLE_FUNCTION_WAITS(newSplashRootEmitter, Verified);

    // newSplashRingEmitter: "splashring".
    uint32_t newSplashRingEmitter(uint32_t effectClass, uint32_t capacity)
    {
        return newClassEmitter(effectClass, capacity, kSplashRingBytes, sym::newSplashRingEmitter);
    }
    READABLE_FUNCTION_WAITS(newSplashRingEmitter, Verified);

    // The effects' starts: the class's particles in `object`'s emitter of
    // it, from `parameters` (the class's spawn reads them).
    // startUserBubbles: a placed effect's bubbles, as many as it has due.
    uint32_t startUserBubbles(ee::Ptr<zge::SceneObject> object, ee::Ptr<UserEffect> effect)
    {
        return game::tail(zge::startEffect, object, *g_userBubbleClass, effect->due, 0, effect);
    }
    READABLE_FUNCTION_WAITS(startUserBubbles, Verified);

    // startUserFireflies: a placed effect's fireflies, as many as it has due.
    uint32_t startUserFireflies(ee::Ptr<zge::SceneObject> object, ee::Ptr<UserEffect> effect)
    {
        return game::tail(zge::startEffect, object, *g_userFireflyClass, effect->due, 0, effect);
    }
    READABLE_FUNCTION_WAITS(startUserFireflies, Verified);

    // startVapor: a vapor streak.
    uint32_t startVapor(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_vaporClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startVapor, Verified);

    // startBloodSquirt: a squirt of blood.
    uint32_t startBloodSquirt(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_bloodSquirtClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startBloodSquirt, Verified);

    // startBloodCloud: a cloud of blood.
    uint32_t startBloodCloud(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_bloodCloudClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startBloodCloud, Verified);

    // startSplashRoot: a splash's root (the column of water).
    uint32_t startSplashRoot(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_splashRootClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startSplashRoot, Verified);

    // startSplashRing: a splash's ring.
    uint32_t startSplashRing(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_splashRingClass, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startSplashRing, Verified);

    // moveBubbles ("bubble" and "userbubble" +0x1C): once a tick each
    // bubble drifts by its own motion; then the move the classes share.
    int32_t moveBubbles(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        ee::Local<zge::Vec4> step;
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                Bubble &bubble = *ee::Ptr<Bubble>(particle);
                step = bubble.drift * *g_tickTime;
                bubble.position = bubble.position + *step;
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveBubbles, Verified);

    // spawnUserBubbles ("userbubble" +0x18): 1 to `due` bubbles at random
    // (the placed effect's), each at the effect's position (with its
    // spread) scattered a little, with its velocity, size, lifetime (each
    // with its spread), drift, colour and texture (the bubble's unless the
    // effect has its own).
    void spawnUserBubbles(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<UserEffect> effect)
    {
        const int32_t clock = effectsClock();
        const uint32_t r = zge::randomNumber();
        const uint32_t due = effect->due;
        // 1 .. due at random (a due of 0 takes the random number whole).
        const uint32_t count = ((due ? r % due : r) + 1) & 0xFFu;
        const ee::Float x = effect->position.x + effect->positionSpread.x * randomUnit(kRandomStep);
        const ee::Float y = effect->position.y + effect->positionSpread.y * randomUnit(kRandomStep);
        const ee::Float z = effect->position.z + effect->positionSpread.z * randomUnit(kRandomStep);
        if (!effectClass || !emitter || !count)
            return;
        for (uint32_t made = 0; made < count; ++made)
        {
            const ee::Ptr<Bubble> bubble = newParticle<Bubble>(*emitter);
            if (!bubble)
                break;
            bubble->born = static_cast<uint32_t>(clock);
            bubble->flags = kParticleMoves;
            bubble->texture = effect->ownTexture ? effect->texture : *g_bubbleTexture;
            const ee::Float sizeSpread = effect->size * kBubbleSizeSpread;
            bubble->size = effect->size + sizeSpread * randomUnit(kRandomStep);
            std::memcpy(bubble->colour, effect->colour, 4);
            bubble->lifetime = static_cast<uint32_t>(ee::toInt(effect->lifetime + effect->lifetimeSpread * randomUnit(kRandomStep)));
            bubble->tail.x = x + *g_bubbleScatter * randomUnit(kRandomStep);
            bubble->tail.y = y + *g_bubbleScatter * randomUnit(kRandomStep);
            bubble->tail.z = z + *g_bubbleScatter * randomUnit(kRandomStep);
            bubble->position = bubble->tail;
            bubble->velocity.x = effect->velocity.x + effect->velocitySpread.x * randomUnit(kRandomStep);
            bubble->velocity.y = effect->velocity.y + effect->velocitySpread.y * randomUnit(kRandomStep);
            bubble->velocity.z = effect->velocity.z + effect->velocitySpread.z * randomUnit(kRandomStep);
            bubble->drift = effect->drift;
            zge::pointerArrayAdd(emitter->particles, bubble);
        }
    }
    READABLE_FUNCTION_WAITS(spawnUserBubbles, Verified);

    namespace
    {
        // .sdata: the fireflies' velocity is this plus a random part in
        // [-5, 5) on each axis (one for the spawn, one for the new velocity
        // every quarter second).
        inline ee::Global<ee::Float, sym::g_fireflySpawnBias> g_fireflySpawnBias;
        inline ee::Global<ee::Float, sym::g_fireflyTurnBias> g_fireflyTurnBias;
        inline constexpr ee::Float kFireflySpeedSpread = 5.0f;
        inline constexpr ee::Float kFireflyTurnTime = 0.25f; // seconds between new velocities

        // A firefly's speed on one axis: the bias and a random part.
        ee::Float fireflySpeed(ee::Float bias)
        {
            return randomUnit(kRandomStep) * kFireflySpeedSpread + bias;
        }
    }

    // spawnUserFireflies ("userfirefly" +0x18): `count` fireflies of the placed
    // effect, each in its box (the effect's position with its spread either
    // way) at a random point of it, with a random velocity, the effect's
    // size, colour, lifetime (with its spread) and texture (the firefly's
    // unless the effect has its own).
    void spawnUserFireflies(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<UserEffect> effect)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        const ee::Float bias = *g_fireflySpawnBias;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Firefly> firefly = newParticle<Firefly>(*emitter);
            if (!firefly)
                break;
            firefly->born = static_cast<uint32_t>(clock);
            firefly->flags = kParticleMoves;
            firefly->texture = effect->ownTexture ? effect->texture : *g_fireflyTexture;
            firefly->size = effect->size;
            std::memcpy(firefly->colour, effect->colour, 4);
            firefly->lifetime = static_cast<uint32_t>(ee::toInt(effect->lifetime + effect->lifetimeSpread * randomUnit(kRandomStep)));
            firefly->tail.x = effect->position.x + effect->positionSpread.x * randomUnit(kRandomStep);
            firefly->tail.y = effect->position.y + effect->positionSpread.y * randomUnit(kRandomStep);
            firefly->tail.z = effect->position.z + effect->positionSpread.z * randomUnit(kRandomStep);
            firefly->position = firefly->tail;
            firefly->velocity.x = fireflySpeed(bias);
            firefly->velocity.y = fireflySpeed(bias);
            firefly->velocity.z = fireflySpeed(bias);
            firefly->boxMin = effect->position - effect->positionSpread;
            firefly->boxMax = effect->position + effect->positionSpread;
            firefly->untilNewVelocity = kFireflyTurnTime;
            zge::pointerArrayAdd(emitter->particles, firefly);
        }
    }
    READABLE_FUNCTION_WAITS(spawnUserFireflies, Verified);

    namespace
    {
        inline constexpr ee::Float kPercentStep = 0.01f;   // a random percent's step
        inline constexpr ee::Float kLifetimeStep = 0.0625f; // 1/16: a random nibble's step
        inline constexpr ee::Float kFireflySize = 0.1f;
        inline constexpr ee::Float kFireflySpeedBias = 0.1f;

        // `lifetime` milliseconds with up to half of it either way, in
        // sixteenths, at random.
        uint32_t lifetimeWithSpread(int32_t lifetime)
        {
            const ee::Float whole = ee::toFloat(lifetime);
            const int32_t sixteenths = static_cast<int32_t>(zge::randomNumber() & 0xFu) - 8;
            return static_cast<uint32_t>(ee::toInt(ee::toFloat(sixteenths) * kLifetimeStep * whole + whole));
        }
    }

    // spawnBubbleParticle ("bubble" +0x18): 1 to the parameters' most
    // bubbles at random (the count asked for is not read), each at the
    // position with its spread, sized at random between the smallest and the
    // largest, in the parameters' colour, with their drift and lifetime,
    // moving on the y axis alone at their speed with its spread.
    void spawnBubbleParticle(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t /*count*/, uint32_t,
                             ee::Ptr<BubbleParameters> bubbles)
    {
        const int32_t clock = effectsClock();
        const uint32_t r = zge::randomNumber();
        const uint32_t most = bubbles->mostPerSpawn;
        // 1 .. most at random (a most of 0 takes the random number whole).
        const uint32_t count = ((most ? r % most : r) + 1) & 0xFFu;
        if (!effectClass || !emitter || !count)
            return;
        for (uint32_t made = 0; made < count; ++made)
        {
            const ee::Ptr<Bubble> bubble = newParticle<Bubble>(*emitter);
            if (!bubble)
                break;
            bubble->born = static_cast<uint32_t>(clock);
            bubble->flags = kParticleMoves;
            bubble->texture = *g_bubbleTexture;
            const ee::Float percent = ee::toFloat(static_cast<int32_t>(zge::randomNumber() % 100u));
            bubble->size = bubbles->smallest + percent * kPercentStep * (bubbles->largest - bubbles->smallest);
            std::memcpy(bubble->colour, bubbles->colour, 4);
            bubble->lifetime = lifetimeWithSpread(bubbles->lifetime);
            bubble->tail.x = bubbles->position.x + bubbles->positionSpread.x * randomUnit(kRandomStep);
            bubble->tail.y = bubbles->position.y + bubbles->positionSpread.y * randomUnit(kRandomStep);
            bubble->tail.z = bubbles->position.z + bubbles->positionSpread.z * randomUnit(kRandomStep);
            bubble->position = bubble->tail;
            bubble->velocity.x = 0.0f;
            bubble->velocity.z = 0.0f;
            bubble->velocity.y = bubbles->speedY + bubbles->speedYSpread * randomUnit(kRandomStep);
            bubble->drift = bubbles->drift;
            zge::pointerArrayAdd(emitter->particles, bubble);
        }
    }
    READABLE_FUNCTION_WAITS(spawnBubbleParticle, Verified);

    // spawnFireflyParticle ("firefly" +0x18): `count` fireflies, each at a
    // random point of the box (the position with its spread either way)
    // and kept in it, with a random velocity, in the parameters' colour,
    // with their lifetime; their size is 0.1.
    void spawnFireflyParticle(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t,
                              ee::Ptr<FireflyParameters> fireflies)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Firefly> firefly = newParticle<Firefly>(*emitter);
            if (!firefly)
                break;
            firefly->born = static_cast<uint32_t>(clock);
            firefly->flags = kParticleMoves;
            firefly->texture = *g_fireflyTexture;
            firefly->size = kFireflySize;
            std::memcpy(firefly->colour, fireflies->colour, 4);
            firefly->lifetime = lifetimeWithSpread(fireflies->lifetime);
            firefly->tail.x = fireflies->position.x + fireflies->positionSpread.x * randomUnit(kRandomStep);
            firefly->tail.y = fireflies->position.y + fireflies->positionSpread.y * randomUnit(kRandomStep);
            firefly->tail.z = fireflies->position.z + fireflies->positionSpread.z * randomUnit(kRandomStep);
            firefly->position = firefly->tail;
            firefly->velocity.x = fireflySpeed(kFireflySpeedBias);
            firefly->velocity.y = fireflySpeed(kFireflySpeedBias);
            firefly->velocity.z = fireflySpeed(kFireflySpeedBias);
            firefly->boxMin = fireflies->position - fireflies->positionSpread;
            firefly->boxMax = fireflies->position + fireflies->positionSpread;
            firefly->untilNewVelocity = kFireflyTurnTime;
            zge::pointerArrayAdd(emitter->particles, firefly);
        }
    }
    READABLE_FUNCTION_WAITS(spawnFireflyParticle, Verified);

    // moveFireflies ("firefly" and "userfirefly" +0x1C): once a tick each
    // firefly takes a new random velocity when its quarter second is up,
    // else turns back on each axis it has left its box by; then the move
    // the classes share.
    int32_t moveFireflies(uint32_t effect, ee::Ptr<Emitter> emitter)
    {
        if (emitter && emitter->particles && emitter->lastMoved < static_cast<uint32_t>(effectsClock()))
        {
            uint32_t particle = first(*emitter->particles);
            // The game reads the turn bias only once there is a firefly.
            const ee::Float bias = particle ? *g_fireflyTurnBias : ee::Float(0.0f);
            for (; particle; particle = next(*emitter->particles))
            {
                Firefly &firefly = *ee::Ptr<Firefly>(particle);
                const ee::Float untilNew = firefly.untilNewVelocity;
                if (untilNew <= *g_tickTime)
                {
                    firefly.velocity.x = fireflySpeed(bias);
                    firefly.velocity.y = fireflySpeed(bias);
                    const ee::Float z = fireflySpeed(bias);
                    firefly.untilNewVelocity = kFireflyTurnTime;
                    firefly.velocity.z = z;
                    continue;
                }
                const zge::Vec4 at = firefly.position;
                if (firefly.boxMax.x < at.x)
                    firefly.velocity.x = -firefly.velocity.x;
                if (firefly.boxMax.y < at.y)
                    firefly.velocity.y = -firefly.velocity.y;
                if (firefly.boxMax.z < at.z)
                    firefly.velocity.z = -firefly.velocity.z;
                if (at.x < firefly.boxMin.x)
                    firefly.velocity.x = -firefly.velocity.x;
                if (at.y < firefly.boxMin.y)
                    firefly.velocity.y = -firefly.velocity.y;
                if (at.z < firefly.boxMin.z)
                    firefly.velocity.z = -firefly.velocity.z;
                firefly.untilNewVelocity = untilNew - *g_tickTime;
            }
        }
        return moveParticles(effect, emitter);
    }
    READABLE_FUNCTION_WAITS(moveFireflies, Verified);

    namespace
    {
        // The bubbles' and the fireflies' draw (the same code): each
        // particle a camera-facing square of its size, its texture and
        // colour. Returns the particles drawn.
        int32_t drawSquares(ee::Ptr<Emitter> emitter)
        {
            ee::Local<Billboard> billboard;
            listCorners(billboard->quad);
            int32_t drawn = 0;
            for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles))
            {
                const Particle &p = *ee::Ptr<Particle>(particle);
                std::memcpy(billboard->colour, p.colour, 4);
                billboard->quad.record.texture = p.texture;
                placeBillboard(billboard, currentCameraMatrix(), p.position);
                ++drawn;
                placeCorners(billboard, p.size);
                drawBillboard(billboard);
            }
            return drawn;
        }
    }

    // The classes' draws (+0x20). drawBubbles: "bubble".
    int32_t drawBubbles(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        return drawSquares(emitter);
    }
    READABLE_FUNCTION_WAITS(drawBubbles, Verified);

    // drawFireflies: "firefly".
    int32_t drawFireflies(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        return drawSquares(emitter);
    }
    READABLE_FUNCTION_WAITS(drawFireflies, Verified);

    // drawUserBubbles: "userbubble": the bubbles' draw unless hidden. (Its
    // third argument, the draw's mode, is not read.) The draw is called by
    // address: drawBubbles and drawFireflies have the same code, which a
    // linker may fold into one.
    int32_t drawUserBubbles(uint32_t effectClass, ee::Ptr<Emitter> emitter, uint32_t /*mode*/)
    {
        return drawUnlessHidden(effectClass, emitter, EmitterDraw(sym::drawBubbles));
    }
    READABLE_FUNCTION_WAITS(drawUserBubbles, Verified);

    // drawUserFireflies: "userfirefly": the fireflies' draw unless hidden.
    int32_t drawUserFireflies(uint32_t effectClass, ee::Ptr<Emitter> emitter, uint32_t /*mode*/)
    {
        return drawUnlessHidden(effectClass, emitter, EmitterDraw(sym::drawFireflies));
    }
    READABLE_FUNCTION_WAITS(drawUserFireflies, Verified);

    namespace
    {
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_bubbleClass> g_bubbleClassWord; // g_bubbleClassWord (g_bubbleClassWord)
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<ee::Ptr<zge::EffectClass>, sym::g_fireflyClass> g_fireflyClassWord; // g_fireflyClassWord (g_fireflyClassWord)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(randomint32_t, ee::game<int32_t>, kRandom);
        }
    }

    uint32_t startBubbles(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_bubbleClassWord, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startBubbles, Verified);

    uint32_t startFireflies(ee::Ptr<zge::SceneObject> object, uint32_t parameters)
    {
        return game::tail(zge::startEffect, object, *g_fireflyClassWord, 1, 0, parameters);
    }
    READABLE_FUNCTION_WAITS(startFireflies, Verified);

}
