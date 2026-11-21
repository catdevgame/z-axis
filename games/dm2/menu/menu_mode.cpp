// DM2's front end's menu mode (menu_mode.h). DM2's own code (its front end
// block).

#include "menu_mode.h"

#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/menu/menu_mode.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_menuMode> g_menuMode;
        // The menus' wait: its frames and how many have passed.
        inline ee::Global<uint32_t, sym::g_menuWaited> g_menuWaited;
        inline ee::Global<uint32_t, sym::g_menuWait> g_menuWait;
    }

    uint32_t menuMode()
    {
        return *g_menuMode;
    }
    READABLE_FUNCTION(menuMode, Verified);

    void setMenuMode(uint32_t mode)
    {
        *g_menuMode = mode;
        *g_introFrames = 0;
        *g_menuWait = 0;
        *g_introMenuSetUp = 0;
    }
    READABLE_FUNCTION(setMenuMode, Verified);

    void setMenuWait(uint32_t frames)
    {
        *g_menuWait = frames;
        *g_menuWaited = 0;
    }
    READABLE_FUNCTION(setMenuWait, Verified);

    uint32_t menuWaiting()
    {
        if (*g_menuWaited == *g_menuWait)
        {
            *g_menuWaited = 0;
            *g_menuWait = 0;
            return 0;
        }
        ++*g_menuWaited;
        return 1;
    }
    READABLE_FUNCTION(menuWaiting, Verified);
}
