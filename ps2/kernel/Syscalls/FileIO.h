#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    void fioOpen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioClose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioRead(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioWrite(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioLseek(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioMkdir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioChdir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioRmdir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioGetstat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fioRemove(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
