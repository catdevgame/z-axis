#pragma once

// The memory card frame's own state (ui_frame.h is what the rest of the game
// sees): the step it is at, what it has asked of the memory card layer, and
// the slot lists that layer fills. DM2's own code (action.c).
//
// A step is one message of the frame (the string table's KSTR_SAVE_MESSAGE
// of the step's number names each) with its buttons, or one piece of card
// work with a progress bar.

#include "ui_frame.h"

#include "dm2/game/memory_card.h"

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/action/card_frame.h.h"

namespace game::dm2
{
    enum CardFrameStep : uint8_t
    {
        kStepChooseSaveSlot = 0,        // "Choose MEMORY CARD slot:"
        kStepNoCardToSave = 1,          // "No memory card (for PS2) found in MEMORY CARD slot %d."
        kStepAskFormat = 2,             // "... is unformatted. Do you want to format?"
        kStepFormatComplete = 3,        // "Format complete."
        kStepNoSpace = 4,               // "There is insufficient space ..."
        kStepPickReplayToSave = 5,      // "Select a replay slot."
        kStepEnterName = 6,             // "Enter name."
        kStepAskSave = 7,               // "Save game?"
        kStepAskOverwrite = 8,          // "Overwrite old game?"
        kStepSaveComplete = 9,          // "Save complete."
        kStepSaveFailed = 10,           // "Save failed."
        kStepFormatFailed = 11,         // "Format failed."
        kStepCheckingToSave = 12,       // "Checking..."
        kStepFormatting = 13,           // "Formatting..."
        kStepSaving = 14,               // "Saving %s data."
        kStepCreating = 15,             // "Creating library..."
        kStepChooseLoadSlot = 16,       // "Choose MEMORY CARD slot:"
        kStepNoCardToLoad = 17,         // "No memory card (for PS2) found ..."
        kStepPickReplayToLoad = 18,     // "Select a replay slot."
        kStepNoData = 19,               // "No Dave Mirra Freestyle BMX 2 %s data was found ..."
        kStepAskLoad = 20,              // "Load game?"
        kStepLoadComplete = 21,         // "Load complete."
        kStepLoadFailed = 22,           // "Load failed."
        kStepCheckingToLoad = 23,       // "Checking..."
        kStepLoading = 24,              // "Loading %s data."
        kStepPickParkToLoad = 25,       // the park slot list (no message of its own)
        kStepPickParkToSave = 26,       // the same list, to save into
        kStepStartCheckReport = 27,     // the start check's KSTR_BOOT_MESSAGE
        kStepCardChanged = 28,          // "%s failed. The memory card ... was changed."
        kStepCardRemoved = 29,          // "%s failed. The memory card ... was removed."
        kStepOver = 30,                 // asked for when the request has been answered: nothing is shown
        kStepCount = 30,
        kStepNone = 0xFF,
    };

    // The card work the frame last started (g_cardOperation).
    enum CardOperation : uint32_t
    {
        kCardCheck = 0,
        kCardCreate = 1,
        kCardFormat = 2,
        kCardLoad = 3,
        kCardSave = 4,
        kCardSpaceCheck = 5,
        kCardFileLookup = 6,
    };

    // A step's buttons: how many, and the first one's KSTR_SAVE_BUTTON number.
    struct StepButtons
    {
        uint8_t count;
        uint8_t first;
    };

    // What the frame is for (UiFrameKind) and the step it is at.
    inline ee::Global<uint8_t, sym::g_uiFrameKind> g_uiFrameKind;
    inline ee::Global<uint8_t, sym::g_uiFrameStep> g_uiFrameStep;
    // The frames the step has run, and the card answers it has had.
    inline ee::Global<uint32_t, sym::g_uiFrameStepFrames> g_uiFrameStepFrames;
    inline ee::Global<uint8_t, sym::g_uiFrameStepResults> g_uiFrameStepResults;
    // The request the frame works for.
    inline ee::Global<UiFrameRequest, sym::g_uiFrameRequest> g_uiFrameRequest;
    // The memory card port the frame works on, what the cards' check found
    // (0: nothing to say), and the last card operation's result
    // (kCardWorking while one runs).
    inline ee::Global<uint32_t, sym::g_cardPort> g_cardPort;
    inline ee::Global<uint32_t, sym::g_cardCheckResult> g_cardCheckResult;
    inline ee::Global<int32_t, sym::g_cardOperationResult> g_cardOperationResult;
    inline ee::Global<uint32_t, sym::g_cardOperation> g_cardOperation;
    inline ee::Global<CardCall, sym::g_cardCall> g_cardCall;
    inline ee::Global<CardRequest, sym::g_cardRequest> g_cardRequest;
    inline ee::Global<ReplaySlot[kReplaySlots], sym::g_replaySlots> g_replaySlots;
    inline ee::Global<ParkSlot[kParkSlots], sym::g_parkSlots> g_parkSlots;
    // The slots the user picked in the two lists.
    inline ee::Global<uint8_t, sym::g_replaySlotPicked> g_replaySlotPicked;
    inline ee::Global<uint8_t, sym::g_parkSlotPicked> g_parkSlotPicked;
    // "MsgType", the variable the frame's scripts set before they call
    // bmxuiMemCardHandler: 1 the user accepted, -1 backed out, 0 neither.
    inline ee::Global<int32_t, sym::g_uiMessageType> g_uiMessageType;
    constexpr int32_t kMessageAccept = 1, kMessageCancel = -1, kMessageNone = 0;
    inline ee::Global<StepButtons[kStepCount + 1], sym::g_stepButtons> g_stepButtons;

    // zgui's root node: the scripts run on it.
    inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_cardFrameUiRoot;
}
