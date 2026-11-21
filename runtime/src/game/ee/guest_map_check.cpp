// ZX_GUEST_MAP_CHECK (runtime/CMakeLists.txt), a tool build: every access of the readable game (ee::at<T>)
// resolved as the guest map does (base + address: runtime/guest_map.h) and as ee::at did before it (the runtime's
// special-address test, getMemPtr), and told where they part or the address is outside RAM's own view (a mirror,
// the scratchpad, the space past RAM): a line per address region and code address the first time ("[guest-map]
// <view> view: <address> from code <code address>", "[guest-map] differs: <address> (before: ..., now: ...) from
// code ..."), and a count per region at exit ("[guest-map] at exit: ...").
//
// The code address is the readable function's own (the noinline call below returns into it); on the Mac `atos -o
// <runner> -l <load address>` names it (the load address is printed once).
#include "ee.h"

#if ZX_GUEST_MAP_CHECK

#include "runtime/guest_map.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ee
{
    namespace
    {
        // Where an address is: RAM's own view (0), a 64 MB step of the space (1 + its number), the scratchpad's
        // window at one of its two addresses (65, 66).
        constexpr size_t kRegions = 67;
        std::array<std::atomic<uint64_t>, kRegions> g_counts{};
        std::atomic<uint64_t> g_differs{0};
        std::mutex g_mutex;
        std::set<std::pair<size_t, uintptr_t>> g_seen;

        // The object's byte an address shows: RAM's offset, or RAM's size + the scratchpad window's.
        uint64_t mappedKey(uint32_t address)
        {
            const uint32_t step = address & ~PS2_RAM_MASK, within = address & PS2_RAM_MASK;
            if ((step == guest_map::kScratchpadAddress || step == guest_map::kScratchpadAlias) && within < guest_map::kScratchpadWindow)
                return PS2_RAM_SIZE + within;
            return within;
        }
        // The same as ee::at resolved it before the map.
        uint64_t previousKey(uint32_t address)
        {
            if (!PS2Runtime::isSpecialAddress(address))
                return address & PS2_RAM_MASK;
            uint32_t offset = 0;
            bool scratch = false;
            ps2ResolveGuestPointer(address, offset, scratch);
            return scratch ? PS2_RAM_SIZE + offset : offset;
        }
        size_t regionOf(uint32_t address)
        {
            const uint32_t step = address & ~PS2_RAM_MASK, within = address & PS2_RAM_MASK;
            if (step == guest_map::kScratchpadAddress && within < guest_map::kScratchpadWindow)
                return 65;
            if (step == guest_map::kScratchpadAlias && within < guest_map::kScratchpadWindow)
                return 66;
            return step == 0 ? 0 : 1 + (address >> 26);
        }
        uint32_t regionAddress(size_t region)
        {
            return region == 65 ? guest_map::kScratchpadAddress
                   : region == 66 ? guest_map::kScratchpadAlias
                                  : static_cast<uint32_t>((region - 1) << 26);
        }
        const char *keyName(uint64_t key) { return key >= PS2_RAM_SIZE ? "scratchpad window" : "RAM"; }
        uint64_t keyOffset(uint64_t key) { return key >= PS2_RAM_SIZE ? key - PS2_RAM_SIZE : key; }

        void atExit()
        {
            std::fprintf(stderr, "[guest-map] at exit:");
            for (size_t r = 1; r < kRegions; ++r)
                if (const uint64_t n = g_counts[r].load())
                    std::fprintf(stderr, " 0x%08x view %" PRIu64 ",", regionAddress(r), n);
            std::fprintf(stderr, " RAM's own %" PRIu64 ", differs %" PRIu64 "\n", g_counts[0].load(), g_differs.load());
        }
        void started()
        {
            static const bool once = []
            {
#if defined(__APPLE__)
                std::fprintf(stderr, "[guest-map] checking every readable access; the runner's slide 0x%" PRIxPTR "\n",
                             static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0)));
#else
                std::fprintf(stderr, "[guest-map] checking every readable access\n");
#endif
                std::atexit(&atExit);
                return true;
            }();
            (void)once;
        }
    }

    void checkGuestAccess(uint32_t address)
    {
        const size_t region = regionOf(address);
        g_counts[region].fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = mappedKey(address), before = previousKey(address);
        if (region == 0 && now == before)
            return;
        started();
        const uintptr_t code = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        if (now != before)
            g_differs.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_seen.insert({now != before ? kRegions : region, code}).second || g_seen.size() > 20000)
            return;
        if (now != before)
            std::fprintf(stderr, "[guest-map] differs: 0x%08x (before: %s 0x%" PRIx64 ", now: %s 0x%" PRIx64 ") from code 0x%" PRIxPTR "\n",
                         address, keyName(before), keyOffset(before), keyName(now), keyOffset(now), code);
        else
            std::fprintf(stderr, "[guest-map] 0x%08x view: 0x%08x from code 0x%" PRIxPTR " (first of this code address)\n",
                         regionAddress(region), address, code);
    }
}

#endif
