// formatLocalizedText, the save system's text shown to the player (zsaveapi.c). It is variadic in the original:
// a call names the words its text reads (ee::VarArg).

#include "save_system.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/zge/platform/save_text.cpp.h"

namespace zge
{
    namespace
    {
        // formatLocalizedText's buffers: the text with its keys expanded, then formatted.
        constexpr uint32_t kLocalizedTextBuffer = sym::g_localizedTextBuffer;
        constexpr uint32_t kLocalizedText = sym::g_localizedTextScratch;

        int32_t vsprintf(uint32_t out, uint32_t format, ee::Address arguments) { return ee::game<int32_t>(sym::vsprintf, out, format, arguments); }
    }

    // formatLocalizedText: the string table's text for key (its $KEYs
    // expanded) formatted with the arguments, and shown: a ShowText message
    // to the interface. Variadic in the original: the arguments go to the
    // frame for vsprintf, as many as the format reads. The message's last
    // two words are left as the stack had them.
    uint32_t formatLocalizedText(ee::Address key, ee::VarArg first = {}, ee::VarArg second = {}, ee::VarArg third = {},
                                 ee::VarArg fourth = {}, ee::VarArg fifth = {}, ee::VarArg sixth = {}, ee::VarArg seventh = {})
    {
        ee::Local<SaveMessage> message;
        ee::Local<uint64_t[7]> arguments; // in order, as vsprintf walks them
        const uint64_t values[] = {first, second, third, fourth, fifth, sixth, seventh};
        for (uint32_t i = 0; i < 7; ++i)
            arguments[i] = values[i];
        expandStringKeys(ee::Ptr<char>(kLocalizedTextBuffer), ee::Ptr<const char>(key));
        vsprintf(kLocalizedText, kLocalizedTextBuffer, arguments);
        const ee::Ptr<SaveMessageQueue> queue(*g_saveMessageQueue);
        message->value = kLocalizedText;
        message->kind = kMessageShowText;
        return postSaveMessage(queue, message);
    }
    READABLE_FUNCTION(formatLocalizedText, Verified);
}
