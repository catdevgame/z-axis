#pragma once

// The memory card frame ("CommonUI.FrameMemCard", ui_frame.cpp): the UI frame
// that saves or loads a game, a replay or a park, checks the cards when the
// game starts, and tells the user what the card says. A menu asks for it
// with a request: the data to save or fill and what to call when the frame
// is over. DM2's own code (action.c).

#include "ee/ee.h"
#include "ee/function_ptr.h"

#include <cstddef>
#include <cstdint>

namespace zge::zgui
{
    struct Node; // zge/zgui/widget.h
}

namespace game::dm2
{
    // What the frame is for. The save kinds write a file, the load kinds read
    // one; the two others belong to the intro: the cards checked before its
    // menu, and the saved game loaded by itself from whichever card has one
    // (the frame's title for it is "AUTO LOAD").
    enum UiFrameKind : uint32_t
    {
        kUiFrameSaveGame = 0,
        kUiFrameSaveReplay = 1,
        kUiFrameLoadGame = 2,
        kUiFrameLoadReplay = 3,
        kUiFrameStartCheck = 4,
        kUiFrameSavePark = 5,
        kUiFrameLoadPark = 6,
        kUiFrameAutoLoad = 7,
    };

    // The kind of file a frame works on (uiFrameFileKind).
    enum CardFileKind : uint32_t
    {
        kCardFileGame = 0,
        kCardFileReplay = 1,
        kCardFilePark = 2,
    };

    // A request's callbacks get its data and the data's handle.
    using UiFrameCallback = ee::FunctionPtr<void(uint32_t data, uint32_t dataHandle)>;

    // What a menu hands the frame: `onDone` runs when the frame's work is
    // done (the file saved or loaded, the cards found in order), `onClosed`
    // when the frame closes without it (the user backs out, or has read what
    // the card said). `data` is the block to save or to load into; the park
    // editor's save also gives the handle that holds it, the others 0.
    struct UiFrameRequest
    {
        UiFrameCallback onClosed;
        uint32_t data;
        uint32_t dataHandle;
        UiFrameCallback onDone;
    };
    static_assert(offsetof(UiFrameRequest, data) == 0x04 && offsetof(UiFrameRequest, onDone) == 0x0C);
    static_assert(sizeof(UiFrameRequest) == 0x10);

    // The frame turned on for a kind of work with a request (0: the last
    // request's callbacks stay), or turned off (`on` 0: the callbacks
    // cleared). The start check tells the request it is done at once when
    // the cards have nothing to say.
    void setUiFrame(uint32_t on, uint32_t kind, ee::Ptr<UiFrameRequest> request);
    // Whether the frame's kind writes a file (a game, a replay or a park saved).
    uint32_t uiFrameSaves();
    // The kind of file a frame kind works on.
    uint32_t uiFrameFileKind(uint32_t kind);
    // The card operation's completion callback: its result kept for the
    // frame's next run.
    void uiFrameCardDone(uint32_t operation, int32_t result);

    // The frame's run, every frame of a game state that can show it: the
    // title set, and at a step that works on the card the work started, its
    // progress bar moved and, once the card layer has answered, the next
    // step chosen. Always 0.
    uint32_t uiFrameUpdate();
    // The frame moved to a step (card_frame.h): the step's parts of the UI
    // shown, its message and its buttons set.
    void setUiFrameStep(uint32_t step);
    // "bmxuiMemCardHandler", the script function the frame's menus call when
    // the user accepts or backs out: the next step from the answer.
    void bmxuiMemCardHandler();
    // The replay slot list's answer (bmxuiMemCardHandler at the two steps
    // that show it).
    void replaySlotMenu();
    // The park slot list's answer; leaving the list frees its items.
    void parkSlotMenu();
    // The park slot list of the memory card frame filled (name, theme and
    // author of each of the card's fifty slots) and shown.
    void showParkSlots(ee::Ptr<zge::zgui::Node> frame);

    // The game's UI set up, once: zgui with its pads, sounds and buttons,
    // the UI's asset library, the memory card frame's script function and
    // variables, the string table and the common UI's script.
    void initGameUi();
    // The common UI deleted and zgui's memory compacted. dmmCompact's result.
    uint32_t unloadCommonUi();
}
