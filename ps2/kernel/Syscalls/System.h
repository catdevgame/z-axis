#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    bool dispatchSyscallOverride(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GsSetCrt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetGsCrt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GsGetIMR(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iGsGetIMR(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GsPutIMR(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iGsPutIMR(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GsSetVideoMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetOsdConfigParam(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetOsdConfigParam(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetOsdConfigParam2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetOsdConfigParam2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t encodedSyscallId);
    void initializeGuestKernelState(uint8_t *rdram, PS2Machine *runtime);
    void SetSyscall(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetupThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetupHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EndOfHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetMemorySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void InitTLB(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void FindAddress(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void Deci2Call(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void QueryBootMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetThreadTLS(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void Copy(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetEntryAddress(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void RegisterExitHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
