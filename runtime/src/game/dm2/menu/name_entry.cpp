// The name entry's check (name_entry.h): DM2's own code, registered as a
// script function by the menu set-up.

#include "name_entry.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/menu/name_entry.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        constexpr ee::Ptr<const char> kCommonUiName{sym::g_textCommonUI}; // "CommonUI"
        constexpr uint32_t kNameVariable = sym::g_textTempString;         // "tempString"
        constexpr uint32_t kValidVariable = sym::g_textTempInt;           // "tempInt"

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> commonUi()
        {
            return zge::zgui::findNode(*g_uiRoot, kCommonUiName, 0);
        }

    }

    void bmxValidateName()
    {
        ee::Local<char[0x100]> name;
        ee::Local<int32_t> valid;

        name[0] = 0;
        *valid = 0;
        zge::zgui::copyVariable(commonUi(), kNameVariable, name);
        const uint32_t length = static_cast<uint32_t>(zge::stringLength(name));
        for (uint32_t at = 0; at < length; ++at)
        {
            if (name[at] != ' ')
            {
                *valid = 1;
                break;
            }
        }
        zge::zgui::setVariable(commonUi(), kValidVariable, valid);
    }
    READABLE_FUNCTION(bmxValidateName, Verified);
}
