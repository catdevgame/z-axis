#pragma once

#include "syscalls.h"

namespace ps2_syscalls
{
    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);

    // IOP -> EE RPC (an IOP module calling a server the EE registered).
    bool lookupEeRpcServer(uint32_t sid, uint32_t &serverData);
    // Copies `send` into the EE server buffer and queues the server function
    // on the EE scheduler. On completion, receiveSize bytes from the pointer the
    // server returned are handed to PS2Machine::completeIopEeRpc(token, ...).
    bool startIopToEeRpc(uint8_t *rdram, PS2Machine *runtime, uint64_t token, uint32_t sid,
                         uint32_t function, const void *send, size_t sendSize, uint32_t receiveSize);
}
