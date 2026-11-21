#pragma once

// The front end's trick menu ("MainMenu.TrickMenu", trick_menu.cpp): the two
// tricks a character takes into the game, picked from a list of eight.

#include <cstdint>

namespace game::dm2
{
    // "InitializeTrickMenu": the menu's Trick1 and Trick2 set to where the
    // tricks of the rider choice's character are in the menu's list.
    void initializeTrickMenu();

    // "HarvestTrickMenu": the menu's Trick1 and Trick2 kept as the tricks of
    // the rider choice's character.
    void harvestTrickMenu();
}
