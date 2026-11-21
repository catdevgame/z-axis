// BMX XXX's menu machine (menu.h): BMX XXX's own code (bmx.c).

#include "menu.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/ui/ui_nodes.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/ui/menu.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The menu that has the screen and the menu asked for.
        inline ee::Global<Menu, sym::g_menu> g_menu;
        inline ee::Global<Menu, sym::g_nextMenu> g_nextMenu;
        // Where a change is, and the frames spent there.
        inline ee::Global<uint32_t, sym::g_menuPhase> g_menuPhase;
        constexpr uint32_t kMenuSteady = 0, kMenuComingIn = 1, kMenuGoingOut = 2;
        inline ee::Global<uint32_t, sym::g_menuFrames> g_menuFrames;
        // How long a coming in or a going out lasts when the handler does
        // not end it.
        constexpr uint32_t kChangeFrames = 180;
        // The message waiting to be sent (the UI's "MsgType").
        inline ee::Global<int32_t, sym::g_msgType> g_msgType;
        // The player whose pad has the menus (the UI's "ModalPlayerNum"; -1:
        // every player's).
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
        constexpr int32_t kEveryPlayer = -1;
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        constexpr uint32_t kUnlockInput = sym::bmx::g_textUnlockInput; // "UNLOCKINPUT %d"
        constexpr uint32_t kLockInput = sym::bmx::g_textLockInput;     // "LOCKINPUT %d"
        constexpr ee::Ptr<const char> kKillFocus{sym::g_textMenuKillFocus}; // "KILLFOCUS"
        // The pad buttons the machine sends as messages, tested as pressed.
        constexpr uint32_t kButton6 = 0x40, kButton14 = 0x4000;
        constexpr uint32_t kPressed = 0;

        // Each pad's input in the UI: unlocked for a player who has the
        // menus, locked for the rest.
        void lockInputs(ee::Address command)
        {
            for (uint32_t player = 0; player < kMaxPlayers; ++player)
            {
                const bool has = player < *g_playerCount &&
                                 (*g_modalPlayerNum == kEveryPlayer || static_cast<int32_t>(player) == *g_modalPlayerNum);
                if (has)
                    zge::formatString(command, kUnlockInput, player);
                else
                    zge::formatString(command, kLockInput, player);
                zge::zgui::runScript(*g_uiRoot, ee::Ptr<char>(command), 0);
            }
        }
    }

    void setMenu(const Menu *menu)
    {
        if (menu->id == kNoMenu)
        {
            *g_menuPhase = kMenuSteady;
            *g_menuFrames = 0;
            zge::memoryCopy(ee::Ptr<Menu>(sym::g_menu), menu, sizeof(Menu));
        }
        game::tail(zge::memoryCopy, ee::Ptr<Menu>(sym::g_nextMenu), menu, sizeof(Menu));
    }
    READABLE_FUNCTION(setMenu, Verified);

    int32_t menuId()
    {
        return g_menu->id;
    }
    READABLE_FUNCTION(menuId, Verified);

    uint32_t menuFrames()
    {
        if (*g_menuPhase != kMenuSteady)
            return 0;
        return *g_menuFrames;
    }
    READABLE_FUNCTION(menuFrames, Verified);

    void bmxuiHandler()
    {
        ee::Local<char[0x40]> command;
        if (*g_msgType == kNoMsg)
            return;
        if (g_menu->id != kNoMenu)
            g_menu->handler(*g_msgType, g_menu->data);
        if (*g_msgType == kMsgCameIn)
        {
            if (*g_menuPhase == kMenuComingIn)
            {
                *g_menuPhase = kMenuSteady;
                *g_menuFrames = 0;
                lockInputs(command);
            }
        }
        else if (*g_msgType == kMsgLeft)
        {
            if (*g_menuPhase == kMenuGoingOut)
            {
                *g_menuFrames = 0;
                *g_menuPhase = kMenuComingIn;
            }
        }
        *g_msgType = kNoMsg;
    }
    READABLE_FUNCTION_WAITS(bmxuiHandler, Verified);

    int32_t menuFrame()
    {
        ee::Local<char[0x40]> command;
        // A step that moves the machine on runs the next at once.
        for (bool again = true; again;)
        {
            again = false;
            const uint32_t phase = *g_menuPhase;
            if (phase == kMenuComingIn)
            {
                const uint32_t frames = ++*g_menuFrames;
                if (frames == 1)
                {
                    // The menu asked for takes the screen (its data word
                    // stays the old menu's) and is told so.
                    g_menu->id = g_nextMenu->id;
                    g_menu->handler = g_nextMenu->handler;
                    if (g_menu->id != kNoMenu)
                    {
                        *g_msgType = kMsgEnter;
                        g_menu->handler(*g_msgType, g_menu->data);
                    }
                    // Another was asked for meanwhile: start over.
                    if (g_nextMenu->id != g_menu->id)
                    {
                        *g_menuFrames = 0;
                        again = true;
                    }
                }
                else if (frames == kChangeFrames)
                {
                    *g_menuPhase = kMenuSteady;
                    *g_menuFrames = 0;
                    lockInputs(command);
                    again = true;
                }
            }
            else if (phase == kMenuSteady)
            {
                if (g_menu->id == g_nextMenu->id)
                {
                    for (uint32_t player = 0; player < kMaxPlayers; ++player)
                    {
                        if (*g_modalPlayerNum != kEveryPlayer && static_cast<int32_t>(player) != *g_modalPlayerNum)
                            continue;
                        if (rider::padButtons(player, kButton6, kPressed))
                        {
                            *g_msgType = kMsgButton6;
                            g_menu->handler(kMsgButton6, g_menu->data);
                        }
                        if (rider::padButtons(player, kButton14, kPressed))
                        {
                            *g_msgType = kMsgButton14;
                            g_menu->handler(kMsgButton14, g_menu->data);
                        }
                    }
                    ++*g_menuFrames;
                }
                else
                {
                    // Another menu is asked for: this one goes out first,
                    // if there is one.
                    *g_menuPhase = g_menu->id == kNoMenu ? kMenuComingIn : kMenuGoingOut;
                    *g_menuFrames = 0;
                    again = true;
                }
            }
            else if (phase == kMenuGoingOut)
            {
                const uint32_t frames = ++*g_menuFrames;
                if (frames == 1)
                {
                    runUiScript(*g_uiRoot, kKillFocus);
                    for (uint32_t player = 0; player < kMaxPlayers; ++player)
                    {
                        zge::formatString(command, kLockInput, player);
                        zge::zgui::runScript(*g_uiRoot, ee::Ptr<char>(command), 0);
                    }
                    *g_msgType = kMsgLeave;
                    g_menu->handler(*g_msgType, g_menu->data);
                }
                else if (frames == kChangeFrames)
                {
                    *g_menuPhase = kMenuComingIn;
                    *g_menuFrames = 0;
                    again = true;
                }
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(menuFrame, Verified);
}
