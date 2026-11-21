#pragma once

// DM2's "sleep" effect class (fx.c; sleep.cpp): the marks that rise,
// swaying, from a rider left alone.

#include "particles.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // The spawn parameters of "sleep".
    struct SleepParams
    {
        zge::Vec4 position; // +0x00
        int32_t lifetime;   // +0x10 milliseconds
        uint8_t _14[0x0C];
    };
    static_assert(offsetof(SleepParams, lifetime) == 0x10 && sizeof(SleepParams) == 0x20);

    // A sleep mark: a particle with the part of its texture it shows. Its
    // size field is not a size: it is the direction the mark sways in, a
    // part of a turn about the upright.
    struct SleepMark : Particle
    {
        ee::Float uLow, uHigh; // +0x70
        ee::Float vLow, vHigh; // +0x78
    };
    static_assert(offsetof(SleepMark, uLow) == 0x70 && offsetof(SleepMark, vLow) == 0x78 && sizeof(SleepMark) == 0x80);

    // One "sleep" mark at a point, for `lifetime` milliseconds; nothing
    // before the effects are set up.
    void sleep(ee::Ptr<zge::Vec4> position, int32_t lifetime);
    // "sleep"'s spawn: one mark at the parameters' position (the effect's
    // defaults when none), rising slowly, showing its whole texture, turned
    // a seventh of a turn on from the mark before.
    void spawnSleep(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t start, uint32_t unused,
                    ee::Ptr<SleepParams> params);
    // "sleep"'s draw: each mark two squares back to back, upright, growing
    // and turning about the upright as it ages, swaying four times in its
    // life further and further out along its own direction, and darkening
    // to black.
    // Returns how many it drew.
    int32_t drawSleep(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
