#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
