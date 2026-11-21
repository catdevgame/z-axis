#pragma once

// DM2's "lightning" effect class (fx.c; lightning.cpp): jagged bolts drawn
// as a chain of short-lived segments.

#include "particles.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // The spawn parameters of "lightning".
    struct LightningParams
    {
        zge::Vec4 start;    // +0x00 from the emitter object's place (x, y and z are used)
        zge::Vec4 end;      // +0x10 the same
        int32_t segments;   // +0x20 how many pieces a bolt is made of
        ee::Float width;    // +0x24 half a segment's width
        ee::Float jitter;   // +0x28 how far a joint strays, each axis, either way
        uint32_t material;  // +0x2C 0: drawn as lines
    };
    static_assert(offsetof(LightningParams, segments) == 0x20 && offsetof(LightningParams, material) == 0x2C && sizeof(LightningParams) == 0x30);

    // "lightning"'s starter: one bolt of eight untextured segments from the
    // object to a random point within 128 of it on each axis. The
    // parameters given are not looked at. Returns startNamedEffect's result.
    uint32_t startLightningEffect(ee::Ptr<zge::SceneObject> object);
    // "lightning"'s spawn: `count` (its low byte) bolts from the
    // parameters' start to their end (the effect's defaults when none),
    // each a chain of segments whose joints stray at random; a segment is a
    // plain particle from its tail to its position, the later ones living
    // a little longer.
    void spawnLightning(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<zge::SceneObject> object, ee::Ptr<Emitter> emitter, uint32_t count,
                        uint32_t unused, ee::Ptr<LightningParams> params);
    // "lightning"'s draw: each segment a line, or with a material a strip
    // its width either side facing the camera, fading over its last 0x100
    // ms. Returns how many it drew.
    int32_t drawLightning(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
