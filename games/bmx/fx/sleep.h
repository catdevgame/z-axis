#pragma once
// BMX XXX's sleep marks (fx.c; sleep.cpp): the "sleep" effect class, the
// "z"s that rise from a sleeper (DM2's fx.c "sleep", games/dm2/fx/sleep.cpp).
// The class's new emitter and move are particles.cpp's (newSleepEmitter,
// moveSleep); its start is startSleepEffect (spawns.cpp), and sleep
// (spawn_effects.cpp) spawns one by name.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a mark starts from (the named effect's parameters; with none, the
    // game's defaults: 5 seconds).
    struct SleepParameters
    {
        zge::Vec4 position; // +0x00
        uint32_t lifetime;  // +0x10 milliseconds
        uint8_t _14[0x0C];
    };
    static_assert(offsetof(SleepParameters, lifetime) == 0x10 && sizeof(SleepParameters) == 0x20);

    // A sleep mark: a particle with the part of its texture it shows. Its
    // size field is not a size: it is the direction the mark sways in, a
    // part of a turn about the upright.
    struct SleepMark : Particle
    {
        // (+0x70: past the particle's padding, which a derived record would
        // otherwise reuse.)
        alignas(16) ee::Float uLow;
        ee::Float uHigh;       // +0x74
        ee::Float vLow, vHigh; // +0x78
    };
    static_assert(offsetof(SleepMark, uLow) == 0x70 && offsetof(SleepMark, vLow) == 0x78 && sizeof(SleepMark) == 0x80);

    // The sleep class's spawn: one mark at the parameters' position, rising
    // slowly, showing its whole texture, turned a seventh of a turn on from
    // the mark before.
    void spawnSleep(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                    ee::Ptr<SleepParameters> given);
    // The sleep class's draw: each mark greying out as it ages, swaying, as
    // two squares back to back turning about the upright. Returns how many
    // it drew.
    int32_t drawSleep(uint32_t effect, ee::Ptr<Emitter> emitter);
}
