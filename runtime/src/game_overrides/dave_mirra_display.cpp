// Dave Mirra Freestyle BMX 2 (SLUS-20159): the display environment.
//
// DM2 runs libgraph's own double-buffer code (sceGsSetDefDBuff,
// sceGsSwapDBuff, sceGsPutDrawEnv ...; recomp/tools/make-config.py in the
// tooling/dm2), so the frame clear it asks for is sent. Its
// sceGsPutDispEnv (0x1CDE60) writes PMODE, SMODE2, DISPFB2, DISPLAY2 and
// BGCOLOR as on hardware: in interlaced field mode (SMODE2 INT=1, FFMD=1)
// DISPLAY.DH counts the lines of both fields (447 for a 224-line buffer).
// The runtime's presenters read DH+1 lines of the buffer and expect the
// HLE's DISPLAY layout (the runtime's HLE sceGsSetDefDBuffDc, which
// Aggressive Inline uses, gives DX 636, DY 32, MAGH 0, DW 639, DH 223), so
// here the same registers are written in that layout (DH halved in field
// mode, the width from DW and MAGH), to both circuits as the HLE does. With
// DISPLAY as libgraph writes it the GPU renderer showed noise over the
// picture.
#include "dave_mirra.h"
#include "ps2_runtime.h"

#include <cstring>
#include <iostream>

namespace dave_mirra::display
{
    constexpr uint32_t kPutDispEnv = 0x001CDE60u;
    constexpr int kRegA0 = 4, kRegRa = 31;

    void putDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint64_t env[5] = {}; // PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR
        const uint8_t *p = getConstMemPtr(rdram, getRegU32(ctx, kRegA0));
        ctx->pc = getRegU32(ctx, kRegRa);
        if (!p || !runtime->syncCoreSubsystems())
            return;
        std::memcpy(env, p, sizeof(env));
        // DISPLAY as the runtime's HLE libgraph writes it (the presenters
        // are tuned to that): width in pixels with MAGH 0, the HLE's
        // DX/DY, DH in buffer lines.
        const uint64_t raw = env[3];
        const uint64_t magh = (raw >> 23) & 0xFu;
        const uint64_t dw = (raw >> 32) & 0xFFFu;
        uint64_t dh = (raw >> 44) & 0x7FFu;
        const uint64_t width = (dw + 1u) / (magh + 1u);
        if ((env[1] & 3u) == 3u) // interlaced field mode: DH counts both fields
            dh = (dh + 1u) / 2u - 1u;
        const uint64_t display = 636ull | (32ull << 12) | ((width - 1u) << 32) | (dh << 44);
        PS2Memory &memory = runtime->memory();
        auto &regs = memory.gs();
        regs.pmode = env[0];
        regs.smode2 = env[1];
        memory.setDisplayBuffer(regs.dispfb1, env[2]);
        regs.display1 = display;
        memory.setDisplayBuffer(regs.dispfb2, env[2]);
        regs.display2 = display;
        regs.bgcolor = env[4];
    }
}

namespace dave_mirra
{
    bool installDisplay(PS2Runtime &runtime)
    {
        if (!runtime.replaceFunction(display::kPutDispEnv, &display::putDispEnv))
        {
            std::cerr << "[dave-mirra] display hook not installed" << std::endl;
            return false;
        }
        return true;
    }
}
