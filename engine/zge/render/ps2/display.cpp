// The display's PS2 form (zge/render/display.h lays its state out;
// zge/render/display.cpp is the frame loop and the video mode around it):
// the GS reset and its start-up registers, libgraph's double buffer of display
// and draw environments (libgraph stays the recompiled original: Z-Axis's
// calls of it and the structs it fills, ps2/gs_buffers.h), the vsync
// interrupt that shows a finished frame, the buffers' swap, the screen's
// colour rectangles and the frame's clear colour.
//
// Original quirks kept: resetGraphics and setGsRegister's callers pass
// arguments the callees do not read (initDisplay's 2 and 1); a colour
// rectangle restores both draw buffers' register counts from the first's.

#include "zge/render/display.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/renderer.h"
#include "zge/render/ps2/dma.h"
#include "zge/render/ps2/gs_buffers.h"
#include "zge/render/ps2/libgraph_calls.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/ps2/display.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_rectanglePacketUnset> g_rectanglePacketUnset;   // 1 until the colour rectangle's packet is made

        // The colour rectangle's register packet (rectanglePacketAddress): a GIF tag and
        // the clear sprite's six registers (sendDisplayPacket).
        struct RectanglePacket
        {
            uint64_t gifTag[2];
            GsClear sprite;
        };
        static_assert(sizeof(RectanglePacket) == 0x70);
        // The clear sprite's registers, a quadword each (A+D).
        constexpr uint32_t kSpriteRegisters = sizeof(GsClear) / sizeof(ee::Qword);
        constexpr uint32_t kRectanglePacketAddress = sym::rectanglePacketAddress;
        inline ee::Global<RectanglePacket, kRectanglePacketAddress> g_rectanglePacket;

        constexpr uint32_t kGifChannel = 2;
        constexpr uint32_t kSendWait = 0x100u;

        constexpr uint32_t kSceGsSyncVCallback = sym::sceGsSyncVCallback; // (handler) libgraph's vsync callback set
        constexpr uint32_t kGsResetPacket = sym::gsResetPacket;
        inline ee::Global<ee::Qword, sym::setupGifTag> g_setupGifTag; // the start-up registers' GIF tag
        constexpr uint32_t kSceGsResetGraph = sym::sceGsResetGraph;   // (mode, interlace, video mode, field mode)
        constexpr uint32_t kSceGsResetPath = sym::sceGsResetPath;
        constexpr uint32_t kSceGsSyncPath = sym::sceGsSyncPath;     // (0, 0) -> -1 on a timeout
        constexpr uint32_t kSceGsSetHalfOffset = sym::sceGsSetHalfOffset;  // (draw env 1, centre x, centre y, field)
        constexpr uint32_t kSceGsSetHalfOffset2 = sym::sceGsSetHalfOffset2; // (draw env 2, ...)
        constexpr uint32_t kSceGifPkAddGsAD = sym::sceGifPkAddGsAD;     // (packet, register, value)
        constexpr uint32_t kFlushCache = sym::flushCache;
        constexpr uint32_t kIFlushCache = sym::iFlushCache;
        constexpr uint32_t kEnableInterrupts = sym::EIntr;    // EIntr
        // The register packet's place in the scratchpad: BMX XXX's sits 16 bytes lower.
        constexpr uint32_t kScratchpadPacket = PS2_SCRATCHPAD_BASE + game::readable::perGame(0x20u, 0x20u, 0x10u);
        constexpr uint32_t kFogColourWhite = 0xFFu << 16 | 0xFFu << 8 | 0xFFu;
        constexpr uint32_t kUncachedRegisterPacket = ee::uncached(sym::g_registerPacket);

        // libgraph's ztest is TEST's ZTST (gs::kDepthGreater), or 0: no depth test (ZNOUSE: the test always passes).
        constexpr uint32_t kNoDepthTest = 0;

        // TEXA: TA0 0x7F, AEM, TA1 0x81; ALPHA: (Cs - Cd) * As + Cd.
        constexpr uint64_t kTexaDefault = gs::TexA{.ta0 = 0x7F, .aem = 1, .ta1 = 0x81}.bits();
        constexpr uint64_t kAlphaBlend =
            gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendFrame, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();

        int16_t toInt16(int32_t value) { return static_cast<int16_t>(value); }
        uint32_t drawBufferQwords(const GsDrawBuffer &buffer)
        {
            return static_cast<uint32_t>(static_cast<int32_t>(buffer.gifTag[0] & gs::kGifTagLoops)) + 1u;
        }
        // A GS register value's bytes or halfwords, as the originals write them.
        template <typename T>
        T *partsOf(uint64_t &value) { return reinterpret_cast<T *>(&value); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void sceGsResetPath();
    }

    // waitForGsPath (finishRenderFrame, ZGE2's swap): the GIF path waited for;
    // on a timeout the DMA channels and the path reset.
    void ps2::waitForGsPath()
    {
        if (sceGsSyncPath(0, 0) == -1)
        {
            resetDmaChannels();
            sceGsResetPath();
        }
    }

    // ---- the GS -----------------------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        void EIntr() { ee::game(kEnableInterrupts); }
    }

    // vsyncHandler: the vsync interrupt: the game's global pointer set and the
    // interrupted code's float registers kept; with a finished frame waiting
    // (and no GS reset or frame end under way, its packet's DMA done) the
    // other field and buffer shown and the next frame packet started; the
    // float registers put back. Interrupts on again. 0.
    uint32_t vsyncHandler()
    {
        restoreGlobalPointer();
        DisplaySetup &display = *g_display;
        display.vsyncs = display.vsyncs + 1;
        saveFloatRegisters(); // as the interrupted code left them
        if (display.framesPending > 0 && *g_gsResetPending == 0 && display.finishingFrame == 0 &&
            display.framePacketSent != 0)
        {
            const int32_t field = display.oddField;
            display.uploadsOff = 1;
            display.oddField = field ^ 1;
            swapDisplayBuffers(1);
            sendFramePacket(1);
        }
        restoreFloatRegisters();
        EIntr();
        return 0;
    }
    READABLE_FUNCTION(vsyncHandler, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceGsResetPath() { ee::game(kSceGsResetPath); }
        void sceGsResetGraph(uint32_t value, uint32_t value2, uint32_t videoMode, uint32_t fieldMode) { ee::game(kSceGsResetGraph, value, value2, videoMode, fieldMode); }
        void sceGsSyncVCallback(uint32_t handler) { ee::game(kSceGsSyncVCallback, handler); }
    }

    // installVsyncHandler: vsyncHandler made libgraph's vsync callback. 1.
    int32_t installVsyncHandler()
    {
        sceGsSyncVCallback(sym::vsyncHandler);
        return 1;
    }
    READABLE_FUNCTION(installVsyncHandler, Verified);

    int32_t sendGsResetPacket(ee::Ptr<uint8_t> colour);

    // resetGraphics: the GS reset in the field mode given (interlaced, the
    // display's video mode), its start-up registers sent and the vsync
    // handler installed (the original's tail call); its 1. ZGE4 answers the vsyncs a second it keeps.
    int32_t resetGraphics(uint32_t fieldMode)
    {
        DisplaySetup &display = *g_display;
        display.fieldMode = fieldMode;
        sceGsResetPath();
        sceGsResetGraph(0, 1, toInt16(display.videoMode), toInt16(static_cast<int32_t>(display.fieldMode)));
        sendGsResetPacket(ee::Ptr<uint8_t>(kGsResetPacket));
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 keeps the video mode's vsyncs a second (50 in PAL, else 60) and answers them.
            installVsyncHandler();
            const int32_t perSecond = display.videoMode == kVideoModePal ? 50 : 60;
            keepVsyncsPerSecond(display, perSecond);
            return perSecond;
        }
        return game::tail(zge::installVsyncHandler); // the original's tail call
    }
    READABLE_FUNCTION(resetGraphics, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t flushCache(uint32_t mode) { return ee::tailCall<int32_t>(kFlushCache, mode); }
        }
    }

    // setupDisplayBuffers: libgraph's double buffer set up for the display's size
    // (the height halved for fields), a Z buffer (PSMZ16S, tested when the
    // display asks) and its clear; with flush, the data cache written back
    // (the original's tail call).
    int32_t setupDisplayBuffers(uint32_t flush)
    {
        DisplaySetup &display = *g_display;
        const int32_t width = ee::toInt(display.width);
        int32_t height = ee::toInt(display.height);
        if ((display.flags & kDisplayField) != 0)
            height = height / 2;
        const uint64_t flags = flagsDoubleword(display);
        const int32_t result =
            sceGsSetDefDBuffDc(kDisplayBuffersAddress, gs::kPsmCt32, toInt16(width), toInt16(height), (flags & kDisplayDepthTest) != 0 ? gs::kDepthGreater : kNoDepthTest, gs::kPsmZ16S, static_cast<uint64_t>((flags & kDisplayClear) != 0 ? 1 : 0));
        if (flush != 0)
            return tail::flushCache(0); // the original's tail call
        return result;
    }
    READABLE_FUNCTION(setupDisplayBuffers, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceGifPkAddGsAD(ee::Ptr<sce::GifPacket> packet, uint32_t addr, uint64_t data) { ee::game(kSceGifPkAddGsAD, packet, addr, data); }
        void startGifPacket(ee::Ptr<sce::GifPacket> packet, uint32_t buffer) { ee::game(sym::gifPacketInit, packet, buffer); }
        void emptyGifPacket(ee::Ptr<sce::GifPacket> packet) { ee::game(sym::gifPacketReset, packet); }
        void finishGifPacket(ee::Ptr<sce::GifPacket> packet) { ee::game(sym::gifPacketTerminate, packet); }
        void addGifDword(ee::Ptr<sce::GifPacket> packet, uint64_t value) { ee::game(sym::gifPacketAddDword, packet, value); }
        // A finished GIF packet sent, waited for.
        uint32_t sendGifPacket(ee::Ptr<sce::GifPacket> packet)
        {
            const uint32_t qwords = sce::gifPacketSize(packet);
            return dmaSend(kGifChannel, packet->start, qwords, kSendWait, 0);
        }
    }

    namespace
    {
        // The reset packet's tag: nine times one address and data pair, the packet's end.
        constexpr uint64_t kResetGifTag = gs::GifTag{.loops = 9, .endOfPacket = 1, .registers = 1}.bits();
        constexpr uint32_t kResetPixels = 1024;
        constexpr uint64_t kFrameWidth1024 = gs::Frame::kFbw.placed(gs::bufferWidthUnits(kResetPixels)); // the buffer at 0
        constexpr uint64_t kScissor1024 = gs::Scissor{.right = kResetPixels, .bottom = kResetPixels}.bits(); // 0 to 1024 both ways
        constexpr uint32_t kQOne = std::bit_cast<uint32_t>(1.0f); // RGBAQ's Q
        constexpr uint64_t kFarCorner = gs::Xyzf{.x = subpixels(kResetPixels), .y = subpixels(kResetPixels)}.bits();
        constexpr uint32_t kResetPacketWords = 0x110u / sizeof(uint32_t);

        void startResetPacket(ee::Ptr<sce::GifPacket> packet, ee::Address buffer) { ee::game(sym::gifPacketInit, packet, buffer); }
        void writeBackDataCache() { ee::game(kFlushCache, 0); }
    }

    // sendGsResetPacket: the frame buffer's first 1024 by 1024 pixels filled with one colour, a
    // sprite on the GIF channel, ahead of the display being set up. colour is its four bytes, red
    // first. The engine's first function after the start-up code, the same in DM2 and BMX XXX.
    int32_t sendGsResetPacket(ee::Ptr<uint8_t> colour)
    {
        ee::Local<sce::GifPacket> packet;
        ee::Local<uint32_t[kResetPacketWords]> buffer;
        startResetPacket(packet, buffer);
        emptyGifPacket(packet);
        const auto setRegister = [&packet](uint64_t value, uint64_t gsRegister) {
            addGifDword(packet, value);
            addGifDword(packet, gsRegister);
        };
        setRegister(kResetGifTag, gs::kAddressData);
        setRegister(gs::kAttributesFromPrim, gs::kPrModeCont);
        setRegister(kFrameWidth1024, gs::kFrame1);
        setRegister(0u, gs::kXyOffset1);
        setRegister(kScissor1024, gs::kScissor1);
        setRegister(0u, gs::kTest1);
        setRegister(gs::kSprite, gs::kPrim);
        setRegister(gs::Rgbaq{.r = colour[0], .g = colour[1], .b = colour[2], .a = colour[3], .q = kQOne}.bits(), gs::kRgbaq);
        setRegister(0u, gs::kXyzf2);
        setRegister(kFarCorner, gs::kXyzf2);
        finishGifPacket(packet);
        writeBackDataCache();
        return static_cast<int32_t>(sendGifPacket(packet));
    }
    READABLE_FUNCTION(sendGsResetPacket, Verified);

    // setupDisplayRegisters: the GS's drawing defaults sent (a GIF packet in the
    // scratchpad): no texture clamping, TEXA, a white fog colour, a texture
    // flush, the usual alpha blend, colour clamping, PABE and FBA_1 off.
    void setupDisplayRegisters()
    {
        ee::Local<sce::GifPacket> packet;
        ee::Local<uint64_t> setupGifTagSlot;
        ee::Local<uint64_t> setupGifTagRegistersSlot;
        *setupGifTagSlot = g_setupGifTag->low;
        *setupGifTagRegistersSlot = g_setupGifTag->high;
        startGifPacket(packet, kScratchpadPacket);
        emptyGifPacket(packet);
        sceGifPkOpenGifTag(packet, ee::Qword{*setupGifTagSlot, *setupGifTagRegistersSlot});
        sceGifPkAddGsAD(packet, gs::kClamp1, 0);
        sceGifPkAddGsAD(packet, gs::kClamp2, 0);
        sceGifPkAddGsAD(packet, gs::kTexA, kTexaDefault);
        sceGifPkAddGsAD(packet, gs::kFogCol, kFogColourWhite);
        sceGifPkAddGsAD(packet, gs::kTexFlush, 1);
        sceGifPkAddGsAD(packet, gs::kAlpha1, kAlphaBlend);
        sceGifPkAddGsAD(packet, gs::kAlpha2, kAlphaBlend);
        sceGifPkAddGsAD(packet, gs::kColClamp, gs::kColoursClamped);
        sceGifPkAddGsAD(packet, gs::kPabe, 0);
        sceGifPkAddGsAD(packet, gs::kFba1, 0);
        sceGifPkCloseGifTag(packet);
        finishGifPacket(packet);
        sendGifPacket(packet);
    }
    READABLE_FUNCTION(setupDisplayRegisters, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceGsSetHalfOffset(uint32_t draw, uint32_t centreX, uint32_t centreY, uint32_t oddField) { ee::game(kSceGsSetHalfOffset, draw, centreX, centreY, oddField); }
        void sceGsSetHalfOffset2(uint32_t draw, uint32_t centreX, uint32_t centreY, uint32_t oddField) { ee::game(kSceGsSetHalfOffset2, draw, centreX, centreY, oddField); }
        void iFlushCache(uint32_t value) { ee::game(kIFlushCache, value); }
        void flushCache(uint32_t mode, uint64_t cutTag1, uint32_t value) { ee::game(kFlushCache, mode, cutTag1, value); }
        int32_t sceGsSyncPath(uint32_t value, uint32_t value2, uint32_t rectanglePacketAddress, uint32_t displayAddress) { return ee::game<int32_t>(kSceGsSyncPath, value, value2, rectanglePacketAddress, displayAddress); }
    }

    // swapDisplayBuffers: the draw buffer about to be shown given the field's half
    // offset, the data cache written back (iFlushCache in the interrupt) and
    // libgraph's buffers swapped (outside the interrupt: then waited for);
    // the other half drawn to next.
    void swapDisplayBuffers(uint32_t inInterrupt)
    {
        DisplaySetup &display = *g_display;
        const auto halfOffset = [&](uint32_t buffer) {
            const uint32_t draw1 = buffer + offsetof(GsDrawBuffer, draw1);
            // The second draw environment, right after the first (DM2's
            // libgraph is the earlier sceGsDBuff: one draw environment).
            const uint32_t draw2 = draw1 + sizeof(GsDrawEnvironment1);
            sceGsSetHalfOffset(draw1, toInt16(ee::toInt(display.centreX)), toInt16(ee::toInt(display.centreY)), toInt16(display.oddField));
            if constexpr (!game::readable::kIsDm2)
                sceGsSetHalfOffset2(draw2, toInt16(ee::toInt(display.centreX)), toInt16(ee::toInt(display.centreY)), toInt16(display.oddField));
        };
        if (display.drawBuffer != 0)
            halfOffset(kDrawBuffer1Address);
        else
            halfOffset(kDrawBuffer0Address);
        if (inInterrupt != 0)
        {
            iFlushCache(0);
            sceGsSwapDBuffDc(kDisplayBuffersAddress, display.drawBuffer);
        }
        else
        {
            flushCache(0);
            sceGsSwapDBuffDc(kDisplayBuffersAddress, display.drawBuffer);
            sceGsSyncPath(0, 0);
        }
        display.drawBuffer ^= 1u;
    }
    // waitAndSwapDisplayBuffers: ZGE2's swapDisplayBuffers, which takes no word (ZGE2 swaps outside the vsync
    // interrupt only) and waits for the GS itself, as ZGE3's finishRenderFrame does before its swap: the GIF path
    // waited for (DMA and the path reset on a timeout), the next vsync's field kept, the draw buffer about to be shown
    // given that field's half offset, the data cache written back and libgraph's buffers swapped; the other half drawn
    // to next.
    void waitAndSwapDisplayBuffers()
    {
        DisplaySetup &display = *g_display;
        ps2::waitForGsPath();
        display.oddField = sceGsSyncV(0) ^ 1;
        const uint32_t buffer = display.drawBuffer != 0 ? kDrawBuffer1Address : kDrawBuffer0Address;
        sceGsSetHalfOffset(buffer + offsetof(GsDrawBuffer, draw1), toInt16(ee::toInt(display.centreX)), toInt16(ee::toInt(display.centreY)),
                           toInt16(display.oddField));
        flushCache(0);
        sceGsSwapDBuffDc(kDisplayBuffersAddress, display.drawBuffer);
        display.drawBuffer ^= 1u;
    }
    READABLE_FUNCTION_OWN(swapDisplayBuffers, Verified, dm2, waitAndSwapDisplayBuffers);

    // initDisplay: the display's start: the first draw buffer, NTSC,
    // field rendering, no vsyncs yet; the GS reset (field mode) and its
    // drawing defaults.
    void initDisplay()
    {
        DisplaySetup &display = *g_display;
        uint64_t &flags = flagsDoubleword(display);
        const uint64_t was = flags;
        display.drawBuffer = 0;
        display.videoMode = kVideoModeNtsc;
        startFrameParities(display);
        flags = was | kDisplayField;
        display.vsyncs = 0;
        display.lastFrameVsync = 0;
        resetGraphics(1);
        setupDisplayRegisters();
    }

    // initFieldDisplay: ZGE2's initDisplay: the GS reset there and then (NTSC, field rendering: ZGE2 has no
    // resetGraphics and installs no vsync handler here), its start-up registers sent from a copy in the frame, the
    // drawing defaults, then the display's flags set to field rendering alone and the first draw buffer taken. ZGE2
    // keeps no video mode, parities or vsync counts in the display.
    void initFieldDisplay()
    {
        DisplaySetup &display = *g_display;
        ee::Local<uint32_t> packet;
        std::memcpy(&*packet, &*ee::Ptr<uint8_t>(kGsResetPacket), sizeof(uint32_t));
        sceGsResetPath();
        sceGsResetGraph(0, 1, toInt16(kVideoModeNtsc), 1);
        sendGsResetPacket(ee::Ptr<uint8_t>(packet.address()));
        setupDisplayRegisters();
        display.flags = kDisplayField;
        display.drawBuffer = 0;
    }
    READABLE_FUNCTION_OWN(initDisplay, Verified, dm2, initFieldDisplay);


    // ---- the screen ------------------------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void sceGsSyncPath(uint32_t value, uint32_t value2) { ee::tailCall(kSceGsSyncPath, value, value2); }
        }
    }

    // clearDisplayRectangle: a rectangle of the screen (x, y, width, height in
    // pixels; colour: R, G, B, A) filled with a colour, by the clear sprite:
    // its six registers copied once into a packet of their own (rectanglePacketAddress),
    // its colour and corners set, the draw buffer's first registers sent
    // (its GIF tag cut to 8 for that), then the sprite.
    void clearDisplayRectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height, ee::Ptr<uint8_t> colour)
    {
        DisplaySetup &display = *g_display;
        RectanglePacket &packet = *g_rectanglePacket;
        const ee::Ptr<uint8_t> rgba = colour;
        if (*g_rectanglePacketUnset != 0)
        {
            *g_rectanglePacketUnset = 0;
            memoryCopy(kRectanglePacketAddress + offsetof(RectanglePacket, sprite), kDrawBuffer0Address + offsetof(GsDrawBuffer, clear), static_cast<uint32_t>(sizeof(GsClear)));
            // Its GIF tag: six times one register (A+D), EOP, no PRIM, PACKED.
            const uint64_t registers = (packet.gifTag[1] & ~gs::kFirstRegister) | gs::kAddressData;
            uint64_t tag = (packet.gifTag[0] & ~gs::kGifTagLoops) | gs::GifTag{.loops = kSpriteRegisters, .endOfPacket = 1}.bits();
            tag &= ~gs::kGifTagSetsPrim & ~gs::kGifTagFormat & ~gs::kGifTagRegisters;
            packet.gifTag[0] = tag | gs::GifTag{.registers = 1}.bits();
            packet.gifTag[1] = registers;
        }
        uint8_t *const spriteColour = partsOf<uint8_t>(packet.sprite.rgbaq.value);
        spriteColour[0] = rgba[0];
        const uint16_t left = static_cast<uint16_t>(display.offsetX);
        const uint32_t flags = display.flags;
        spriteColour[1] = rgba[1];
        spriteColour[2] = rgba[2];
        uint16_t *const corner0 = partsOf<uint16_t>(packet.sprite.xyz2a.value);
        uint16_t *const corner1 = partsOf<uint16_t>(packet.sprite.xyz2b.value);
        corner0[0] = static_cast<uint16_t>(left + subpixels(x));
        spriteColour[3] = rgba[3];
        // In field rendering a pixel row is half a GS row (always, in ZGE2's: it reads no flags here).
        uint32_t rows = 0;
        if (game::readable::kIsDm2 || (flags & kDisplayField) != 0)
            rows = fieldRowSubpixels(y);
        else
            rows = subpixels(y);
        corner0[1] = static_cast<uint16_t>(static_cast<uint16_t>(display.offsetY) + rows);
        corner1[0] = static_cast<uint16_t>(static_cast<uint16_t>(display.offsetX) + subpixels(x + width));
        if (game::readable::kIsDm2 || (display.flags & kDisplayField) != 0)
            rows = fieldRowSubpixels(y + height);
        else
            rows = subpixels(y + height);
        corner1[1] = static_cast<uint16_t>(static_cast<uint16_t>(display.offsetY) + rows);
        if constexpr (game::readable::kIsDm2) // (ZGE3's call leaves the packet's and the display's addresses in its words)
            sceGsSyncPath(0, 0);
        else
            sceGsSyncPath(0, 0, kRectanglePacketAddress, kDisplayAddress);
        // Both draw buffers' packets cut to their draw environment's
        // registers (the first one's count kept), sent with their GIF tag.
        constexpr uint32_t kDrawEnvironmentRegisters = sizeof(GsDrawEnvironment1) / sizeof(ee::Qword);
        uint64_t &tag0 = display.buffers.draw[0].gifTag[0];
        uint64_t &tag1 = display.buffers.draw[1].gifTag[0];
        const uint64_t firstTag = tag0;
        const uint64_t cutTag1 = (tag1 & ~gs::kGifTagLoops) | gs::GifTag{.loops = kDrawEnvironmentRegisters}.bits();
        const uint64_t count = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(firstTag & gs::kGifTagLoops)));
        tag0 = (firstTag & ~gs::kGifTagLoops) | gs::GifTag{.loops = kDrawEnvironmentRegisters}.bits();
        tag1 = cutTag1;
        flushCache(0, cutTag1, kDrawEnvironmentRegisters);
        if (display.drawBuffer == 0)
            dmaSend(kGifChannel, kDrawBuffer0Address, kDrawEnvironmentRegisters + 1, kSendWait, 0);
        else
            dmaSend(kGifChannel, kDrawBuffer1Address, kDrawEnvironmentRegisters + 1, kSendWait, 0);
        sceGsSyncPath(0, 0);
        const uint64_t restored1 = (tag1 & ~gs::kGifTagLoops) | count;
        tag0 = (tag0 & ~gs::kGifTagLoops) | count;
        tag1 = restored1;
        dmaSend(kGifChannel, kRectanglePacketAddress, kSpriteRegisters + 1, kSendWait, 0); // its GIF tag, the sprite
        tail::sceGsSyncPath(0, 0); // the original's tail call
    }
    READABLE_FUNCTION(clearDisplayRectangle, Verified);

    // applyDisplayBuffers: unless a GS reset is pending, the GIF path and a vsync
    // waited for, the second buffer made current and both draw buffers'
    // registers sent (then waited for: the original's tail call).
    void applyDisplayBuffers()
    {
        if constexpr (!game::readable::kIsDm2) // (ZGE2 does not look for a pending GS reset)
            if (*g_gsResetPending != 0)
                return;
        DisplaySetup &display = *g_display;
        sceGsSyncPath(0, 0);
        sceGsSyncV(0);
        display.drawBuffer = 1;
        sceGsSwapDBuffDc(kDisplayBuffersAddress, 1);
        dmaSend(kGifChannel, kDrawBuffer0Address, drawBufferQwords(display.buffers.draw[0]), kSendWait, 0);
        dmaSend(kGifChannel, kDrawBuffer1Address, drawBufferQwords(display.buffers.draw[1]), kSendWait, 0);
        tail::sceGsSyncPath(0, 0); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(applyDisplayBuffers, Verified);


    // setGsRegister (no caller): one GS register set: a GIF packet of one
    // A+D pair (uncached, uncachedRegisterPacket) sent and waited for.
    uint32_t setGsRegister(uint32_t gsRegister, uint32_t value)
    {
        ee::Local<sce::GifPacket> packet;
        startGifPacket(packet, kUncachedRegisterPacket);
        emptyGifPacket(packet);
        addGifDword(packet, gs::GifTag{.loops = 1, .endOfPacket = 1, .registers = 1}.bits());
        addGifDword(packet, gs::kAddressData);
        addGifDword(packet, static_cast<uint64_t>(value));
        addGifDword(packet, static_cast<uint64_t>(gsRegister));
        finishGifPacket(packet);
        return sendGifPacket(packet);
    }
    READABLE_FUNCTION(setGsRegister, Draft);


    // setClearColour: the frame's clear colour (R, G, B, A at colour) kept and
    // given to both draw buffers' clear sprites (RGBAQ: Q the near depth's
    // bits).
    void setClearColour(ee::Ptr<uint8_t> colour)
    {
        DisplaySetup &display = *g_display;
        const uint32_t q = display.depthAtNear.bits();
        const ee::Ptr<uint8_t> source = colour; // any alignment
        std::memcpy(display.clearColour, source.get(), sizeof(display.clearColour));
        const uint64_t rgbaq = gs::Rgbaq{.r = display.clearColour[0], .g = display.clearColour[1], .b = display.clearColour[2],
                                         .a = display.clearColour[3], .q = q}
                                   .bits();
        display.buffers.draw[1].clear.rgbaq.value = rgbaq;
        display.buffers.draw[0].clear.rgbaq.value = rgbaq;
    }
    READABLE_FUNCTION(setClearColour, Verified);



    // drawEnvironment0 (ZGE4 only): the display's first draw environment (g_drawEnvironment0), whose FRAME_1 and
    // XYOFFSET_1 drawBlur reads (ZGE3 keeps copies in the scratchpad).
    inline ee::Ptr<GsDrawEnvironment> drawEnvironment0()
    {
        return ee::Ptr<GsDrawEnvironment>(sym::g_drawEnvironment0);
    }
    READABLE_FUNCTION_ONLY(bmx, drawEnvironment0);

}
