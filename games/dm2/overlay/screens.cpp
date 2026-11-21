// DM2's game screens (screens.h): the screens' task and the helpers the
// screens share. DM2's own code (overlay.c).

#include "screens.h"
#include "overlay_items.h"
#include "../game/players.h"
#include "../rider/rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screens.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The scripts that take the top and bottom headers away and drop them
        // down ("PLAY Root.CommonUI.TopHeader.DropUp" and the three like it),
        // and the play overlay's two widgets with their "Visible" variable.
        constexpr ee::Ptr<char> kHeadersTopUp{sym::headersTopUpScript};
        constexpr ee::Ptr<char> kHeadersBottomUp{sym::headersBottomUpScript};
        constexpr ee::Ptr<char> kHeadersTopDown{sym::headersTopDownScript};
        constexpr ee::Ptr<char> kHeadersBottomDown{sym::headersBottomDownScript};
        constexpr ee::Ptr<const char> kOverlayPath{sym::g_textOverlayPath};             // "InGameUI.Overlay"
        constexpr ee::Ptr<const char> kOverlayTogglePath{sym::g_textOverlayTogglePath}; // "InGameUI.OverlayToggle"
        constexpr ee::Ptr<const char> kVisible{sym::g_textVisible};                     // "Visible"

        void showPlayOverlay(ee::Local<uint32_t> &visible)
        {
            zge::zgui::setVariable(uiWidget(kOverlayPath), kVisible, visible);
            zge::zgui::setVariable(uiWidget(kOverlayTogglePath), kVisible, visible);
        }
    }

    uint32_t gameScreensFrame()
    {
        ee::Local<uint32_t> visible;
        const uint8_t shown = *g_shownScreenHeaders;
        if (shown != *g_wantedScreenHeaders)
        {
            if (shown == kHeadersDown)
            {
                runUiScript(kHeadersTopUp);
                runUiScript(kHeadersBottomUp);
            }
            else if (shown == kHeadersPlay)
            {
                visible = 0;
                showPlayOverlay(visible);
            }

            const uint8_t wanted = *g_wantedScreenHeaders;
            if (wanted == kHeadersDown)
            {
                runUiScript(kHeadersTopDown);
                runUiScript(kHeadersBottomDown);
            }
            else if (wanted == kHeadersPlay)
            {
                visible = 1;
                showPlayOverlay(visible);
            }
            *g_shownScreenHeaders = *g_wantedScreenHeaders;
        }

        const ee::Ptr<GameScreenRecord> screen = *g_gameScreenRecord;
        if (!screen)
            updateOverlayFrames();
        else if (screen->frame)
            screen->frame();
        return 1;
    }
    READABLE_FUNCTION(gameScreensFrame, Verified);

    namespace
    {
        // A menu item's colour when its choice is not the one picked (the
        // screen colours the picked one itself).
        constexpr uint32_t kMenuItemColour = 0x1A3040E0;

        // The choice the menu is on moves off its item, which is coloured
        // back unless the screen has a frame of its own.
        void leaveChoice(ee::Ptr<GameScreenRecord> screen)
        {
            if (!screen->frame)
                setOverlayItemColour((*g_screenItems)[screen->choice], kMenuItemColour);
        }
    }

    uint32_t gameScreenFrame()
    {
        const ee::Ptr<GameScreenRecord> screen = *g_gameScreenRecord;
        if (!screen)
            return 0;
        ++*g_screenFramesSinceMove;
        if (*g_screenFrames >= *g_screenInputDelay)
        {
            uint8_t choices = screen->choices;
            if (screen->input)
            {
                const uint32_t taken = screen->input();
                if (taken != 0)
                    return taken;
                // (The screen's input may have closed it.)
                const ee::Ptr<GameScreenRecord> still = *g_gameScreenRecord;
                if (!still)
                    return 0;
                choices = still->choices;
            }
            if (choices >= 2)
            {
                for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                {
                    if (*g_currentPlayerOnly != 0 && player != *g_currentPlayer)
                        continue;
                    if (rider::padButtons(player, rider::kButtonDown, rider::kAnyPressed))
                    {
                        leaveChoice(*g_gameScreenRecord);
                        GameScreenRecord &menu = **g_gameScreenRecord;
                        menu.choice = static_cast<int32_t>(menu.choice) < static_cast<int32_t>(menu.choices) - 1
                                        ? static_cast<uint8_t>(menu.choice + 1)
                                        : 0;
                        *g_screenFramesSinceMove = 0;
                        return 1;
                    }
                    if (rider::padButtons(player, rider::kButtonUp, rider::kAnyPressed))
                    {
                        leaveChoice(*g_gameScreenRecord);
                        GameScreenRecord &menu = **g_gameScreenRecord;
                        menu.choice = static_cast<uint8_t>((menu.choice == 0 ? menu.choices : menu.choice) - 1);
                        *g_screenFramesSinceMove = 0;
                        return 1;
                    }
                }
            }
        }
        ++*g_screenFrames;
        return 0;
    }
    READABLE_FUNCTION(gameScreenFrame, Verified);

    void startScreenStep(uint32_t step)
    {
        const ee::Ptr<GameScreenRecord> screen = *g_gameScreenRecord;
        (*g_screenStep)[0] = static_cast<uint8_t>(step);
        (*g_screenStep)[1] = 0;
        *g_screenFrames = 0;
        *g_screenFramesSinceMove = 0;
        if (screen)
            screen->choice = 0;
    }
    READABLE_FUNCTION(startScreenStep, Verified);

    namespace
    {
        // The root's messages that start and stop an action of a widget.
        constexpr uint32_t kMessagePlayAction = 0x8000;
        constexpr uint32_t kMessageStopAction = 0x8001;

    }

    uint32_t playWidgetAction(ee::Ptr<zge::zgui::Node> widget, ee::Ptr<const char> name)
    {
        const ee::Ptr<zge::zgui::ListedAction> action = widgetAction(widget, name);
        if (!action)
            return 0;
        return static_cast<uint32_t>(game::tail(zge::sendNewMessage, widget, *g_uiRoot, kMessagePlayAction, action, 1));
    }
    READABLE_FUNCTION(playWidgetAction, Verified);

    uint32_t stopWidgetAction(ee::Ptr<zge::zgui::Node> widget, ee::Ptr<const char> name)
    {
        const ee::Ptr<zge::zgui::ListedAction> action = widgetAction(widget, name);
        if (!action)
            return 0;
        return static_cast<uint32_t>(game::tail(zge::sendNewMessage, widget, *g_uiRoot, kMessageStopAction, action, 1));
    }
    READABLE_FUNCTION(stopWidgetAction, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_metresPerFoot> g_metresPerFoot; // 0.3048
        constexpr ee::Ptr<const char> kFeetAndInchesFormat{sym::g_textFeetAndInchesFormat}; // "%0ld' %ld\""
        constexpr ee::Float kInchesPerFoot = 12.0f;
        constexpr uint32_t kWholeInchesPerFoot = 12;
    }

    int32_t formatFeetAndInches(ee::Ptr<char> out, ee::Float metres)
    {
        const uint32_t inches = static_cast<uint32_t>(ee::toInt(metres / *g_metresPerFoot * kInchesPerFoot + ee::Float(0.5f)));
        return game::tail(zge::formatString, out, kFeetAndInchesFormat, inches / kWholeInchesPerFoot, inches % kWholeInchesPerFoot);
    }
    READABLE_FUNCTION(formatFeetAndInches, Verified);

    int32_t breakTextLines(ee::Ptr<char> out, ee::Ptr<const char> text, uint32_t lineChars)
    {
        uint32_t lineLength = 0;
        int32_t braces = 0;
        int32_t breaks = 0;
        for (char c = *text; c != '\0'; c = *++text)
        {
            if (c == '{')
                ++braces;
            else if (c == '}')
                braces -= braces > 0 ? 1 : 0;
            else if (braces == 0)
                ++lineLength;

            if (lineLength >= lineChars && c == ' ' && braces == 0)
            {
                *out = '\\';
                ++out;
                *out = 'n';
                lineLength = 0;
                ++breaks;
            }
            else
            {
                *out = c;
            }
            ++out;
        }
        *out = '\0';
        return breaks;
    }
    READABLE_FUNCTION(breakTextLines, Verified);

    void joinTextLines(ee::Ptr<char> out, ee::Ptr<const char> text)
    {
        for (char c = *text; c != '\0'; c = *++text)
        {
            if (c == '\r')
                continue;
            *out = c == '\n' ? ' ' : c;
            ++out;
        }
        *out = '\0';
    }
    READABLE_FUNCTION(joinTextLines, Verified);
}
