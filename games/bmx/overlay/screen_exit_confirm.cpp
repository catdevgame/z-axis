// BMX XXX's exit confirmation (screens.h; "FrameExitConf"): asked over
// another screen before the game being played ends, or the level is left.
// BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "bmx/weapon/player_weapon.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_exit_confirm.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        // What the confirmation does once its frame has left.
        enum ExitStep : uint32_t
        {
            kExitNothing = 0,
            kExitToFrontEnd = 1,   // (nothing more: the leaving screen is on its way)
            kExitToEditor = 2,     // (the same)
            kExitEndTurn = 3,      // the turn ends; play's screen if no turn is on
            kExitShowRiders = 4,   // the riders, weapons and cameras put away for the game camera
        };
        inline ee::Global<uint32_t, sym::g_exitStep> g_exitStep;

        constexpr uint32_t kFrameExitConf = sym::g_textFrameExitConf;         // "FrameExitConf"
        constexpr uint32_t kFrameExitConfMenu = sym::g_textFrameExitConfMenu; // "FrameExitConf.Menu"
        // The menu's second line: yes.
        constexpr uint32_t kYesLine = 1;
        // The round's first phase of all (no game started).
        constexpr uint32_t kNoGamePhase = 0;
        constexpr uint32_t kNoPlayPhase = 0;
    }

    void exitConfirmScreenMessage(int32_t message)
    {
        ee::Local<uint32_t> selected;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameExitConf, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
            playUiNode(frame, kInitAction);
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenLeft:
        {
            const uint32_t step = *g_exitStep;
            if (step == kExitEndTurn)
            {
                // A turn that is running ends as it is; one that is over
                // (or the game) first gives each riding player's camera
                // back its first mode.
                const uint32_t phase = roundPhase();
                if (phase < calls::kTurnRunningPhase || phase > kGameOverPhase)
                {
                    showGameScreen(kScreenPlay);
                    *g_resumeAction = kResumePlay;
                    return;
                }
                if (phase != calls::kTurnRunningPhase)
                {
                    for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
                    {
                        if (playerRides(player & 0xFF) != 0)
                            setCameraMode(playerActionCamera(player), 0, 0);
                    }
                }
                setRoundPhase(calls::kTurnOverPhase);
                setGameMode(calls::kSoundModePlay);
            }
            else if (step == kExitShowRiders)
            {
                for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
                {
                    rider::riderReset(riderIndexOf(player), 0);
                    resetWeapon(playerWeapon(player), 0);
                    showCamera(playerActionCamera(player), 0);
                }
                showCamera(*calls::g_gameCamera, 1);
            }
            return;
        }

        case kScreenChosen:
        case kScreenCancelled:
            zge::zgui::copyVariable(zge::zgui::findNode(*g_overlayUi, kFrameExitConfMenu, 1), kSelectedIndexName, selected);
            if (message != kScreenChosen || selected != kYesLine)
            {
                showGameScreen(*g_screenBehindExitConfirm);
                *g_exitStep = kExitNothing;
                return;
            }

            if (*g_gameScreen == kScreenExitGame)
            {
                *g_exitStep = kExitEndTurn;
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumePlay;
                // A multiplayer game goes to its end's screen instead,
                // unless a turn is running or nothing was started.
                if ((gameModeRecord()->flags & kModeMultiplayer) == 0)
                    return;
                const uint32_t phase = roundPhase();
                if (phase == kNoGamePhase || phase == calls::kTurnRunningPhase || phase > kGameOverPhase)
                    return;
                showGameScreen(kScreenAfterGame);
                *g_exitStep = kExitShowRiders;
                return;
            }

            if (*calls::g_sessionKind == calls::kSessionParkTest)
            {
                *g_exitStep = kExitToEditor;
                showGameScreen(kScreenLeaveToEditor);
                return;
            }
            *g_exitStep = kExitToFrontEnd;
            if ((gameModeRecord()->flags & kModeMultiplayer) != 0)
                setPlayPhase(kNoPlayPhase);
            showGameScreen(kScreenLeave);
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(exitConfirmScreenMessage, Verified);
}
