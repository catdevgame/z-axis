#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    void FlushCache(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void EnableCache(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DisableCache(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ResetEE(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetMemoryMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void InitThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
