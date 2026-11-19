#pragma once

// Room for the collection's own content in one of the Z-Axis engine's small
// heaps (game/zge/memory/heaps.h), from host memory: guest RAM above the
// PS2's 32 MB, so the game's own memory, and where everything in it lands,
// stays as the game made it. One body for every game on the engine.

#include <cstdint>

class PS2Runtime;

namespace zge_host
{
    // The heap at guest `heap` (a Heap: its first block, its sentinel) given
    // `bytes` more room after its own: a block of extended memory linked in
    // after the heap's sentinel, at the top of the RAM above the 32 MB, which
    // no other heap gets (PS2Runtime::reserveExtendedTop: what the heaps
    // there hold sits where it would without the room). The heap's sentinel
    // stays a used block whose room is the gap between the two (never free,
    // so never taken or merged), and the new end has a sentinel of its own.
    // heapTake looks through the heap's own blocks first, as before, and
    // takes from the new room only what they cannot give; heapGive frees a
    // block there as anywhere. False (the heap as it was) when the top of
    // extended memory cannot be had or the heap is not one.
    bool extendHeap(PS2Runtime &runtime, uint8_t *rdram, uint32_t heap, uint32_t bytes);
}
