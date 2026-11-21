#pragma once

// The display (g_displaySetupFlags, 0x3F0 bytes; zge/render/display.cpp, its GS side ps2/display.cpp): the video
// mode resetDisplayMode sets (resetDisplayMode; BRINGUP.md "Image quality
// settings"), what the cameras' projections map onto, libgraph's double
// buffer of display and draw environments, and the frame loop's counters.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/render/ps2/gs_buffers.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/display.h.h"

namespace zge
{
    struct Matrix; // zge/math/types.h

    enum DisplayFlags : uint32_t
    {
        // Field rendering: the game draws 224-line frames for a 448-line
        // display, so vertical offsets on the screen are halved.
        kDisplayField = 1u << 0,
        kDisplayClear = 1u << 1,     // each frame starts with the draw environment's clear
        kDisplayDepthTest = 1u << 2, // a Z buffer, tested "greater" (ZTST GREATER; else none)
    };

    // sceGsResetGraph's video modes.
    constexpr int32_t kVideoModeNtsc = 2, kVideoModePal = 3;

    // (The offsets in the comments are AI's. BMX XXX's display has a word more at +0x14 and at +0x68, its double
    // buffer at +0x80 and is 0x400 bytes: its asserts below.)
    struct DisplaySetup
    {
        uint32_t flags;             // +0x00 DisplayFlags (the low word of a 64-bit field)
#if !ZX_GAME_DM2 // (the offsets here are AI's; DM2 has neither of these two words, nor the two PSMs below, see the end)
        uint32_t fieldMode;         // +0x04 sceGsResetGraph's frame / field argument (1: field rendering)
        int32_t videoMode;          // +0x08 kVideoModeNtsc or kVideoModePal
#endif
        ee::Float width;            // +0x0C (DM2 +0x04) 640
        ee::Float height;           // +0x10 (DM2 +0x08) 448 (512 in PAL)
#if ZX_GAME_BMX
        uint32_t vsyncsPerSecond;   // BMX +0x14: 50 in PAL, else 60 (ZGE4's resetGraphics; resetDisplayMode writes the next word at +0x18)
#endif
        // +0x14, +0x18 (DM2 +0x0C, +0x10; BMX +0x18, +0x1C) the GS depth at
        // the far and the near clip (1 and 65525: Z is reversed, near is
        // large). The widescreen hook writes 0.75 over the 1 (the PCSX2
        // patch it copies changes the constant resetDisplayMode stores here
        // and at +0x48).
        ee::Float depthAtFar;
        ee::Float depthAtNear;
        ee::Float centreX;          // +0x1C (DM2 +0x14, BMX +0x20) 2048: the screen's centre in GS coordinates
        ee::Float centreY;          // +0x20 (DM2 +0x18, BMX +0x24) 2048
        ee::Float bufferWidth;      // +0x24 (DM2 +0x1C, BMX +0x28) the frame buffer's size (the height halved for fields)
        ee::Float bufferHeight;     // +0x28 (DM2 +0x20, BMX +0x2C)
        ee::Float cornerX;          // +0x2C (DM2 +0x24, BMX +0x30) offsetX, offsetY as floats
        ee::Float cornerY;          // +0x30 (DM2 +0x28, BMX +0x34)
        // +0x34, +0x38 (DM2 +0x2C, +0x30; BMX +0x38, +0x3C) the screen's top
        // left corner in GS coordinates (12.4 fixed point): (4096 - width) / 2
        // and (4096 - height) / 2, times 16.
        int32_t offsetX;
        int32_t offsetY;
        uint8_t clearColour[4];     // +0x3C (DM2 +0x34, BMX +0x40) R, G, B, A of the frame's clear (setClearColour)
#if !ZX_GAME_DM2
        uint32_t framePsm;          // +0x40 (BMX +0x44) 0: PSMCT32
        uint32_t depthPsm;          // +0x44 (BMX +0x48) 0x3A: PSMZ16S
#endif
        ee::Float depthScale;       // +0x48 (DM2 +0x38, BMX +0x4C) 1 (copied to the cameras' defaults, g_defaultFieldOfView)
        ee::Float aspect;           // +0x4C (DM2 +0x3C, BMX +0x50) height / width times 4 / 3 (the pixels' aspect; g_defaultAspect)
        ee::Float half;             // +0x50 (DM2 +0x40, BMX +0x54) 0.5
        ee::Float guardBand;        // +0x54 (DM2 +0x44, BMX +0x58) 200 [inferred: the projection's guard band]
        ee::Float maxLines;         // +0x58 (DM2 +0x48, BMX +0x5C) 512
        uint32_t shadowBufferWord;  // +0x5C (DM2 +0x4C, BMX +0x60) the first GS word after the frame and Z buffers (the shadow buffer: shadow.cpp)
#if ZX_GAME_DM2
        uint32_t drawBuffer;        // DM2 +0x50 the double buffer's half drawn to (0, 1)
        uint32_t _54[3];
        GsDoubleBuffer buffers;     // DM2 +0x60 sceGsSetDefDBuff's
        int32_t oddField;           // DM2 +0x290 sceGsSyncV's field, flipped
        // The words of AI's that DM2's display does not have: only functions with no counterpart there use them, and
        // those are never bound in DM2.
        uint32_t fieldMode, framePsm, depthPsm, frameParity;
        int32_t videoMode;
#else
        uint32_t frameParity;       // +0x60 (BMX +0x64) flipped each frame (finishRenderFrame)
#if ZX_GAME_BMX
        uint32_t lastFrameParity;   // BMX +0x68: BMX XXX's own word, frameParity before finishRenderFrame's flip
#endif
        uint32_t drawBuffer;        // +0x64 (DM2 +0x50, BMX +0x6C) the double buffer's half drawn to (0, 1)
        int32_t oddField;           // +0x68 (DM2 +0x290, BMX +0x70) sceGsSyncV's field, flipped (the field the next frame is for)
#if ZX_GAME_BMX
        uint32_t _74[3];            // BMX +0x74
#else
        uint32_t _6c;               // +0x6C
#endif
        GsDoubleBuffer buffers;     // +0x70 (DM2 +0x60, BMX +0x80) sceGsSetDefDBuffDc's
        uint32_t _3a0;              // +0x3A0 (BMX +0x3B0)
#endif
        int32_t vsyncs;             // +0x3A4 (BMX +0x3B4) counted by vsyncHandler
        int32_t lastFrameVsync;     // +0x3A8 (BMX +0x3B8) vsyncs when the last frame was finished
        int32_t levelFrame;         // +0x3AC (BMX +0x3BC) frames drawn since the level started (resetLevelFrame)
        int8_t framesPending;       // +0x3B0 (BMX +0x3C0) > 0: a finished frame waits for the vsync handler to show it
        uint8_t uploadsOff;         // +0x3B1 (BMX +0x3C1) set while the vsync handler swaps and sends (texture uploads held off)
        uint8_t framePacketSent;    // +0x3B2 (BMX +0x3C2) the frame packet's DMA has ended (onFramePacketSent)
        int8_t finishingFrame;      // +0x3B3 (BMX +0x3C3) 1 while renderFrame finishes a frame
        uint8_t _3b4[0x3C];         // +0x3B4 (BMX +0x3C4) the view's words (+0x3B8 .. +0x3E8; zge/scene/camera.cpp)
    };
#if ZX_GAME_BMX
    inline void keepVsyncsPerSecond(DisplaySetup &display, uint32_t perSecond) { display.vsyncsPerSecond = perSecond; }
    inline uint32_t vsyncsPerSecond(const DisplaySetup &display) { return display.vsyncsPerSecond; }
#else
    // ZGE2 and ZGE3 keep no such word: their renderFrame counts 60 a second.
    inline void keepVsyncsPerSecond(DisplaySetup &, uint32_t) {}
    inline uint32_t vsyncsPerSecond(const DisplaySetup &) { return 60; }
#endif
    // Each game's offsets, as its machine code uses them (BMX XXX's: resetDisplayMode, setClearColour,
    // applyDisplayBuffers, initDisplayState); a word of AI's DM2 does not have stands past its end.
    static_assert(offsetof(DisplaySetup, width) == game::readable::perGame(0x0Cu, 0x04u, 0x0Cu) && offsetof(DisplaySetup, height) == game::readable::perGame(0x10u, 0x08u, 0x10u) &&
                  offsetof(DisplaySetup, depthAtFar) == game::readable::perGame(0x14u, 0x0Cu, 0x18u) && offsetof(DisplaySetup, depthAtNear) == game::readable::perGame(0x18u, 0x10u, 0x1Cu) &&
                  offsetof(DisplaySetup, centreX) == game::readable::perGame(0x1Cu, 0x14u, 0x20u) && offsetof(DisplaySetup, centreY) == game::readable::perGame(0x20u, 0x18u, 0x24u));
    static_assert(offsetof(DisplaySetup, bufferHeight) == game::readable::perGame(0x28u, 0x20u, 0x2Cu) && offsetof(DisplaySetup, cornerX) == game::readable::perGame(0x2Cu, 0x24u, 0x30u) &&
                  offsetof(DisplaySetup, offsetX) == game::readable::perGame(0x34u, 0x2Cu, 0x38u) && offsetof(DisplaySetup, clearColour) == game::readable::perGame(0x3Cu, 0x34u, 0x40u) &&
                  offsetof(DisplaySetup, aspect) == game::readable::perGame(0x4Cu, 0x3Cu, 0x50u) && offsetof(DisplaySetup, maxLines) == game::readable::perGame(0x58u, 0x48u, 0x5Cu) &&
                  offsetof(DisplaySetup, shadowBufferWord) == game::readable::perGame(0x5Cu, 0x4Cu, 0x60u) && offsetof(DisplaySetup, drawBuffer) == game::readable::perGame(0x64u, 0x50u, 0x6Cu) &&
                  offsetof(DisplaySetup, oddField) == game::readable::perGame(0x68u, 0x290u, 0x70u) && offsetof(DisplaySetup, buffers) == game::readable::perGame(0x70u, 0x60u, 0x80u));
    static_assert(game::readable::perGame(offsetof(DisplaySetup, depthPsm) == 0x44, offsetof(DisplaySetup, depthPsm) > offsetof(DisplaySetup, oddField),
                    offsetof(DisplaySetup, depthPsm) == 0x48) &&
                  game::readable::perGame(offsetof(DisplaySetup, frameParity) == 0x60, offsetof(DisplaySetup, frameParity) > offsetof(DisplaySetup, oddField),
                    offsetof(DisplaySetup, frameParity) == 0x64));
    // (DM2's: not checked past its oddField, as before)
    static_assert(game::readable::perGame(offsetof(DisplaySetup, vsyncs) == 0x3A4 && offsetof(DisplaySetup, levelFrame) == 0x3AC &&
                                              offsetof(DisplaySetup, framesPending) == 0x3B0 && offsetof(DisplaySetup, finishingFrame) == 0x3B3 &&
                                              sizeof(DisplaySetup) == 0x3F0 && offsetof(DisplaySetup, buffers) + offsetof(GsDoubleBuffer, draw) == 0xC0 &&
                                              offsetof(DisplaySetup, buffers) + offsetof(GsDoubleBuffer, draw[1]) == 0x230,
                                          true,
                                          offsetof(DisplaySetup, vsyncs) == 0x3B4 && offsetof(DisplaySetup, levelFrame) == 0x3BC &&
                                              offsetof(DisplaySetup, framesPending) == 0x3C0 && offsetof(DisplaySetup, finishingFrame) == 0x3C3 &&
                                              sizeof(DisplaySetup) == 0x400 && offsetof(DisplaySetup, buffers) + offsetof(GsDoubleBuffer, draw) == 0xD0 &&
                                              offsetof(DisplaySetup, buffers) + offsetof(GsDoubleBuffer, draw[1]) == 0x240));

    constexpr uint32_t kDisplayAddress = sym::g_displaySetupFlags;
    // The set-up's bytes initDisplayState clears: the whole record; ZGE2's is 0x2D0 bytes (DM2's initDisplayState clears that many).
    constexpr uint32_t kDisplaySetupBytes = game::readable::kIsDm2 ? 0x2D0u : static_cast<uint32_t>(sizeof(DisplaySetup));
    inline ee::Global<DisplaySetup, kDisplayAddress> g_display;
    // The display's flags, as the originals read and write them: a doubleword.
    inline uint64_t &flagsDoubleword(DisplaySetup &display) { return *reinterpret_cast<uint64_t *>(&display.flags); }
    // libgraph's double buffer in the display, and its two draw buffers.
    constexpr uint32_t kDisplayBuffersAddress = kDisplayAddress + offsetof(DisplaySetup, buffers);
    constexpr uint32_t kDrawBuffer0Address = kDisplayBuffersAddress + offsetof(GsDoubleBuffer, draw[0]);
    constexpr uint32_t kDrawBuffer1Address = kDisplayBuffersAddress + offsetof(GsDoubleBuffer, draw[1]);
    // The GS has to be reset at the next frame's end (the field mode
    // changed: resetDisplayMode); until then the vsync interrupt shows
    // nothing.
    inline ee::Global<uint32_t, sym::g_gsResetPending> g_gsResetPending;

    // The display's vsyncs a second (BMX XXX's DisplaySetup::vsyncsPerSecond: 50 in PAL, else 60): what ZGE4's
    // billboard effects count their frames by. ZGE2's and ZGE3's displays keep no such word (never asked).
    inline uint32_t vsyncsASecond([[maybe_unused]] const DisplaySetup &display)
    {
#if ZX_GAME_BMX
        return display.vsyncsPerSecond;
#else
        return 0;
#endif
    }
    // finishRenderFrame's flip: BMX XXX's display keeps the parity from before it.
    inline void keepLastFrameParity([[maybe_unused]] DisplaySetup &display)
    {
#if ZX_GAME_BMX
        display.lastFrameParity = display.frameParity;
#endif
    }
    // initDisplay's start of the parities: BMX XXX's display starts its frame parity at 0 and the one kept
    // before the flip at -1 (none yet); ZGE2's and ZGE3's initDisplay leave them.
    inline void startFrameParities([[maybe_unused]] DisplaySetup &display)
    {
#if ZX_GAME_BMX
        display.lastFrameParity = ~0u;
        display.frameParity = 0;
#endif
    }

    // The engine's frame state (g_cameraDefaults' record, which startEngine clears): the frame loop's words, the
    // screen's size and the cameras' default scales the video mode gives. ZGE2 (DM2) has neither the first word
    // nor the rendered-frame count, so its words are 4 bytes sooner and none stands between the height and the
    // scales.
    struct FrameState
    {
#if !ZX_GAME_DM2
        uint32_t _00;
#endif
        uint32_t quitRequested;     // +0x04 (DM2 +0x00) g_quitRequested
        uint32_t renderingFrame;    // +0x08 (DM2 +0x04) g_renderingFrame
        int32_t width;              // +0x0C (DM2 +0x08) g_screenWidth
        int32_t height;             // +0x10 (DM2 +0x0C) g_screenHeight
#if !ZX_GAME_DM2
        int32_t renderedFrameCount; // +0x14 g_renderedFrameCount
#endif
        ee::Float horizontalScale;  // +0x18 (DM2 +0x10) the display's depthScale (g_defaultFieldOfView)
        ee::Float verticalScale;    // +0x1C (DM2 +0x14) the display's aspect (g_defaultAspect)
    };
    static_assert(offsetof(FrameState, horizontalScale) == game::readable::perGame(0x18u, 0x10u));

    // resetDisplayMode (display.cpp): the video mode set (the fourth argument
    // is not read).
    void resetDisplayMode(uint32_t width, uint32_t height, uint32_t depthTest, uint32_t unused, uint32_t field,
                          uint32_t clear, uint32_t resetGs);
    // showAssertScreen (display.cpp): assertFailed's screen.
    void showAssertScreen(uint32_t why, uint32_t file, uint32_t line, uint32_t function, uint32_t colour);
}

namespace zge
{
    // (display.cpp)
    void initDisplayState();
    void initGraphicsSystems();
    void shutDownGraphicsSystems();
    int32_t renderCountedFrame();
    void affineMatrixCopy(ee::Ptr<Matrix> dst, ee::Ptr<const Matrix> src);
    void resetCameraViewports(uint32_t width, uint32_t height);
    void resetFramePackets();
    void resetLevelFrame();
    void waitForFrameReady();
    uint32_t displayNoop();
}

namespace zge
{
    // (ps2/display.cpp) The display as the GS takes it: libgraph's buffers set up, swapped and sent, the GS reset
    // and its start-up registers, the screen's colour rectangles and the frame's clear colour.
    void initDisplay();
    int32_t resetGraphics(uint32_t fieldMode);
    int32_t setupDisplayBuffers(uint32_t flush);
    void swapDisplayBuffers(uint32_t inInterrupt);
    void waitAndSwapDisplayBuffers(); // ZGE2's swapDisplayBuffers
    void applyDisplayBuffers();
    void clearDisplayRectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, ee::Ptr<uint8_t> colour);
    void setClearColour(ee::Ptr<uint8_t> colour);
}

namespace zge::ps2
{
    // The GIF path waited for (sceGsSyncPath); on a timeout the DMA channels and the path reset (finishRenderFrame,
    // ZGE2's swap).
    void waitForGsPath();
}

namespace zge
{
    // (screen_capture.cpp)
    uint32_t endViewOverlay();
    uint32_t captureScreenToHost(uint32_t advance);
    uint32_t showRawImage(ee::Address path, uint32_t width, uint32_t height, uint32_t bitsPerPixel);
    uint32_t showScreenSizedRawImage(ee::Address path); // ZGE2's showRawImage (its own body)
}
