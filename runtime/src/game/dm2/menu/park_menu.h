#pragma once

// The front end's park menu ("ParkMenu", park_menu.cpp): the script
// functions behind the park editor's theme choice.

#include <cstdint>

namespace game::dm2
{
    // "bmxSetupTheme": the theme choice's ThemeLock set to whether the theme
    // it stands on is still locked (1) or open (0).
    void bmxSetupTheme();
}
