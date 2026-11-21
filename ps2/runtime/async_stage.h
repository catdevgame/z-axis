#ifndef PS2_ASYNC_STAGE_H
#define PS2_ASYNC_STAGE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include "runtime/frame_trace.h"
#include "runtime/thread_cpu.h"
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

// One worker thread that runs posted tasks in order. Used to run VIF1/VU1
// and GS packet processing beside the EE: the poster calls drain() before it
// observes anything the tasks change, so it sees the same state as if every
// task had run synchronously at post time.
class Ps2AsyncStage
{
public:
    explicit Ps2AsyncStage(const char *name) : m_name(name), m_traceId(name[0] == 'v' ? 1u : 2u) {}
    ~Ps2AsyncStage() { stop(); }

    Ps2AsyncStage(const Ps2AsyncStage &) = delete;
    Ps2AsyncStage &operator=(const Ps2AsyncStage &) = delete;

    void post(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_thread.joinable())
            {
                m_stopping = false;
                m_thread = std::thread([this]()
                                       { workerLoop(); });
                m_threadId = m_thread.get_id();
            }
            m_tasks.push_back(std::move(task));
            m_posted.fetch_add(1u, std::memory_order_release);
        }
        m_workCv.notify_one();
    }

    // True when every task posted so far has finished.
    bool idle() const
    {
        return m_done.load(std::memory_order_acquire) == m_posted.load(std::memory_order_acquire);
    }

    // Waits until every task posted so far has finished. Called on the
    // worker itself it does nothing (tasks run in order anyway).
    void drain()
    {
        const uint64_t target = m_posted.load(std::memory_order_acquire);
        if (m_done.load(std::memory_order_acquire) >= target || isWorkerThread())
            return;
        m_drainWaits.fetch_add(1u, std::memory_order_relaxed);
        zx::ftrace::Scope traceWait(zx::ftrace::kDrainBegin, zx::ftrace::kDrainEnd, m_traceId);
        // Most waits are short: spin a little before sleeping.
        for (int spin = 0; spin < 2000; ++spin)
        {
            if (m_done.load(std::memory_order_acquire) >= target)
                return;
#if defined(__aarch64__)
            __asm__ __volatile__("yield");
#elif defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idleCv.wait(lock, [&]()
                      { return m_done.load(std::memory_order_acquire) >= target; });
    }

    bool isWorkerThread() const { return std::this_thread::get_id() == m_threadId; }

    // Called on the worker whenever it has run every task posted so far.
    // Set before the first post.
    void setIdleHook(std::function<void()> hook) { m_idleHook = std::move(hook); }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_thread.joinable())
                return;
            m_stopping = true;
        }
        m_workCv.notify_one();
        m_thread.join();
        m_thread = std::thread();
        m_threadId = std::thread::id();
    }

    // Statistics: the worker's CPU time so far (ms, 0 where unsupported).
    double cpuMilliseconds() const
    {
#if defined(__APPLE__)
        const uint32_t port = m_machThread.load(std::memory_order_acquire);
        if (port == 0u)
            return 0.0;
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
            return 0.0;
        return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
               (info.user_time.microseconds + info.system_time.microseconds) * 1e-3;
#else
        return zx::threadCpuMilliseconds(m_cpuHandle.load(std::memory_order_acquire));
#endif
    }

    // Statistics: drain() calls that had to wait.
    uint64_t drainWaits() const { return m_drainWaits.load(std::memory_order_relaxed); }

private:
    void workerLoop()
    {
#if defined(__APPLE__)
        pthread_setname_np(m_name);
        // The graphics pipeline sets the frame rate: keep it on performance
        // cores when the machine is busy.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        m_machThread.store(pthread_mach_thread_np(pthread_self()), std::memory_order_release);
#elif defined(_WIN32)
        m_cpuHandle.store(zx::currentThreadCpuHandle(), std::memory_order_release);
#elif defined(__linux__)
        // Without a name the worker shows under its parent's (Linux threads
        // inherit it): three "GameThread"s in top.
        pthread_setname_np(pthread_self(), m_name);
        m_cpuHandle.store(zx::currentThreadCpuHandle(), std::memory_order_release);
#endif
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workCv.wait(lock, [this]()
                              { return m_stopping || !m_tasks.empty(); });
                if (m_tasks.empty())
                    return; // stopping with nothing left
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            try
            {
                zx::ftrace::Scope traceTask(zx::ftrace::kTaskBegin, zx::ftrace::kTaskEnd, m_traceId);
                task();
            }
            catch (const std::exception &e)
            {
                std::fprintf(stderr, "[%s] task failed: %s\n", m_name, e.what());
            }
            catch (...)
            {
                std::fprintf(stderr, "[%s] task failed\n", m_name);
            }
            m_done.fetch_add(1u, std::memory_order_acq_rel);
            bool empty;
            {
                // Pairs with the predicate check in drain().
                std::lock_guard<std::mutex> lock(m_mutex);
                empty = m_tasks.empty();
            }
            m_idleCv.notify_all();
            if (empty && m_idleHook)
                m_idleHook();
        }
    }

    const char *m_name;
    uint16_t m_traceId;
    std::mutex m_mutex;
    std::condition_variable m_workCv;
    std::condition_variable m_idleCv;
    std::deque<std::function<void()>> m_tasks;
    std::function<void()> m_idleHook;
    std::thread m_thread;
    std::thread::id m_threadId;
    bool m_stopping = false;
    std::atomic<uint64_t> m_posted{0};
    std::atomic<uint64_t> m_done{0};
    std::atomic<uint64_t> m_drainWaits{0};
    std::atomic<uint32_t> m_machThread{0};
    std::atomic<uint64_t> m_cpuHandle{0}; // Windows, Linux: the worker's thread handle
};

#endif
