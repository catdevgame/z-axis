// The engine's sprintf wrappers (the text block, after the string routines;
// DM2 and BMX XXX have them word for word): formatString writes into the
// caller's buffer, formatScratch into one of two scratch buffers. Both are
// variadic in the original: a call names the words its format reads
// (cstring.h, ee::VarArg). The string routines are in cstring.cpp.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/zge/text/format.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kLibcVsprintf = sym::vsprintf;

        // formatScratch's two 256-byte buffers (one each call, in turn).
        inline ee::Global<uint32_t, sym::g_scratchBuffer> g_scratchBuffer;
        constexpr uint32_t kScratchBuffers = sym::scratchBuffers;
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t vsprintf(uint32_t str, uint32_t format, ee::Address ap) { return ee::game<int32_t>(kLibcVsprintf, str, format, ap); }
    }

    // formatString: sprintf(out, format, ...): the arguments after the
    // format spilled to the original's frame for vsprintf.
    int32_t formatString(ee::Address out, ee::Address format, ee::VarArg first, ee::VarArg second, ee::VarArg third,
                         ee::VarArg fourth, ee::VarArg fifth, ee::VarArg sixth)
    {
        ee::Local<uint64_t[6]> arguments; // in order, as vsprintf walks them
        const uint64_t values[] = {first, second, third, fourth, fifth, sixth};
        for (uint32_t i = 0; i < 6; ++i)
            arguments[i] = values[i];
        return vsprintf(out, format, arguments);
    }
    READABLE_FUNCTION(formatString, Verified);

    // formatScratch: sprintf into the next of two scratch buffers (256
    // bytes each, taken in turn); the buffer.
    uint32_t formatScratch(ee::Address format, ee::VarArg first, ee::VarArg second, ee::VarArg third, ee::VarArg fourth,
                           ee::VarArg fifth, ee::VarArg sixth, ee::VarArg seventh)
    {
        ee::Local<uint64_t[7]> arguments; // in order, as vsprintf walks them
        const uint64_t values[] = {first, second, third, fourth, fifth, sixth, seventh};
        const uint32_t buffer = *g_scratchBuffer ^ 0x1u;
        for (uint32_t i = 0; i < 7; ++i)
            arguments[i] = values[i];
        *g_scratchBuffer = buffer;
        vsprintf((buffer << 8) + kScratchBuffers, format, arguments);
        return (*g_scratchBuffer << 8) + kScratchBuffers;
    }
    READABLE_FUNCTION(formatScratch, Verified);
}
