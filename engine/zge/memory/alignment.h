#pragma once

// The engine's allocators hand out memory in 16-byte steps: a quadword, what
// DMA and VU0 loads want. The heaps, the handle regions, the record pools and
// the memory regions round every size up to it.

#include <cstdint>

namespace zge
{
    constexpr uint32_t kAllocationAlignment = 16u;

    // `value` rounded up to a multiple of `alignment` (a power of two).
    constexpr uint32_t roundUp(uint32_t value, uint32_t alignment)
    {
        return (value + (alignment - 1u)) & ~(alignment - 1u);
    }

    // `bytes` rounded up to the next multiple of 16.
    constexpr uint32_t roundUp16(uint32_t bytes)
    {
        return (bytes + kAllocationAlignment - 1u) & ~(kAllocationAlignment - 1u);
    }
}
