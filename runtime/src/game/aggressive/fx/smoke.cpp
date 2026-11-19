// The smoke's starts and spawns ("smoke" and "usersmoke"; inlineFx.c, were
// the sweep's drafts of startUserSmoke .. spawnSmoke): puffs at a point with a
// spread, with a velocity and its spread, drifting, their colour going
// through four keys (fx/particles.h's Puff). Their move is particles.cpp's
// moveSmoke, their draw particle_draws.cpp's drawSmoke.
//

#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/text/cstring.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"

#include <cstring>
#include "sym/aggressive/fx/smoke.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        inline constexpr ee::Float kRandomStep = 0.0078125f;    // 1/128: a random byte's step in [-1, 1)
        inline constexpr ee::Float kLifetimeStep = 0.0625f;     // 1/16: a random nibble's step in [-0.5, 0.5)

        // A random number's low byte as a step in [-1, 1), times `step`
        // (the original's register).
        ee::Float randomUnit(ee::Float step)
        {
            const uint32_t r = zge::randomNumber();
            return ee::toFloat(static_cast<int32_t>((r & 0xFFu) - 0x80)) * step;
        }
    }

    // startUserSmoke: a placed effect's smoke, as many puffs as it has due.
    uint32_t startUserSmoke(ee::Ptr<zge::SceneObject> object, ee::Ptr<UserEffect> effect)
    {
        return game::tail(zge::startEffect, object, *g_userSmokeClass, effect->due, 0, effect);
    }
    READABLE_FUNCTION_WAITS(startUserSmoke, Verified);

    // spawnUserSmoke ("usersmoke" +0x18): `count` puffs of the placed effect:
    // at its position with its spread, with its velocity and spread, its
    // drift, size, texture, colour (all four keys) and lifetime (with its
    // spread), its puffs' growth and fade.
    void spawnUserSmoke(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<UserEffect> effect)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Puff> puff = newParticle<Puff>(*emitter);
            if (!puff)
                return;
            puff->born = static_cast<uint32_t>(clock);
            puff->flags = kParticleMoves;
            puff->texture = effect->texture;
            puff->size = effect->size;
            std::memcpy(puff->colour, effect->colour, 4);
            puff->lifetime = static_cast<uint32_t>(ee::toInt(effect->lifetime + effect->lifetimeSpread * randomUnit(kRandomStep)));
            puff->tail.x = effect->position.x + effect->positionSpread.x * randomUnit(kRandomStep);
            puff->tail.y = effect->position.y + effect->positionSpread.y * randomUnit(kRandomStep);
            puff->tail.z = effect->position.z + effect->positionSpread.z * randomUnit(kRandomStep);
            puff->position = puff->tail;
            puff->velocity.x = effect->velocity.x + effect->velocitySpread.x * randomUnit(kRandomStep);
            puff->velocity.y = effect->velocity.y + effect->velocitySpread.y * randomUnit(kRandomStep);
            puff->velocity.z = effect->velocity.z + effect->velocitySpread.z * randomUnit(kRandomStep);
            puff->drift = effect->drift;
            zge::memoryFill(puff->colourRate, 0, sizeof(puff->colourRate));
            // Every key the effect's colour, copied down from the last.
            std::memcpy(puff->keyColour[3], effect->colour, 4);
            std::memcpy(puff->keyColour[2], puff->keyColour[3], 4);
            std::memcpy(puff->keyColour[1], puff->keyColour[2], 4);
            std::memcpy(puff->keyColour[0], puff->keyColour[1], 4);
            puff->keyTime[0] = 1;
            puff->key = 0;
            puff->keyTime[1] = puff->lifetime;
            puff->keyTime[3] = puff->lifetime;
            puff->keyTime[2] = puff->lifetime;
            puff->growth = effect->growth;
            puff->fade = effect->fade;
            zge::pointerArrayAdd(emitter->particles, puff);
        }
    }
    READABLE_FUNCTION_WAITS(spawnUserSmoke, Verified);

    // startSmoke: a smoke effect, its parameters' count of puffs.
    uint32_t startSmoke(ee::Ptr<zge::SceneObject> object, ee::Ptr<SmokeParameters> smoke)
    {
        return game::tail(zge::startEffect, object, *g_smokeClass, smoke->count, 0, smoke);
    }
    READABLE_FUNCTION_WAITS(startSmoke, Verified);

    // spawnSmoke ("smoke" +0x18): `count` puffs from the parameters: at
    // the position with its spread, with the velocity and its spread, the
    // drift, size, texture, the colour keys and their rates and times, the
    // growth and fade; each living the lifetime give or take a half (in
    // sixteenths).
    void spawnSmoke(uint32_t effectClass, uint32_t /*object*/, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<SmokeParameters> smoke)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        if (!effectClass || !emitter || !wanted)
            return;
        for (uint32_t made = 0; made < wanted; ++made)
        {
            const ee::Ptr<Puff> puff = newParticle<Puff>(*emitter);
            if (!puff)
                break;
            puff->born = static_cast<uint32_t>(clock);
            puff->flags = kParticleMoves;
            puff->texture = smoke->texture;
            puff->size = smoke->size;
            std::memcpy(puff->colour, smoke->keyColour[0], 4);
            const ee::Float lifetime = ee::toFloat(smoke->lifetime);
            const int32_t sixteenths = static_cast<int32_t>((zge::randomNumber() & 0xFu) - 8);
            puff->lifetime = static_cast<uint32_t>(ee::toInt(ee::toFloat(sixteenths) * kLifetimeStep * lifetime + lifetime));
            puff->tail.x = smoke->position.x + smoke->positionSpread.x * randomUnit(kRandomStep);
            puff->tail.y = smoke->position.y + smoke->positionSpread.y * randomUnit(kRandomStep);
            puff->tail.z = smoke->position.z + smoke->positionSpread.z * randomUnit(kRandomStep);
            puff->position = puff->tail;
            puff->velocity.x = smoke->velocity.x + smoke->velocitySpread.x * randomUnit(kRandomStep);
            puff->velocity.y = smoke->velocity.y + smoke->velocitySpread.y * randomUnit(kRandomStep);
            puff->velocity.z = smoke->velocity.z + smoke->velocitySpread.z * randomUnit(kRandomStep);
            puff->drift = smoke->drift;
            zge::memoryCopy(puff->colourRate, smoke->colourRate, sizeof(puff->colourRate));
            zge::memoryCopy(puff->keyColour, smoke->keyColour, sizeof(puff->keyColour));
            zge::memoryCopy(puff->keyTime, smoke->keyTime, sizeof(puff->keyTime));
            puff->key = 0;
            puff->growth = smoke->growth;
            puff->fade = smoke->fade;
            zge::pointerArrayAdd(emitter->particles, puff);
        }
    }
    READABLE_FUNCTION_WAITS(spawnSmoke, Verified);
}
