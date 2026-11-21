#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void scePrintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
