#pragma once
// BMX XXX's menu machine (bmx.c, menu.cpp): the menu that has the screen,
// the one asked for next, and the change from one to the other. BMX XXX's
// own code.
//
// A menu is an id, a handler the machine sends messages to, and a word of the
// handler's own. A change takes the old menu out (the input locked, the
// leave event, then up to 180 frames or until the handler says it has
// left), then brings the new one in the same way.

#include "ee/ee.h"
#include "ee/function_ptr.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // The messages a menu's handler gets (the UI's "MsgType" property: its
    // scripts set it and call "bmxuiHandler"). Two are answers from a
    // handler's side: the menu has come in, it has left.
    constexpr int32_t kNoMsg = -2;
    constexpr int32_t kMsgButton6 = 4;    // the pad's button bit 6 went down (which button: not named yet)
    constexpr int32_t kMsgButton14 = 5;   // the pad's button bit 14
    constexpr int32_t kMsgEnter = 10;     // the menu has the screen
    constexpr int32_t kMsgCameIn = 11;    // from the handler: the coming in is over
    constexpr int32_t kMsgLeave = 12;     // the menu is being taken out
    constexpr int32_t kMsgLeft = 13;      // from the handler: the going out is over

    struct Menu
    {
        int32_t id;                                                 // +0x00 kNoMenu: none
        ee::FunctionPtr<void(int32_t event, uint32_t data)> handler; // +0x04
        uint32_t data;                                              // +0x08
    };
    static_assert(sizeof(Menu) == 0x0C);
    constexpr int32_t kNoMenu = -1;

    // The menu asked for: the machine changes to it. No menu (kNoMenu) is
    // taken at once.
    void setMenu(const Menu *menu);
    // The id of the menu that has the screen.
    int32_t menuId();
    // The frames the menu has had the screen (0 during a change).
    uint32_t menuFrames();
    // The UI's script function "bmxuiHandler": the pending message sent to
    // the menu's handler, and the two answers taken; no message is pending
    // after.
    void bmxuiHandler();
    // The machine's frame: the pads' two buttons sent as messages, a change
    // stepped. Always 1.
    int32_t menuFrame();
}
