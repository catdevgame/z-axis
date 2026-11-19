// The display (zge/render/display.h lays its state out): the frame loop
// (renderFrame, finishRenderFrame), the video mode and the cameras'
// viewports on it, the display's start and the graphics systems'. The GS
// and libgraph's double buffer are its PS2 form, ps2/display.cpp.
//
// Original quirks kept: renderFrame's frame-rate wait divides 60 by the
// rate without a check (the game's rate is never below 1).

#include "display.h"

#include "ee/abi.h"
#include "ee/interrupt_wait.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/part.h"
#include "zge/render/renderer.h"
#include "zge/render/texture.h"
#include "zge/render/material.h"
#include "zge/render/ps2/frame_packets.h"
#include "zge/render/ps2/libgraph_calls.h"
#include "zge/debug/profiler.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/display.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_displayStateReady> g_displayStateReady;
        inline ee::Global<uint32_t, sym::g_renderingFrame> g_renderingFrame;
        inline ee::Global<int32_t, sym::g_renderedFrameCount> g_renderedFrameCount;
        inline ee::Global<ee::Ptr<Camera>, sym::g_renderCamera> g_renderCamera;   // renderFrame's camera
        inline ee::Global<uint32_t, sym::g_renderProfileTimer> g_renderProfileTimer;    // "render"
        inline ee::Global<uint32_t, sym::g_frameProfileTimer> g_frameProfileTimer;     // the frame's end
        // The game's frame rate (frames a second; 0: each vsync).
        inline ee::Global<ee::Float, sym::g_targetFrameRate> g_targetFrameRate;
        // The frame state (display.h): the screen's size and the cameras' defaults the video mode gives.
        inline ee::Global<FrameState, sym::g_cameraDefaults> g_frameState;

        constexpr uint32_t kRenderTimerName = sym::renderTimerName, kRenderTimerGroup = sym::renderTimerGroup;
        constexpr uint32_t kFrameTimerGroup = sym::frameTimerGroup;
        inline ee::Global<uint32_t, sym::frameTimerName> g_frameTimerName; // a name of three letters, as a word

        constexpr ee::Float kPixelAspect = std::bit_cast<float>(0x3FAAAAABu); // 4 / 3
        constexpr ee::Float kDepthAtNear = std::bit_cast<float>(0x477FF500u); // 65525

        int16_t toInt16(int32_t value) { return static_cast<int16_t>(value); }
    }

    // ---- the frame's end ------------------------------------------------------------

    // finishRenderFrame: the frame's end: its profile timer stopped (made the
    // first time), the GIF path waited for (reset on a timeout), the queued
    // material textures applied, a pending GS reset done, the next vsync
    // waited for, the buffers swapped, the frame's fix-ups flipped and a new
    // profile frame started. (The original returns what its last call
    // returned; neither caller reads it.)
    void finishRenderFrame()
    {
        ee::Local<uint32_t> colour;
        DisplaySetup &display = *g_display;
        const uint32_t timer = *g_frameProfileTimer;
        if (timer == 0)
        {
            *colour = *g_frameTimerName;
            const uint32_t made = newProfileBar(0, 0, colour, kFrameTimerGroup);
            *g_frameProfileTimer = made;
            if (made != 0)
            {
                endProfileBarFrame(made);
                startProfileBar(*g_frameProfileTimer);
            }
        }
        else
            stopProfileBar(timer);
        ps2::waitForGsPath();
        applyQueuedMaterialTextures();
        if (*g_gsResetPending != 0)
        {
            *g_gsResetPending = 0;
            sceGsSyncV(0);
            resetGraphics(display.fieldMode);
            setupDisplayBuffers(1);
        }
        const int32_t field = sceGsSyncV(0);
        const int32_t vsyncs = display.vsyncs;
        display.oddField = field ^ 1;
        display.lastFrameVsync = vsyncs;
        display.uploadsOff = 0;
        swapDisplayBuffers(0);
        keepLastFrameParity(display);
        display.frameParity ^= 1u;
        flipFrameFixups();
        const uint32_t timerNow = *g_frameProfileTimer;
        if (timerNow != 0)
        {
            endProfileBarFrame(timerNow);
            startProfileBar(*g_frameProfileTimer);
        }
    }
    READABLE_FUNCTION_WAITS(finishRenderFrame, Verified);

    // showAssertScreen (its one caller is assertFailed; dead): the screen cleared
    // to `colour` and two frames shown. DM2 puts the text on it (function,
    // file, line and reason); this build ignores the four.
    void showAssertScreen(uint32_t /*why*/, uint32_t /*file*/, uint32_t /*line*/, uint32_t /*function*/, uint32_t colour)
    {
        setClearColour(ee::Ptr<uint8_t>(colour));
        finishRenderFrame();
        game::tail(zge::finishRenderFrame); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(showAssertScreen, Draft);

    // initDisplayState: the display's state cleared and the display started
    // (ZGE2 clears one state word and its 0x2D0-byte set-up).
    void initDisplayState()
    {
        memoryFill(sym::g_displayStateReady, 0, game::readable::perGame(0x10u, 0x4u, 0x10u));
        memoryFill(kDisplayAddress, 0, kDisplaySetupBytes);
        *g_displayStateReady = 1;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps the global pointer in initDmaChannels)
            saveGlobalPointer();
        initDisplay();
    }
    READABLE_FUNCTION(initDisplayState, Verified);

    // shutDownDisplayState (no caller). 1.
    uint32_t shutDownDisplayState()
    {
        if (*g_displayStateReady == 0)
            return 1;
        *g_displayStateReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownDisplayState, Draft);

    // initGraphicsSystems: the texture cache, the GS textures, then the renderer
    // started, each only if the one before started.
    void initGraphicsSystems()
    {
        if (initTextureCache() != 0)
        {
            if (initGsTextures() != 0)
                initRenderer();
        }
    }
    READABLE_FUNCTION_WAITS(initGraphicsSystems, Verified);

    // shutDownGraphicsSystems: the renderer, the GS textures, then the texture cache
    // shut down, each only if the one before did.
    void shutDownGraphicsSystems()
    {
        if (shutDownRenderer() != 0)
        {
            if (shutDownGsTextures() != 0)
                shutDownTextureCache();
        }
    }
    READABLE_FUNCTION_WAITS(shutDownGraphicsSystems, Verified);

    namespace
    {
        // renderFrame's camera: set current and its stages 1 and 2 run; false
        // when it will not clear.
        bool renderCameraStages(ee::Ptr<Camera> camera)
        {
            if (setCurrentCamera(camera) != 0)
            {
                runRenderStage(1);
                runRenderStage(2);
                if (clearCurrentCamera() == 0)
                    return false;
            }
            return true;
        }
    }

    // renderFrame: a frame drawn: render stage 0, then each camera of kind
    // 0 set current and its stages 1 and 2 (0 if a camera will not clear),
    // the 2D pass, the frame packet closed, the parts' packets built on the
    // level's first frame, vsyncs waited for the game's frame rate, the
    // frame's end. 1.
    uint32_t renderFrame()
    {
        DisplaySetup &display = *g_display;
        const uint32_t timer = *g_renderProfileTimer;
        if (timer == 0)
            *g_renderProfileTimer = newProfileBar(2, 0, kRenderTimerName, kRenderTimerGroup);
        else
            startProfileBar(timer);
        runRenderStage(0);
        beginFramePackets();
        if constexpr (game::readable::kIsBmx) // (ZGE4 walks the cameras in a local of its own)
        {
            for (ee::Ptr<Camera> camera = firstCameraOfKind(0); camera; camera = nextCameraOfKind(camera))
                if (!renderCameraStages(camera))
                    return 0;
        }
        else
        {
            for (*g_renderCamera = firstCameraOfKind(0); *g_renderCamera; *g_renderCamera = nextCameraOfKind(*g_renderCamera))
                if (!renderCameraStages(*g_renderCamera))
                    return 0;
        }
        draw2DPass();
        closeFramePacket();
        stopProfileBar(*g_renderProfileTimer);
        endProfileBarFrame(*g_renderProfileTimer);
        if (display.levelFrame == 0)
            buildAllPartPackets();
        // The vsyncs a frame takes at the game's rate, less one.
        const ee::Float rate = *g_targetFrameRate;
        int32_t vsyncsPerFrame = 0;
        if (ee::Float(0.0f) < rate)
        {
            const int32_t perSecond = ee::toInt(rate);
            // (the original traps on 0: BREAK 7; ZGE4 divides its display's vsyncs a second, unsigned)
            int32_t quotient = 0;
            if constexpr (game::readable::kIsBmx)
                quotient = perSecond != 0 ? static_cast<int32_t>(vsyncsPerSecond(display) / static_cast<uint32_t>(perSecond)) : 0;
            else
                quotient = perSecond != 0 ? static_cast<int32_t>(vsyncsPerSecond(display)) / perSecond : 0;
            vsyncsPerFrame = quotient - 1;
        }
        if (display.vsyncs - display.lastFrameVsync < vsyncsPerFrame)
        {
            for (;;)
            {
                const int32_t field = sceGsSyncV(0);
                const bool early = display.vsyncs - display.lastFrameVsync < vsyncsPerFrame;
                display.oddField = field ^ 1;
                if (!early)
                    break;
            }
        }
        display.finishingFrame = 1;
        finishRenderFrame();
        display.finishingFrame = 0;
        finishFrame();
        const int8_t pending = display.framesPending;
        display.levelFrame = display.levelFrame + 1;
        if (pending < 0)
            ee::Ptr<DisplaySetup>(kDisplayAddress)->framesPending = 1;
        return 1;
    }

    // renderFrameWithViews: ZGE2's renderFrame: render stage 0, then each
    // camera of kind 0 set current drawn in a view of its own (its VU1
    // constants begun, stages 1 and 2, the view ended; 0 if the camera will
    // not clear), then stages 3 and 4, the display's buffers swapped and the
    // frame finished, between the two calls of an empty function. No profile
    // bars, 2D pass, frame packet closed here (closeAndFinishFrame does),
    // part packets built, vsync wait or frame counted, as ZGE3's. 1.
    uint32_t renderFrameWithViews()
    {
        frameRenderingNoop(1);
        runRenderStage(0);
        for (*g_renderCamera = firstCameraOfKind(0); *g_renderCamera; *g_renderCamera = nextCameraOfKind(*g_renderCamera))
        {
            if (setCurrentCamera(*g_renderCamera) == 0)
                continue;
            beginViewDraws(*g_renderCamera);
            runRenderStage(1);
            runRenderStage(2);
            endViewDraws();
            if (clearCurrentCamera() == 0)
                return 0;
        }
        runRenderStage(3);
        runRenderStage(4);
        frameRenderingNoop(0);
        waitAndSwapDisplayBuffers(); // (ZGE2's swapDisplayBuffers)
        finishFrame();
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(renderFrame, Verified, dm2, renderFrameWithViews);

    // renderCountedFrame: a frame drawn and counted; the flag says one is being
    // drawn (engineMain's loop, each frame). ZGE2 does not count them.
    int32_t renderCountedFrame()
    {
        if constexpr (!game::readable::kIsDm2)
            *g_renderedFrameCount += 1;
        *g_renderingFrame = 1;
        const int32_t result = static_cast<int32_t>(renderFrame());
        *g_renderingFrame = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(renderCountedFrame, Verified);


    // affineMatrixCopy: a matrix's rotation rows copied with w 0 and its
    // translation row with w 1 (an affine matrix's homogeneous form).
    void affineMatrixCopy(ee::Ptr<Matrix> dst, ee::Ptr<const Matrix> src)
    {
        Matrix &out = *dst;
        const Matrix &in = *src;
        for (uint32_t row = 0; row < 3; ++row)
        {
            out.m[row][0] = in.m[row][0];
            out.m[row][1] = in.m[row][1];
            out.m[row][3] = ee::Float(0.0f);
            out.m[row][2] = in.m[row][2];
        }
        out.m[3][0] = in.m[3][0];
        out.m[3][1] = in.m[3][1];
        out.m[3][3] = ee::Float(1.0f);
        out.m[3][2] = in.m[3][2];
    }
    READABLE_FUNCTION(affineMatrixCopy, Verified);


    // resetDisplayMode: the video mode: the screen's size (512 lines in PAL),
    // field rendering, the Z buffer's test and the frame's clear; the GS
    // reset at the next frame's end when field rendering changed (or at
    // once with resetGs, the buffers then swapped to the first); the
    // centre, corners, depths and aspect; libgraph's buffers set up; the GS
    // memory after the two frame buffers and the Z buffer (each at least
    // 256 lines for the Z and shadow buffers) given to the texture cache;
    // the cameras' defaults. A frame waiting to be shown is dropped (-1).
    // (The fourth argument is not read.) ZGE4 keeps the height in PAL and
    // never clears a reset already asked for.
    void resetDisplayMode(uint32_t width, uint32_t height, uint32_t depthTest, uint32_t unused, uint32_t field,
                          uint32_t clear, uint32_t resetGs)
    {
        (void)unused;
        DisplaySetup &display = *g_display;
        if (display.framesPending > 0)
            display.framesPending = -1;
        uint64_t &flags = flagsDoubleword(display);
        const uint32_t wasField = display.flags & kDisplayField;
        flags = ((((flags & ~1ull) | (field & 1u)) & ~4ull) | static_cast<uint64_t>(depthTest & 1u) << 2) & ~2ull |
                static_cast<uint64_t>(clear & 1u) << 1;
        uint32_t lines = height;
        if constexpr (!game::readable::kIsBmx) // (ZGE4 takes the height it is given in PAL too)
            if (display.videoMode == kVideoModePal)
                lines = 0x200;
        const ee::Float screenWidth = ee::toFloat(static_cast<int32_t>(width));
        const ee::Float screenHeight = ee::toFloat(static_cast<int32_t>(lines));
        if constexpr (game::readable::kIsBmx) // ZGE4 only asks for the reset (a reset already asked for stays)
        {
            if (wasField != field)
                *g_gsResetPending = 1u;
        }
        else
            *g_gsResetPending = (wasField ^ field) != 0 ? 1u : 0u;
        display.fieldMode = field != 0 ? 1u : 0u;
        display.centreY = ee::Float(2048.0f);
        display.width = screenWidth;
        display.height = screenHeight;
        display.centreX = ee::Float(2048.0f);
        display.bufferWidth = screenWidth;
        ee::Float bufferHeight = screenHeight;
        if ((display.flags & kDisplayField) != 0)
            bufferHeight = bufferHeight * ee::Float(0.5f);
        display.bufferHeight = bufferHeight;
        // The screen's corner, the screen in the middle of the drawing space,
        // in subpixels.
        const int32_t left = subpixels((kScreenSpacePixels - ee::toInt(screenWidth)) / 2);
        const int32_t top = subpixels((kScreenSpacePixels - ee::toInt(bufferHeight)) / 2);
        display.offsetX = left;
        display.offsetY = top;
        display.cornerX = ee::toFloat(left);
        display.cornerY = ee::toFloat(top);
        display.aspect = (bufferHeight / screenWidth) * kPixelAspect;
        display.half = ee::Float(0.5f);
        display.guardBand = ee::Float(200.0f);
        display.maxLines = ee::Float(512.0f);
        display.depthAtFar = ee::Float(1.0f);
        display.depthAtNear = kDepthAtNear;
        display.depthScale = ee::Float(1.0f);
        if (resetGs != 0)
        {
            sceGsSyncV(0);
            resetGraphics(display.fieldMode);
        }
        setupDisplayBuffers(1);
        if (resetGs != 0)
            sceGsSwapDBuffDc(kDisplayBuffersAddress, 0);
        const uint32_t pixelsWide = static_cast<uint32_t>(ee::toInt(display.width));
        int32_t frameLines = ee::toInt(display.height);
        if ((display.flags & kDisplayField) != 0)
            frameLines = frameLines / 2;
        display.depthPsm = gs::kPsmZ16S;
        display.framePsm = gs::kPsmCt32;
        uint32_t depthLines = static_cast<uint32_t>(frameLines);
        if (display.videoMode == kVideoModePal && depthLines <= 0xFF)
            depthLines = 0x100;
        const uint32_t frameWords = pixelsWide * depthLines; // (the PAL minimum applied once)
        if (depthLines <= 0xFF)
            depthLines = 0x100;
        const uint32_t depthWords = pixelsWide * depthLines;
        // Two 32-bit frame buffers and a 16-bit Z buffer; the shadow
        // buffer's 16 bits after them.
        const uint32_t buffersBytes = (frameWords << 3) + (depthWords << 1);
        display.shadowBufferWord = buffersBytes >> 2;
        resetTextureCacheAndFlush((buffersBytes + (depthWords << 1)) >> 2);
        FrameState &state = *g_frameState;
        state.width = static_cast<int32_t>(width);
        state.height = static_cast<int32_t>(lines);
        state.verticalScale = display.aspect;
        state.horizontalScale = display.depthScale;
    }
    READABLE_FUNCTION(resetDisplayMode, Verified);

    namespace
    {
        struct ViewportRectangle
        {
            ee::Float left, top, width, height;
        };
    }

    // resetCameraViewports: the video mode set for (width, height) (field rendering,
    // a Z buffer and a clear, no GS reset), each camera of kind 0 given the
    // whole screen and the mode's scales, the clear colour set again and the
    // buffers applied.
    void resetCameraViewports(uint32_t width, uint32_t height)
    {
        ee::Local<ViewportRectangle> viewport;
        ee::Local<ViewportRectangle> built;
        resetDisplayMode(width, height, 1, 1, 1, 1, 0);
        for (ee::Ptr<Camera> camera = firstCameraOfKind(0); camera; camera = nextCameraOfKind(camera))
        {
            const DisplaySetup &display = *g_display;
            built->width = display.width;
            built->height = display.height;
            built->left = ee::Float(0.0f);
            built->top = ee::Float(0.0f);
            viewport = *built;
            if constexpr (game::readable::kIsBmx)
            {
                // ZGE4 keeps a camera's own viewport, cut to the screen's size: read before the screen is set
                // (which sets the viewport to the screen too), then set back.
                cameraViewport(camera, built);
                setCameraViewport(camera, viewport);
                built->width = ee::min(viewport->width, built->width);
                built->height = ee::min(viewport->height, built->height);
                swapCameraViewport(camera, built);
            }
            else
                setCameraViewport(camera, viewport);
            setCameraScales(camera, g_frameState->horizontalScale, g_frameState->verticalScale);
        }
        setClearColour(g_display->clearColour);
        applyDisplayBuffers();
    }

    namespace
    {
        // An unsigned word as a float, as the compiler converts one (halved with its low bit kept, then doubled).
        inline ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value >> 1) | (value & 1u)));
            return half + half;
        }
    }

    // resetDisplayAndCameraViewports: ZGE2's resetCameraViewports, with its
    // video mode set-up written in (ZGE2 has no resetDisplayMode): the
    // screen's size (as given, no PAL lines; unsigned), its centre, corners,
    // depths and aspect as ZGE3's, the buffer halved for fields when the
    // display's flags word is not 0, libgraph's double buffer set up (a Z
    // test, PSMZ16S and a clear, always) and the data cache written back, the
    // GS memory after the two frame buffers and the Z buffer (at least 256
    // lines) given to the texture cache, the cameras' defaults; then, as
    // ZGE3's, each camera of kind 0 given the whole screen and the mode's
    // scales, the clear colour set again and the buffers applied. No frame
    // dropped, GS reset or field mode kept, as ZGE3's are.
    void resetDisplayAndCameraViewports(uint32_t width, uint32_t height)
    {
        ee::Local<ViewportRectangle> viewport;
        ee::Local<ViewportRectangle> built;
        DisplaySetup &display = *g_display;
        display.width = unsignedToFloat(width);
        display.height = unsignedToFloat(height);
        display.centreY = ee::Float(2048.0f);
        display.centreX = ee::Float(2048.0f);
        const ee::Float screenWidth = display.width;
        const bool fields = display.flags != 0; // (ZGE2 tests the whole word)
        display.bufferWidth = screenWidth;
        ee::Float bufferHeight = display.height;
        if (fields)
            bufferHeight = bufferHeight * ee::Float(0.5f);
        display.bufferHeight = bufferHeight;
        // The screen's corner, the screen in the middle of the drawing space,
        // in subpixels.
        const int32_t left = subpixels((kScreenSpacePixels - ee::toInt(screenWidth)) / 2);
        const int32_t top = subpixels((kScreenSpacePixels - ee::toInt(bufferHeight)) / 2);
        display.offsetX = left;
        display.offsetY = top;
        display.cornerX = ee::toFloat(left);
        display.cornerY = ee::toFloat(top);
        const ee::Float aspect = (bufferHeight / screenWidth) * kPixelAspect;
        const uint32_t lines = fields ? height >> 1 : height;
        FrameState &state = *g_frameState;
        display.half = ee::Float(0.5f);
        state.verticalScale = aspect;
        display.guardBand = ee::Float(200.0f);
        display.maxLines = ee::Float(512.0f);
        display.depthAtNear = kDepthAtNear;
        state.horizontalScale = ee::Float(1.0f);
        display.depthScale = ee::Float(1.0f);
        display.aspect = aspect;
        display.depthAtFar = ee::Float(1.0f);
        state.width = static_cast<int32_t>(width);
        state.height = static_cast<int32_t>(height);
        // (DM2's libgraph's sceGsSetDefDBuff, which the bind names so)
        sceGsSetDefDBuffDc(kDisplayBuffersAddress, gs::kPsmCt32, toInt16(static_cast<int32_t>(width)), toInt16(static_cast<int32_t>(lines)),
                           gs::kDepthGreater, gs::kPsmZ16S, 1);
        flushCache(0);
        // Two 32-bit frame buffers and a 16-bit Z buffer; the shadow buffer's
        // 16 bits after them.
        const uint32_t depthLines = lines <= 0xFF ? 0x100u : lines;
        const uint32_t frameWords = width * lines;
        const uint32_t depthWords = width * depthLines;
        const uint32_t buffersBytes = (frameWords << 3) + (depthWords << 1);
        display.shadowBufferWord = buffersBytes >> 2;
        resetTextureCacheAndFlush((buffersBytes + (depthWords << 1)) >> 2);
        for (ee::Ptr<Camera> camera = firstCameraOfKind(0); camera; camera = nextCameraOfKind(camera))
        {
            built->width = g_display->width;
            built->height = g_display->height;
            built->left = ee::Float(0.0f);
            built->top = ee::Float(0.0f);
            viewport = *built;
            setCameraViewport(camera, viewport);
            setCameraScales(camera, g_frameState->horizontalScale, g_frameState->verticalScale);
        }
        setClearColour(g_display->clearColour);
        applyDisplayBuffers();
    }
    READABLE_FUNCTION_WAITS_OWN(resetCameraViewports, Verified, dm2, resetDisplayAndCameraViewports);


    // resetFramePackets: the GS waited for, the frame packets cleared and a
    // texture cache flush requested (the original's tail call). ZGE2 shows
    // the frame in hand first (a swap and a finished frame) and has no
    // packets to clear.
    void resetFramePackets()
    {
        if constexpr (game::readable::kIsDm2)
        {
            waitAndSwapDisplayBuffers(); // (ZGE2's swapDisplayBuffers)
            finishFrame();
        }
        sceGsSyncPath(0, 0);
        sceGsSyncV(0);
        if constexpr (!game::readable::kIsDm2)
            clearFramePackets();
        game::tail(zge::requestTextureCacheFlush); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(resetFramePackets, Verified);

    // resetLevelFrame: the level's frame count back to 0.
    void resetLevelFrame()
    {
        g_display->levelFrame = 0;
    }
    READABLE_FUNCTION(resetLevelFrame, Verified);

    // waitForFrameReady: no frame pending, then a wait until the frame
    // packet's DMA is done (its end interrupt sets framePacketSent).
    void waitForFrameReady()
    {
        namespace here = points::waitForFrameReady;
        DisplaySetup &display = *g_display;
        display.framesPending = 0;
        ee::waitForInterrupt(here::loop, [&] { return display.framePacketSent != 0; });
    }
    READABLE_FUNCTION(waitForFrameReady, Verified);

    // displayNoop (the collision world's set-up end; nothing on the PS2): 1,
    // which DM2's caller reads.
    uint32_t displayNoop()
    {
        return 1;
    }
    READABLE_FUNCTION(displayNoop, Verified);


    // clipSegmentToFrustumPlane: a segment against a plane (its normal, and w the
    // distance along it): how many of its ends are on the plane or past it
    // along the normal, 0 or 2 left as they are; 1 when the segment crosses,
    // the end short of the plane moved onto it. Its first argument is not
    // read (clipSegmentsToPlanes hands it the camera). (Nothing in the game
    // calls it.)
    uint32_t clipSegmentToFrustumPlane(ee::Ptr<Camera> /*not read*/, ee::Ptr<Plane> plane, ee::Ptr<Vec4> start, ee::Ptr<Vec4> end)
    {
        const ee::Float startAlong = dot(*start, *plane);
        const ee::Float planeDistance = plane->w;
        const uint32_t startPast = planeDistance <= startAlong ? 1 : 0;
        const ee::Float endAlong = dot(*end, *plane);
        const uint32_t endPast = planeDistance <= endAlong ? 1 : 0;
        if (startPast == endPast)
            return startPast + endPast;
        // `moved` taken to t of the way from `kept` to it, x, y and z.
        const auto moveOntoPlane = [](Vec4 &moved, const Vec4 &kept, ee::Float t)
        {
            moved.x = kept.x + (moved.x - kept.x) * t;
            moved.y = kept.y + (moved.y - kept.y) * t;
            moved.z = kept.z + (moved.z - kept.z) * t;
        };
        if (startPast)
            moveOntoPlane(*end, *start, (planeDistance - startAlong) / (endAlong - startAlong));
        else
            moveOntoPlane(*start, *end, (planeDistance - endAlong) / (startAlong - endAlong));
        return startPast + endPast;
    }
    READABLE_FUNCTION(clipSegmentToFrustumPlane, Draft);

    // clipSegmentsToPlanes: the segment start .. end cut to the camera's
    // frustum, plane by plane (the world's planes, or with inWorld 0 the
    // view space's, whose top and bottom planes are swapped as compiled:
    // zge/scene/camera.h); 0 as soon as it lies wholly behind one, else 1.
    // (Nothing in the game calls it.)
    uint32_t clipSegmentsToPlanes(ee::Ptr<Camera> camera, uint32_t inWorld, ee::Ptr<Vec4> start, ee::Ptr<Vec4> end)
    {
        if (inWorld)
        {
            for (uint32_t i = 0; i < camera->planeCount; ++i)
                if (clipSegmentToFrustumPlane(camera, &camera->planes[i], start, end) == 0)
                    return 0;
        }
        else
        {
            for (uint32_t i = 0; i < camera->viewPlaneCount; ++i)
                if (clipSegmentToFrustumPlane(camera, &camera->viewPlanes[i], start, end) == 0)
                    return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(clipSegmentsToPlanes, Draft);
}
