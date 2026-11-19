#pragma once
// BMX XXX's posters (posters.cpp): the "poster" effect class, a textured
// quad on four corners. Aggressive Inline's records
// (game/aggressive/fx/posters.cpp), at the offsets BMX XXX's code reads.

#include "particles.h"

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What a poster starts from (the named effect's parameters; with none,
    // the game's defaults).
    struct PosterParameters
    {
        zge::Vec4 corners[4]; // +0x00
        uint32_t lifetime;    // +0x40 milliseconds
        uint8_t colour[4];    // +0x44 RGBA
        uint32_t texture;     // +0x48
    };
    static_assert(offsetof(PosterParameters, lifetime) == 0x40 && offsetof(PosterParameters, texture) == 0x48);

    // A poster: a particle that stays where it is, with its four corners.
    struct Poster
    {
        uint32_t flags;       // +0x00
        uint8_t _04[0x0C];
        zge::Vec4 position;   // +0x10 its first corner
        uint8_t _20[0x10];
        zge::Vec4 velocity;   // +0x30 none
        uint8_t _40[0x16];
        uint8_t colour[4];    // +0x56 RGBA (unaligned)
        uint8_t _5a[2];
        ee::Float size;       // +0x5C
        uint32_t texture;     // +0x60
        uint32_t lifetime;    // +0x64 milliseconds
        uint32_t born;        // +0x68 the effects' clock
        uint32_t _6c;
        zge::Vec4 corners[4]; // +0x70
    };
    static_assert(offsetof(Poster, colour) == 0x56 && offsetof(Poster, size) == 0x5C && offsetof(Poster, born) == 0x68);
    static_assert(offsetof(Poster, corners) == 0x70 && sizeof(Poster) == 0xB0);

    // The poster class's spawn: one poster on the parameters' corners.
    void spawnPoster(uint32_t effectClass, uint32_t object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t unused,
                     ee::Ptr<PosterParameters> given);
    // The poster class's draw; returns how many it drew.
    int32_t drawPosters(uint32_t effectClass, ee::Ptr<Emitter> emitter);
}
