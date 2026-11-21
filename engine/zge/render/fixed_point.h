#pragma once

// The fixed-point units the renderer's packets carry, and the engine's
// conversions to them:
//   - screen positions in sixteenths of a pixel (the GS's 12.4 fixed point:
//     pixel column x is x * 16): DisplaySetup's offsetX and offsetY, a
//     sprite's corners, a vertex's X and Y;
//   - texture coordinates in 4096ths (4.12 fixed point: 1.0 is 4096): the
//     class packets' and the geometry files' UVs.
// Each is the game's own operation (a shift; the multiply and CVT.W.S, in
// that order), so a value is the same as the game's for every input.

#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    // A pixel is 16 subpixels.
    constexpr uint32_t kSubpixelBits = 4;
    // The drawing space is 4096 pixels each way (12.4's whole part); the
    // screen sits in its middle (DisplaySetup's offsetX and offsetY).
    constexpr int32_t kScreenSpacePixels = 4096;

    // A screen position or size in pixels, in subpixels (the type the
    // shift gives: an int for a small integer type, as the game's C).
    template <typename T>
    constexpr auto subpixels(T pixels)
    {
        return pixels << kSubpixelBits;
    }

    // A subpixel in pixels.
    constexpr float kPixelsPerSubpixel = 1.0f / 16;

    // A count of pixel rows in subpixels in field rendering, where a pixel
    // row is every other raster row: half of subpixels().
    template <typename T>
    constexpr auto fieldRowSubpixels(T rows)
    {
        return rows << (kSubpixelBits - 1);
    }

    // A texture coordinate (1.0 across the texture) in 4.12 fixed point,
    // converted towards zero; and back.
    inline int32_t fixedUv(ee::Float uv) { return ee::toInt(uv * ee::Float(4096.0f)); }
    inline ee::Float uvFromFixed(int32_t fixed) { return ee::toFloat(fixed) * ee::Float(1.0f / 4096.0f); }
}
