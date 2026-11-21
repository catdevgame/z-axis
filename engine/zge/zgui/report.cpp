// zgui's script error report (script.h): the statement's line and the
// message formatted into the frame. report is variadic in the original
// (format, ...): a call names the words its format reads (ee::VarArg). The
// interpreter is script.cpp.

#include "zge/zgui/script.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "sym/zge/zgui/report.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kVsprintf = sym::vsprintf; // libc vsprintf (out, format, arguments)
        int32_t vsprintf(ee::Ptr<char> str, uint32_t format, ee::Ptr<uint64_t> ap) { return ee::game<int32_t>(kVsprintf, str, format, ap); }
    }

    int32_t report(ee::Address format, ee::VarArg first, ee::VarArg second, ee::VarArg third, ee::VarArg fourth,
                   ee::VarArg fifth, ee::VarArg sixth, ee::VarArg seventh)
    {
        // A script error: the statement's first line (to a carriage return)
        // and the message formatted (printf-like) into the frame; the
        // release build prints neither. Returns vsprintf's count.
        struct Buffers
        {
            char line[0x100];
            char message[0x198];
            uint64_t arguments[7]; // the variadic arguments, as vsprintf reads them
        };
        ee::Local<Buffers> buffers;
        const ee::Ptr<char> line(buffers->line);
        const ee::Ptr<char> message(buffers->message);
        const ee::Ptr<uint64_t> arguments(buffers->arguments);
        const uint64_t spilled[7] = {first, second, third, fourth, fifth, sixth, seventh};
        for (int i = 0; i < 7; ++i)
            arguments[i] = spilled[i];
        const ee::Ptr<ScriptContext> script = *g_script;
        // The statement running, the one before next, indexed as the
        // original's 32-bit address arithmetic (ee::Ptr): before a script's
        // first statement (next 0) that is the word before the table, next
        // itself: the text at address 0.
        const auto statement = [](ee::Ptr<ScriptContext> context)
        { return ee::Ptr<ee::Ptr<char>>(context->statement)[context->next - 1]; };
        ee::Ptr<char> end = statement(script);
        bool carriageReturn = false;
        if (*end != 0)
        {
            if (*end != '\r')
            {
                ++end;
                for (;;)
                {
                    if (*end == 0)
                        break;
                    if (*end == '\r')
                    {
                        carriageReturn = true;
                        break;
                    }
                    ++end;
                }
            }
            else
                carriageReturn = true;
        }
        if (carriageReturn)
        {
            // The line up to the carriage return.
            const ee::Ptr<char> start = statement(script);
            zge::stringCopyN(line, start, static_cast<uint32_t>(end - start));
            line[static_cast<uint32_t>(end - statement(*g_script))] = 0;
        }
        else
        {
            // No carriage return: the whole statement.
            zge::stringCopy(line, statement(script));
        }
        return vsprintf(message, format, arguments);
    }
    READABLE_FUNCTION(report, Verified);
}
