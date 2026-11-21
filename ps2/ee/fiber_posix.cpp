// ee::fiber on POSIX: pooled stacks, entered and left by a register-only
// switch on arm64 and x86-64 (ucontext elsewhere).
//
// A level load runs about 1.3 million suspendable calls (the chunk and GEO
// readers) with 4 switches each. swapcontext saves and restores the signal
// mask with system calls (sigprocmask, and on macOS getrlimit and
// sigaltstack too: about 1.3 us a switch on an M5 Pro), which made switching
// most of a load's time. Only the callee-saved registers need keeping here:
// a switch is an ordinary call into the switch function.
//
// A fiber is reused: when its entry returns, the start loop marks it
// finished and leaves; destroy() puts a finished fiber in the pool, and the
// next create() with the same stack size gives it a new entry, which the
// loop runs when the fiber is entered again.
#if !defined(_WIN32)

#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "fiber.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#if defined(__aarch64__) || defined(__x86_64__)
#define EE_FIBER_ASM 1
#else
#define EE_FIBER_ASM 0
#include <ucontext.h>
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#endif

#if EE_FIBER_ASM
// ee_fiber_switch(from, to): pushes the callee-saved registers on the current
// stack, stores the stack pointer in *from, loads `to` as the stack pointer,
// pops the registers saved there and returns into that stack.
extern "C" void ee_fiber_switch(void **from, void *to);

#if defined(__APPLE__)
#define EE_FIBER_SYM "_ee_fiber_switch"
#else
#define EE_FIBER_SYM "ee_fiber_switch"
#endif

#if defined(__aarch64__)
// x19-x28, x29 (fp), x30 (lr), d8-d15: 160 bytes, 16-byte aligned.
asm(".text\n"
    ".globl " EE_FIBER_SYM "\n"
    ".p2align 4\n" EE_FIBER_SYM ":\n"
    "    sub sp, sp, #160\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mov x2, sp\n"
    "    str x2, [x0]\n"
    "    mov sp, x1\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #160\n"
    "    ret\n");
#else
// System V x86-64: rbp, rbx, r12-r15 (the return address is already pushed).
asm(".text\n"
    ".globl " EE_FIBER_SYM "\n"
    ".p2align 4\n" EE_FIBER_SYM ":\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n");
#endif
#endif

namespace ee::fiber
{
    struct Context
    {
#if EE_FIBER_ASM
        void *self = nullptr; // the fiber's saved stack pointer while it is out
        void *host = nullptr; // the enterer's, while the fiber runs
#else
        ucontext_t self;
        ucontext_t host;
#endif
        void *stack = nullptr;
        std::size_t stackBytes = 0;
        void (*entry)(void *) = nullptr;
        void *arg = nullptr;
        bool finished = false; // the entry returned: parked in start's loop
    };

    namespace
    {
        constexpr std::size_t kPoolSize = 16;
        thread_local Context *t_starting = nullptr;

        // A stack is mapped, not allocated: its pages are only backed once
        // touched, and the page below it is a guard, so running past its end
        // faults there instead of writing over whatever the heap put next to
        // it (a 512 KB stack from malloc, overrun by a deep tree walk, crashed
        // the game later and elsewhere).
        std::size_t guardBytes() { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }
        void *mapStack(std::size_t stackBytes)
        {
            const std::size_t guard = guardBytes();
            void *at = mmap(nullptr, stackBytes + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (at == MAP_FAILED)
                std::abort();
            mprotect(at, guard, PROT_NONE);
            return static_cast<uint8_t *>(at) + guard;
        }
        void unmapStack(void *stack, std::size_t stackBytes)
        {
            const std::size_t guard = guardBytes();
            munmap(static_cast<uint8_t *>(stack) - guard, stackBytes + guard);
        }
        // The pool goes with its thread (the game thread of one run: a game
        // booted again in the process gets a new one).
        struct Pool
        {
            std::vector<Context *> fibers;
            ~Pool()
            {
                for (Context *fiber : fibers)
                {
                    unmapStack(fiber->stack, fiber->stackBytes);
                    delete fiber;
                }
            }
        };
        thread_local Pool t_pool;

        void leaveTo(Context *fiber);

        // A fiber's first code: runs each entry it is given (see the top).
        void start()
        {
            Context *fiber = t_starting;
            for (;;)
            {
                fiber->entry(fiber->arg);
                fiber->finished = true;
                leaveTo(fiber);
            }
        }

#if EE_FIBER_ASM
        // The new stack as ee_fiber_switch leaves it: the saved registers
        // (zero) under a return into start(), with start() entered as by a
        // call, its stack aligned as the ABI asks.
        void prepare(Context *fiber)
        {
            uintptr_t top = reinterpret_cast<uintptr_t>(fiber->stack) + fiber->stackBytes;
            top &= ~static_cast<uintptr_t>(15);
            void **sp = reinterpret_cast<void **>(top);
#if defined(__aarch64__)
            sp -= 20; // 160 bytes
            for (int i = 0; i < 20; ++i)
                sp[i] = nullptr;
            sp[11] = reinterpret_cast<void *>(&start); // x30 (x29 = sp[10] stays 0: the last frame)
#else
            *--sp = nullptr;                           // start()'s return address (never used)
            *--sp = reinterpret_cast<void *>(&start);  // ee_fiber_switch's ret
            for (int i = 0; i < 6; ++i)
                *--sp = nullptr;                       // r15 r14 r13 r12 rbx rbp
#endif
            fiber->self = sp;
        }
#endif
    }

    Context *create(void (*entry)(void *), void *arg, std::size_t stackBytes)
    {
        Context *fiber = nullptr;
        for (auto it = t_pool.fibers.begin(); it != t_pool.fibers.end(); ++it)
            if ((*it)->stackBytes == stackBytes)
            {
                fiber = *it;
                t_pool.fibers.erase(it);
                break;
            }
        if (!fiber)
        {
            fiber = new Context;
            fiber->stack = mapStack(stackBytes);
            fiber->stackBytes = stackBytes;
#if EE_FIBER_ASM
            prepare(fiber);
#else
            getcontext(&fiber->self);
            fiber->self.uc_stack.ss_sp = fiber->stack;
            fiber->self.uc_stack.ss_size = stackBytes;
            fiber->self.uc_link = nullptr;
            makecontext(&fiber->self, &start, 0);
#endif
        }
        fiber->entry = entry;
        fiber->arg = arg;
        fiber->finished = false;
        return fiber;
    }

    void destroy(Context *fiber)
    {
        if (fiber->finished && t_pool.fibers.size() < kPoolSize)
            t_pool.fibers.push_back(fiber);
        else
        {
            unmapStack(fiber->stack, fiber->stackBytes);
            delete fiber;
        }
    }

    void enter(Context *fiber)
    {
        Context *outer = t_starting;
        t_starting = fiber;
#if EE_FIBER_ASM
        ee_fiber_switch(&fiber->host, fiber->self);
#else
        swapcontext(&fiber->host, &fiber->self);
#endif
        t_starting = outer;
    }

    void leave(Context *fiber) { leaveTo(fiber); }

    namespace
    {
        void leaveTo(Context *fiber)
        {
#if EE_FIBER_ASM
            ee_fiber_switch(&fiber->self, fiber->host);
#else
            swapcontext(&fiber->self, &fiber->host);
#endif
        }
    }
}

#endif
