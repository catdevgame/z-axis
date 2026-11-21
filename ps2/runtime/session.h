#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

class PS2Machine;

namespace zx::settings
{
    struct Setting;
}

// The game overrides' state for one game boot (a session): what a module
// kept in file statics, so a game booted again in the same process (the
// collection's menu, zx-runner --sequence) starts as it does in a
// process of its own, and what it registered with the host is undone when
// the game is left.
//
//   struct Disc { ... };                     // a module's state
//   Disc &disc() { return zx::session::state<Disc>(); }
//   zx::session::onEnd([] { ... });        // undoes a host registration
//
// A session ends with the boot (zx::boot::end, when the game's runtime is
// destroyed, its threads stopped; and zx::boot::begin, so what was made
// between two boots is not the next game's): the onEnd work runs, the last
// added first, then every state is destroyed, the last made first. The next
// use of state<T>() makes a new one. A single-game run never ends its
// session (the process exits).
namespace zx::session
{
    namespace detail
    {
        std::recursive_mutex &mutex();
        uint64_t number();
        // Keeps value (destroyed by destroy at the end of the session).
        void keep(void *value, void (*destroy)(void *));
    }

    // The session's T, made (value-initialized) at the first use in it.
    // Any thread; a reference stays good until the session ends. A state's
    // constructor or destructor must not ask for another state.
    template <typename T>
    T &state()
    {
        struct Slot
        {
            std::atomic<uint64_t> session{~uint64_t{0}};
            T *value = nullptr;
        };
        static Slot slot;
        const uint64_t now = detail::number();
        if (slot.session.load(std::memory_order_acquire) != now)
        {
            std::lock_guard<std::recursive_mutex> lock(detail::mutex());
            const uint64_t current = detail::number();
            if (slot.session.load(std::memory_order_relaxed) != current)
            {
                slot.value = new T();
                detail::keep(slot.value, [](void *value) { delete static_cast<T *>(value); });
                slot.session.store(current, std::memory_order_release);
            }
        }
        return *slot.value;
    }

    // Work for the end of the session: undoing a registration with the host
    // (a settings row, a game file provider, a disc patch, pad state).
    void onEnd(std::function<void()> work);

    // A settings row of this game (zx::settings::add), removed when the
    // session ends. Window thread.
    void addSetting(const settings::Setting &setting);

    // A shutdown hook of this session only: run by
    // ps2_game_overrides::runShutdownHooks after the game stopped, with its
    // memory still there (a module saving progress), dropped when the
    // session ends. Once per function.
    using ShutdownFn = void (*)(PS2Machine &runtime);
    void addShutdownHook(ShutdownFn fn);
    void runShutdownHooks(PS2Machine &runtime);

    // Ends the session (zx::boot::begin and end call it). Only while no
    // game thread runs.
    void end();

    // Changes with every session ended.
    uint64_t number();
}
