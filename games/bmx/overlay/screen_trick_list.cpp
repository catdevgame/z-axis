// BMX XXX's trick list screen (screens.h; "FrameTrickList"): bmx.c fills
// and runs the list (ui/trick_list.h); the screen opens and closes it and goes back to the
// screen it was asked from. BMX XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/overlay/screen_calls.h"
#include "bmx/ui/trick_list.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_trick_list.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameTrickList = sym::g_textFrameTrickList;       // "FrameTrickList"
        constexpr uint32_t kTreeViewTrickList = sym::g_textTreeViewTrickList; // "TreeViewTrickList"
    }

    void trickListScreenMessage(int32_t message)
    {
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameTrickList, 1);
        if (!frame)
            return;
        zge::zgui::findNode(frame, kTreeViewTrickList, 1); // (looked up; nothing here uses it)

        switch (message)
        {
        case kScreenEnter:
            game::tail(openTrickList, frame);
            return;
        case kScreenLeaveNow:
            game::tail(closeTrickList, frame);
            return;
        case kScreenChosen:
        case kScreenCancelled:
            switch (*g_gameScreen)
            {
            case kScreenTrickList:
                game::tail(showGameScreen, kScreenPause);
                return;
            case kScreenRoundOverTrickList:
                game::tail(showGameScreen, kScreenRoundOver);
                return;
            case kScreenGameOverTrickList:
                game::tail(showGameScreen, kScreenGameOver);
                return;
            case kScreenNextPlayerTrickList:
                game::tail(showGameScreen, kScreenNextPlayer);
                return;
            default:
                return;
            }
        default:
            return;
        }
    }
    READABLE_FUNCTION(trickListScreenMessage, Verified);

    // The list's two script functions: the selected trick's, and the
    // category's. (Only the second checks that the frame is there.)
    void bmxoverlayTrickListUIUpdateTrick()
    {
        game::tail(updateTrickListTrick, zge::zgui::findNode(*g_overlayUi, kFrameTrickList, 1));
    }
    READABLE_FUNCTION(bmxoverlayTrickListUIUpdateTrick, Verified);

    void bmxoverlayTrickListUIUpdateCategory()
    {
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameTrickList, 1);
        if (frame)
            game::tail(updateTrickListCategory, frame);
    }
    READABLE_FUNCTION(bmxoverlayTrickListUIUpdateCategory, Verified);
}
