#pragma once

// A level's light objects, read from its chunk file (readLitoChunk in
// zge/assets/chunk_reader.cpp). AI's own levels have none; DM2's do.
//
// The lights are kept on one list (g_litoRecords), the directional ones at
// its front: they light every object of their groups, the others only the
// objects within their reach (litoLightsForNode).

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // A light object's type. drawLitoRecords draws a directional light as a
    // line along its direction, type 2 as a point and type 3 as a line along
    // its direction as long as its reach; types 1 and 4 it does not draw.
    constexpr uint32_t kDirectionalLight = 0;

    struct LitoRecord
    {
        ListNode link;     // +0x00 on g_litoRecords
        uint32_t hash;     // +0x08 its hash, as findLitoRecord looks lights up
        // +0x0C the object groups it lights: litoLightsForNode takes it for
        // an object whose group bits share one with these.
        uint16_t groups;
        // +0x0E bit 0: litoLightsForNode passes over a light without it.
        // readLitoChunk does not write it.
        uint16_t flags;
        uint32_t type;     // +0x10 kDirectionalLight or another
        Vec4 position;     // +0x20 its w not written
        Vec4 direction;    // +0x30 its w not written
        // +0x40 how far it reaches: litoLightsForNode takes it for an object
        // whose centre is within twice this plus the object's radius.
        ee::Float reach;
        // +0x44 the chunk's three floats after the reach, copied; nothing in
        // AI's PS2 build reads them.
        ee::Float unreadFloats[3];
        // +0x50 its colour, four bytes, the fourth the alpha (drawLitoRecords
        // draws with it at 255).
        uint8_t colour[4];
    };
    static_assert(offsetof(LitoRecord, hash) == 0x08 && offsetof(LitoRecord, groups) == 0x0C);
    static_assert(offsetof(LitoRecord, type) == 0x10 && offsetof(LitoRecord, position) == 0x20);
    static_assert(offsetof(LitoRecord, reach) == 0x40 && offsetof(LitoRecord, colour) == 0x50);
    static_assert(sizeof(LitoRecord) == 0x60); // the pool's record size
}
