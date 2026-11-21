// The front end's credits (credits_menu.h): DM2's own code, registered as a
// script function by the menu set-up.

#include "credits_menu.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/credits_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kCreditsFile = sym::creditsFilePath;     // "assets\credits.txt"
        constexpr ee::Ptr<const char> kCreditsTextPath{sym::creditsTextPath}; // "MainMenu.CreditsFrame.TextScroll.Text"
    }

    // Must run with the file there: the original does not test loadFile's
    // result.
    void bmxGetCredits()
    {
        ee::Local<int32_t> bytes;
        const uint32_t text = zge::loadFile(kCreditsFile, bytes);
        *(ee::Ptr<uint8_t>(text) + static_cast<ptrdiff_t>(*bytes)) = 0;
        zge::zgui::setVariableAtPath(kCreditsTextPath, text);
        zge::freeLoadedFile(text);
    }
    READABLE_FUNCTION(bmxGetCredits, Verified);
}
