// Room for the collection's own content in one of the Z-Axis engine's small
// heaps, from host memory (zge_heap_room.h).
#include "zge_heap_room.h"

#include "ps2_runtime.h"

#include <cstring>
#include <iostream>

namespace zge_host
{
    namespace
    {
        // The engine's heap and its blocks as game/zge/memory/heaps.h has
        // them (the same at the start in every game: ZGE4's search start
        // comes after these): a heap is one allocation cut into blocks, each
        // a header and its memory, linked by the neighbours' addresses, the
        // last a used sentinel.
        struct HeapHead
        {
            uint32_t first; // the first block's header
            uint32_t last;  // the sentinel's header
        };
        struct BlockHeader
        {
            uint32_t next;  // the next block's header (0 after the sentinel)
            uint32_t prev;  // the previous block's header (0 before the first)
            uint32_t free;  // 1 when free
            uint32_t bytes; // what heapTake was asked for
        };
        static_assert(sizeof(BlockHeader) == 0x10);

        template <typename T>
        bool readGuest(uint8_t *rdram, uint32_t address, T &out)
        {
            const uint8_t *p = address ? getConstMemPtr(rdram, address) : nullptr;
            if (!p)
                return false;
            std::memcpy(&out, p, sizeof(T));
            return true;
        }

        template <typename T>
        bool writeGuest(uint8_t *rdram, uint32_t address, const T &value)
        {
            uint8_t *p = address ? getMemPtr(rdram, address) : nullptr;
            if (!p)
                return false;
            std::memcpy(p, &value, sizeof(T));
            return true;
        }
    }

    bool extendHeap(PS2Runtime &runtime, uint8_t *rdram, uint32_t heap, uint32_t bytes)
    {
        HeapHead head{};
        BlockHeader sentinel{};
        bytes = (bytes + 15u) & ~15u;
        if (bytes < 2u * sizeof(BlockHeader) || !readGuest(rdram, heap, head) || !readGuest(rdram, head.last, sentinel) ||
            sentinel.next != 0u || sentinel.free != 0u)
            return false;
        // The top of extended memory, kept out of the heaps there (the
        // Zoo's, DM2's Xbox levels', malloc's past the 32 MB): they fill
        // from the bottom, so all they hold sits where it would without the
        // room. The same range at every boot of the runtime.
        const uint32_t room = runtime.reserveExtendedTop(bytes);
        if (room == 0u)
            return false;
        const uint32_t end = room + bytes - sizeof(BlockHeader);
        // The heap's sentinel stays used; its room becomes the gap up to
        // the new block. The new block is free, its room up to the new
        // sentinel.
        sentinel.next = room;
        const BlockHeader block{end, head.last, 1u, 0u};
        const BlockHeader last{0u, room, 0u, 0u};
        const uint32_t oldLast = head.last;
        head.last = end;
        if (!writeGuest(rdram, room, block) || !writeGuest(rdram, end, last) || !writeGuest(rdram, oldLast, sentinel) ||
            !writeGuest(rdram, heap, head))
            return false;
        return true;
    }
}
