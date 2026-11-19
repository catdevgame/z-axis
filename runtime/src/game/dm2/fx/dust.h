#pragma once

// DM2's "dust" effect class (fx.c; dust.cpp): a grey puff a spawn.

#include "particles.h"

#include <cstdint>

namespace game::dm2
{
    struct DustParams; // spawns.h

    // "dust"'s spawn: one half see-through grey puff at the parameters'
    // position (the effect's defaults when none) with their velocity, size
    // and lifetime.
    void spawnDust(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t start, uint32_t unused,
                   ee::Ptr<DustParams> params);
    // "dust"'s draw: each puff as a square facing the camera, through the
    // sixteen dust materials over its last second, fading out over its last
    // quarter second. Returns how many it drew.
    int32_t drawDust(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
