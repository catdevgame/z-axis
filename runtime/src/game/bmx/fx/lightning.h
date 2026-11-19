#pragma once
// BMX XXX's lightning (fx.c; lightning.cpp): the "lightning" effect class,
// a jagged bolt of segments from one point to another. Aggressive Inline's
// (game/aggressive/fx/lightning.cpp), started between two points instead
// of from the sky.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // A bolt (the named effect's parameters; with none, the game's defaults).
    struct LightningParameters
    {
        ee::Float fromX, fromY, fromZ; // +0x00 from the effect's position
        uint32_t _0c;
        ee::Float toX, toY, toZ;       // +0x10
        uint32_t _1c;
        int32_t segments;              // +0x20
        ee::Float width;               // +0x24 half a segment's width
        ee::Float jitter;              // +0x28 a point is pushed aside up to this either way
        uint32_t texture;              // +0x2C 0: drawn as lines
    };
    static_assert(offsetof(LightningParameters, toX) == 0x10 && offsetof(LightningParameters, segments) == 0x20);
    static_assert(offsetof(LightningParameters, texture) == 0x2C && sizeof(LightningParameters) == 0x30);

    // The named effect's start: one untextured bolt of 8 segments from the
    // object to a random point up to 128 away on each axis.
    uint32_t startLightningEffect(ee::Ptr<zge::SceneObject> object);
    // The lightning class's spawn: `count` bolts from the parameters.
    void spawnLightning(uint32_t effectClass, ee::Ptr<zge::SceneObject> object, ee::Ptr<Emitter> emitter, uint32_t count,
                        uint32_t unused, ee::Ptr<LightningParameters> given);
    // The lightning class's draw; returns how many segments it drew.
    int32_t drawLightning(uint32_t effect, ee::Ptr<Emitter> emitter);
}
