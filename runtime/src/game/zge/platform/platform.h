#pragma once

// The engine's PS2 layer (zge/ps2, in the ps2Movie.c block; zge/platform):
// start-up and the main loop (boot.cpp), the engine's systems up and down and
// the frame's step (engine.cpp), the clock and debug output, memory
// by kind (memory.cpp: the main region a level's data comes from, else the
// heap; aligned blocks for DMA), the interrupt handlers' register saves, the
// scratchpad's save, the IOP's RPC calls and the matrix copies the game uses
// (ps2_platform.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/platform.h.h"

namespace zge
{
    // allocateIn's kinds (the jump table's cases; the users as the engine's
    // callers pass them). All but kMemoryMain are 128-byte aligned blocks
    // from the heap (kMemoryAligned64: 64-byte), cleared; freeIn hands every
    // kind back to the heap.
    enum MemoryKind : uint32_t
    {
        kMemoryPackets = 0,   // DMA packets and buffers; the size rounded up to 64
        kMemoryAligned64 = 1, // [unknown: no caller passes it]
        kMemoryMain = 2,      // allocateMemory: the main region while it has room, else the heap
        kMemoryTexels = 3,    // texture images (ps2/gs_texture.cpp)
        kMemoryCluts = 4,     // their palettes
        kMemoryMovie = 5,     // the movie player's buffers; rounded as kMemoryPackets
    };
    // DM2's tables stop before kMemoryMovie; ZGE4 has a seventh kind (6: no
    // caller passes it), aligned as texels are.
    inline constexpr uint32_t kMemoryKinds = game::readable::perGame(6u, 5u, 7u);

    // How the IOP boots (iopBootAddress; initPlatformSettings clears it,
    // ps2SifInit reads it).
    struct IopBoot
    {
        uint8_t _00[8];      // +0x00 cleared with the rest; nothing in the game reads or writes it after
        uint32_t fromDisc;   // +0x08 modules from the disc, the IOP rebooted first
        uint32_t onDebugger; // +0x0C a development kit's debugger is attached (else: the host's files)
        uint32_t debugger;   // +0x10 kProView, else ATMON
    };
    static_assert(offsetof(IopBoot, debugger) == 0x10);
    constexpr uint32_t kIopBootAddress = sym::iopBootAddress;
    constexpr uint32_t kProView = 1u;
    inline ee::Global<IopBoot, kIopBootAddress> g_iopBoot;

    // The platform's settings (g_platformSettings, 0x50 bytes; initPlatformSettings
    // clears and fills them at start-up).
    struct PlatformSettings
    {
        uint32_t title;            // +0x00 "PS2 APP"
        uint32_t width;            // +0x04 512
        uint32_t height;           // +0x08 448
        uint32_t clearedAtStart;   // +0x0C
        uint32_t hangOnAssert;     // +0x10 assertFailed hangs (else the debugger's trap; set by nothing here)
        char devicePrefix[0x20];   // +0x14 before every file name ("cdrom0:")
        uint32_t consoleOff;       // +0x34 debugWrite leaves scePrintf out
        int32_t debugFile;         // +0x38 debugWrite's file too (a descriptor; < 0 none)
        uint8_t _3c[0x0A];
        uint16_t cdMode;           // +0x46 sceCdMmode's (2: DVD)
        uint8_t _48[8];
    };
    static_assert(offsetof(PlatformSettings, devicePrefix) == 0x14 && offsetof(PlatformSettings, consoleOff) == 0x34 &&
                  offsetof(PlatformSettings, cdMode) == 0x46 &&
                  sizeof(PlatformSettings) == 0x50);

    // The bytes initPlatformSettings clears: ZGE2's settings end at +0x3C
    // (DM2's initPlatformSettings), before the CD mode ZGE3 added.
    constexpr uint32_t kPlatformSettingsBytes = game::readable::perGame(static_cast<uint32_t>(sizeof(PlatformSettings)), 0x3Cu);

    constexpr uint32_t kPlatformSettingsAddress = sym::g_platformSettings;
    inline ee::Global<PlatformSettings, kPlatformSettingsAddress> g_platformSettings;

    // boot.cpp
    uint32_t ps2SifInit();
    uint32_t initPlatformSettings();
    uint32_t initCdDevice();
    void resetHardware();
    uint32_t reserveMallocArena(uint32_t top);
    uint32_t initPlatform();
    uint32_t engineMain(uint32_t argumentCount, uint32_t arguments);
    void assertFailed(uint32_t why, uint32_t file, uint32_t line, uint32_t function);

    // engine.cpp
    uint32_t scanArguments(uint32_t argumentCount, uint32_t arguments);
    uint32_t initEngine(uint32_t argumentCount, uint32_t arguments);
    // The rate frames are paced to (0: off) (engine.cpp).
    int32_t setMotionRate(ee::Float targetFrameRate);
    ee::Float frameSeconds();
    int32_t frameMilliseconds();
    uint32_t updateFrameRate(ee::Float seconds);
    // restartFrameRate (ZGE4 only, engine.cpp): the next frame counts as the first again (enterScene).
    void restartFrameRate();
    void setFrameCallback(uint32_t callback);
    void requestQuit();
    uint32_t shutdownEngineSystems();
    uint32_t reportEngineObjects();
    uint32_t shutdownEngine();
    int32_t runEngineHooksAlias();
    uint32_t engineStartHook();

    // memory.cpp
    uint32_t allocateIn(uint32_t bytes, uint32_t kind, uint32_t writeBack);
    uint32_t reallocateIn(ee::Address memory, uint32_t bytes, uint32_t kind);
    int32_t freeIn(ee::Address memory, uint32_t kind);
    uint32_t allocateMemory(uint32_t bytes);
    int32_t freeMemory(ee::Address memory);
    int32_t mainRegionFreeBytes();
    int32_t setMainRegion(uint32_t base, uint32_t bytes);

    // ps2_platform.cpp
    uint64_t gameClock();
    int32_t debugWrite(uint32_t text);
    void saveGlobalPointer();
    void frameRenderingNoop(uint32_t rendering); // ZGE2 only: empty (its renderFrame's)
    void restoreGlobalPointer();
    void saveFloatRegisters();
    void restoreFloatRegisters();
    uint32_t saveScratchpad();
    void restoreScratchpad();
    int32_t callIopRpc(uint32_t client, uint32_t function, uint32_t mode, uint32_t send, uint32_t sendBytes,
                       uint32_t receive, uint32_t receiveBytes, uint32_t endFunction);
    uint32_t initMatrixUnit();
    uint32_t shutDownMatrixUnit();
    uint32_t copyTranslation(ee::Address dst, ee::Address src);
    uint32_t matrixCopyRotation(ee::Address dst, ee::Address src);
    // copyRotationAndTranslation (ZGE2 only, ps2_platform.cpp): a matrix's
    // translation and rotation copied (no row's w); ZGE3 copies them inline.
    uint32_t copyRotationAndTranslation(ee::Address dst, ee::Address src);

    struct Matrix; // (zge/math/types.h)
    // The same on matrices: out gets in's rotation and translation; out's
    // address. (ZGE2 only, as copyRotationAndTranslation: reach it from a
    // step only DM2's instance keeps.)
    inline uint32_t copyMatrixRotationAndTranslation(ee::Ptr<Matrix> out, ee::Ptr<const Matrix> in)
    {
        return copyRotationAndTranslation(out, in);
    }
}

namespace zge
{
    // (ps2_platform.cpp)
    int32_t platformAssertFailed(uint32_t why, uint32_t file, uint32_t line, uint32_t function);
}

namespace zge
{
    // (timers.cpp)
    int32_t initEngineTimers(uint32_t capacity);
    int32_t shutdownEngineTimers();
}
