#pragma once
// DM2's memory card layer (game.c's range): the card port in use and the
// kind of file meant, under the memory card frame (action/ui_frame.h).

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // What the card layer answers (g_cardOperationResult, and checkMemoryCards
    // for the start check), named from the step each leads to.
    enum CardResult : int32_t
    {
        kCardWorking = -1,
        kCardDone = 0,
        kCardMissing = 1,
        kCardChanged = 3,
        kCardRemoved = 4,
        kCardUnformatted = 5,
        kCardNoFile = 6,
        kCardHasFile = 7,
        kCardFull = 8,
        kCardBadSave = 9,  // a game save whose size word is not a game save's
        kCardFailed = 10,  // the card work itself failed (a file not opened, read or written whole)
    };

    // The game save, the file as the card has it (BMX2SAVE.DAT) and as the
    // native saves keep it: the saved game (g_savedGame) and every level's
    // saved records (g_savedRecords), after a magic word and the file's size
    // (a load checks the size alone).
    constexpr uint32_t kSavedGameBytes = 0x4F98;
    constexpr uint32_t kSavedRecordsBytes = 0x10E0;
    constexpr uint32_t kGameSaveMagic = 0x2A81511C;
    struct GameSaveFile
    {
        uint32_t magic;
        uint32_t bytes;
        uint8_t game[kSavedGameBytes];
        uint8_t records[kSavedRecordsBytes];
    };
    static_assert(offsetof(GameSaveFile, records) == 0x4FA0 && sizeof(GameSaveFile) == 0x6080);

    // A park as the card's park library keeps each one: one park stream.
    constexpr uint32_t kParkBytes = 0x2565;

    // A replay slot of the card as the look-up lists it.
    struct ReplaySlot
    {
        uint32_t flags; // kSlotUsed
        char name[0x1C];
    };
    static_assert(sizeof(ReplaySlot) == 0x20);
    constexpr uint32_t kReplaySlots = 8;

    // A park slot of the card.
    struct ParkSlot
    {
        uint32_t flags; // kSlotUsed
        char name[0x20];
        char author[0x20];
        char theme[0x14];
    };
    static_assert(offsetof(ParkSlot, author) == 0x24 && offsetof(ParkSlot, theme) == 0x44 && sizeof(ParkSlot) == 0x58);
    constexpr uint32_t kParkSlots = 50;

    constexpr uint32_t kSlotUsed = 0x1;

    // What a card operation works on: the slot picked, the names entered for
    // it, the lists a look-up fills, and the request's data.
    struct CardRequest
    {
        uint32_t slot;
        ee::Ptr<char> name;
        ee::Ptr<char> author;
        ee::Ptr<ReplaySlot> replaySlots;
        ee::Ptr<ParkSlot> parkSlots;
        uint32_t data;
    };
    static_assert(offsetof(CardRequest, replaySlots) == 0x0C && sizeof(CardRequest) == 0x18);

    // What the memory card frame hands a piece of card work: the callback
    // for its result (uiFrameCardDone) and that callback's first word.
    struct CardCall
    {
        ee::FunctionPtr<void(uint32_t operation, int32_t result)> done;
        uint32_t argument;
    };
    static_assert(sizeof(CardCall) == 0x08);

    // The kind of file the card functions that follow mean (a CardFileKind,
    // action/ui_frame.h: the game's save "BMX2SAVE.DAT", a replay
    // "BMX2RPLY.DAT", a park "BMX2PARK.DAT", each in its own folder).
    void setCardFileKind(uint32_t fileKind);

    // A card port (0 or 1) made the one in use: the memory card device
    // found and the port's card mounted. The mount's answer.
    uint32_t selectCardPort(uint32_t port);

    // Both ports looked at for a card the game's files can go on; what there
    // is to tell the user (0: nothing).
    constexpr uint32_t kCardsNoCard = 1; // no card in either port
    constexpr uint32_t kCardsFull = 8;   // a card without the room for the files
    uint32_t checkMemoryCards(ee::Ptr<uint32_t> port);

    // Called at the memory card frame's start and end; both are empty in
    // this build.
    void cardFrameStartHook();
    void cardFrameEndHook();

    // After work on the card failed: whether the card was changed or taken
    // out meanwhile. If so, `result` becomes the reason (kCardChanged or
    // kCardRemoved, action/card_frame.h) and the answer is 1, else 0.
    uint32_t recheckCard(ee::Ptr<int32_t> result);

    // The save folder of the kind of file meant made on the card when it is
    // not there yet, with the files the PS2's browser shows it by (the icon,
    // icon.sys and the folder's own file); what was written is deleted again
    // when one of them fails. A CardResult (0: done).
    int32_t createSaveFolder();

    // Whether the save folder and the files it is shown by are on the card
    // whole, and, for a replay or a park library, the library file too
    // (1: they are).
    uint32_t cardSaveComplete();

    // The pieces of card work the memory card frame starts. Each answers a
    // CardResult at once when it could not start (having told the call's
    // callback); else 0, and the callback hears the result when the card has
    // done the work.
    int32_t startCardCheck(ee::Ptr<CardCall> call);      // a card in the port, and formatted
    int32_t startCardFileLookup(ee::Ptr<CardCall> call); // the file there (a library's slots read into the argument)
    int32_t startCardSpaceCheck(ee::Ptr<CardCall> call); // the room for the file
    int32_t startCardFormat(ee::Ptr<CardCall> call);
    int32_t startCardLoad(ee::Ptr<CardCall> call);
    int32_t startCardSave(ee::Ptr<CardCall> call);
    int32_t startCardCreate(ee::Ptr<CardCall> call);     // an empty library file made
}
