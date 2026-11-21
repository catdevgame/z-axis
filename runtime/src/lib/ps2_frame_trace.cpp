#include "runtime/ps2_frame_trace.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <memory>
#include <mutex>

namespace zx::ftrace
{
    std::atomic<bool> g_on{false};

    namespace
    {
        std::mutex g_mutex;
        std::unique_ptr<Event[]> g_events;
        uint32_t g_capacity = 0;
        std::atomic<uint32_t> g_count{0};
        std::atomic<uint32_t> g_nextThread{1};
        const auto g_epoch = std::chrono::steady_clock::now();

        uint32_t threadIndex()
        {
            thread_local uint32_t index = g_nextThread.fetch_add(1u, std::memory_order_relaxed);
            return index;
        }
    }

    void markSlow(uint16_t kind, uint16_t arg)
    {
        const uint32_t i = g_count.fetch_add(1u, std::memory_order_relaxed);
        if (i >= g_capacity)
        {
            g_on.store(false, std::memory_order_relaxed);
            return;
        }
        Event &e = g_events[i];
        e.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_epoch).count();
        e.thread = threadIndex();
        e.kind = kind;
        e.arg = arg;
    }

    bool start(uint32_t capacity)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_on.store(false);
        // The buffer stays allocated once made: a mark racing with stop()
        // may still write into it.
        if (!g_events)
        {
            g_events.reset(new Event[capacity]());
            g_capacity = capacity;
        }
        g_count.store(0);
        g_on.store(true);
        return true;
    }

    bool stop(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_on.store(false);
        if (!g_events)
            return false;
        // Marks in flight finish their store well within this.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint32_t n = std::min(g_count.load(), g_capacity);
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(g_events.get(), sizeof(Event), n, f) == n;
        std::fclose(f);
        return ok;
    }
}
