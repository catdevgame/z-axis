// DM2's round over screen (screens.h; "InGameUI.FrameRoundOver"): its show
// and its menu's script function. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/menu/front_end.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_round_over.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kRoundOverFrame{sym::g_textFrameRoundOver}; // "InGameUI.FrameRoundOver"
        // Where the front end opens when the game is left (front_end.h).
        inline ee::Global<uint8_t, sym::g_frontEndOpens> g_frontEndOpens;
        // The menu's lines, in its order (-1: backed out of, nothing done).
        enum RoundOverLine : int32_t
        {
            kRoundOverNextTurn = 0,
            kRoundOverLeaderboard = 1,
            kRoundOverViewReplay = 2,
            kRoundOverTrickList = 3,
            kRoundOverRecords = 4,
            kRoundOverExit = 5,
        };
    }

    // As the screen opens, its frame's Init; as it closes, its DeInit.
    void showRoundOverScreen(uint32_t shown)
    {
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kRoundOverFrame);
        if (!frame)
            return;
        game::tail(playWidgetAction, frame, shown != 0 ? kInitAction : kDeInitAction);
    }
    READABLE_FUNCTION(showRoundOverScreen, Verified);

    // bmxoverlayRoundOverHandler: the menu's line taken: the next turn, the
    // leaderboard, the round's replay (from the current player's view), the
    // trick list, the records, or leaving the game through the exit
    // confirmation (for the front end's main menu).
    void bmxoverlayRoundOverHandler()
    {
        ee::Local<int32_t> picked;
        zge::zgui::copyVariable(zge::zgui::findNode(uiWidget(kRoundOverFrame), kMenuName, 0), kSelectedIndexName, picked);
        switch (*picked)
        {
        case kRoundOverNextTurn:
            showGameScreen(kScreenNone);
            setRoundPhase(kTurnStartPhase);
            break;
        case kRoundOverLeaderboard:
            showGameScreen(kScreenLeaderboard);
            break;
        case kRoundOverViewReplay:
            showGameScreen(kScreenNone);
            actionStart(kActionRoundReplay);
            *g_actionRequest = static_cast<int8_t>(*g_currentPlayer);
            break;
        case kRoundOverTrickList:
            showGameScreen(kScreenRoundOverTrickList);
            break;
        case kRoundOverRecords:
            showGameScreen(kScreenRoundOverRecords);
            break;
        case kRoundOverExit:
            *g_screenBehindExitConfirm = kScreenRoundOver;
            showGameScreen(kScreenExitConfirm);
            *g_frontEndOpens = kOpensMainMenu;
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayRoundOverHandler, Verified);
}
