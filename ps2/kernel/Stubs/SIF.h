#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    bool dispatchSifCommand(uint8_t *rdram, PS2Machine *runtime, uint32_t commandId, const void *packet, size_t packetSize) noexcept;
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void resetSifState();
    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
