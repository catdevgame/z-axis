#pragma once

// The EE kernel's parameter records (Sony's eekernel.h, by its field names):
// what game code fills in for CreateSema and CreateThread; and the SIF RPC
// client's record (sifrpc.h).

#include <cstddef>
#include <cstdint>

namespace sce
{
    struct SemaParam
    {
        int32_t currentCount;
        int32_t maxCount;
        int32_t initCount;
        int32_t numWaitThreads;
        uint32_t attr;
        uint32_t option;
    };
    static_assert(offsetof(SemaParam, maxCount) == 0x4 && offsetof(SemaParam, initCount) == 0x8 && sizeof(SemaParam) == 0x18);

    struct ThreadParam
    {
        int32_t status;
        uint32_t entry;       // the thread's function
        uint32_t stack;
        int32_t stackSize;
        uint32_t gpReg;       // the global pointer it runs with
        int32_t initPriority;
        int32_t currentPriority;
        uint32_t attr;
        uint32_t option;
    };
    static_assert(offsetof(ThreadParam, entry) == 0x4 && offsetof(ThreadParam, gpReg) == 0x10 &&
                  offsetof(ThreadParam, initPriority) == 0x14 && offsetof(ThreadParam, option) == 0x20);

    struct sceSifRpcData
    {
        uint32_t paddr;
        uint32_t pid;
        int32_t tid;
        uint32_t mode;
    };

    struct sceSifClientData
    {
        sceSifRpcData rpcd;
        uint32_t command;
        uint32_t buff;
        uint32_t gp_buff;
        uint32_t func;
        uint32_t para;
        uint32_t serve;     // the server, 0 until the IOP has bound it
    };
    static_assert(offsetof(sceSifClientData, serve) == 0x24);
}
