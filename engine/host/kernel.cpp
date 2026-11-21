// The kernel as a host form uses it (kernel.h).

#include "kernel.h"
#include "ee/host_form.h"

#include "machine.h"
#include "syscalls.h"
#include "runtime/ee_scheduler.h"

#include <cstdint>

namespace game::host
{
    namespace
    {
        constexpr int kRegV0 = 2, kRegA0 = 4, kRegSp = 29, kRegRa = 31;
    }

    void waitAsForReply()
    {
        afterReturn([](const Call &call)
        {
            EeScheduler &ee = call.runtime->eeScheduler();
            if (!ee.canAwaitRpcReply())
                return;
            const uint32_t result = getRegU32(call.ctx, kRegV0);
            ee.awaitRpcReply([result](R5900Context &resumed) { setReturnS32(&resumed, static_cast<int32_t>(result)); });
        });
    }

    void letReadyThreadRun()
    {
        afterReturn([](const Call &call) { call.runtime->eeScheduler().transferIfRequested(false); });
    }

    void sleepInto(uint32_t function)
    {
        afterReturn([function](const Call &call)
        {
            call.ctx->pc = function;
            ps2_syscalls::SleepThread(call.rdram, call.ctx, call.runtime);
        });
    }

    int createSemaphore(int count, int maxCount)
    {
        return currentCall().runtime->eeScheduler().createSemaphore(count, maxCount, 0u, 0u);
    }

    int createThread(uint32_t entry, uint32_t stack, uint32_t stackBytes, uint32_t globalPointer, int priority)
    {
        EeThreadCreateParams params{};
        params.entry = entry;
        params.stack = stack;
        params.stackSize = stackBytes;
        params.gp = globalPointer;
        params.priority = priority;
        return currentCall().runtime->eeScheduler().createThread(params);
    }

    void startThread(int thread)
    {
        if (thread > 0)
            currentCall().runtime->eeScheduler().startThread(thread, 0u, *currentCall().ctx, false);
    }

    void changeOwnPriority(int priority)
    {
        EeScheduler &ee = currentCall().runtime->eeScheduler();
        int oldPriority = 0;
        ee.changePriority(ee.currentThreadId(), priority, false, oldPriority);
    }

    void endThread()
    {
        afterReturn([](const Call &call) { ps2_syscalls::ExitDeleteThread(call.rdram, call.ctx, call.runtime); });
    }

    uint64_t vsyncNow() { return ps2_syscalls::GetCurrentVSyncTick(currentCall().runtime); }

    int currentThread() { return currentCall().runtime->eeScheduler().currentThreadId(); }

    uint32_t belowStack(uint32_t bytes) { return (getRegU32(currentCall().ctx, kRegSp) - bytes) & ~0xFu; }

    bool callGameFunction(uint32_t function, std::initializer_list<uint32_t> arguments)
    {
        const Call &call = currentCall();
        return call.runtime->callGuestFunction(call.rdram, call.ctx, function, arguments, belowStack(kGameCallBelowStack));
    }

    bool callGameFunction(uint32_t function, std::initializer_list<uint32_t> arguments, uint32_t &result)
    {
        const Call &call = currentCall();
        return call.runtime->callGuestFunction(call.rdram, call.ctx, function, arguments, belowStack(kGameCallBelowStack),
                                               &result);
    }

    void callGameFunctionAfterReturn(uint32_t function, uint32_t argument, int32_t result)
    {
        afterReturn([function, argument, result](const Call &call)
        {
            EeScheduler &ee = call.runtime->eeScheduler();
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::HleCall;
            invocation.context = *call.ctx;
            invocation.context.pc = function;
            SET_GPR_U32(&invocation.context, kRegA0, argument);
            SET_GPR_U32(&invocation.context, kRegSp, ee.invocationStackTop());
            SET_GPR_U32(&invocation.context, kRegRa, 0u);
            invocation.onComplete = [result](const R5900Context &, R5900Context &caller) { setReturnS32(&caller, result); };
            ee.invokeCurrent(std::move(invocation));
        });
    }
}
