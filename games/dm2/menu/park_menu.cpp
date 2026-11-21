// The front end's park menu (park_menu.h): DM2's own code, registered as
// script functions by the menu set-up.

#include "park_menu.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/menu/park_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        // How many of the park themes are open: the themes from this number
        // on are locked (the saved game's settings keep it).
        inline ee::Global<uint8_t, sym::g_openThemeCount> g_openThemeCount;

        constexpr ee::Ptr<const char> kThemeChoicePath{sym::selectThemePath}; // "ParkMenu.SelectTheme"
        constexpr uint32_t kCurrentValueName = sym::g_textCurValueLower;      // "curValue"
        constexpr uint32_t kThemeLockName = sym::g_textThemeLock;             // "ThemeLock"

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> themeChoice()
        {
            return zge::zgui::findNode(*g_uiRoot, kThemeChoicePath, 0);
        }

    }

    void bmxSetupTheme()
    {
        ee::Local<ee::Float> theme;
        ee::Local<int32_t> locked;
        *locked = 1;
        zge::zgui::copyVariable(themeChoice(), kCurrentValueName, theme);
        if (*theme < static_cast<float>(*g_openThemeCount))
            *locked = 0;
        zge::zgui::setVariable(themeChoice(), kThemeLockName, locked);
    }
    READABLE_FUNCTION(bmxSetupTheme, Verified);
}
