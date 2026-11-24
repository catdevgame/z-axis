#include "runtime/play_link.h"

#include "runtime/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ps2_play_link
{
namespace
{
    constexpr uint32_t kRamMask = PS2_RAM_MASK;
    constexpr int kButtons = 16;

    // An end of the link: a FIFO's file descriptor, on Windows a named pipe's
    // handle.
#if defined(_WIN32)
    using End = HANDLE;
    const End kNoEnd = INVALID_HANDLE_VALUE;
#else
    using End = int;
    constexpr End kNoEnd = -1;
#endif

    struct Link
    {
        End up = kNoEnd;
        End down = kNoEnd;
        uint32_t reads = 0;
        uint32_t skip = 0;
        uint16_t buttons = 0;
        uint8_t axes = 0;
        uint8_t sticks[4]{};
    };

    [[noreturn]] void playerGone()
    {
        std::fprintf(stderr, "[play] the player is gone: exiting\n");
        std::fflush(stderr);
#if defined(_WIN32)
        ExitProcess(0);
#else
        _exit(0);
#endif
    }

    // At most `size` bytes: how many came, 0 or less when the player is gone.
    long readSome(End end, uint8_t *to, size_t size)
    {
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(end, to, static_cast<DWORD>(size), &got, nullptr))
            return -1;
        return static_cast<long>(got);
#else
        return static_cast<long>(::read(end, to, size));
#endif
    }

    long writeSome(End end, const uint8_t *from, size_t size)
    {
#if defined(_WIN32)
        DWORD put = 0;
        if (!WriteFile(end, from, static_cast<DWORD>(size), &put, nullptr))
            return -1;
        return static_cast<long>(put);
#else
        return static_cast<long>(::write(end, from, size));
#endif
    }

    // The player's end of `up` opened for writing or of `down` for reading.
    End openEnd(const std::string &path, bool forWriting)
    {
#if defined(_WIN32)
        return CreateFileA(path.c_str(), forWriting ? GENERIC_WRITE : GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
#else
        return ::open(path.c_str(), forWriting ? O_WRONLY : O_RDONLY);
#endif
    }

    void closeEnd(End end)
    {
        if (end == kNoEnd)
            return;
#if defined(_WIN32)
        CloseHandle(end);
#else
        ::close(end);
#endif
    }

    void readAll(End end, void *to, size_t size)
    {
        auto *at = static_cast<uint8_t *>(to);
        while (size)
        {
            const long got = readSome(end, at, size);
            if (got <= 0)
                playerGone();
            at += got;
            size -= static_cast<size_t>(got);
        }
    }

    void writeAll(End end, const void *from, size_t size)
    {
        const auto *at = static_cast<const uint8_t *>(from);
        while (size)
        {
            const long put = writeSome(end, at, size);
            if (put <= 0)
                playerGone();
            at += put;
            size -= static_cast<size_t>(put);
        }
    }

    // The link of this process (ZX_SPOT_FORK: a forked child's own, relink()).
    bool g_checked = false, g_on = false;
    Link &state()
    {
        static Link l;
        return l;
    }
    bool g_opened = false;

    // <dir>/up and <dir>/down; on Windows the pipes \\.\pipe\<name>\up and
    // \\.\pipe\<name>\down (ZX_PLAY_LINK=\\.\pipe\<name>).
#if defined(_WIN32)
    constexpr char kSeparator = '\\';
#else
    constexpr char kSeparator = '/';
#endif

    Link &link()
    {
        Link &l = state();
        if (g_opened)
            return l;
        g_opened = true;
        const char *dir = std::getenv("ZX_PLAY_LINK");
        if (!dir || !dir[0])
            return l;
        // The player opens up for reading and down for writing first.
        l.up = openEnd(std::string(dir) + kSeparator + "up", true);
        l.down = openEnd(std::string(dir) + kSeparator + "down", false);
        if (l.up == kNoEnd || l.down == kNoEnd)
        {
            std::fprintf(stderr, "[play] no link at %s\n", dir);
            std::exit(1);
        }
        std::fprintf(stderr, "[play] linked at %s\n", dir);
        return l;
    }

    // The answer on a read: buttons added at full pressure, the sticks it sets.
    void applyAnswer(const Link &l, uint16_t &pressed, uint8_t *sticks, uint8_t *pressure)
    {
        pressed = static_cast<uint16_t>(pressed | l.buttons);
        for (int bit = 0; bit < kButtons; ++bit)
            if (l.buttons & (1u << bit))
                pressure[bit] = 0xFFu;
        for (int axis = 0; axis < 4; ++axis)
            if (l.axes & (1u << axis))
                sticks[axis] = l.sticks[axis];
    }

    void answer(Link &l, const uint8_t *rdram)
    {
        for (;;)
        {
            uint8_t kind = 0;
            readAll(l.down, &kind, 1);
            if (kind == 'R')
            {
                uint32_t request[2];
                readAll(l.down, request, sizeof(request));
                const uint32_t address = request[0] & kRamMask;
                if (request[1] > kRamMask + 1u - address)
                    request[1] = kRamMask + 1u - address;
                writeAll(l.up, rdram + address, request[1]);
            }
            else if (kind == 'G')
            {
                uint8_t go[11];
                readAll(l.down, go, sizeof(go));
                std::memcpy(&l.buttons, go, 2);
                l.axes = go[2];
                std::memcpy(l.sticks, go + 3, 4);
                std::memcpy(&l.skip, go + 7, 4);
                return;
            }
            else
            {
                std::fprintf(stderr, "[play] unknown request %u\n", kind);
                playerGone();
            }
        }
    }
}

bool enabled()
{
    if (!g_checked)
    {
        g_checked = true;
        const char *dir = std::getenv("ZX_PLAY_LINK");
        g_on = dir && dir[0];
    }
    return g_on;
}

void relink()
{
    Link &l = state();
    closeEnd(l.up);
    closeEnd(l.down);
    l = Link{};
    g_opened = false;
    g_checked = false;
}

void onPadRead(const uint8_t *rdram, uint64_t tick, int64_t scriptTime, uint16_t &pressed, uint8_t *sticks,
               uint8_t *pressure)
{
    Link &l = link();
    ++l.reads;
    if (l.skip)
    {
        --l.skip;
    }
    else
    {
        uint8_t head[21];
        head[0] = 'T';
        std::memcpy(head + 1, &tick, 8);
        std::memcpy(head + 9, &scriptTime, 8);
        std::memcpy(head + 17, &l.reads, 4);
        writeAll(l.up, head, sizeof(head));
        answer(l, rdram);
    }
    applyAnswer(l, pressed, sticks, pressure);
}

void onMirroredRead(uint16_t &pressed, uint8_t *sticks, uint8_t *pressure)
{
    applyAnswer(link(), pressed, sticks, pressure);
}
}
