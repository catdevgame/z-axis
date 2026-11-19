// BMX XXX's debug strings (ZGE4's zdebug.c; ZGE3's are zge/debug/debug_print.cpp and debug_draw.cpp): each
// string kept with the colour it is printed in (zge::DebugTextEntry::colour) and on a word, set in that colour
// through the text interface's fifth method before it is drawn. The buffer and its state are ZGE3's, less the
// shared format buffer: debugPrint formats on its own stack. resetDebugText (zge/debug/debug_draw.cpp) empties
// the buffer and puts the colour back to grey.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/debug/debug.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/debug/debug_print.cpp.h"

namespace game::bmx
{
    namespace
    {
        constexpr uint32_t kDebugTextStart = sym::debugTextStart; // the strings: x, y, colour, text, on a word
        // The strings end where the colour the next ones get is kept, 0x400 bytes after they start.
        constexpr uint32_t kDebugTextEnd = sym::g_debugPrintColour;
        constexpr uint32_t kDebugTextAlignment = 4;
        inline ee::Global<int32_t, sym::g_debugTextLineHeight> g_debugTextLineHeight;
        // The text interface's methods: the draw method (x, y, text), and ZGE4's fifth, the colour the next
        // strings are drawn in (its R, G, B, A's address).
        inline ee::Global<uint32_t, sym::g_debugDrawTextHook> g_debugDrawTextHook;
        inline ee::Global<uint32_t, sym::g_debugTextColourHook> g_debugTextColourHook;
        void drawTextThroughHook(int32_t x, int32_t y, uint32_t text) { ee::game(*g_debugDrawTextHook, x, y, text); }
        void setTextColourThroughHook(ee::Address colour) { ee::game(*g_debugTextColourHook, colour); }

        constexpr uint32_t kVsprintf = sym::vsprintf;
        void vsprintf(uint32_t str, uint32_t format, ee::Ptr<uint64_t> ap) { ee::game(kVsprintf, str, format, ap); }
    }

    // drawColouredDebugText: BMX XXX's drawDebugText (the strings' task): each
    // string set in its own colour and drawn, then the buffer emptied and the
    // colour reset. A negative y counts lines down from the last string's.
    uint32_t drawColouredDebugText()
    {
        uint32_t entry = kDebugTextStart;
        if (*zge::g_debugEnabled != 0 && entry < *zge::g_debugTextCursor)
        {
            int32_t nextY = 0;
            for (;;)
            {
                const ee::Ptr<zge::DebugTextEntry> line(entry);
                int32_t y = line->y;
                const int32_t x = line->x;
                if (y < 0)
                    y = static_cast<int32_t>(static_cast<uint32_t>(~y) * static_cast<uint32_t>(*g_debugTextLineHeight)) + nextY;
                uint32_t text = entry + static_cast<uint32_t>(sizeof(zge::DebugTextEntry));
                nextY = y + *g_debugTextLineHeight;
                setTextColourThroughHook(&line->colour);
                drawTextThroughHook(x, zge::signed16(static_cast<uint32_t>(y)), text);
                while (*ee::Ptr<int8_t>(text++) != 0)
                {
                }
                entry = text + (kDebugTextAlignment - 1) & ~(kDebugTextAlignment - 1);
                if (entry < *zge::g_debugTextCursor)
                    continue;
                break;
            }
        }
        zge::resetDebugText();
        return 1;
    }
    READABLE_FUNCTION(drawColouredDebugText, Draft);

    // debugPrintInColour: BMX XXX's debugPrint: the string formatted on its own
    // stack and kept with the colour the strings are printed in; the same
    // overflow test as ZGE3's and the same result, its length (0 if it isn't
    // kept). Variadic in the original, as zge::debugPrint.
    uint64_t debugPrintInColour(uint32_t x, uint32_t y, ee::Address format, ee::VarArg first, ee::VarArg second,
                                ee::VarArg third, ee::VarArg fourth, ee::VarArg fifth)
    {
        constexpr uint32_t kFormattedBytes = 0x200;
        ee::Local<char[kFormattedBytes]> formatted;
        ee::Local<uint64_t> ap;
        ee::Local<uint64_t> secondSlot;
        ee::Local<uint64_t> thirdSlot;
        ee::Local<uint64_t> fourthSlot;
        ee::Local<uint64_t> fifthSlot;
        const int32_t screenX = zge::signed16(x), screenY = zge::signed16(y);
        const uint32_t enabled = *zge::g_debugEnabled;
        *ap = first;
        *secondSlot = second;
        *thirdSlot = third;
        *fourthSlot = fourth;
        *fifthSlot = fifth;
        if (enabled == 0)
            return 0;
        vsprintf(formatted.address(), format, ap);
        const int32_t length = zge::stringLength(formatted.address());
        const uint32_t cursor = *zge::g_debugTextCursor;
        const uint64_t lengthZeroExtended = static_cast<uint32_t>(length);
        if (cursor + (static_cast<uint32_t>(length) + 6) >= kDebugTextEnd)
        {
            *zge::g_debugTextOverflowed = 1;
            return 0;
        }
        const ee::Ptr<zge::DebugTextEntry> line(cursor);
        line->x = static_cast<int16_t>(screenX);
        line->y = static_cast<int16_t>(screenY);
        std::memcpy(line->colour, ee::Ptr<uint8_t>(sym::g_debugPrintColour).get(), sizeof(line->colour));
        zge::stringCopy(cursor + static_cast<uint32_t>(sizeof(zge::DebugTextEntry)), formatted.address());
        *zge::g_debugTextCursor = static_cast<uint32_t>(length) + *zge::g_debugTextCursor +
                                      static_cast<uint32_t>(sizeof(zge::DebugTextEntry)) + kDebugTextAlignment &
                                  ~(kDebugTextAlignment - 1);
        return lengthZeroExtended;
    }
    READABLE_FUNCTION(debugPrintInColour, Draft);
}
