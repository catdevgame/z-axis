#include "runtime/session.h"

#include "runtime/settings_registry.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace zx::session
{
    namespace
    {
        struct Kept
        {
            void *value = nullptr;
            void (*destroy)(void *) = nullptr;
        };

        struct Current
        {
            std::atomic<uint64_t> number{0};
            std::vector<Kept> states;
            std::vector<std::function<void()>> endWork;
            std::vector<ShutdownFn> shutdownHooks;
        };

        Current &current()
        {
            static Current instance;
            return instance;
        }
    }

    namespace detail
    {
        std::recursive_mutex &mutex()
        {
            static std::recursive_mutex instance;
            return instance;
        }

        uint64_t number() { return current().number.load(std::memory_order_acquire); }

        void keep(void *value, void (*destroy)(void *))
        {
            std::lock_guard<std::recursive_mutex> lock(mutex());
            current().states.push_back({value, destroy});
        }
    }

    void onEnd(std::function<void()> work)
    {
        if (!work)
            return;
        std::lock_guard<std::recursive_mutex> lock(detail::mutex());
        current().endWork.push_back(std::move(work));
    }

    void addSetting(const settings::Setting &setting)
    {
        settings::add(setting);
        onEnd([key = setting.key]() { settings::remove(key); });
    }

    void addShutdownHook(ShutdownFn fn)
    {
        std::lock_guard<std::recursive_mutex> lock(detail::mutex());
        std::vector<ShutdownFn> &hooks = current().shutdownHooks;
        if (fn && std::find(hooks.begin(), hooks.end(), fn) == hooks.end())
            hooks.push_back(fn);
    }

    void runShutdownHooks(PS2Machine &runtime)
    {
        std::vector<ShutdownFn> hooks;
        {
            std::lock_guard<std::recursive_mutex> lock(detail::mutex());
            hooks = current().shutdownHooks;
        }
        for (ShutdownFn fn : hooks)
            fn(runtime);
    }

    void end()
    {
        // The end work first, with the session's states still there.
        std::vector<std::function<void()>> work;
        {
            std::lock_guard<std::recursive_mutex> lock(detail::mutex());
            work.swap(current().endWork);
        }
        for (auto it = work.rbegin(); it != work.rend(); ++it)
            (*it)();
        std::vector<Kept> states;
        {
            std::lock_guard<std::recursive_mutex> lock(detail::mutex());
            Current &c = current();
            states.swap(c.states);
            c.endWork.clear();
            c.shutdownHooks.clear();
            // From here state<T>() makes new ones (for the next session).
            c.number.fetch_add(1u, std::memory_order_acq_rel);
        }
        for (auto it = states.rbegin(); it != states.rend(); ++it)
            it->destroy(it->value);
    }

    uint64_t number() { return detail::number(); }
}
