#pragma once

// Host code about to write guest memory through the operating system (a
// file read straight into EE RAM: a large fread reads into its destination
// with the read system call). The fncheck recorder (game/ee/fncheck.cpp)
// keeps EE RAM read-only around the calls a recorded function makes and
// learns of each write from its first fault on a page; a system call that
// writes a read-only page fails instead (EFAULT), so code that hands guest
// memory to the system says so first. Without a recorder the hook is null
// and this is one load.

#include <atomic>
#include <cstddef>

using Ps2HostWriteHook = void (*)(void *destination, size_t size);

inline std::atomic<Ps2HostWriteHook> &ps2HostWriteHookStorage()
{
    static std::atomic<Ps2HostWriteHook> hook{nullptr};
    return hook;
}

inline void ps2BeforeHostWrite(void *destination, size_t size)
{
    if (const Ps2HostWriteHook hook = ps2HostWriteHookStorage().load(std::memory_order_acquire))
        hook(destination, size);
}
