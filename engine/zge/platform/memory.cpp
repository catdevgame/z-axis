// Memory by kind (zge/platform/platform.h; the PS2 layer, from allocateIn on):
// aligned, cleared blocks from the heap for DMA and the GS, and the main
// region, a level's memory (a block the game sets aside: setMainRegion),
// taken from the top while it has room, else from the heap; and ZGE4's count
// of the bytes the C library's heap has left (mallocFreeBytes).

#include "platform.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "sce/libc.h"

#include <cstdint>
#include "sym/zge/platform/memory.cpp.h"

namespace zge
{
    constexpr uint32_t kCacheLineBytes = 64;


    namespace
    {
        constexpr uint32_t kHeapFree = sym::heapFree;         // (block)
        constexpr uint32_t kReleaseRegionTop = sym::releaseRegionTop; // (region, memory)
        constexpr uint32_t kRegionFreeBytes = sym::regionFreeBytes;  // (region) -> bytes
        constexpr uint32_t kRegionContains = sym::regionContains;   // (region, memory) -> 1 when inside it
        constexpr uint32_t kFlushCache = sym::flushCache;       // FlushCache(0): the data cache written back

        constexpr uint32_t kDmaAlignment = 0x80u;
        constexpr uint32_t kSmallAlignment = 0x40u;

        // The main region (g_mainRegion): 0 when none.
        inline ee::Global<uint32_t, sym::g_mainRegion> g_mainRegion;
    }

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
    }

    // allocateIn: `bytes` of a kind (MemoryKind), or 0 for an unknown
    // kind; the data cache written back after when asked (memory DMA reads
    // at once).
    uint32_t allocateIn(uint32_t bytes, uint32_t kind, uint32_t writeBack)
    {
        if (kind >= kMemoryKinds)
            return 0;
        // The kind's case .
        uint32_t memory = 0;
        uint32_t size = bytes;
        if (kind == kMemoryMain)
            memory = allocateMemory(size);
        else
        {
            uint32_t alignment = kDmaAlignment;
            if (kind == kMemoryAligned64)
                alignment = kSmallAlignment;
            else if constexpr (game::readable::kIsBmx) // (ZGE4: only packets and movie memory rounded, not its seventh kind)
            {
                if (kind == kMemoryPackets || kind == kMemoryMovie)
                    size = (size + kCacheLineBytes - 1) & ~(kCacheLineBytes - 1);
            }
            else if (kind != kMemoryTexels)
            {
                if (kind != kMemoryCluts)
                    size = (size + kCacheLineBytes - 1) & ~(kCacheLineBytes - 1);
            }
            const uint32_t block = allocateAligned(alignment, size);
            memory = memoryFill(block, 0, size);
        }
        if (writeBack != 0)
            flushCache(0);
        return memory;
    }
    READABLE_FUNCTION_WAITS(allocateIn, Verified);

    // reallocateIn: memory of a kind grown or moved (the heap's reallocate
    // for every kind; an unknown kind keeps it as it is).
    uint32_t reallocateIn(ee::Address memory, uint32_t bytes, uint32_t kind)
    {
        uint32_t result = memory;
        if (kind < kMemoryKinds)
            result = reallocate(memory, bytes);
        return result;
    }
    READABLE_FUNCTION_WAITS(reallocateIn, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t heapFree(uint32_t block, uint32_t kind, uint32_t memory) { return ee::tailCall<int32_t>(kHeapFree, block, kind, memory); }
        int32_t heapFree(uint32_t block) { return ee::tailCall<int32_t>(kHeapFree, block); }
        }
    }

    // freeIn: memory of a kind handed back to the heap (all kinds,
    // the main one too: bug kept, a block from the main region goes to the
    // heap's free as well); the heap's result, 0 for an unknown kind. The
    // tail jumps hand the kind and the memory's copy on (the free reads
    // neither).
    int32_t freeIn(ee::Address memory, uint32_t kind)
    {
        if (kind >= kMemoryKinds)
            return 0;
        if constexpr (game::readable::kIsBmx) // (ZGE4 frees every kind with a tail jump)
            return tail::heapFree(memory, kind, memory);
        if (kind == kMemoryMain)
        {
            return tail::heapFree(memory, kind, memory); // the original's tail jump
        }
        if (kind == kMemoryMovie)
        {
            const int32_t result = heapFree(memory);
            return result;
        }
        const int32_t result = heapFree(memory);
        return result;
    }
    READABLE_FUNCTION_WAITS(freeIn, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t regionFreeBytes(uint32_t regionAddress) { return ee::game<uint32_t>(kRegionFreeBytes, regionAddress); }
    }

    // allocateMemory: `bytes` from the main region while it has more free,
    // else from the heap. ZGE2 allocates from the main region alone (DM2's
    // allocateMemory: no free bytes asked, no heap); ZGE3 added the heap.
    uint32_t allocateMemory(uint32_t bytes)
    {
        if constexpr (game::readable::kIsDm2)
            return regionAllocate(ee::Ptr<MemoryRegion>(*g_mainRegion), bytes);
        const uint32_t free = regionFreeBytes(*g_mainRegion);
        const uint32_t region = *g_mainRegion;
        if (bytes < free)
        {
            const uint32_t memory = regionAllocate(ee::Ptr<MemoryRegion>(region), bytes);
            return memory;
        }
        const uint32_t memory = heapAllocate(bytes);
        return memory;
    }
    READABLE_FUNCTION_WAITS(allocateMemory, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t regionContains(uint32_t regionAddress, uint32_t memory) { return ee::game<int32_t>(kRegionContains, regionAddress, memory); }
        namespace tail
        {
        int32_t releaseRegionTop(uint32_t regionAddress, uint32_t memory) { return ee::tailCall<int32_t>(kReleaseRegionTop, regionAddress, memory); }
        }
    }

    // freeMemory: memory from allocateMemory handed back: to the main
    // region when it is in it (its top released), else to the heap (the
    // original's tail jumps).
    // ZGE2's memory is all the main region's: its top released, with no test.
    int32_t freeMemory(ee::Address memory)
    {
        if constexpr (game::readable::kIsDm2)
            return tail::releaseRegionTop(*g_mainRegion, memory);
        if (regionContains(*g_mainRegion, memory) != 0)
            return tail::releaseRegionTop(*g_mainRegion, memory);
        return tail::heapFree(memory);
    }
    READABLE_FUNCTION_WAITS(freeMemory, Verified);

    // mainRegionFreeBytes: the main region's free bytes.
    int32_t mainRegionFreeBytes()
    {
        return static_cast<int32_t>(regionFreeBytes(*g_mainRegion));
    }
    READABLE_FUNCTION(mainRegionFreeBytes, Verified);

    // setMainRegion: the main region made at `base` (`bytes` long), or with
    // base 0 freed; the callee's result.
    int32_t setMainRegion(uint32_t base, uint32_t bytes)
    {
        if (base != 0)
        {
            const ee::Ptr<MemoryRegion> region = newMemoryRegion(base, bytes);
            *g_mainRegion = ee::Address(region);
            return static_cast<int32_t>(ee::Address(region));
        }
        const int32_t result = static_cast<int32_t>(freeMemoryRegion(ee::Ptr<MemoryRegion>(*g_mainRegion)));
        *g_mainRegion = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(setMainRegion, Verified);

    namespace
    {
        // mallocFreeBytes's top of memory: a constant of the link (its high and low halves are relocated, as
        // crt0's link constants are), -1 in BMX XXX's link, which stands for none: then 128 MB is the top.
        // crt0 passes two link constants of that value, SetupThread's stack and SetupHeap's heap size; which of
        // them this one is, the machine code cannot tell (both absolute, both -1).
        constexpr uint32_t kLinkedMemoryTop = 0xFFFFFFFFu;
        constexpr uint32_t kNoLinkedTop = 0xFFFFFFFFu;
        constexpr uint32_t kTopWithoutLink = 128u << 20;
    }

    // mallocFreeBytes (ZGE4 only; the PS2 half of freeHeapBytes): the bytes the C library's heap has left: the
    // memory from the program's end to the top, less what malloc has in use, plus what it holds free. Bug kept:
    // malloc's bytes in use already leave its free bytes out, so those are counted twice.
    inline uint32_t mallocFreeBytes()
    {
        ee::Local<sce::libc::MallocInfo> heap;
        sce::libc::mallinfo(heap);
        const uint32_t top = kLinkedMemoryTop == kNoLinkedTop ? kTopWithoutLink : kLinkedMemoryTop;
        return top - sym::g_programEnd - static_cast<uint32_t>(heap->uordblks) + static_cast<uint32_t>(heap->fordblks);
    }
    READABLE_FUNCTION_ONLY(bmx, mallocFreeBytes);
}
