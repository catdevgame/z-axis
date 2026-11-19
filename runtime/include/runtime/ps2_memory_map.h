#ifndef PS2_MEMORY_MAP_H
#define PS2_MEMORY_MAP_H

// The EE's address map: where RAM, the scratchpad, the registers and the VU memories are. Freestanding (no library
// header): the game modules' code (game_module/) includes it as the runtime does.

#include <stddef.h>
#include <stdint.h>

// The PS2's main memory as the game sees it: 32 MB (GetMemorySize, the
// default stacks, the runtime's own areas at its top, RAM dumps).
constexpr uint32_t PS2_LEGACY_RAM_SIZE = 32u * 1024u * 1024u;
// The host's copy is larger (extended memory, like a 128 MB devkit's but
// 64 MB): what lies above PS2_LEGACY_RAM_SIZE is only used once a game
// override gives it to the game's heap (PS2Runtime::addGuestHeapRegion), as
// the first-fit heap hands it out only when the 32 MB are full. Addresses
// that mirrored the 32 MB on a PS2 reach it instead (no game here uses them).
constexpr uint32_t PS2_RAM_SIZE = 64u * 1024u * 1024u;
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;
constexpr uint32_t PS2_RAM_BASE = 0x00000000;          // Physical base of RDRAM
constexpr uint32_t PS2_SCRATCHPAD_BASE = 0x70000000;
constexpr uint32_t PS2_SCRATCHPAD_ALIAS_BASE = 0xF0000000;
constexpr uint32_t PS2_SCRATCHPAD_SIZE = 16u * 1024u;  // 16KB
constexpr uint32_t PS2_IO_BASE = 0x10000000;           // Base for many I/O regs (Timers, DMAC, INTC)
constexpr uint32_t PS2_IO_SIZE = 0x10000;              // 64KB
constexpr uint32_t PS2_BIOS_BASE = 0x1FC00000;         // Or BFC00000 depending on KSEG
constexpr uint32_t PS2_BIOS_SIZE = 4u * 1024u * 1024u; // 4MB

constexpr uint32_t PS2_VU0_CODE_BASE = 0x11000000; // Base address as seen from EE
constexpr uint32_t PS2_VU0_DATA_BASE = 0x11004000;
constexpr uint32_t PS2_VU0_CODE_SIZE = 4u * 1024u; // 4KB Micro Memory
constexpr uint32_t PS2_VU0_DATA_SIZE = 4u * 1024u; // 4KB Data Memory (VU Mem)

constexpr uint32_t PS2_VU1_CODE_BASE = 0x11008000;
constexpr uint32_t PS2_VU1_DATA_BASE = 0x1100C000;
constexpr uint32_t PS2_VU1_MEM_BASE = PS2_VU1_CODE_BASE; // Alias used by older code paths
constexpr uint32_t PS2_VU1_CODE_SIZE = 16u * 1024u;      // 16KB Micro Memory
constexpr uint32_t PS2_VU1_DATA_SIZE = 16u * 1024u;      // 16KB Data Memory (VU Mem)

constexpr uint32_t PS2_GS_BASE = 0x12000000;
constexpr uint32_t PS2_GS_PRIV_REG_BASE = PS2_GS_BASE; // GS Privileged Registers
constexpr uint32_t PS2_GS_PRIV_REG_SIZE = 0x2000;
constexpr size_t PS2_GS_VRAM_SIZE = 4u * 1024u * 1024u; // 4MB GS VRAM

static_assert((PS2_RAM_SIZE & (PS2_RAM_SIZE - 1u)) == 0u, "PS2_RAM_SIZE must be a power of two");
static_assert(PS2_RAM_MASK == (PS2_RAM_SIZE - 1u), "PS2_RAM_MASK must match PS2_RAM_SIZE");

#endif // PS2_MEMORY_MAP_H
