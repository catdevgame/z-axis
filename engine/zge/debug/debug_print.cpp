// The debug text overlay's printf (zdebug.c; debug_draw.cpp has the rest
// of the debug drawing and text). debugPrint is variadic in the original
// (x, y, format, ...): a call names the words its format reads (debug.h,
// ee::VarArg).

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/debug/debug.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/zge/debug/debug_print.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kVsprintf = sym::vsprintf;
        void vsprintf(uint32_t str, uint32_t format, ee::Ptr<uint64_t> ap) { ee::game(kVsprintf, str, format, ap); }
    }

    // debugPrint: a formatted string into the buffer, or the overflow
    // flag; returns its length, 0 if it isn't kept.
    uint64_t debugPrint(uint32_t x, uint32_t y, ee::Address format, ee::VarArg first, ee::VarArg second, ee::VarArg third,
                        ee::VarArg fourth, ee::VarArg fifth)
    {
        ee::Local<uint64_t> ap;
        ee::Local<uint64_t> secondSlot;
        ee::Local<uint64_t> thirdSlot;
        ee::Local<uint64_t> fourthSlot;
        ee::Local<uint64_t> fifthSlot;
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const uint32_t enabled = *g_debugEnabled;
        *ap = first;
        *secondSlot = second;
        *thirdSlot = third;
        *fourthSlot = fourth;
        *fifthSlot = fifth;
        if (enabled == 0)
            return 0;
        vsprintf(kDebugFormatted, format, ap);
        const int32_t length = stringLength(kDebugFormatted);
        const uint32_t cursor = *g_debugTextCursor;
        const uint64_t lengthZeroExtended = static_cast<uint32_t>(length);
        if (cursor + (static_cast<uint32_t>(length) + 6) >= kDebugTextEnd)
        {
            *g_debugTextOverflowed = 1;
            return 0;
        }
        const ee::Ptr<DebugTextEntry> line(cursor);
        line->x = static_cast<int16_t>(screenX);
        const uint32_t text = cursor + 4;
        line->y = static_cast<int16_t>(screenY);
        *g_debugTextCursor = text;
        stringCopy(text, kDebugFormatted);
        *g_debugTextCursor = static_cast<uint32_t>(length) + *g_debugTextCursor + 2 & ~1u;
        return lengthZeroExtended;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION(debugPrint, Verified)); // (BMX XXX's: games/bmx/debug/debug_print.cpp)
}
