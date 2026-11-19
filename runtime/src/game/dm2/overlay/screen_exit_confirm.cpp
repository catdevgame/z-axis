// DM2's exit confirmation (screens.h; "InGameUI.FrameExitConf"): the
// screen asked over another one before the game is left. DM2's own code
// (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/camera/camera.h"
#include "dm2/game/goals.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_exit_confirm.cpp.h"

namespace game::dm2::rider::physics
{
    void riderReset(uint32_t index, uint32_t shown); // respawn.cpp
}

namespace game::dm2
{
    namespace
    {
        // "PLAY Root.InGameUI.FrameExitConf.Init" and "... .DeInit".
        constexpr ee::Ptr<char> kExitConfirmInit{sym::exitConfirmInitScript};
        constexpr ee::Ptr<char> kExitConfirmDeInit{sym::exitConfirmDeInitScript};
        constexpr ee::Ptr<const char> kExitConfirmMenu{sym::exitConfirmMenuPath}; // "InGameUI.FrameExitConf.Menu"
        // The menu's line that confirms.
        constexpr int32_t kConfirmYes = 1;
        // Leaving: the fade out (its end leaves the game: the frame's script
        // calls bmxoverlayExitConfReturnToMenu), the frame closed and the
        // three overlay layouts hidden, run on no node.
        constexpr ee::Ptr<char> kLeaveScripts[] = {
            ee::Ptr<char>(sym::exitFadeOutScript),         // "PLAY InGameUI.FadeFilter.FadeOut"
            ee::Ptr<char>(sym::exitConfirmClosedScript),   // "PLAY InGameUI.FrameExitConf.DeInit"
            ee::Ptr<char>(sym::overlayHiddenScript),       // "SET InGameUI.Overlay.Visible {0}"
            ee::Ptr<char>(sym::overlayToggleHiddenScript), // "SET InGameUI.OverlayToggle.Visible {0}"
            ee::Ptr<char>(sym::overlayReplayHiddenScript), // "SET InGameUI.OverlayReplay.Visible {0}"
        };
        // The play overlay's frames hidden as the game is left, in this order.
        constexpr uint32_t kFramesHiddenOnLeaving[] = {kOverlayRuler, kOverlayGauge, kOverlayTrick, kOverlayScore,
                                                       kOverlayTimer};
        // The camera mode a multiplayer game's choice of game is shown with.
        constexpr uint32_t kGameSelectCameraMode = 4;

    }

    // showExitConfirmScreen: the frame's opening or closing script.
    void showExitConfirmScreen(uint32_t shown)
    {
        game::tail(zge::zgui::runScriptLines, *g_uiRoot, shown != 0 ? kExitConfirmInit : kExitConfirmDeInit);
    }
    READABLE_FUNCTION(showExitConfirmScreen, Verified);

    // bmxoverlayExitConfReturnToMenu: the game left: a park test goes back
    // to the park editor, any other game to the front end.
    void bmxoverlayExitConfReturnToMenu()
    {
        game::tail(setGameState, singlePlayerMode() == kParkTest ? kGameStateEditor : kGameStateFrontEnd);
    }
    READABLE_FUNCTION(bmxoverlayExitConfReturnToMenu, Verified);

    // bmxoverlayExitConfHandler: the menu's script function. Not confirmed:
    // back to the screen it was asked over. Confirmed during a turn: the
    // turn ends at once. Confirmed in a multiplayer game between turns
    // (unless asked over the choice of game): every rider put back and the
    // choice of game asked again. Otherwise the game is left: no screen, a
    // ProQuest level's goal entity made active, the fade out, the overlay
    // hidden.
    void bmxoverlayExitConfHandler()
    {
        ee::Local<int32_t> picked;
        zge::zgui::copyVariable(uiWidget(kExitConfirmMenu), kSelectedIndexName, picked);
        if (*picked != kConfirmYes)
        {
            showGameScreen(*g_screenBehindExitConfirm);
            return;
        }
        bool leave = false;
        switch (roundPhase())
        {
        case kGameStartPhase:
        case kTurnStartPhase:
        case kTurnOverPhase:
        case kGameOverPhase:
            if (multiplayerMode() == 0 || *g_screenBehindExitConfirm == kScreenMultiplayerGameSelect)
            {
                leave = true;
                break;
            }
            for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
                rider::physics::riderReset(riderIndexOf(player), 0);
            setCameraPlayer(0);
            setCameraMode(0, 0, kGameSelectCameraMode);
            showGameScreen(kScreenMultiplayerGameSelect);
            break;
        case kFreeTurnPhase:
        case kTurnRunningPhase:
        case kExitPhase:
            endTurnEarly();
            break;
        default:
            leave = true;
            break;
        }
        if (!leave)
            return;
        showGameScreen(kScreenNone);
        if (multiplayerMode() == 0 && singlePlayerMode() == kLevelGoals)
            restartLevelGoals(*g_currentPlayer);
        for (const ee::Ptr<char> script : kLeaveScripts)
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), script);
        for (const uint32_t frame : kFramesHiddenOnLeaving)
            showOverlayFrame(frame, 0);
    }
    READABLE_FUNCTION(bmxoverlayExitConfHandler, Verified);
}
