// DM2's "dust" effect class (dust.h). DM2's own code (fx.c).

#include "dust.h"

#include "effects_clock.h"
#include "spawns.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/memory/pool.h"

#include <cstdint>
#include "sym/dm2/fx/dust.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The dust's materials (MatFxDust00 on): a puff starts with the first
        // and is drawn with the one its age picks.
        inline ee::Global<uint32_t[16], sym::g_dustMaterials> g_dustMaterials;
        constexpr uint32_t kDustEffectDefaults = sym::dustEffectDefaults;

        // A puff's colour: mid grey, half see-through.
        constexpr uint8_t kPuffGrey = 0x80, kPuffAlpha = 0x40;

        // A puff's look goes by the milliseconds it has left: the materials
        // run over the last second, the fade over the last quarter.
        constexpr int32_t kLookTime = 1000, kFadeTime = 250;
        constexpr int32_t kLastMaterial = 15;
    }

    void spawnDust(ee::Ptr<zge::EffectClass> effectClass, uint32_t, ee::Ptr<Emitter> emitter, uint32_t, uint32_t, ee::Ptr<DustParams> params)
    {
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        if (!params)
            params = ee::Ptr<DustParams>(kDustEffectDefaults);
        if (!effectClass || !emitter)
            return;
        const ee::Ptr<Particle> puff(zge::poolAlloc(emitter->pool));
        puff->born = now;
        puff->flags = zge::kParticleMoves;
        puff->lifetime = static_cast<uint32_t>(params->lifetime);
        puff->material = (*g_dustMaterials)[0];
        puff->size = params->size;
        puff->colour[0] = kPuffGrey;
        puff->colour[1] = kPuffGrey;
        puff->colour[2] = kPuffGrey;
        puff->colour[3] = kPuffAlpha;
        puff->position.x = puff->tail.x = params->position.x;
        puff->position.y = puff->tail.y = params->position.y;
        puff->position.z = puff->tail.z = params->position.z;
        puff->velocity = params->velocity;
        game::tail(zge::pointerArrayAdd, emitter->particles, puff);
    }
    READABLE_FUNCTION_WAITS(spawnDust, Verified);

    int32_t drawDust(ee::Ptr<zge::EffectClass>, ee::Ptr<Emitter> emitter)
    {
        ee::Local<uint8_t[4]> colour;
        int32_t drawn = 0;
        const int32_t now = effectsClock();
        for (ee::Ptr<Particle> puff(zge::arrayFirst(emitter->particles)); puff; puff = ee::Ptr<Particle>(zge::arrayNext(emitter->particles)))
        {
            for (int i = 0; i < 4; ++i)
                colour[i] = puff->colour[i];
            int32_t left = static_cast<int32_t>(puff->lifetime) - (now - static_cast<int32_t>(puff->born));
            if (left > kLookTime)
                left = kLookTime;
            if (left < 0)
                left = 0;
            if (left < kFadeTime)
                colour[3] = static_cast<uint8_t>(static_cast<uint32_t>(puff->colour[3] * left) / static_cast<uint32_t>(kFadeTime));
            ++drawn;
            const uint32_t material = (*g_dustMaterials)[((kLookTime - left) * kLastMaterial) / kLookTime];
            drawParticleBillboard(puff, material, ee::Ptr<uint8_t>(colour));
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawDust, Verified);
}
