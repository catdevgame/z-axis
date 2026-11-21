#pragma once

// The front end's controller menu ("MainMenu.ControllerFrame",
// controller_menu.cpp): the script functions that show a player's controller
// settings and take the changes.

#include <cstdint>

namespace game::dm2
{
    // "BmxReadCtrlSetting": the menu's two choices (the configuration, the
    // vibration) set from the settings of the player the frame is for.
    void bmxReadCtrlSetting();
    // "BmxChangeCtrlConfig": the player's settings taken from the menu's two
    // choices; vibration newly turned on gives the pad a short rumble.
    void bmxChangeCtrlConfig();
}
