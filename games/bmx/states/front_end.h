#pragma once
// BMX XXX's front end state (the game state kGameStateFrontEnd: the menus,
// "UI_menu"): BMX XXX's own code (bmx.c's menu block, front_end.cpp). Its
// menus and their handler: front_end_menus.h.

#include <cstdint>

namespace game::bmx
{
    // The state's enter: its loading from the first step. Always 1.
    int32_t frontEndEnter();
    // The state's loading, a step a frame: 0 once it is over, else 1.
    int32_t frontEndLoad();
    // The state's exit: the menus taken down, the scene left, the sound off.
    void frontEndExit();
    // The state's frame: the pads, the menu machine, the idle front end's
    // demo, and the frame of the menu that has the screen. Always 1.
    int32_t frontEndFrame();
}
