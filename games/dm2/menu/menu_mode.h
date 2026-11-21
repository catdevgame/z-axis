#pragma once
// DM2's front end's menu mode (menu_mode.cpp): what the menus are doing, as
// the script's handlers set it ("bmxmenuSetModeLoadGame" 1, "SaveGame" 2,
// "BmxViewReplay" 6). DM2's own code (its front end block).

#include "ee/ee.h"

#include <cstdint>
#include "sym/dm2/menu/menu_mode.h.h"

namespace game::dm2
{
    // The intro state's frame count towards its next UI frame (60 frames),
    // and whether that frame has been set up (setMenuMode starts both over).
    inline ee::Global<uint32_t, sym::g_introFrames> g_introFrames;
    inline ee::Global<uint32_t, sym::g_introMenuSetUp> g_introMenuSetUp;

    // The menu modes (g_menuMode): where the front end's menus are. The
    // intro state's frame (the game's start) opens the intro menu from the
    // front end mode, its callbacks close it; BmxEndIntro ends the intro and
    // the frame that follows closes with kMenuIntroEndClosed, which sends the
    // game to the front end state. The load and save modes are the memory
    // card menus' (bmxmenuSetModeLoadGame, SaveGame); a replay being watched
    // ("BmxViewReplay") has its level set up with the replay's rider.
    constexpr uint32_t kMenuFrontEnd = 0;         // BmxCoolStuffGetFocus sets it
    constexpr uint32_t kMenuLoadGame = 1;
    constexpr uint32_t kMenuSaveGame = 2;
    constexpr uint32_t kMenuIntroMenu = 3;        // the intro's menu (UI frame 4) is up
    constexpr uint32_t kMenuIntroMenuClosed = 4;  // closeIntroMenu, closeIntroMenuAtOnce
    constexpr uint32_t kMenuIntroEnded = 5;       // BmxEndIntro: UI frame 7 follows
    constexpr uint32_t kMenuViewReplay = 6;
    constexpr uint32_t kMenuIntroEndClosed = 7;   // closeIntroEnd

    // The menu mode, and setting it (the intro's counters start over).
    uint32_t menuMode();
    void setMenuMode(uint32_t mode);

    // A wait of `frames` for the menus (the intro's frame holds while one runs):
    // setting it, and whether it is still running (0 once it is over and cleared).
    void setMenuWait(uint32_t frames);
    uint32_t menuWaiting();

    // The intro menu's callbacks (UI frame 4's, then frame 7's after the
    // intro ends): the UI faded out or not, the user locked, the UI frame
    // cleared, the mode moved on and, with a fade, a 90-frame wait.
    void closeIntroMenu();
    void closeIntroMenuAtOnce();
    void closeIntroEnd();
    // The menu's script functions "BmxEndIntro" and "BmxCoolStuffGetFocus".
    void bmxEndIntro();
    void bmxCoolStuffGetFocus();

    // The intro state (the game state's tables; introStateExit leaves it).
    // Its enter: the first time, the front end's script variables and
    // functions given to zgui; then the screen and the menu scene set up,
    // the common background hidden, the main menu's script loaded once and
    // the pads let go. Answers 1.
    uint32_t introStateEnter();
    // Its frame: 60 frames after the menu mode last changed, the memory card
    // start check is put up (from the front end mode) or, once the intro
    // has ended, the common UI fades in and the saved game is loaded; then
    // the pads are read and the memory card frame runs while either is up.
    // When the start check closes, the Acclaim movie fades in (once after
    // each intro); when the load closes, the game goes to the front end.
    // Answers 1.
    uint32_t introStateFrame();
}
