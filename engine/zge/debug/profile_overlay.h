#pragma once

// The profiler overlay's primitives and the number steps its drawing shares
// (zge/debug/profiler.cpp, and BMX XXX's own drawProfileTime and
// drawProfileTicks in games/bmx/debug/profiler.cpp).

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zge::profile_overlay
{
    // The overlay's primitives: the 2D packet, the default texture's
    // second slot (render/ps2/primitive_packets.cpp's flags).
    constexpr uint32_t kOverlayPrimitives = 0x02000002u;

    // A vertex of the overlay's primitives (render/ps2/primitive_packets.cpp's
    // PrimitiveVertex: x, y, then the colour at +0x18).
    struct OverlayVertex
    {
        ee::Float x, y;
        uint8_t _08[0x10];
        uint8_t colour[4];
        uint8_t _1c[4];
    };
    static_assert(offsetof(OverlayVertex, colour) == 0x18 && sizeof(OverlayVertex) == 0x20);

    // A primitive list's entry: a texture (0: the default) and its
    // vertices' addresses (three or four).
    struct alignas(16) OverlayQuadList
    {
        uint32_t texture;
        ee::Ptr<OverlayVertex> vertices[4];
    };
    struct alignas(16) OverlayTriangleList
    {
        uint32_t texture;
        ee::Ptr<OverlayVertex> vertices[3];
    };
    inline OverlayVertex &vertexOf(uint32_t list, int index) { return *ee::Ptr<OverlayQuadList>(list)->vertices[index]; }
    inline void setColour(OverlayVertex &vertex, ee::Ptr<uint8_t> colour) { std::memcpy(vertex.colour, colour.get(), 4); }

    // An unsigned value as a float: with the top bit set, half of it (the
    // low bit kept) doubled.
    inline ee::Float unsignedToFloat(uint32_t value)
    {
        if (static_cast<int32_t>(value) >= 0)
            return ee::toFloat(static_cast<int32_t>(value));
        const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1u) | (value >> 1)));
        return half + half;
    }

    // Whether a value is a whole number (to the tolerance).
    inline bool isWhole(ee::Float value, ee::Float tolerance)
    {
        return ee::abs(value - ee::toFloat(ee::toInt(value + tolerance))) < tolerance;
    }
}
