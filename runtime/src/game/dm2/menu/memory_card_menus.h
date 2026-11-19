#pragma once

// The front end's memory card menus (memory_card_menus.cpp): the script
// functions that open the memory card frame (action/ui_frame.h) to load or
// save a game, load a park or load a replay to watch, and what the frame
// calls when it is over.

#include <cstdint>

namespace game::dm2
{
    // The script's "bmxmenuSetModeLoadGame" and "bmxmenuSetModeSaveGame": the
    // frame opened from the options menu; it goes back to the options.
    void bmxmenuSetModeLoadGame();
    void bmxmenuSetModeSaveGame();
    // "menuSetModeLoadPark" (the park menu) and "menuSetModePlayPark" (the
    // level menu): the frame opened to load a park into a new park buffer.
    void menuSetModeLoadPark();
    void menuSetModePlayPark();
    // "BmxViewReplay": the frame opened to load a replay to watch.
    void bmxViewReplay();

    // The frame's callbacks. Closed without a file: back to the options, the
    // park menu, the level menu or the cool stuff menu.
    void backToOptionsFrame();
    void backToParkMenu();
    void backToLevelMenu();
    void backToCoolStuffFrame();
    // A file loaded: the park menu's or the level menu's loaded-park script
    // played, or the game started on the loaded replay behind a load screen.
    void parkMenuParkLoaded();
    void levelMenuParkLoaded();
    void startLoadedReplay();

    // The replay loaded to watch (the block "BmxViewReplay" has filled).
    uint32_t loadedReplay();
}
