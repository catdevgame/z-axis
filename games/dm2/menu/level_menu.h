#pragma once

// The front end's level menu (level_menu.cpp): the script functions behind
// the level choice.

#include <cstdint>

namespace game::dm2
{
    // "bmxmenuGameLevelUpdate": CurrentGameLevel set to the game level of
    // the level menu's entry CurrentLevel.
    void bmxmenuGameLevelUpdate();

    // "bmxSetupLevelStats": the level choice's frame for the level it is on:
    // whether the level is open and its competition can be entered, and the
    // first player's character's ProQuest lines for it (respect, gaps found,
    // medal, the next challenge).
    void bmxSetupLevelStats();
}
