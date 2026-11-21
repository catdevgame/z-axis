#pragma once
// BMX XXX's dust (fx.c; dust.cpp): the "dust" effect class's puffs (DM2's
// fx.c "dust", games/dm2/fx/dust.cpp), and the camera-facing square they are
// drawn as. The class's new emitter and move are the sparks' (particles.cpp);
// its start is startDustEffect (spawns.cpp). BMX XXX's other "dust",
// spawnDust (emitter_objects.cpp), is the ZPE_SFX_dust emitter object.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a puff starts from (the named effect's parameters; with none,
    // the game's defaults: rising at 0.1 a tick, size 0.3, one second).
    struct DustParams
    {
        zge::Vec4 position; // +0x00
        zge::Vec4 velocity; // +0x10 units a tick
        ee::Float size;     // +0x20
        int32_t lifetime;   // +0x24 milliseconds
    };
    static_assert(offsetof(DustParams, velocity) == 0x10 && offsetof(DustParams, size) == 0x20);
    static_assert(offsetof(DustParams, lifetime) == 0x24);

    // The dust class's spawn (DM2's spawnDust): one half see-through grey
    // puff at the parameters' position with their velocity, size and life.
    void spawnDustPuff(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                       ee::Ptr<DustParams> given);
    // The dust class's draw: each puff a square facing the camera, through
    // the sixteen dust materials over its last second, fading over its last
    // quarter second. Returns how many it drew.
    int32_t drawDust(uint32_t effect, ee::Ptr<Emitter> emitter);
    // A particle as a square of its size facing the camera, with the
    // material and the colour.
    void drawParticleBillboard(ee::Ptr<Particle> particle, uint32_t material, ee::Ptr<uint8_t> colour);
}
