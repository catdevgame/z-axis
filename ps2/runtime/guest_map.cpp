#include "runtime/guest_map.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

static_assert(sizeof(void *) == 8, "the guest map reserves 4 GB of address space: 64-bit hosts only");

namespace guest_map
{
    std::vector<View> viewsOf(uint32_t ramBytes, Layout layout)
    {
        const uint64_t ram = ramBytes;
        std::vector<View> views;
        // Every RAM-sized step of the space shows RAM, but the ones that start with the scratchpad's window: its two
        // addresses, and the checker's key for it (RAM's size).
        for (uint64_t address = 0; address < kSpace; address += ram)
        {
            if (address == kScratchpadAddress || address == kScratchpadAlias || (layout == Layout::Checker && address == ram))
            {
                views.push_back({address, ram, kScratchpadWindow});
                views.push_back({address + kScratchpadWindow, kScratchpadWindow, ram - kScratchpadWindow});
            }
            else
                views.push_back({address, 0, ram});
        }
        views.push_back({kSpace, 0, kGuard});
        return views;
    }

    bool Map::protect(uint64_t offset, uint64_t bytes, bool access)
    {
        bool ok = true;
        for (const View &view : m_views)
        {
            const uint64_t from = std::max(offset, view.offset), to = std::min(offset + bytes, view.offset + view.bytes);
            if (from >= to)
                continue;
            uint8_t *at = m_base + view.address + (from - view.offset);
#if defined(_WIN32)
            DWORD old = 0;
            ok = VirtualProtect(at, static_cast<SIZE_T>(to - from), access ? PAGE_READWRITE : PAGE_NOACCESS, &old) && ok;
#else
            ok = mprotect(at, static_cast<size_t>(to - from), access ? PROT_READ | PROT_WRITE : PROT_NONE) == 0 && ok;
#endif
        }
        return ok;
    }

    bool Map::protectAt(uint64_t address, uint64_t bytes, bool access)
    {
#if defined(_WIN32)
        DWORD old = 0;
        return VirtualProtect(m_base + address, static_cast<SIZE_T>(bytes), access ? PAGE_READWRITE : PAGE_NOACCESS, &old) != 0;
#else
        return mprotect(m_base + address, static_cast<size_t>(bytes), access ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
#endif
    }

#if defined(_WIN32)
    namespace
    {
        // Windows 10 1803's placeholder calls, found at run time (kernelbase.dll): an older SDK's headers need not
        // declare them.
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
        using VirtualAlloc2Call = PVOID(WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
        using MapViewOfFile3Call = PVOID(WINAPI *)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);

        struct PlaceholderCalls
        {
            VirtualAlloc2Call virtualAlloc2 = nullptr;
            MapViewOfFile3Call mapViewOfFile3 = nullptr;
        };
        const PlaceholderCalls &placeholderCalls()
        {
            static const PlaceholderCalls calls = []
            {
                PlaceholderCalls c;
                if (HMODULE kernel = GetModuleHandleW(L"kernelbase.dll"))
                {
                    c.virtualAlloc2 = reinterpret_cast<VirtualAlloc2Call>(GetProcAddress(kernel, "VirtualAlloc2"));
                    c.mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Call>(GetProcAddress(kernel, "MapViewOfFile3"));
                }
                return c;
            }();
            return calls;
        }
    }

    bool Map::create(uint32_t ramBytes, Layout layout, bool access)
    {
        release();
        const PlaceholderCalls &calls = placeholderCalls();
        if (!calls.virtualAlloc2 || !calls.mapViewOfFile3)
        {
            std::fprintf(stderr, "[guest-map] this Windows has no VirtualAlloc2 or MapViewOfFile3 (Windows 10 1803 or later)\n");
            return false;
        }
        const uint64_t objectBytes = static_cast<uint64_t>(ramBytes) + kScratchpadWindow;
        m_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(objectBytes >> 32),
                                       static_cast<DWORD>(objectBytes), nullptr);
        if (!m_section)
        {
            std::fprintf(stderr, "[guest-map] CreateFileMapping of %llu bytes failed (%lu)\n",
                         static_cast<unsigned long long>(objectBytes), GetLastError());
            return false;
        }
        const uint64_t span = kSpace + kGuard;
        m_base = static_cast<uint8_t *>(calls.virtualAlloc2(GetCurrentProcess(), nullptr, static_cast<SIZE_T>(span),
                                                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!m_base)
        {
            std::fprintf(stderr, "[guest-map] reserving 4 GB of address space failed (%lu)\n", GetLastError());
            CloseHandle(m_section);
            m_section = nullptr;
            return false;
        }
        m_ramBytes = ramBytes;
        // The placeholder is cut from its start, view by view: [cursor, span) is what is left of it.
        uint64_t cursor = 0;
        for (const View &view : viewsOf(ramBytes, layout))
        {
            if (view.address > cursor)
            {
                if (!VirtualFree(m_base + cursor, static_cast<SIZE_T>(view.address - cursor), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                {
                    std::fprintf(stderr, "[guest-map] splitting the reservation at 0x%llx failed (%lu)\n",
                                 static_cast<unsigned long long>(cursor), GetLastError());
                    m_holes.push_back({cursor, 0, span - cursor});
                    release();
                    return false;
                }
                m_holes.push_back({cursor, 0, view.address - cursor});
            }
            if (view.address + view.bytes < span &&
                !VirtualFree(m_base + view.address, static_cast<SIZE_T>(view.bytes), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            {
                std::fprintf(stderr, "[guest-map] splitting the reservation at 0x%llx failed (%lu)\n",
                             static_cast<unsigned long long>(view.address), GetLastError());
                m_holes.push_back({view.address, 0, span - view.address});
                release();
                return false;
            }
            if (!map(view, access))
            {
                m_holes.push_back({view.address, 0, span - view.address});
                release();
                return false;
            }
            cursor = view.address + view.bytes;
        }
        if (cursor < span)
            m_holes.push_back({cursor, 0, span - cursor});
        return true;
    }

    bool Map::map(const View &view, bool access)
    {
        void *at = placeholderCalls().mapViewOfFile3(m_section, GetCurrentProcess(), m_base + view.address, view.offset,
                                                     static_cast<SIZE_T>(view.bytes), MEM_REPLACE_PLACEHOLDER,
                                                     PAGE_READWRITE, nullptr, 0);
        if (at != m_base + view.address)
        {
            std::fprintf(stderr, "[guest-map] mapping 0x%llx bytes at guest 0x%llx failed (%lu)\n",
                         static_cast<unsigned long long>(view.bytes), static_cast<unsigned long long>(view.address), GetLastError());
            return false;
        }
        m_views.push_back(view);
        if (!access)
        {
            DWORD old = 0;
            VirtualProtect(at, static_cast<SIZE_T>(view.bytes), PAGE_NOACCESS, &old);
        }
        return true;
    }

    void Map::release()
    {
        if (m_base)
        {
            for (const View &view : m_views)
                UnmapViewOfFile(m_base + view.address);
            for (const View &hole : m_holes)
                VirtualFree(m_base + hole.address, 0, MEM_RELEASE);
        }
        if (m_section)
            CloseHandle(m_section);
        m_section = nullptr;
        m_base = nullptr;
        m_ramBytes = 0;
        m_views.clear();
        m_holes.clear();
    }
#else
    namespace
    {
        // A shared memory object with no name in the file system.
        int anonymousObject()
        {
#if defined(__APPLE__)
            static unsigned counter = 0;
            char name[32];
            std::snprintf(name, sizeof(name), "/zaxis.%d.%u", static_cast<int>(getpid()), counter++);
            const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0)
                shm_unlink(name);
            return fd;
#elif defined(__linux__)
            return static_cast<int>(syscall(SYS_memfd_create, "zaxis-guest-ram", 1u /* MFD_CLOEXEC */));
#else
            return -1;
#endif
        }
    }

    bool Map::create(uint32_t ramBytes, Layout layout, bool access)
    {
        release();
        const uint64_t objectBytes = static_cast<uint64_t>(ramBytes) + kScratchpadWindow;
        m_fd = anonymousObject();
        if (m_fd < 0 || ftruncate(m_fd, static_cast<off_t>(objectBytes)) != 0)
        {
            std::fprintf(stderr, "[guest-map] a shared memory object of %llu bytes: %s\n",
                         static_cast<unsigned long long>(objectBytes), std::strerror(errno));
            release();
            return false;
        }
        int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
        flags |= MAP_NORESERVE;
#endif
        void *reserved = mmap(nullptr, static_cast<size_t>(kSpace + kGuard), PROT_NONE, flags, -1, 0);
        if (reserved == MAP_FAILED)
        {
            std::fprintf(stderr, "[guest-map] reserving 4 GB of address space: %s\n", std::strerror(errno));
            release();
            return false;
        }
        m_base = static_cast<uint8_t *>(reserved);
        m_ramBytes = ramBytes;
        for (const View &view : viewsOf(ramBytes, layout))
            if (!map(view, access))
            {
                release();
                return false;
            }
        return true;
    }

    bool Map::map(const View &view, bool access)
    {
        void *at = mmap(m_base + view.address, static_cast<size_t>(view.bytes), access ? PROT_READ | PROT_WRITE : PROT_NONE,
                        MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(view.offset));
        if (at != m_base + view.address)
        {
            std::fprintf(stderr, "[guest-map] mapping 0x%llx bytes at guest 0x%llx: %s\n",
                         static_cast<unsigned long long>(view.bytes), static_cast<unsigned long long>(view.address),
                         std::strerror(errno));
            return false;
        }
        m_views.push_back(view);
        return true;
    }

    void Map::release()
    {
        if (m_base)
            munmap(m_base, static_cast<size_t>(kSpace + kGuard));
        if (m_fd >= 0)
            close(m_fd);
        m_fd = -1;
        m_base = nullptr;
        m_ramBytes = 0;
        m_views.clear();
    }
#endif
}
