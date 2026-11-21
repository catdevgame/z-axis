#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    void CreateSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iDeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SignalSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iSignalSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void WaitSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void PollSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iPollSema(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ReferSemaStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iReferSemaStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void CreateEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void DeleteEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iSetEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ClearEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iClearEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void WaitEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void PollEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iPollEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ReferEventFlagStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iReferEventFlagStatus(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void InitAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SetAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iSetAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void CancelAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iCancelAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ReleaseAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void iReleaseAlarm(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
