#include "runtime/play_link.h"

#include "runtime/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
namespace ps2_play_link
{
    bool enabled() { return false; }
    void onPadRead(const uint8_t *, uint64_t, int64_t, uint16_t &, uint8_t *, uint8_t *) {}
    void onMirroredRead(uint16_t &, uint8_t *, uint8_t *) {}
    void relink() {}
}
#else
#include <fcntl.h>
#include <unistd.h>

namespace ps2_play_link
{
namespace
{
    constexpr uint32_t kRamMask = PS2_RAM_MASK;
    constexpr int kButtons = 16;

    struct Link
    {
        int up = -1;
        int down = -1;
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
        _exit(0);
    }

    void readAll(int fd, void *to, size_t size)
    {
        auto *at = static_cast<uint8_t *>(to);
        while (size)
        {
            const ssize_t got = ::read(fd, at, size);
            if (got <= 0)
                playerGone();
            at += got;
            size -= static_cast<size_t>(got);
        }
    }

    void writeAll(int fd, const void *from, size_t size)
    {
        const auto *at = static_cast<const uint8_t *>(from);
        while (size)
        {
            const ssize_t put = ::write(fd, at, size);
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
        l.up = ::open((std::string(dir) + "/up").c_str(), O_WRONLY);
        l.down = ::open((std::string(dir) + "/down").c_str(), O_RDONLY);
        if (l.up < 0 || l.down < 0)
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
    if (l.up >= 0)
        ::close(l.up);
    if (l.down >= 0)
        ::close(l.down);
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
#endif
