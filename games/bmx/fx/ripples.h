#pragma once
// BMX XXX's ripples (fx.c; ripples.cpp): the "ripple" effect class, flat
// squares on the water that grow and fade. Aggressive Inline's
// (games/aggressive/fx/ripples.cpp) without a rider to follow.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a ripple starts from (the named effect's parameters; with none,
    // the game's defaults: 3 seconds, size 0.1, growth 0.05, white at alpha 0x70).
    struct RippleParameters
    {
        ee::Float x, y, z;  // +0x00
        uint32_t _0c;
        uint32_t lifetime;  // +0x10 milliseconds
        ee::Float size;     // +0x14 half the square's side
        ee::Float growth;   // +0x18 size added a second
        uint8_t colour[4];  // +0x1C RGBA
        uint32_t texture;   // +0x20
    };
    static_assert(offsetof(RippleParameters, lifetime) == 0x10 && offsetof(RippleParameters, colour) == 0x1C);
    static_assert(offsetof(RippleParameters, texture) == 0x20);

    // The named effect's start: with no parameters, one ring of the
    // defaults; else three rings of the given ones, untextured, each later
    // one longer lived, smaller, slower to grow and a little higher.
    uint32_t startRippleEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<RippleParameters> ripple);
    // The ripple class's spawn: one ripple from the parameters.
    void spawnRipple(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<RippleParameters> given);
    // The ripple class's move: each ripple grows by the time since the last
    // move; returns how many it looked at.
    int32_t moveRipples(uint32_t effect, ee::Ptr<Emitter> emitter);
    // The ripple class's draw; returns how many it drew.
    int32_t drawRipples(uint32_t effect, ee::Ptr<Emitter> emitter);
}
