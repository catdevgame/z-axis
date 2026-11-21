#pragma once

#include "machine.h"
#include "ps2x/iop/iop_subsystem.h"

#include <utility>

class PS2IopTransport
{
public:
    [[nodiscard]] static ps2x::iop::RpcAbi selectRpcAbi(
        const PS2Machine *runtime,
        const ps2x::iop::RpcAbiRequest &request)
    {
        return runtime
                   ? runtime->selectIopRpcAbi(request)
                   : ps2x::iop::RpcAbi::RuntimeDefault;
    }

    [[nodiscard]] static ps2x::iop::RpcResult handleRpc(
        PS2Machine *runtime,
        uint8_t *rdram,
        R5900Context *context,
        ps2x::iop::RpcRequest request)
    {
        return runtime
                   ? runtime->handleIopRpc(rdram, context, std::move(request))
                   : ps2x::iop::RpcResult{};
    }

    [[nodiscard]] static bool canBindRpc(const PS2Machine *runtime, uint32_t sid)
    {
        return !runtime || runtime->canBindIopRpc(sid);
    }

    [[nodiscard]] static bool hasModuleRpcServer(const PS2Machine *runtime, uint32_t sid)
    {
        return runtime && runtime->hasIopModuleRpcServer(sid);
    }

    static void notifyTransfer(
        PS2Machine *runtime,
        uint8_t *rdram,
        const ps2x::iop::SifTransfer &transfer)
    {
        if (runtime)
        {
            runtime->notifyIopSifTransfer(rdram, transfer);
        }
    }

    static void reset(PS2Machine *runtime)
    {
        if (runtime)
        {
            runtime->resetIop();
        }
    }
};
