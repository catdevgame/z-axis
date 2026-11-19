#pragma once

// The EE's views of one byte of RAM: its cached address, its uncached mirror
// (0x20000000 up: what records shared with DMA are written and listed
// through) and the physical address the DMA controller takes (the mirror
// bits cleared). The game's C makes them with a mask on the pointer (the
// SDK's UNCACHED and DMA_ADDR); these do the same on an ee::Ptr, keeping its
// type, and dmaAddress gives the number a DMA register is written.

#include "ee/ee.h"

#include <cstdint>

namespace ee
{
    template <typename T>
    constexpr Ptr<T> uncachedMirror(Ptr<T> pointer)
    {
        return Ptr<T>(pointer.address | 0x20000000u);
    }

    template <typename T>
    constexpr Ptr<T> physical(Ptr<T> pointer)
    {
        return Ptr<T>(pointer.address & 0x0FFFFFFFu);
    }

    // The same on an address kept as a number (a pool's block, a packet's
    // buffer): its uncached mirror, its uncached accelerated mirror
    // (0x30000000 up) and its physical address.
    constexpr uint32_t uncachedMirror(uint32_t address) { return address | 0x20000000u; }
    constexpr uint32_t uncachedAcceleratedMirror(uint32_t address) { return address | 0x30000000u; }
    constexpr uint32_t physical(uint32_t address) { return address & 0x0FFFFFFFu; }

    // The number a DMA channel's address register takes for a pointer (its
    // physical address; the SDK's DMA_ADDR).
    template <typename T>
    constexpr uint32_t dmaAddress(Ptr<T> pointer)
    {
        return pointer.address & 0x0FFFFFFFu;
    }

    // The bytes from `start` to `end` (end - start), as the game subtracts
    // the two addresses: a wrapped 32-bit difference.
    template <typename From, typename To>
    constexpr int32_t bytesFrom(Ptr<From> start, Ptr<To> end)
    {
        return static_cast<int32_t>(end.address - start.address);
    }
}
