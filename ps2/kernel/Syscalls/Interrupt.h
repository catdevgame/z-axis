#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Machine *runtime, uint32_t cause);
    uint64_t GetCurrentVSyncTick(PS2Machine *runtime);
    void WaitVSyncTick(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, int fixedResult);
    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
