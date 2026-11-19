#pragma once
// BMX XXX's AV menu state (bmx.c, av_menu.cpp): the developers' start menu
// ("UI_AVMenu", AVMenu.zsc), which the game opens instead of the intro when
// it is started for it: Level View, Motion View, Game Test, Park Editor
// Test, Menu Test and Movie Test, each picking what it needs (the levels'
// export, a level, the players, a park, a screen image, a movie) and then
// leaving for the state it tests. BMX XXX's own code.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The state's enter: an empty scene with the UI's camera, the AV menu's
    // UI with its build's date, and the menu's first page; one player in
    // session 2. Always 1.
    int32_t avMenuEnter();
    // The state's exit: the menu's UI, the camera and the scene let go.
    void avMenuExit();
    // The state's frame: the pads read, the menu machine stepped; a screen
    // image shown goes back to its list at a button pressed, a movie when
    // it ends. Always 1.
    int32_t avMenuFrame();
    // The AV menu's handler (the menu machine's): its pages by id.
    void avMenu(int32_t message, uint32_t data);
}
