#pragma once

// DM2's lens flare (SpecialFX/zfxMgr.c; lens_flare.cpp): the sun's glow, star
// and ray where the sun is on screen, and rings and glows along the line from
// the sun through the middle of the screen. Aggressive Inline has the same
// code (game/aggressive/fx/lens_flare.cpp).

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // One of the flare's rings and glows, on the line from the sun through
    // the middle of the screen.
    struct LensFlareElement
    {
        ee::Float distance; // +0x00 where on the line: 1 at the sun, 0 the middle, -1 as far the other side
        ee::Float scale;    // +0x04 its size, times the flare's elementScaleX / Y
        uint8_t colour[4];  // +0x08 RGBA; the alpha fades as the camera turns from the sun
        uint32_t texture;   // +0x0C the flare's glow or second ring
    };
    static_assert(sizeof(LensFlareElement) == 0x10);

    constexpr int kMaxLensFlareElements = 16;

    // A lens flare (newLensFlare): its five textures, where the sun is, its
    // colour and its elements.
    struct LensFlare
    {
        uint32_t glow;       // +0x00
        uint32_t ring1;      // +0x04
        uint32_t ray;        // +0x08
        uint32_t unusedSlot; // +0x0C made 0 with the textures; read nowhere
        uint32_t _10;
        uint32_t star;       // +0x14
        uint32_t ring2;      // +0x18
        uint32_t _1c;
        zge::Vec4 position;  // +0x20 the sun
        uint8_t colour[4];   // +0x30 RGBA
        uint32_t elementCount;                            // +0x34 9
        LensFlareElement elements[kMaxLensFlareElements]; // +0x38
        uint32_t elementScaleX; // +0x138 an element's half width is its scale times half this (256)
        uint32_t elementScaleY; // +0x13C and its half height
        uint8_t _140[0x10];
    };
    static_assert(sizeof(LensFlare) == 0x150);
    static_assert(offsetof(LensFlare, star) == 0x14 && offsetof(LensFlare, position) == 0x20 && offsetof(LensFlare, colour) == 0x30);
    static_assert(offsetof(LensFlare, elements) == 0x38 && offsetof(LensFlare, elementScaleX) == 0x138);

    // A flare, its textures not yet loaded: nine elements, 256 by 256.
    ee::Ptr<LensFlare> newLensFlare();
    // The nine elements laid out, with the flare's glow and second ring.
    void setupLensFlareElements(ee::Ptr<LensFlare> flare);
    // Whether something stands between the eye and the sun (tested every
    // 15th frame). Nonzero while the sun is hidden.
    int32_t lensFlareHidden(ee::Ptr<zge::Vec4> sun, ee::Ptr<zge::Vec4> eye);
    // The flare's frame: drawn unless the sun is hidden or behind the camera.
    void drawLensFlare(ee::Ptr<LensFlare> flare);
}
