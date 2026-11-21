// DM2's save screens (screens.h): the game and the shown replay saved to
// a memory card from the game's end, through the UI frame (action/ui_frame.h).
// DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/ui_frame.h"
#include "dm2/game/round.h"
#include "dm2/replay/replays.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_save.cpp.h"

namespace game::dm2
{
    // saveScreenClosed: the UI frame of a save screen closed: back to the
    // game over screen, and the UI frame off.
    void saveScreenClosed()
    {
        showGameScreen(kScreenGameOver);
        game::tail(setUiFrame, 0, 0, ee::Ptr<UiFrameRequest>());
    }
    READABLE_FUNCTION(saveScreenClosed, Verified);

    // showSaveGameScreen: the save game screen opens or closes: the UI
    // frame turned on to save the game (off as the screen closes), with
    // nothing to hand it but the way back.
    void showSaveGameScreen(uint32_t shown)
    {
        ee::Local<UiFrameRequest> request;
        request->onClosed = UiFrameCallback(sym::saveScreenClosed);
        request->data = 0;
        request->dataHandle = 0;
        request->onDone = UiFrameCallback();
        setUiFrame(shown, kUiFrameSaveGame, request);
    }
    READABLE_FUNCTION(showSaveGameScreen, Verified);

    // showSaveReplayScreen: the same for the replay the game shows: the UI
    // frame is handed the replay's saved part.
    void showSaveReplayScreen(uint32_t shown)
    {
        ee::Local<UiFrameRequest> request;
        ee::Local<uint32_t> start;
        ee::Local<uint32_t> bytes;
        request->onClosed = UiFrameCallback(sym::saveScreenClosed);
        replaySavedPart(shownReplay(), start, bytes);
        request->data = start;
        request->dataHandle = 0;
        request->onDone = UiFrameCallback();
        setUiFrame(shown, kUiFrameSaveReplay, request);
    }
    READABLE_FUNCTION(showSaveReplayScreen, Verified);

    // saveScreenInput: the save screens' own input (their records' input
    // handler): the memory card frame runs; the screens' menu input then
    // goes on (0).
    uint32_t saveScreenInput()
    {
        uiFrameUpdate();
        return 0;
    }
    READABLE_FUNCTION(saveScreenInput, Verified);
}
