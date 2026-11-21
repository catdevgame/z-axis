#pragma once

// The engine's debug drawing and text (ngengine/zge3/zdebug.c;
// zge/debug/debug_draw.cpp, debug_text.cpp, debug.cpp, and debugPrint in
// debug_print.cpp): primitives added during the frame and drawn by a task,
// and a buffer of strings printed by another.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstdint>
#include "sym/zge/debug/debug.h.h"

namespace zge
{
    // The text buffer debugPrint fills and drawDebugText empties: a string's
    // place, then its characters.
    struct DebugTextEntry
    {
        int16_t x, y;
#if ZX_GAME_BMX
        uint8_t colour[4]; // (ZGE4) R, G, B, A: debugPrint's colour when it was printed
#endif
    };
    inline ee::Global<uint32_t, sym::g_debugEnabled> g_debugEnabled;
    constexpr uint32_t kDebugFormatted = sym::debugFormatted;  // a string while it is measured
    inline ee::Global<uint32_t, sym::g_debugTextCursor> g_debugTextCursor; // the buffer ends where this is kept
    constexpr uint32_t kDebugTextEnd = sym::g_debugTextCursor;
    inline ee::Global<uint32_t, sym::g_debugTextOverflowed> g_debugTextOverflowed;

    // A screen coordinate: the low 16 bits, signed.
    inline int32_t signed16(uint32_t value) { return static_cast<int16_t>(value); }

    // (debug_draw.cpp)
    uint32_t drawDebugText();
    void resetDebugText(); // ZGE4 only: the buffer emptied and the colour reset
    uint32_t newDebugPrimitive();
    int32_t initDebug();
    uint32_t shutdownDebugSystem();
    uint32_t setDebugMenuSelection(uint32_t enabled);
    uint32_t addDebugLine(ee::Address start, ee::Address end, ee::Address colour, uint32_t flags);
    int32_t addDebugText(ee::Address corners, ee::Ptr<Matrix> transform, ee::Address colour, uint32_t flags);
    uint32_t drawTriangle(ee::Address corner0, ee::Address corner1, ee::Address corner2, ee::Address colour, uint32_t flags);
    uint32_t addDebugPoint(ee::Address position, ee::Address colour, uint32_t flags, uint32_t, ee::Float radius);
    uint32_t sceneObjectsHook();

    // (debug_print.cpp) debugPrint: printf onto the debug text overlay at (x, y); the text's length, 0 if it
    // isn't kept. Variadic in the original: a call names the words its format reads (ee::VarArg).
    uint64_t debugPrint(uint32_t x, uint32_t y, ee::Address format, ee::VarArg first = {}, ee::VarArg second = {},
                        ee::VarArg third = {}, ee::VarArg fourth = {}, ee::VarArg fifth = {});
}
