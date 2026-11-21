// ee::fiber on Windows: the Fibers API, with fibers reused.
//
// A level load runs about 1.3 million suspendable calls (the chunk and GEO
// readers). CreateFiber maps a new stack and DeleteFiber unmaps it (fresh
// kernel pages on every call; 7.6 us a pair on a Ryzen 5900X before the
// stack is even used): one of each per call made a Movie Lot load take 28 s
// (5.6 s with reuse). SwitchToFiber itself is cheap (about 25 ns).
//
// A fiber is reused: when its entry returns, the start loop marks it
// finished and leaves; destroy() puts a finished fiber in the pool, and the
// next create() with the same stack size gives it a new entry, which the
// loop runs when the fiber is entered again.
#if defined(_WIN32)

#include "fiber.h"

#define NOMINMAX
#include <windows.h>

#include <vector>

namespace ee::fiber
{
    struct Context
    {
        void *self = nullptr;
        void *host = nullptr;
        std::size_t stackBytes = 0;
        void (*entry)(void *) = nullptr;
        void *arg = nullptr;
        bool finished = false; // the entry returned: parked in start's loop
    };

    namespace
    {
        constexpr std::size_t kPoolSize = 16;
        // The pool goes with its thread (the game thread of one run: a game
        // booted again in the process gets a new one).
        struct Pool
        {
            std::vector<Context *> fibers;
            ~Pool()
            {
                for (Context *fiber : fibers)
                {
                    DeleteFiber(fiber->self);
                    delete fiber;
                }
            }
        };
        thread_local Pool t_pool;

        VOID CALLBACK start(PVOID parameter)
        {
            Context *fiber = static_cast<Context *>(parameter);
            for (;;)
            {
                fiber->entry(fiber->arg);
                fiber->finished = true;
                SwitchToFiber(fiber->host);
            }
        }

        // The calling thread as a fiber (once per thread).
        void *threadFiber()
        {
            thread_local void *self = nullptr;
            if (!self)
                self = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
            return self;
        }
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
            fiber->stackBytes = stackBytes;
            // stackBytes reserved, a page committed: the rest is committed as the
            // stack grows into it (Windows' guard page), as a thread's stack is.
            fiber->self = CreateFiberEx(0x1000, stackBytes, 0, &start, fiber);
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
            DeleteFiber(fiber->self);
            delete fiber;
        }
    }

    void enter(Context *fiber)
    {
        threadFiber();
        fiber->host = GetCurrentFiber();
        SwitchToFiber(fiber->self);
    }

    void leave(Context *fiber) { SwitchToFiber(fiber->host); }
}

#endif
