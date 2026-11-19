#pragma once

// libgraph's structures as Z-Axis's display keeps and fills them
// (zge/render/display.h's DisplaySetup, ps2/display.cpp): a register value
// and its GS register's address, the display and draw environments, the
// clear sprite and the double buffer of both. The layouts are those of
// libgraph.h of Sony's runtime library release 2.4 (PsIIlibgraph 2400: AI's
// and BMX XXX's); DM2's is release 2.2 (2200), the earlier sceGsDBuff with
// one draw environment (GsDrawBuffer below). libgraph itself is not ported:
// its functions run as the recompiled originals (sce/not_ported.tsv).

#include "ee/ee.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // libgraph's structures as the display keeps them (Runtime Library
    // 2.x, libgraph.h): a register value and its GS register's address.
    struct GsRegisterPair
    {
        uint64_t value;
        uint64_t address;
    };
    static_assert(sizeof(GsRegisterPair) == 0x10);

    // sceGsDispEnv: the read circuit's registers.
    struct GsDisplayEnvironment
    {
        uint64_t pmode, smode2, dispfb, display, bgcolor;
    };
    static_assert(sizeof(GsDisplayEnvironment) == 0x28);

    // sceGsDrawEnv1 (the context 1 drawing registers), sceGsDrawEnv2 (the
    // shared ones) and sceGsClear (the sprite that clears the frame).
    struct GsDrawEnvironment1
    {
        GsRegisterPair frame, zbuf, xyoffset, scissor, prmodecont, colclamp, dthe, test;
    };
    struct GsDrawEnvironment2
    {
        GsRegisterPair frame, zbuf, xyoffset, scissor, prmodecont, colclamp, dthe, test;
    };
    struct GsClear
    {
        GsRegisterPair testa, prim, rgbaq, xyz2a, xyz2b, testb;
    };
    static_assert(sizeof(GsDrawEnvironment1) == 0x80 && sizeof(GsClear) == 0x60);

    // A DMA-sendable register packet: its GIF tag (NLOOP in the low 15
    // bits), then the A+D pairs.
    struct GsDrawBuffer
    {
        uint64_t gifTag[2];          // +0x00
        GsDrawEnvironment1 draw1;    // +0x10
#if !PS2X_GAME_DM2 // (DM2's libgraph is the earlier sceGsDBuff: one draw environment)
        GsDrawEnvironment2 draw2;    // +0x90
#endif
        GsClear clear;               // +0x110 (DM2: +0x90)
    };
    static_assert(sizeof(GsDrawBuffer) == game::readable::perGame(0x170u, 0xF0u));

    // sceGsDBuffDc: two display environments and two draw buffers.
    struct GsDoubleBuffer
    {
        GsDisplayEnvironment display[2]; // +0x000
        GsDrawBuffer draw[2];            // +0x050
    };
    static_assert(sizeof(GsDoubleBuffer) == game::readable::perGame(0x330u, 0x230u));

    // The display's draw environment (sceGsDrawEnv1: register values and
    // their GS addresses) as the fix-ups read it.
    struct GsDrawEnvironment
    {
        uint64_t frame, frameAddress;       // +0x00 FRAME_1 (FBP in bits 0-8)
        uint64_t zbuf, zbufAddress;         // +0x10 ZBUF_1
        uint64_t xyOffset, xyOffsetAddress; // +0x20 XYOFFSET_1
        uint64_t scissor, scissorAddress;   // +0x30 SCISSOR_1
    };
    static_assert(offsetof(GsDrawEnvironment, xyOffset) == 0x20 && offsetof(GsDrawEnvironment, scissor) == 0x30);
    // (ZGE4 only; ps2/display.cpp) the display's first draw environment.
    ee::Ptr<GsDrawEnvironment> drawEnvironment0();
}
