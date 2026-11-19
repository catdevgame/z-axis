#pragma once
// BMX XXX's tracers (fx.c; tracers.cpp): the "tracer" effect class, a
// weapon's round drawn as a streak with a trail of where it has been.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a tracer starts from (the named effect's parameters; with none,
    // the game's defaults: size 0.1, 450 milliseconds, yellow).
    struct TracerEffectParameters
    {
        zge::Vec4 position; // +0x00
        zge::Vec4 velocity; // +0x10 units a tick
        ee::Float size;     // +0x20
        int32_t lifetime;   // +0x24 milliseconds (it lives 360 more: its trail's)
        uint8_t colour[4];  // +0x28 RGBA
    };
    static_assert(offsetof(TracerEffectParameters, size) == 0x20 && offsetof(TracerEffectParameters, colour) == 0x28);

    // A tracer: a particle and the last six places its tail has been drawn at.
    constexpr uint32_t kTracerTrail = 6;
    struct Tracer : Particle
    {
        zge::Vec4 trail[kTracerTrail]; // +0x70 the newest first
    };
    static_assert(offsetof(Tracer, trail) == 0x70 && sizeof(Tracer) == 0xD0);

    // The tracer class's spawn: one tracer from the parameters.
    void spawnTracers(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                      ee::Ptr<TracerEffectParameters> given);
    // The tracer class's draw; returns how many tracers it looked at.
    int32_t drawTracers(uint32_t effect, ee::Ptr<Emitter> emitter);
}
