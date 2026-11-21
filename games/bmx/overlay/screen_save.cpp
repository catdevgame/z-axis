// BMX XXX's two save screens (screens.h): the save system puts up its own
// screens; these only start it. BMX XXX's own code
// (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_save.cpp.h"

namespace game::bmx
{
    // saveGameScreenMessage: the save started, and the machine left with no
    // menu.
    void saveGameScreenMessage(int32_t message)
    {
        ee::Local<Menu> menu;
        if (message != kScreenEnter)
            return;
        startGameSave();
        menu->data = 0;
        menu->handler = {};
        menu->id = kNoMenu;
        setMenu(menu);
    }
    READABLE_FUNCTION(saveGameScreenMessage, Verified);

    // saveReplayScreenMessage: the shown replay's saved part is asked for
    // at every message (nothing here uses the answer); the save starts as
    // the screen enters.
    void saveReplayScreenMessage(int32_t message)
    {
        ee::Local<uint32_t> start;
        ee::Local<uint32_t> bytes;
        replaySavedPart(shownReplay(), start, bytes);
        if (message == kScreenEnter)
            startGameSave();
    }
    READABLE_FUNCTION(saveReplayScreenMessage, Draft);
}
