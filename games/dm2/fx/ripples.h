#pragma once

// DM2's "ripple" effect class (fx.c; ripples.cpp): flat rings on the water
// that grow and fade.

#include "particles.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // The spawn parameters of "ripple".
    struct RippleParams
    {
        zge::Vec4 position; // +0x00 (x, y and z are used)
        int32_t lifetime;   // +0x10 milliseconds
        ee::Float size;     // +0x14 half its width at the start
        ee::Float growth;   // +0x18 size added a second
        uint8_t colour[4];  // +0x1C (the draw uses its own grey)
        uint32_t material;  // +0x20
        uint8_t _24[0x0C];
    };
    static_assert(offsetof(RippleParams, lifetime) == 0x10 && offsetof(RippleParams, colour) == 0x1C && sizeof(RippleParams) == 0x30);

    // "ripple"'s starter: with no parameters one ripple of the effect's
    // defaults; with parameters three rings at their position, untextured,
    // each slower, longer lived and a little higher than the one before
    // (the parameters' lifetime, size, growth, material and height are
    // overwritten). Returns the last start's result.
    uint32_t startRippleEffect(ee::Ptr<zge::SceneObject> object, ee::Ptr<RippleParams> params);
    // "ripple"'s spawn: one ripple from the parameters (the effect's
    // defaults when none). A ripple is a plain particle: its growth is kept
    // where a particle has its velocity's x.
    void spawnRipple(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t start, uint32_t unused,
                     ee::Ptr<RippleParams> params);
    // "ripple"'s draw: each ripple a flat square in the world, its size
    // each way from its position, grey and as bright as it is solid,
    // fading over its last 0x800 ms. Returns how many it drew.
    int32_t drawRipples(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
    // "ripple"'s move: by the milliseconds since the emitter's last move,
    // every ripple grown, the moving ones moved, the spinning ones turned,
    // the ones past their lifetime freed; then the class's affectors run.
    // Returns how many ripples it went through.
    int32_t moveRipples(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
