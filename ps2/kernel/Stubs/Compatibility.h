#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void calloc_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ret0(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ret1(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void reta0(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void free_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void realloc_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memalign_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void malloc_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void malloc_extend_top(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void malloc_trim_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mbtowc_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void printf_r(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __malloc_lock(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __malloc_unlock(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
