// The save system (zsaveapi.c, zge/platform/save_system.cpp): its clients,
// its two message queues and the jobs it starts (the memory card's in
// zge/platform/memory_card/, the host's in zge/host/save_jobs.cpp).
#pragma once

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/save_system.h.h"

namespace zge
{
    struct SaveMessageQueue;

    // What a game gives registerSaveClient: 0x40 bytes, copied. The save system
    // asks the game what to save and tells it what to show through these.
    // (BMX XXX's has a word more in front, 0x44 bytes: registerSaveClient
    // copies 0x44, selectSaveClient reads prepare at +0x24.)
    struct SaveClient
    {
#if ZX_GAME_BMX
        uint32_t _00;
#endif
        uint32_t listBufferBytes;                  // one save's bytes when listing the card
        ee::FunctionPtr<int32_t()> dataSize;       // the save's bytes
        ee::FunctionPtr<uint32_t()> data;          // where they are
        ee::FunctionPtr<uint32_t()> title;         // its title
        ee::FunctionPtr<uint32_t()> fileName;      // its file's name
        ee::FunctionPtr<int32_t()> iconSize;       // the icon's bytes
        ee::FunctionPtr<uint32_t()> icon;          // where they are
        ee::FunctionPtr<int32_t()> singleFile;     // nonzero: this client's save counts
        ee::FunctionPtr<void()> prepare;           // on being selected
        ee::FunctionPtr<void(int32_t kind)> finished;       // the job has ended, with the last message's kind
        ee::FunctionPtr<void(uint32_t name, uint32_t listBuffer)> saveFound; // a save on the card; name 0 starts the list
        ee::FunctionPtr<void(uint32_t text)> showMessage;
#if ZX_GAME_BMX
        // ZGE4's takes a third word: 1 for kMessageShowChoice, 0 for kMessageShowChoiceNoCancel (BMX XXX's
        // showSaveChoice reads it as allowCancel)
        ee::FunctionPtr<void(uint32_t buttons, uint32_t firstChoice, uint32_t allowCancel)> showChoice;
#else
        ee::FunctionPtr<void(uint32_t buttons, uint32_t firstChoice)> showChoice;
#endif
        ee::FunctionPtr<void(ee::Ptr<SaveMessageQueue> queue, ee::Address message, ee::Float word)> message13;
        ee::FunctionPtr<void(uint32_t menu)> showSlotMenu;  // the slot menu (kMessageSlotMenu)
        ee::FunctionPtr<void(uint32_t up)> showFileMenu;    // the list of saves to choose from (kMessageFileMenu)
    };
    static_assert(sizeof(SaveClient) == game::readable::perGame(0x40u, 0x40u, 0x44u));

    // The word ZGE4's record has in front (BMX XXX's game clears it in both of its clients; what reads it
    // is not known yet). The other versions have no such word.
    inline void clearSaveClientLeadWord(SaveClient &client)
    {
#if ZX_GAME_BMX
        client._00 = 0;
#else
        (void)client;
#endif
    }

    // Selected by selectSaveClient: 0 before the first.
    inline ee::Global<ee::Ptr<SaveClient>, sym::g_saveClient> g_saveClient;
    inline ee::Global<ee::Ptr<SaveMessageQueue>, sym::g_saveMessageQueue> g_saveMessageQueue; // what the job says (saveSystemTask reads it)
    inline ee::Global<ee::Ptr<SaveMessageQueue>, sym::g_saveReplyQueue> g_saveReplyQueue;     // what the player says (the job reads it)

    // A message on either queue: its kind (below; 0 is a free slot) and
    // what the kind carries.
    struct SaveMessage
    {
        int32_t kind;       // +0x00
        uint32_t value;     // +0x04 the text, the menu's data, the choice's buttons, the answer, a found save's name
        uint32_t listBuffer; // +0x08 a found save's first bytes
        uint32_t choice;    // +0x0C the choice's button chosen at first
    };
    static_assert(offsetof(SaveMessage, listBuffer) == 0x08 && sizeof(SaveMessage) == 0x10);

    // newSaveMessageQueue's: a ring of `limit` messages, taken oldest first.
    struct SaveMessageQueue
    {
        uint32_t limit;    // +0x00
        uint32_t oldest;   // +0x04 the next message to take
        SaveMessage first; // +0x08, the first of `limit`
    };
    static_assert(offsetof(SaveMessageQueue, first) == 0x08);

    // postSaveMessage: a copy of the message on the queue; takeSaveMessage:
    // the oldest taken off it (with wait, once there is one).
    uint32_t postSaveMessage(ee::Ptr<SaveMessageQueue> queue, ee::Ptr<SaveMessage> message);
    uint32_t takeSaveMessage(ee::Ptr<SaveMessageQueue> queue, ee::Ptr<SaveMessage> messageOut, uint32_t wait);

    // The messages' kinds. ZGE4 has one more, kMessageShowChoiceNoCancel, after kMessageShowChoice: the three
    // after it are one higher there (BMX XXX's saveSystemTask's jump table, kinds 8 .. 16, and its save
    // jobs' file menu, 16). kMessage13 is ZGE3's kind 13 (what it shows is not known). A kind below
    // kMessageSaveFound is a job's end, with how it went: done, cancelled (or nothing there to load),
    // failed (its message has been shown).
    constexpr int32_t kMessageDone = 2, kMessageCancelled = 4, kMessageFailed = 5,
                      kMessageSaveFound = 8, kMessageAnswer = 9, kMessageFileChosen = 10,
                      kMessageShowText = 11, kMessageShowChoice = 12,
                      kMessageShowChoiceNoCancel = 13, // ZGE4 only: showChoice with its third word 0
                      kMessage13 = game::readable::perGame(13, 13, 14),
                      kMessageSlotMenu = game::readable::perGame(14, 14, 15),
                      kMessageFileMenu = game::readable::perGame(15, 15, 16);

    // The save jobs: each save or load runs as a job on a thread of its own
    // (one at a time), which talks to the client through the two queues and
    // ends with a message of how it went (the kinds above); saveSystemTask
    // wakes it every frame. The PS2 form of the jobs is the memory card's
    // (zge/platform/memory_card/save_jobs.cpp: the slot asked for, the card
    // checked, libmc's commands); the host's is zge/host/save_jobs.cpp, on
    // plain files.
    uint32_t initSaveJobs();
    void freeSaveBuffer();
    void wakeSaveJob();
    int32_t waitMilliseconds(uint32_t milliseconds);
    // A job started, after waiting `delayMilliseconds` first: the boot's
    // check for room, a save, a load from the menus, the boot's quiet load,
    // a save and a load from a list of saves the player chooses from (the
    // parks), and the job that only says it is done (no caller).
    int32_t startAutoLoadJob(uint32_t delayMilliseconds);
    int32_t startSaveJob(uint32_t delayMilliseconds);
    int32_t startLoadJob(uint32_t delayMilliseconds);
    int32_t startSilentLoadJob(uint32_t delayMilliseconds);
    int32_t startLoadChoiceJob(uint32_t delayMilliseconds);
    int32_t startSaveChoiceJob(uint32_t delayMilliseconds);
    int32_t startNullJob(uint32_t delayMilliseconds);
    // The job's thread (0: none) and the list buffer of a choice job (each
    // save's first bytes, listBufferBytes of the client's, as the list reads them).
    inline ee::Global<int32_t, sym::g_jobThread> g_jobThread;
    inline ee::Global<uint32_t, sym::g_saveListBuffer> g_saveListBuffer;
}

namespace zge
{
    // forEachSaveClient's visit: (client, value), 0 to stop.
    using SaveClientVisit = ee::FunctionPtr<int32_t(ee::Ptr<SaveClient> client, uint32_t value)>;

    // (save_system.cpp)
    uint32_t registerSaveClient(ee::Address client);
    uint32_t forEachSaveClient(SaveClientVisit callback, ee::Address value);
    int32_t answerSaveMenu(uint32_t answer);
    int32_t cancelSaveMenu();
    int32_t answerSaveFileChoice(ee::Address name);
    int32_t startSaveSystem();
    int32_t stopSaveSystem();
    int32_t saveApiAutoLoad(uint32_t client, uint32_t delayMilliseconds);
    int32_t saveApiSave(uint32_t client, uint32_t delayMilliseconds);
    int32_t saveApiLoad(uint32_t client, uint32_t delayMilliseconds READABLE_PARAM_ONLY(bmx, uint32_t unreadWord));
    int32_t saveApiSilentLoad(uint32_t client, uint32_t delayMilliseconds);
    int32_t saveApiSaveChoice(uint32_t client, uint32_t delayMilliseconds);
    int32_t saveApiLoadChoice(uint32_t client, uint32_t delayMilliseconds READABLE_PARAM_ONLY(bmx, uint32_t unreadWord));
    int32_t byteChecksum(ee::Address data, uint32_t bytes);
    int32_t askSaveQuestion(uint32_t buttons, uint32_t firstChoice, uint32_t poll);
}
