#pragma once

// DM2's "poster" effect class (fx.c; posters.cpp): a flat four-cornered
// mark left where it is spawned. "skid" and "pockmark" are the same class
// under other names.

#include "particles.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // The spawn parameters of "poster".
    struct PosterParams
    {
        zge::Vec4 corners[4]; // +0x00
        int32_t lifetime;     // +0x40 milliseconds
        uint8_t colour[4];    // +0x44
        uint32_t material;    // +0x48
        uint32_t _4c;
    };
    static_assert(offsetof(PosterParams, lifetime) == 0x40 && offsetof(PosterParams, material) == 0x48 && sizeof(PosterParams) == 0x50);

    // A poster: a particle that stays, with its four corners.
    struct Poster : Particle
    {
        zge::Vec4 corners[4]; // +0x70
    };
    static_assert(offsetof(Poster, corners) == 0x70 && sizeof(Poster) == 0xB0);

    // "poster"'s spawn: one poster with the parameters' corners, lifetime,
    // colour and material (the effect's defaults when none), at its first corner.
    void spawnPoster(ee::Ptr<zge::EffectClass> effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t start, uint32_t unused,
                     ee::Ptr<PosterParams> params);
    // "poster"'s draw: each poster as a quad of its four corners in the
    // world, fading out over its last second. Returns how many it drew.
    int32_t drawPosters(ee::Ptr<zge::EffectClass> effectClass, ee::Ptr<Emitter> emitter);
}
