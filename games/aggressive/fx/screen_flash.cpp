// The screen flash (gamehand14; were the sweep's drafts of startScreenFlash ..
// unusedFlashHook): a full-screen quad of one colour, its alpha faded out to 0 or
// in to 255 over the flash's time, or held. The level scripts' camera flash
// (createCameraFlash, nativeCreateCameraFlash) and the effects' frame
// (effectsStep3) start it; updateEffects moves it each frame and the
// effects' overlay draws it.
//

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include "aggressive/fx/billboard.h"
#include "zge/math/vec.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/fx/screen_flash.cpp.h"

namespace zge
{
}

namespace aggressive::fx
{
    namespace
    {
        enum class FlashKind : uint8_t
        {
            Hold = 0,    // the colour as given for the time
            FadeOut = 1, // its alpha down to 0 over the time
            FadeIn = 2,  // up to 255
        };

        // The flash (.sbss g_screenFlash; the colour unaligned, as the original
        // stores it).
        struct ScreenFlash
        {
            FlashKind kind;    // +0x00
            uint8_t rgba[4];   // +0x01
            uint8_t _05[3];
            ee::Float timeLeft; // +0x08 seconds
            ee::Float alphaRate; // +0x0C alpha a second
            uint32_t on;       // +0x10
        };
        static_assert(offsetof(ScreenFlash, timeLeft) == 0x08 && offsetof(ScreenFlash, on) == 0x10);
        inline ee::Global<ScreenFlash, sym::g_screenFlash> g_screenFlash;

        // The flash's quad as the primitive API takes it: no texture, its
        // four corners (billboard.h's QuadVertex: zge/render/ps2/primitive_packets.cpp's
        // PrimitiveVertex).
        struct FlashQuad
        {
            uint32_t texture;                // +0x00
            ee::Ptr<QuadVertex> vertices[4]; // +0x04
            uint8_t _14[0x0C];
            QuadVertex corners[4];           // +0x20
        };
        static_assert(offsetof(FlashQuad, corners) == 0x20 && sizeof(FlashQuad) == 0xA0);
        // The 2D packet (primitive flags kPrimitiveScreen | kPrimitiveSecondSlot).
        constexpr uint32_t kScreenQuads = 0x02000002u;
        constexpr ee::Float kScreenWidth = 512.0f, kScreenHeight = 480.0f;
    }

    // startScreenFlash (kind, RGBA, seconds): a flash of that colour for that
    // long (see FlashKind).
    void startScreenFlash(uint32_t kind, uint32_t rgba, ee::Float seconds)
    {
        ee::Local<uint8_t[4]> colour; // the RGBA's bytes
        const uint8_t which = static_cast<uint8_t>(kind);
        std::memcpy(&colour[0], &rgba, 4);
        ScreenFlash &flash = *g_screenFlash;
        flash.kind = static_cast<FlashKind>(which);
        std::memcpy(flash.rgba, &colour[0], 4);
        flash.timeLeft = seconds;
        const uint8_t alpha = colour[3];
        if (which == static_cast<uint8_t>(FlashKind::FadeOut))
            flash.alphaRate = ee::toFloat(-static_cast<int32_t>(alpha)) / seconds;
        else if (which == static_cast<uint8_t>(FlashKind::FadeIn))
            flash.alphaRate = ee::toFloat(255 - static_cast<int32_t>(alpha)) / seconds;
        else
            flash.alphaRate = ee::Float(0.0f);
        flash.on = 1;
    }
    READABLE_FUNCTION(startScreenFlash, Verified);

    // updateScreenFlash (seconds, the frame's): the flash's alpha moved by its
    // rate; when its time is up it ends.
    void updateScreenFlash(ee::Float seconds)
    {
        ScreenFlash &flash = *g_screenFlash;
        if (flash.on)
        {
            const ee::Float left = flash.timeLeft;
            if (seconds < left)
            {
                const ee::Float alpha = ee::toFloat(static_cast<int32_t>(flash.rgba[3]));
                const ee::Float change = flash.alphaRate * seconds;
                flash.timeLeft = left - seconds;
                flash.rgba[3] = static_cast<uint8_t>(ee::toInt(alpha + change));
                return;
            }
            flash.timeLeft = ee::Float(0.0f);
            flash.on = 0;
        }
    }
    READABLE_FUNCTION(updateScreenFlash, Verified);

    // drawScreenFlash: the flash drawn over the screen while it is on.
    void drawScreenFlash()
    {
        ee::Local<FlashQuad> quad;
        for (uint32_t i = 0; i < 4; ++i)
            quad->vertices[i] = &quad->corners[i];
        quad->texture = 0;
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i].position = zge::kZero;
        if (g_screenFlash->on)
        {
            // Corners clockwise from the top right.
            const ee::Float xs[4] = {kScreenWidth, 0.0f, 0.0f, kScreenWidth};
            const ee::Float ys[4] = {0.0f, 0.0f, kScreenHeight, kScreenHeight};
            for (uint32_t i = 0; i < 4; ++i)
            {
                QuadVertex &corner = quad->corners[i];
                std::memcpy(corner.colour, g_screenFlash->rgba, 4);
                corner.u = ee::Float(0.0f);
                corner.v = ee::Float(0.0f);
                corner.position.x = xs[i];
                corner.position.y = ys[i];
            }
            zge::drawPrimitiveQuads(kScreenQuads, ee::Ptr<uint32_t>(&quad->texture), 1, 0);
        }
    }
    READABLE_FUNCTION_WAITS(drawScreenFlash, Verified);

    // unusedFlashHook: empty (no caller and no reference in the game).
    void unusedFlashHook()
    {
    }
    READABLE_FUNCTION(unusedFlashHook, Verified);
}
