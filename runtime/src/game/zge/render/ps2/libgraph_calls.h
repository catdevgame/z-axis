#pragma once

// The calls of Sony's libgraph (and the kernel's cache write-back) both sides
// of the display make (zge/render/display.cpp's frame loop and video mode,
// ps2/display.cpp's GS): the recompiled originals, each one typed line over
// ee::game (docs/READABLE.md). Only those two files include it.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/zge/render/ps2/libgraph_calls.h.h"

namespace zge
{
    // sceGsSyncV: the next vsync waited for (0); the field it starts.
    inline int32_t sceGsSyncV(uint32_t value) { return ee::game<int32_t>(sym::sceGsSyncV, value); }
    // sceGsSyncPath: the GS paths waited for (0, 0); -1 on a timeout.
    inline int32_t sceGsSyncPath(uint32_t value, uint32_t value2) { return ee::game<int32_t>(sym::sceGsSyncPath, value, value2); }
    // sceGsSwapDBuffDc: the double buffer's half `which` made the one shown.
    inline void sceGsSwapDBuffDc(uint32_t buffers, uint32_t which) { ee::game(sym::sceGsSwapDBuffDc, buffers, which); }
    // sceGsSetDefDBuffDc: the double buffer set up (buffers, psm, width, height, ztest, zpsm, clear).
    inline int32_t sceGsSetDefDBuffDc(uint32_t buffers, uint32_t psm, uint32_t width, uint32_t height, uint32_t ztest, uint32_t zpsm, uint64_t clear)
    {
        return ee::game<int32_t>(sym::sceGsSetDefDBuffDc, buffers, psm, width, height, ztest, zpsm, clear);
    }
    // FlushCache: the data cache written back (0).
    inline void flushCache(uint32_t mode) { ee::game(sym::flushCache, mode); }
}
