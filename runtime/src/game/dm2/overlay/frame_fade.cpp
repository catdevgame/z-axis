// DM2's in-game overlay: the screen fade (kOverlayFade). The game sends a
// colour and a number of frames (ScreenFade); the frame draws a rectangle
// over the whole screen that reaches the colour, or leaves it, in that time.
// DM2's own code (overlay.c).

#include "frames.h"
#include "overlay_items.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/frame_fade.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The fade that is running.
        struct FadeState
        {
            uint8_t flags;        // +0x00 the ScreenFade's
            uint8_t pad01[3];     // padding: no reader, no writer (pad2: only overlayFadeFrame and overlayFadeMessage form the address, and neither touches it)
            uint16_t framesLeft;  // +0x04 0: no fade
            uint16_t pad06;       // padding: no reader, no writer (pad2: only overlayFadeFrame and overlayFadeMessage form the address, and neither touches it)
            ee::Float colour[3];  // +0x08 red, green, blue now
            ee::Float step[3];    // +0x14 what each gains a frame
        };
        static_assert(offsetof(FadeState, framesLeft) == 0x04 && offsetof(FadeState, colour) == 0x08 &&
                      offsetof(FadeState, step) == 0x14 && sizeof(FadeState) == 0x20);
        inline ee::Global<FadeState, sym::g_screenFadeState> g_screenFadeState;

        constexpr int32_t kScreenWidth = 0x200;
        constexpr int32_t kScreenHeight = 0x1C0;
        // The rectangle's top byte (zoverlay's colours: the blend mode in the
        // top four bits, then the alpha): full alpha in blend mode 3, or in
        // blend mode 2 for a fade sent with kFadeBlendMode2.
        // Scaffolding (value-named): what the two blend modes do is
        // zoverlay's drawing code's to say (not readable yet).
        constexpr uint32_t kFadeStyle = 0x3F000000;
        constexpr uint32_t kBlendMode2FadeStyle = 0x2F000000;
        constexpr uint8_t kFadeBlendMode2 = 0x02; // ScreenFade::flags
    }

    // The fade's frame: the colour moves a step and is drawn over the screen,
    // until the fade's frames are used up.
    void overlayFadeFrame()
    {
        FadeState &fade = *g_screenFadeState;
        if (fade.framesLeft == 0)
            return;
        for (uint32_t i = 0; i < 3; ++i)
            fade.colour[i] = fade.colour[i] + fade.step[i];
        const uint32_t style = (fade.flags & kFadeBlendMode2) != 0 ? kBlendMode2FadeStyle : kFadeStyle;
        const uint32_t colour = style | static_cast<uint32_t>(ee::toInt(fade.colour[0]) << 16) |
                                static_cast<uint32_t>(ee::toInt(fade.colour[1]) << 8) |
                                static_cast<uint32_t>(ee::toInt(fade.colour[2]));
        drawOverlayRectangle(0, 0, kScreenWidth, kScreenHeight, colour);
        --fade.framesLeft;
    }
    READABLE_FUNCTION(overlayFadeFrame, Verified);

    // A fade asked for (none when it has no frames): to the colour from
    // black, or with kFadeFromColour from the colour down to black.
    void overlayFadeMessage(ee::Ptr<const ScreenFade> asked)
    {
        FadeState &fade = *g_screenFadeState;
        fade.framesLeft = asked->frames;
        if (fade.framesLeft == 0)
            return;
        const uint8_t target[3] = {asked->red, asked->green, asked->blue};
        const ee::Float frames = ee::toFloat(fade.framesLeft);
        for (uint32_t i = 0; i < 3; ++i)
            fade.step[i] = ee::toFloat(target[i]) / frames;
        fade.flags = asked->flags;
        if ((fade.flags & kFadeFromColour) != 0)
        {
            for (uint32_t i = 0; i < 3; ++i)
            {
                fade.colour[i] = ee::toFloat(target[i]);
                fade.step[i] = -fade.step[i];
            }
        }
        else
        {
            for (uint32_t i = 0; i < 3; ++i)
                fade.colour[i] = 0.0f;
        }
    }
    READABLE_FUNCTION(overlayFadeMessage, Verified);
}
