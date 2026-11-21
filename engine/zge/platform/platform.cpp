// The PS2 layer's small services (zge/platform/platform.h; gameClock-shutDownMatrixUnit,
// copyTranslation, matrixCopyRotation): the clock, debug output, what the interrupt handlers
// save and restore (gp, the float registers), the scratchpad's save, IOP RPC
// calls with their send data made DMA-safe, and two matrix row copies.

#include "ee/io.h"
#include "platform.h"
#include "sce/eeregs.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/debug/cycle_counter.h"
#include "zge/math/vector.h"

#include <cstdint>
#include "sym/zge/platform/platform.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kScePrintf = sym::scePrintf;
        constexpr uint32_t kSceWrite = sym::sceWrite;      // (descriptor, data, bytes)
        constexpr uint32_t kSceSifCallRpc = sym::sceSifCallRpc;
        constexpr uint32_t kLibcMemcpy = sym::memcpy;

        // debugWrite's copy of the text (debugTextBuffer).
        constexpr uint32_t kDebugTextBuffer = sym::debugTextBuffer;

        // The interrupt handlers' saves: gp (savedGlobalPointer) and f0..f31 (savedFloatRegisters).
        inline ee::Global<uint32_t, sym::savedGlobalPointer> g_savedGlobalPointer;
        inline ee::Global<ee::Float[32], sym::savedFloatRegisters> g_savedFloatRegisters;

        // The scratchpad (16 KB) and its save (scratchpadSave).
        constexpr uint32_t kScratchpadSave = sym::scratchpadSave;
        constexpr uint32_t kScratchpadBytes = 0x4000u;
        constexpr uint32_t kRowBytes = 16; // a matrix row

        // callIopRpc's DMA-safe copy of small send data (rpcSendBuffer, 512
        // bytes): SIF DMA moves 64-byte aligned blocks of whole 64-byte units.
        constexpr uint32_t kRpcSendBuffer = sym::rpcSendBuffer;
        constexpr uint32_t kRpcSendBufferBytes = 0x200u;
        constexpr uint32_t kSifDmaUnit = 0x40u;

        inline ee::Global<uint32_t, sym::g_matrixUnitReady> g_matrixUnitReady;

    }

    // gameClock: the platform's clock: milliseconds since start (the
    // cycle count's; collisionClock and the timers read it).
    uint64_t gameClock()
    {
        return timeMilliseconds();
    }
    READABLE_FUNCTION(gameClock, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t scePrintf(uint32_t text) { return ee::game<int32_t>(kScePrintf, text); }
        namespace tail
        {
        int32_t sceWrite(uint32_t descriptor, uint32_t data, uint32_t bytes) { return ee::tailCall<int32_t>(kSceWrite, descriptor, data, bytes); }
        int32_t scePrintf(uint32_t text) { return ee::tailCall<int32_t>(kScePrintf, text); }
        }
    }

    // debugWrite: debug text out: to the console (scePrintf, the text as
    // its format with the caller's other registers) unless it is off, then to
    // the debug file when one is open (a copy, with its terminator; the
    // original's tail jump). The file's descriptor when none is open.
    int32_t debugWrite(uint32_t text)
    {
        if (g_platformSettings->consoleOff == 0)
            scePrintf(text);
        const int32_t file = g_platformSettings->debugFile;
        if (file < 0)
            return file;
        stringCopy(kDebugTextBuffer, text);
        const int32_t length = stringLength(text);
        return tail::sceWrite(g_platformSettings->debugFile, kDebugTextBuffer, length + 1);
    }

    // debugWriteConsoleOrFile: ZGE2's debugWrite: the text to the console
    // (scePrintf) while it is on, else to the debug file unless ProView is
    // the debugger (ZGE3 writes the file too while the console is on, and
    // only when there is one). scePrintf's or sceWrite's result.
    int32_t debugWriteConsoleOrFile(uint32_t text)
    {
        if (g_platformSettings->consoleOff == 0)
            return tail::scePrintf(text); // (the original's tail jump)
        if (g_iopBoot->debugger == kProView)
            return static_cast<int32_t>(ee::high(kIopBootAddress)); // (what the original leaves in its result)
        stringCopy(kDebugTextBuffer, text);
        const int32_t length = stringLength(text);
        return tail::sceWrite(g_platformSettings->debugFile, kDebugTextBuffer, length + 1);
    }
    READABLE_FUNCTION_OWN(debugWrite, Verified, dm2, debugWriteConsoleOrFile);

    // saveGlobalPointer: the game's global pointer kept for the interrupt handlers.
    void saveGlobalPointer()
    {
        *g_savedGlobalPointer = ee::io::globalPointerRegister();
    }
    READABLE_FUNCTION(saveGlobalPointer, Verified);

    // frameRenderingNoop (ZGE2 only): an empty function where ZGE3 keeps
    // saveGlobalPointer (DM2 has that one beside its DMA set-up); ZGE2's
    // renderFrame calls it with 1 before it draws and 0 after. No build has
    // a body or a name for it.
    inline void frameRenderingNoop(uint32_t)
    {
    }
    READABLE_FUNCTION_ONLY(dm2, frameRenderingNoop);

    // restoreGlobalPointer: an interrupt handler's global pointer made the game's
    // again. It holds the same value all game long, so this changes nothing here.
    void restoreGlobalPointer()
    {
    }
    READABLE_FUNCTION(restoreGlobalPointer, Verified);

    // saveFloatRegisters: an interrupt handler's float registers kept: all 32 as
    // the interrupted code left them.
    void saveFloatRegisters()
    {
        for (int f = 0; f < 32; ++f)
            (*g_savedFloatRegisters)[f] = ee::io::floatRegister(f);
    }
    READABLE_FUNCTION(saveFloatRegisters, Verified);

    // restoreFloatRegisters: the float registers put back for the interrupted code.
    void restoreFloatRegisters()
    {
        for (int f = 0; f < 32; ++f)
            ee::io::setFloatRegister(f, (*g_savedFloatRegisters)[f]);
    }
    READABLE_FUNCTION(restoreFloatRegisters, Verified);

    // saveScratchpad: the scratchpad kept (memoryCopy's result: the save).
    uint32_t saveScratchpad()
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            return game::tail(memoryCopy, kScratchpadSave, sce::SPR, kScratchpadBytes);
        return memoryCopy(kScratchpadSave, sce::SPR, kScratchpadBytes);
    }
    READABLE_FUNCTION(saveScratchpad, Verified);

    // restoreScratchpad: the scratchpad put back.
    void restoreScratchpad()
    {
        if constexpr (game::readable::kIsBmx) // (ZGE4's is the copy's tail call)
            game::tail(memoryCopy, sce::SPR, kScratchpadSave, kScratchpadBytes);
        else
            memoryCopy(sce::SPR, kScratchpadSave, kScratchpadBytes);
    }
    READABLE_FUNCTION(restoreScratchpad, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void memcpy(uint32_t dst0, uint32_t src0, uint32_t len0) { ee::game(kLibcMemcpy, dst0, src0, len0); }
        int32_t sceSifCallRpc(uint32_t client, uint32_t function, uint32_t mode, uint32_t send, uint32_t sendBytes, uint32_t receive, uint32_t receiveBytes, uint32_t endFunction) { return ee::game<int32_t>(kSceSifCallRpc, client, function, mode, send, sendBytes, receive, receiveBytes, endFunction); }
    }

    // callIopRpc: sceSifCallRpc, with send data the SIF cannot take as it
    // is (not 64-byte aligned, or not whole 64-byte units) copied to an
    // aligned buffer first when it fits (512 bytes); the ninth argument (the
    // end function's data, on the stack) handed on.
    int32_t callIopRpc(uint32_t client, uint32_t function, uint32_t mode, uint32_t send, uint32_t sendBytes,
                       uint32_t receive, uint32_t receiveBytes, uint32_t endFunction)
    {
        ee::Local<uint32_t> endFunctionData; // sceSifCallRpc's ninth argument, on the stack
        const bool fits = static_cast<int32_t>(sendBytes) <= static_cast<int32_t>(kRpcSendBufferBytes);
        const bool unsuited = (sendBytes != 0 && (send & (kSifDmaUnit - 1)) != 0) || (sendBytes & (kSifDmaUnit - 1)) != 0;
        if (unsuited && fits)
        {
            memcpy(kRpcSendBuffer, send, sendBytes);
            send = kRpcSendBuffer;
        }
        *endFunctionData = ee::io::stackArgument<uint32_t>(0);
        return sceSifCallRpc(client, function, mode, send, sendBytes, receive, receiveBytes, endFunction);
    }
    READABLE_FUNCTION_WAITS(callIopRpc, Verified);

    // initMatrixUnit (no caller): the matrix unit's flag set. 1.
    uint32_t initMatrixUnit()
    {
        *g_matrixUnitReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initMatrixUnit, Verified);

    // shutDownMatrixUnit (no caller): the flag cleared. 1.
    uint32_t shutDownMatrixUnit()
    {
        if (*g_matrixUnitReady == 0)
            return 1;
        *g_matrixUnitReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownMatrixUnit, Draft);

    // copyTranslation: a matrix's translation (row 3) copied; dst. ZGE2
    // copies its three floats, the w left as it was; ZGE3 the row whole.
    uint32_t copyTranslation(ee::Address dst, ee::Address src)
    {
        if constexpr (game::readable::kIsDm2)
        {
            for (uint32_t column = 0; column < 3; ++column)
            {
                const uint32_t offset = 3 * kRowBytes + column * 4u;
                *ee::Ptr<ee::Float>(dst + offset) = *ee::Ptr<ee::Float>(src + offset);
            }
            return dst;
        }
        copyQuadword(dst + 3 * kRowBytes, src + 3 * kRowBytes);
        return dst;
    }
    READABLE_FUNCTION(copyTranslation, Verified);

    // matrixCopyRotation: a matrix's rotation (rows 2, 1, 0) copied; dst.
    // ZGE2 copies the nine floats of rows 0 to 2 one at a time, each row's w
    // left as it was; ZGE3 copies the three rows whole.
    uint32_t matrixCopyRotation(ee::Address dst, ee::Address src)
    {
        if constexpr (game::readable::kIsDm2)
        {
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t column = 0; column < 3; ++column)
                {
                    const uint32_t offset = row * kRowBytes + column * 4u;
                    *ee::Ptr<ee::Float>(dst + offset) = *ee::Ptr<ee::Float>(src + offset);
                }
            return dst;
        }
        copyQuadword(dst + 2 * kRowBytes, src + 2 * kRowBytes);
        copyQuadword(dst + kRowBytes, src + kRowBytes);
        copyQuadword(dst, src);
        return dst;
    }
    READABLE_FUNCTION(matrixCopyRotation, Verified);

    // copyRotationAndTranslation: a matrix copied as its translation and its
    // rotation (no row's w); dst. ZGE2 has it as a function its matrixScale
    // and rider code call.
    inline uint32_t copyRotationAndTranslation(ee::Address dst, ee::Address src)
    {
        copyTranslation(dst, src);
        matrixCopyRotation(dst, src);
        return dst;
    }
    READABLE_FUNCTION_ONLY(dm2, copyRotationAndTranslation);

    namespace
    {
        constexpr uint32_t kAssertFailed = sym::assertFailed; // assertFailed (readable: assertFailed, Draft)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(assertFailedTailint32_t, ee::tailCall<int32_t>, kAssertFailed);
        }
    }

    // Forwards its four arguments to assertFailed (assertFailed; was showLegalScreenAlias); its layer from
    // its address, nothing calls it.
    int32_t platformAssertFailed(uint32_t why, uint32_t file, uint32_t line, uint32_t function)
    {
        return routed::assertFailedTailint32_t(why, file, line, function);
    }
    READABLE_FUNCTION_WAITS(platformAssertFailed, Draft);
}
