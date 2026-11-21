// DM2's in-game overlay (overlay.h): the frame table's functions. DM2's own
// code (overlay.c).

#include "overlay.h"

#include "frames.h"
#include "overlay_calls.h"
#include "overlay_items.h"
#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/sound/sound_mode.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/overlay/overlay.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kTimerLogoFunction{sym::g_textDisplayTimerUpdate}; // "bmxoverlayDisplayTimerUpdate"
        constexpr ee::Ptr<const char> kPlayOverlay{sym::g_textPlayOverlayPath};             // "InGameUI.Overlay"
        constexpr ee::Ptr<const char> kPlayOverlayToggle{sym::g_textPlayOverlayTogglePath}; // "InGameUI.OverlayToggle"
        constexpr ee::Ptr<const char> kPlayOverlayReplay{sym::g_textPlayOverlayReplayPath}; // "InGameUI.OverlayReplay"
        constexpr ee::Ptr<const char> kTinyFontAsset{sym::g_textFontTiny};                  // "Common.mnu_tiny"
        constexpr ee::Ptr<const char> kSmallFontAsset{sym::g_textFontSmall};                // "Common.mnu_small"

        // The fonts the frames that draw their own text use (0: not loaded).
        inline ee::Global<ee::Ptr<zge::Font>, sym::g_overlayTinyFont> g_overlayTinyFont;
        inline ee::Global<ee::Ptr<zge::Font>, sym::g_overlaySmallFont> g_overlaySmallFont;

        ee::Ptr<zge::Font> fontNamed(ee::Ptr<const char> name)
        {
            const ee::Ptr<zge::zgui::UiAsset> asset = zge::zgui::findGuiAsset(name, zge::zgui::kAssetFont);
            return ee::Ptr<zge::Font>(asset ? asset->object : 0);
        }
    }

    // The overlay starts: the timer logo's script function registered, every
    // frame told and hidden, the three overlay layouts hidden, and the fonts
    // found.
    void openOverlayFrames()
    {
        ee::Local<uint32_t> visible;
        zge::zgui::registerScriptFunction(kTimerLogoFunction, sym::bmxoverlayDisplayTimerUpdate);
        for (uint32_t i = 0; i < kOverlayFrames; ++i)
        {
            OverlayFrameRecord &frame = (*g_overlayFrames)[i];
            if (frame.setUp)
                frame.setUp(1);
            frame.flags = 0;
            showOverlayFrame(i, 0);
        }
        visible = 0;
        zge::zgui::setVariable(uiWidget(kPlayOverlay), kVisibleName, visible);
        zge::zgui::setVariable(uiWidget(kPlayOverlayToggle), kVisibleName, visible);
        zge::zgui::setVariable(uiWidget(kPlayOverlayReplay), kVisibleName, visible);
        *g_overlayTinyFont = fontNamed(kTinyFontAsset);
        *g_overlaySmallFont = fontNamed(kSmallFontAsset);
    }
    READABLE_FUNCTION(openOverlayFrames, Verified);

    void closeOverlayFrames()
    {
        for (uint32_t i = 0; i < kOverlayFrames; ++i)
        {
            OverlayFrameRecord &frame = (*g_overlayFrames)[i];
            if (frame.setUp)
                frame.setUp(0);
        }
    }
    READABLE_FUNCTION(closeOverlayFrames, Verified);

    void updateOverlayFrames()
    {
        for (uint32_t i = 0; i < kOverlayFrames; ++i)
        {
            OverlayFrameRecord &frame = (*g_overlayFrames)[i];
            if ((frame.flags & kOverlayFrameHidden) == 0 && frame.frame)
                frame.frame();
        }
    }
    READABLE_FUNCTION(updateOverlayFrames, Verified);

    void showOverlayFrame(uint32_t frame, uint32_t shown)
    {
        OverlayFrameRecord &record = (*g_overlayFrames)[frame];
        const uint32_t shownNow = (record.flags ^ kOverlayFrameHidden) & kOverlayFrameHidden;
        if (shown == shownNow)
            return;
        if (record.show)
            record.show(shown);
        if (shown == 0)
            record.flags |= kOverlayFrameHidden;
        else
            record.flags &= ~kOverlayFrameHidden;
    }
    READABLE_FUNCTION(showOverlayFrame, Verified);

    void overlayMessage(uint32_t frame, ee::Address message)
    {
        OverlayFrameRecord &record = (*g_overlayFrames)[frame];
        if (record.message)
            record.message(message);
    }
    READABLE_FUNCTION(overlayMessage, Verified);

    void overlayFrameState(uint32_t frame, ee::Address out)
    {
        OverlayFrameRecord &record = (*g_overlayFrames)[frame];
        if (record.state)
            record.state(out);
    }
    READABLE_FUNCTION(overlayFrameState, Verified);

    void overlayShownMessage(uint32_t frame, ee::Address message)
    {
        OverlayFrameRecord &record = (*g_overlayFrames)[frame];
        if ((record.flags & kOverlayFrameHidden) == 0 && record.shownMessage)
            record.shownMessage(message);
    }
    READABLE_FUNCTION(overlayShownMessage, Verified);

    namespace
    {
        constexpr uint32_t kScreenInputDelay = 30;
        // The ready screen's choices; HORSE's later turns have one more.
        constexpr uint8_t kReadyChoices = 2;
        constexpr uint8_t kHorseReadyChoices = 3;

        // (zoverlay's stand-ins are overlay_calls.h's.)
        // Scaffolding (value-named): the action modes the screens ask for
        // (action.c's, not read: 0 is play): as a screen takes over from
        // another, while a screen is up, and play in a game that counts no
        // players.
        constexpr uint32_t kActionLeaveScreen = 0xFFFFFFFF;
        constexpr uint32_t kActionPlay = 0;
        constexpr uint32_t kActionScreen = 1;
        constexpr uint32_t kActionPlayNoPlayers = 4;
    }

    void setScreenHeaders(uint32_t headers)
    {
        *g_wantedScreenHeaders = static_cast<uint8_t>(headers);
    }
    READABLE_FUNCTION(setScreenHeaders, Verified);

    // showGameScreen: the screen that is up closes (its handler told, its
    // overlay items given up, the screens' input state cleared), and the new
    // one opens: the action pauses for it, its menu starts at the first
    // choice, and its handler is told. kScreenNone instead gives the play
    // overlay back and play goes on. The exit confirmation is asked over
    // another screen: opening it, and going back from it to that screen,
    // leave the action mode alone.
    void showGameScreen(uint32_t screen)
    {
        if (screen == kScreenLast)
            screen = *g_lastGameScreen;
        else if (screen != kScreenNone)
            *g_lastGameScreen = static_cast<uint8_t>(screen);

        const ee::Ptr<GameScreenRecord> closing = *g_gameScreenRecord;
        if (closing && closing->show)
            closing->show(0);
        for (uint32_t i = 0; i < kScreenItems; ++i)
        {
            uint16_t &item = (*g_screenItems)[i];
            if (item != kNoScreenItem)
            {
                removeOverlayItem(item);
                item = kNoScreenItem;
            }
        }
        *g_screenInputDelay = 0;
        *g_currentPlayerOnly = 1;
        startScreenStep(0);

        if (screen == kScreenNone)
        {
            setScreenHeaders(kHeadersPlay);
            setGameMode(kSoundModeLevel);
            showOverlayItemGroup(0);
            if (*g_gameScreen == kScreenExitConfirm)
                *g_gameScreen = *g_screenBehindExitConfirm;
            actionStart(*g_multiplayerPlayers != 0 ? kActionPlay : kActionPlayNoPlayers);
            *g_gameScreen = kScreenNone;
            *g_gameScreenRecord = (*g_gameScreens)[kScreenNone];
            return;
        }

        const uint8_t open = *g_gameScreen;
        if (open == kScreenExitConfirm && screen == *g_screenBehindExitConfirm)
        {
            *g_gameScreen = static_cast<uint8_t>(screen);
            *g_gameScreenRecord = (*g_gameScreens)[screen];
            (*g_gameScreenRecord)->choice = 0;
        }
        else
        {
            if (open != kScreenNone && screen != kScreenExitConfirm)
                actionStart(kActionLeaveScreen);
            *g_gameScreen = static_cast<uint8_t>(screen);
            *g_gameScreenRecord = (*g_gameScreens)[screen];
            (*g_gameScreenRecord)->choice = 0;
            if (screen != kScreenExitConfirm)
                actionStart(kActionScreen);
        }

        switch (screen)
        {
        case kScreenGameOver:
            *g_screenInputDelay = kScreenInputDelay;
            if (multiplayerMode() != 0)
                *g_currentPlayerOnly = 0;
            break;
        case kScreenInstructions:
        case kScreenHorseWord:
            *g_currentPlayerOnly = 0;
            *g_screenInputDelay = kScreenInputDelay;
            break;
        case kScreenMultiplayerGameSelect:
            *g_currentPlayerOnly = 0;
            break;
        case kScreenMultiplayerReady:
        {
            const uint32_t mode = multiplayerMode();
            const bool laterHorseTurn = mode == kHorseMode && !roundFlags(kFirstTurnFlag);
            (*g_gameScreenRecord)->choices = laterHorseTurn ? kHorseReadyChoices : kReadyChoices;
            *g_screenInputDelay = kScreenInputDelay;
            break;
        }
        case kScreenHighScore:
        case kScreenMultiplayerTimeUp:
        case kScreenNpc:
            *g_screenInputDelay = kScreenInputDelay;
            break;
        default:
            break;
        }

        showOverlayItemGroup(1);
        const ee::Ptr<GameScreenRecord> opened = *g_gameScreenRecord;
        if (opened->show)
            opened->show(1);
        game::tail(setScreenHeaders, screen == kScreenNpc ? kHeadersNone : kHeadersDown);
    }
    READABLE_FUNCTION(showGameScreen, Verified);

    uint8_t gameScreen()
    {
        const uint8_t screen = *g_gameScreen;
        return screen == kScreenExitConfirm ? *g_screenBehindExitConfirm : screen;
    }
    READABLE_FUNCTION(gameScreen, Verified);
}
