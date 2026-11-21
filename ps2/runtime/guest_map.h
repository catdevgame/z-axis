#pragma once

// The guest's 32-bit address space as one host reservation: a guest address is the host's base + address.
//
// EE RAM is one shared memory object mapped at its own address and again at every address the EE folds onto it
// (the uncached mirrors at 0x20000000 and 0x30000000, kseg0 and kseg1 at 0x80000000 and 0xA0000000, and the rest
// of the space a 64 MB step at a time, as the runtime's resolution takes an address modulo RAM's size), and the
// scratchpad is mapped at 0x70000000 and at its alias 0xF0000000. So the readable game's every access
// (ee::at<T>, ps2/ee/ee.h) is one add: the special cases are where the mapping puts them, not a test on every
// access. The recompiled code's loads and stores are unchanged (rdram is the base: RAM's first view).
//
// What the mapping does not do: I/O registers (0x10000000..., the GS's, the VUs' memories, the BIOS) are RAM's
// bytes there, as they were for ee::at before; the readable game reaches the hardware through ee::io
// (ps2/ee/io.h), which goes to the runtime, as the recompiled code's special-address path does. The 48 KB after
// the scratchpad's 16 KB at each of its addresses are the scratchpad window's own (64 KB: Windows maps views in
// 64 KB steps), where an address modulo RAM's size gave RAM's bytes: nothing there is the game's.
//
// Mac (shm_open), Linux and Android (memfd_create), Windows (placeholders: VirtualAlloc2 and MapViewOfFile3).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace guest_map
{
    // The span reserved: the 32-bit space and a guard after it, where a record read at an address just below 4 GB
    // runs on (RAM's first bytes, as 32-bit address arithmetic would wrap).
    constexpr uint64_t kSpace = 1ull << 32;
    constexpr uint64_t kGuard = 0x10000u;
    // The scratchpad (16 KB at 0x70000000 and 0xF0000000) is the first part of a 64 KB window.
    constexpr uint32_t kScratchpadAddress = 0x70000000u;
    constexpr uint32_t kScratchpadAlias = 0xF0000000u;
    constexpr uint32_t kScratchpadWindow = 0x10000u;

    // Which addresses show RAM.
    enum class Layout
    {
        // The game's: every address the runtime's resolution folds onto RAM, the scratchpad at its two addresses.
        Game,
        // zx-fncheck's replay memory: the game's, but for the scratchpad's window at RAM's size too (the
        // recordings' key for it, fncheck::kScratchpadKey, where the game has RAM's first 64 KB again), so the
        // recordings' memory keys are addresses from the base. The checker opens a key's page there, and a page
        // of another view of it (a mirror, the scratchpad's own addresses) when an access faults on it
        // (fncheck_main.cpp GuestRam).
        Checker,
    };

    // One mapping: guest addresses [address, address + bytes) show the object's [offset, offset + bytes).
    struct View
    {
        uint64_t address = 0;
        uint64_t offset = 0;
        uint64_t bytes = 0;
    };

    class Map
    {
    public:
        Map() = default;
        Map(const Map &) = delete;
        Map &operator=(const Map &) = delete;
        ~Map() { release(); }

        // RAM of ramBytes (a power of two of at least 64 KB that divides 4 GB) and the scratchpad window after it in
        // one object, zeroed, mapped as `layout` says; `access` false maps every view without access (the checker
        // opens pages itself). False if the host would not reserve or map it (the reason on stderr).
        bool create(uint32_t ramBytes, Layout layout, bool access = true);
        void release();

        bool valid() const { return m_base != nullptr; }
        // Guest address 0: RAM's first view.
        uint8_t *base() const { return m_base; }
        // The scratchpad at its own address.
        uint8_t *scratchpad() const { return m_base ? m_base + kScratchpadAddress : nullptr; }
        uint32_t ramBytes() const { return m_ramBytes; }
        const std::vector<View> &views() const { return m_views; }
        // Whether a host pointer is in the reservation.
        bool contains(const void *pointer) const
        {
            const uint8_t *p = static_cast<const uint8_t *>(pointer);
            return m_base && p >= m_base && p < m_base + kSpace + kGuard;
        }
        // Access to the object's [offset, offset + bytes) on (read and write) or off, in every view that shows it.
        // Page-aligned; false if the host refused one.
        bool protect(uint64_t offset, uint64_t bytes, bool access);
        // The same for the map's own [address, address + bytes) (guest addresses, in one view).
        bool protectAt(uint64_t address, uint64_t bytes, bool access);

    private:
        bool map(const View &view, bool access);

        uint8_t *m_base = nullptr;
        uint32_t m_ramBytes = 0;
        std::vector<View> m_views;
#if defined(_WIN32)
        void *m_section = nullptr;
        std::vector<View> m_holes; // placeholders no view replaced (released apart)
#else
        int m_fd = -1;
#endif
    };

    // The views of a layout, in address order (create's; tests check them without mapping anything).
    std::vector<View> viewsOf(uint32_t ramBytes, Layout layout);
}
