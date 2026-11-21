#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void sceFsDbChk(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsIntrSigSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsSemExit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsSemInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsSigSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void close(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fstat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void lseek(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void open(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void read(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceClose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceFsReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceIoctl(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceLseek(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceOpen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceRead(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceWrite(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void stat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void write(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void cvFsSetDefDev(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
