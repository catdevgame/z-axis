#pragma once

// The front end's bike choice ("MainMenu.SelectBike", bike_menu.cpp): the
// script functions behind it.

#include <cstdint>

namespace game::dm2
{
    // "bmxmenuBikeModelUpdate": the bike choice's CurBike set to the name of
    // the bike model to show: "Bike<rider>_<choice>" for the rider of the
    // player the main menu is on.
    void bmxmenuBikeModelUpdate();

    // "bmxSetupBikeStat": the bike choice's frame for the bike it is on: the
    // eight stat bars (the rider's own, then what the bike adds), whether the
    // bike is open to the rider yet, and the rider's respect.
    void bmxSetupBikeStat();
}
