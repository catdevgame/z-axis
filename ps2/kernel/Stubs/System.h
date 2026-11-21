#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void builtin_set_imask(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceIDC(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSDC(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void exit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void getpid(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSetBrokenLink(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSetPtm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDevVif0Reset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDevVu0Reset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
