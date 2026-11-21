#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void DmaAddr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaCallback(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaDebug(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaGetChan(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaGetEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaLastSyncTime(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaPause(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaPutEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaPutStallAddr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaRecv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaRecvI(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaRecvN(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaRestart(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSend(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSendI(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSendM(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSendN(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSync(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaSyncN(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceDmaWatch(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
