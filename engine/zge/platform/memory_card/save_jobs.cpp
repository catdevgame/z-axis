// The memory card saves (the first part of zge/ps2/Ps2MemCard.c, initSaveJobs ..
// were the sweep's drafts): one job thread at a time runs a save, a
// load or a card check, tells the save client's interface what to show through
// the save system's message queues, and asks the player through
// askSaveQuestion; a libmc command ends with sceMcSync, polled with a sleep
// between polls. The job bodies (autoLoadJob .. nullJob) are at the end of the
// file: a job ends by deleting its thread, so it never returns (see
// endSaveJob for how one is recorded).
//
// A step of a job answers what the job does next:
//   kCardOk      go on
//   kCardAgain   start over (the card changed, or Retry)
//   kCardCancel  leave without saving or loading
//   kCardFailed  the command failed; the message has been shown
//   kCardSkip    go on without the card

#include "zge/platform/save_system.h"
#include "zge/platform/memory_card/memory_card.h"

#include "ee/io.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/kernel.h"
#include "sce/libmc.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/debug/cycle_counter.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/memory_card/save_jobs.cpp.h"

namespace zge
{
    void toSjisSaveName(ee::Ptr<const char> text, ee::Ptr<uint16_t> out);
    uint32_t asciiToSjis(uint32_t character);
    bool saveJobReturnsForRecording(); // job_recording.cpp

    namespace
    {
        using sce::sceMcResChangedCard;
        using sce::sceMcResDeniedPermit;
        using sce::sceMcResFullDevice;
        using sce::sceMcResNoEntry;
        using sce::sceMcResNoFormat;
        // libmc's card types (sceMcTypePS1, sceMcTypePS2): a PlayStation card, a PlayStation 2 card.
        constexpr int32_t kPlayStationCard = sce::sceMcTypePS1;
        constexpr int32_t kPlayStation2Card = sce::sceMcTypePS2;

        // The game functions held as data: the question's card poll, the walk over the clients, the job bodies.
        constexpr uint32_t kMemoryCardPresent = sym::memoryCardPresent;
        constexpr uint32_t kMemoryCardFormatted = sym::memoryCardFormatted;
        constexpr uint32_t kFindMemoryCard = sym::findMemoryCard;
        constexpr uint32_t kSaveClientFits = sym::saveClientFits;
        constexpr uint32_t kAppendClientRequiredSpace = sym::appendClientRequiredSpace;

        // A port's only slot, and an output of a library call nobody wants.
        constexpr int32_t kSlot = 0;
        constexpr uint32_t kNotWanted = 0;

        constexpr int32_t kOpenRead = sce::kMcOpenRead, kOpenWrite = sce::kMcOpenWrite;
        constexpr int32_t kOpenCreate = sce::kMcOpenReadWrite | sce::sceMcFileCreateFile;
        constexpr int32_t kSeekEnd = 2;
        constexpr int32_t kCardGoneBelow = -9; // a result under this: the card has gone

        enum : int32_t
        {
            kCardOk = 2,
            kCardAgain = 3,
            kCardCancel = 4,
            kCardFailed = 5,
            kCardSkip = 6,
        };

        // What askSaveQuestion gives: the button, or the player backing out, or the card poll saying the card changed.
        constexpr int32_t kFirstButton = 0, kSecondButton = 1, kThirdButton = 2;
        constexpr int32_t kAnswerBack = -1, kAnswerCardChanged = -2;

        constexpr uint32_t kTextNoCardInserted = sym::g_textMemcardNoCardInsertedNN;  // "$MEMCARD_NO_CARD_INSERTED\\n\\n"
        constexpr uint32_t kTextCheckingSlot = sym::g_textMemcardCheckingSlot;    // "$MEMCARD_CHECKING_SLOT"
        constexpr uint32_t kTextLibraryEmpty = sym::textLibraryEmpty;    // "$MEMCARD_LIB_EMPTY"
        constexpr uint32_t kTextSlotSelect = sym::textSlotSelect;      // "$MEMCARD_SLOT_SELECT"
        constexpr uint32_t kTextListingFailed = sym::textListingFailed;   // "Listing failed\nMemory card has changed"
        constexpr uint32_t kTextRemoved = sym::textRemoved;         // "$MEMCARD_REMOVED"
        constexpr uint32_t kTextSavingFailed = sym::textSavingFailed;    // "$MEMCARD_SAVING_FAILED"
        constexpr uint32_t kTextOverwrite = sym::textOverwrite;       // "$MEMCARD_OVERWRITE"
        constexpr uint32_t kTextSavingComplete = sym::textSavingComplete;  // "$MEMCARD_SAVING_COMPLETE"
        constexpr uint32_t kTextLoadingFailed = sym::textLoadingFailed;   // "$MEMCARD_LOADING_FAILED"
        constexpr uint32_t kTextConfirmLoad = sym::textConfirmLoad;     // "$MEMCARD_CONFIRM_LOAD"
        constexpr uint32_t kTextLoading = sym::textLoading;         // "$MEMCARD_LOADING"
        constexpr uint32_t kTextLoadingCorrupt = sym::textLoadingCorrupt;  // "$MEMCARD_LOADING_FAILED_CORRUPT"
        constexpr uint32_t kTextLoadingComplete = sym::textLoadingComplete; // "$MEMCARD_LOADING_COMPLETE"
        constexpr uint32_t kTextNoCard = sym::textNoCard;          // "$MEMCARD_NO_CARD_INSLOT"
        constexpr uint32_t kTextNeedFormatReadOnly = sym::textNeedFormatReadOnly; // "$MEMCARD_NEEDFORMAT_READONLY"
        constexpr uint32_t kTextFormatting = sym::textFormatting;      // "$MEMCARD_FORMATTING"
        constexpr uint32_t kTextFormatFailedChange = sym::textFormatFailedChange; // "$MEMCARD_FORMAT_FAILED_CHANGE"
        constexpr uint32_t kTextFormatFailedRemove = sym::textFormatFailedRemove; // "$MEMCARD_FORMAT_FAILED_REMOVE"
        constexpr uint32_t kTextFormatSuccess = sym::textFormatSuccess;   // "$MEMCARD_FORMAT_SUCCESS"
        constexpr uint32_t kTextFormatFailed = sym::textFormatFailed;    // "$MEMCARD_FORMAT_FAILED"
        constexpr uint32_t kTextNeedSpaceOne = sym::textNeedSpaceOne;    // "$MEMCARD_NEEDSPACE\n$MEMCARD_REQUIRED_ONE"
        constexpr uint32_t kTextNeedSpaceAll = sym::textNeedSpaceAll;    // "$MEMCARD_NEEDSPACE\n\n"
        constexpr uint32_t kTextNeedFormat = sym::textNeedFormat;      // "$MEMCARD_NEEDFORMAT"
        constexpr uint32_t kTextConfirmSave = sym::textConfirmSave;     // "$MEMCARD_CONFIRM_SAVE"
        constexpr uint32_t kTextNeedSpace = sym::textNeedSpace;       // "$MEMCARD_NEEDSPACE\n"
        constexpr uint32_t kTextAccessError = sym::textAccessError;     // "Error accessing memory card (PS2) in MEMORY CARD slot %d"
        constexpr uint32_t kTextNoNewSave = sym::textNoNewSave;       // "Cannot generate a new save game"
        constexpr uint32_t kTextFileNotFound = sym::textFileNotFound;    // "$MEMCARD_FILE_NOT_FOUND"
        constexpr uint32_t kTextRequiredPrefix = sym::textRequiredPrefix;  // "$MEMCARD_REQUIRED_ALL_PREFIX"
        constexpr uint32_t kTextRequiredSuffix = sym::textRequiredSuffix;  // "$MEMCARD_REQUIRED_ALL_SUFFIX"
        constexpr uint32_t kTextRequiredEntry = sym::textRequiredEntry;   // "$MEMCARD_REQUIRED_ENUM"
        constexpr uint32_t kTextNoText = sym::textNoText;          // ""
        constexpr uint32_t kTextOutOfSpace = sym::textOutOfSpace;      // "Out of space"
        constexpr uint32_t kTextSaving = sym::textSaving;          // "$MEMCARD_SAVING"
        constexpr uint32_t kButtonsRetryContinueWithoutSaving = sym::g_textRetryContinueWithoutSaving; // "Retry\nContinue without saving"
        constexpr uint32_t kButtonsOverwriteCancel = sym::buttonsOverwriteCancel; // "Overwrite\nCancel"
        constexpr uint32_t kButtonsRetryFormatCancel = sym::buttonsRetryFormatCancel; // "Retry\nFormat\nCancel"
        constexpr uint32_t kButtonsRetryCancel = sym::buttonsRetryCancel;  // "Retry\nCancel"
        constexpr uint32_t kButtonsRetryCancelEscaped = sym::buttonsRetryCancelEscaped; // "Retry\\nCancel": the backslash is in the data
        constexpr uint32_t kButtonsYesNo = sym::buttonsYesNo;        // "Yes\nNo"
        constexpr uint32_t kButtonsRetryContinue = sym::buttonsRetryContinue; // "Retry\nContinue"

        constexpr uint32_t kTestDirectory = sym::testDirectory;       // "/CRAP"
        constexpr uint32_t kTestFile = sym::testFile;            // "CRAP.DAT"
        constexpr uint32_t kRootPath = sym::rootPath;            // "/"
        constexpr uint32_t kAllFiles = sym::allFiles;            // "*"
        constexpr uint32_t kCurrentDirectory = sym::currentDirectory;    // "."
        constexpr uint32_t kIconFile = sym::iconFile;            // "game_icon.ico"
        constexpr uint32_t kIconSysFile = sym::iconSysFile;         // "icon.sys"
        constexpr uint32_t kDirectoryFormatStart = sym::directoryFormatStart; // "/%s%0": the number's width goes after it
        constexpr uint32_t kWidthFormat = sym::widthFormat;         // "%dd"
        constexpr int32_t kDirectoryNameDigits = 20;           // the client's name and the number together

        constexpr uint32_t kNoCardText = sym::noCardText;     // the auto-load job's message
        constexpr uint32_t kNeedSpaceText = sym::needSpaceText;  // checkMemoryCardSpace's message
        constexpr uint32_t kRequiredEntryText = sym::requiredEntryText;

        constexpr uint32_t kFormatNoticeMilliseconds = 750u, kShowMilliseconds = 2000u, kBetweenStepsMilliseconds = 1000u,
                           kPollMilliseconds = 0x10u, kSpaceMessageMilliseconds = 250u;

        constexpr uint32_t kJobThreadStack = sym::jobThreadStack;
        constexpr uint32_t kJobThreadStackBytes = 0x1000u;
        constexpr uint32_t kJobThreadParameters = sym::jobThreadParameters;
        constexpr uint32_t kIconSys = sym::iconSys;
        constexpr uint32_t kIconSysBytes = sizeof(sce::mcIcon);
        inline ee::Global<int32_t, sym::g_memoryCardPort> g_cardPort;
        inline ee::Global<sce::ThreadParam, kJobThreadParameters> g_jobThreadParameters;
        inline ee::Global<uint32_t, sym::g_jobDelayMilliseconds> g_jobDelayMilliseconds; // what the job waits first
        inline ee::Global<sce::mcIcon, kIconSys> g_iconSys;
        inline ee::Global<uint32_t, sym::g_saveMark> g_saveMark; // markSaveTime's milliseconds

        // The game functions below call: ee/plain.h. libmc first: each of these
        // starts a command on the IOP, and sceMcSync tells when it has ended.
        void sceMcInit() { ee::game(sym::sceMcInit); }
        int32_t sceMcSync(int32_t mode, ee::Address command, ee::Address result) { return ee::game<int32_t>(sym::sceMcSync, mode, command, result); }
        void sceMcGetInfo(int32_t port, int32_t slot, ee::Address type, ee::Address free, ee::Address format) { ee::game(sym::sceMcGetInfo, port, slot, type, free, format); }
        void sceMcOpen(int32_t port, int32_t slot, ee::Address name, int32_t mode) { ee::game(sym::sceMcOpen, port, slot, name, mode); }
        void sceMcClose(int32_t file) { ee::game(sym::sceMcClose, file); }
        void sceMcSeek(int32_t file, int32_t offset, int32_t origin) { ee::game(sym::sceMcSeek, file, offset, origin); }
        void sceMcRead(int32_t file, ee::Address buffer, int32_t bytes) { ee::game(sym::sceMcRead, file, buffer, bytes); }
        void sceMcWrite(int32_t file, ee::Address buffer, int32_t bytes) { ee::game(sym::sceMcWrite, file, buffer, bytes); }
        void sceMcFlush(int32_t file) { ee::game(sym::sceMcFlush, file); }
        void sceMcMkdir(int32_t port, int32_t slot, ee::Address name) { ee::game(sym::sceMcMkdir, port, slot, name); }
        void sceMcChdir(int32_t port, int32_t slot, ee::Address directory, ee::Address currentOut) { ee::game(sym::sceMcChdir, port, slot, directory, currentOut); }
        void sceMcGetDir(int32_t port, int32_t slot, ee::Address pattern, int32_t mode, int32_t maxEntries, ee::Address table) { ee::game(sym::sceMcGetDir, port, slot, pattern, mode, maxEntries, table); }
        int32_t sceMcFormat(int32_t port, int32_t slot) { return ee::game<int32_t>(sym::sceMcFormat, port, slot); }

        // The kernel's.
        void sleepThread() { ee::game(sym::sleepThread); }
        int32_t createThread(uint32_t parameters, uint32_t delayMilliseconds, uint32_t globalPointer) { return ee::game<int32_t>(sym::createThread, parameters, delayMilliseconds, globalPointer); }
        namespace tail
        {
        int32_t startThread(int32_t thread, uint32_t argument) { return ee::tailCall<int32_t>(sym::startThread, thread, argument); }
        void releaseWaitThread(int32_t thread) { ee::tailCall<int32_t>(sym::releaseWaitThread, thread); }
        int32_t exitDeleteThread() { return ee::tailCall<int32_t>(sym::exitDeleteThread); }
        }

        // The save system's: the line the interface shows, with the words its text reads.
        void formatLocalizedText(ee::Address text) { ee::game(sym::formatLocalizedText, text); }
        void formatLocalizedText(ee::Address text, int32_t word) { ee::game(sym::formatLocalizedText, text, word); }
        void formatLocalizedText(ee::Address text, uint32_t word, int32_t word2) { ee::game(sym::formatLocalizedText, text, word, word2); }
        void formatLocalizedText(ee::Address text, int32_t word, int32_t word2, uint32_t word3) { ee::game(sym::formatLocalizedText, text, word, word2, word3); }
        void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game(sym::memset, destination, byte, bytes); }

        // sceMcSync until the command is done, sleeping between polls.
        void waitForCard(ee::Local<int32_t> &result, ee::Address command = 0u)
        {
            while (sceMcSync(sce::sceMcSyncPoll, command, result) == 0)
                sleepThread();
        }

        // The same without the sleep; what sceMcSync said.
        int32_t spinForCard(ee::Local<int32_t> &result, ee::Address command = 0u)
        {
            for (;;)
            {
                const int32_t done = sceMcSync(sce::sceMcSyncPoll, command, result);
                if (done != 0)
                    return done;
            }
        }

        // sceMcGetInfo's result: did a card answer (the same one, another, or unformatted)?
        bool cardAnswered(int32_t result) { return result >= sceMcResNoFormat && result <= 0; }

        // A path on the card: "/", the rest clear for the name that gets appended.
        void startRootPath(ee::Local<char[64]> &path)
        {
            path[0] = '/';
            path[1] = '\0';
            memset(ee::Ptr<char>(path) + 2, 0, 62);
        }

        // A path's directory name, past its "/".
        ee::Ptr<char> pastSlash(ee::Local<char[64]> &path) { return ee::Ptr<char>(path) + 1; }

        // Rounded up to whole kilobytes (the +3 is the checksum's word).
        int32_t cardBytes(int32_t bytes)
        {
            return static_cast<int32_t>(((static_cast<uint32_t>(bytes) + 1027u) >> 10) << 10);
        }
    }

    // initSaveJobs: the card library up and no job, no list buffer.
    uint32_t initSaveJobs()
    {
        sceMcInit();
        *g_saveListBuffer = 0;
        *g_jobThread = 0;
        formatLocalizedText(kTextNoText);
        return 1;
    }
    READABLE_FUNCTION(initSaveJobs, Verified);

    // memoryCardPresent: is the card there (the same or another, formatted or
    // read-protected PS1)? The dialogs' poll: askSaveQuestion ends when it
    // changes. It spins without sleeping, unlike every other wait here.
    uint32_t memoryCardPresent()
    {
        ee::Local<int32_t> cardType;
        ee::Local<int32_t> result;
        sceMcGetInfo(*g_cardPort, kSlot, cardType, kNotWanted, kNotWanted);
        spinForCard(result);
        if (*result == 0)
            return 1;
        return *cardType == kPlayStationCard && *result == sceMcResDeniedPermit;
    }
    READABLE_FUNCTION_WAITS(memoryCardPresent, Verified);

    // memoryCardFormatted: the other dialogs' poll: 0 once a formatted PS2 card is in
    // (any answer but the three good ones counts as not there: 1).
    int32_t memoryCardFormatted()
    {
        ee::Local<int32_t> cardType;
        ee::Local<int32_t> result;
        sceMcGetInfo(*g_cardPort, kSlot, cardType, kNotWanted, kNotWanted);
        waitForCard(result);
        if (cardAnswered(*result))
            return *cardType != kPlayStation2Card;
        return 1;
    }
    READABLE_FUNCTION_WAITS(memoryCardFormatted, Verified);

    // findMemoryCard: nothing calls it. 1 when neither port answers.
    int32_t findMemoryCard()
    {
        ee::Local<int32_t> result;
        sceMcGetInfo(0, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result);
        if (cardAnswered(*result))
            return 0;
        sceMcGetInfo(1, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result);
        return !cardAnswered(*result);
    }
    READABLE_FUNCTION_WAITS(findMemoryCard, Verified);

    // wakeSaveJob: a job asleep in a sleep of its waits goes on.
    void wakeSaveJob()
    {
        const int32_t thread = *g_jobThread;
        if (thread != 0)
            tail::releaseWaitThread(thread);
    }
    READABLE_FUNCTION(wakeSaveJob, Verified);

    // endSaveJob: the job's thread gone, with its id.
    int32_t endSaveJob()
    {
        *g_jobThread = 0;
        if (saveJobReturnsForRecording())
            return 0;
        return tail::exitDeleteThread();
    }
    READABLE_FUNCTION(endSaveJob, Verified);

    // startSaveJobThread: a job thread on the module's stack, with the game's global pointer.
    int32_t startSaveJobThread(uint32_t entry, uint32_t delayMilliseconds)
    {
        sce::ThreadParam &thread = *g_jobThreadParameters;
        thread.entry = entry;
        thread.stack = kJobThreadStack;
        thread.stackSize = kJobThreadStackBytes;
        thread.gpReg = sym::gp;
        thread.initPriority = 0;
        const int32_t id = createThread(kJobThreadParameters, delayMilliseconds, sym::gp);
        *g_jobDelayMilliseconds = delayMilliseconds;
        *g_jobThread = id;
        return tail::startThread(id, 0);
    }
    READABLE_FUNCTION(startSaveJobThread, Verified);

    // freeSaveBuffer
    void freeSaveBuffer()
    {
        const uint32_t buffer = *g_saveListBuffer;
        if (buffer != 0)
        {
            heapFree(buffer);
            *g_saveListBuffer = 0;
        }
    }
    READABLE_FUNCTION_WAITS(freeSaveBuffer, Verified);

    // startAutoLoadJob .. startSaveChoiceJob, startNullJob: the jobs by name.
    int32_t startAutoLoadJob(uint32_t delayMilliseconds)
    {
        return game::tail(startSaveJobThread, sym::autoLoadJob, delayMilliseconds);
    }
    READABLE_FUNCTION(startAutoLoadJob, Verified);

    int32_t startSaveJob(uint32_t delayMilliseconds)
    {
        return game::tail(startSaveJobThread, sym::saveJob, delayMilliseconds);
    }
    READABLE_FUNCTION(startSaveJob, Verified);

    int32_t startLoadJob(uint32_t delayMilliseconds)
    {
        return game::tail(startSaveJobThread, sym::loadJob, delayMilliseconds);
    }
    READABLE_FUNCTION(startLoadJob, Verified);

    int32_t startSilentLoadJob(uint32_t delayMilliseconds)
    {
        return game::tail(startSaveJobThread, sym::silentLoadJob, delayMilliseconds);
    }
    READABLE_FUNCTION(startSilentLoadJob, Verified);

    int32_t startLoadChoiceJob(uint32_t delayMilliseconds)
    {
        *g_saveListBuffer = heapAllocate((*g_saveClient)->listBufferBytes);
        return game::tail(startSaveJobThread, sym::loadChoiceJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startLoadChoiceJob, Verified);

    int32_t startSaveChoiceJob(uint32_t delayMilliseconds)
    {
        *g_saveListBuffer = heapAllocate((*g_saveClient)->listBufferBytes);
        return game::tail(startSaveJobThread, sym::saveChoiceJob, delayMilliseconds);
    }
    READABLE_FUNCTION_WAITS(startSaveChoiceJob, Verified);

    int32_t startNullJob(uint32_t delayMilliseconds)
    {
        return game::tail(startSaveJobThread, sym::nullJob, delayMilliseconds);
    }
    READABLE_FUNCTION(startNullJob, Draft); // no caller

    // memoryCardTestWrite: a programmer's test, nothing calls it: all of RAM from 0
    // up to `bufferEnd` less 3 KB written to /CRAP/CRAP.DAT on port 0.
    int32_t memoryCardTestWrite(uint32_t bufferEnd)
    {
        ee::Local<int32_t> cardType;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> command;
        ee::Local<int32_t> result;
        sceMcGetInfo(0, kSlot, cardType, kNotWanted, formatted);
        spinForCard(result, command);
        if (*result < kCardGoneBelow)
            return 1;
        if (*cardType != kPlayStation2Card)
            return 2; // not a PlayStation 2 card
        if (*formatted == 0)
            return 0;
        sceMcMkdir(0, kSlot, kTestDirectory);
        spinForCard(result);
        if (*result != 0)
            return *result;
        sceMcChdir(0, kSlot, kTestDirectory, kNotWanted);
        spinForCard(result);
        if (*result != 0)
            return *result;
        sceMcOpen(0, kSlot, kTestFile, kOpenCreate);
        int32_t done = spinForCard(result);
        const int32_t file = *result;
        if (file >= 0)
        {
            sceMcWrite(file, 0u, static_cast<int32_t>(bufferEnd - 3072u));
            spinForCard(result);
            sceMcClose(file);
            done = spinForCard(result);
        }
        return done;
    }
    READABLE_FUNCTION_WAITS(memoryCardTestWrite, Draft);

    // waitMilliseconds
    int32_t waitMilliseconds(uint32_t milliseconds)
    {
        const uint32_t start = static_cast<uint32_t>(timeMilliseconds());
        while (static_cast<uint32_t>(timeMilliseconds()) - start < milliseconds)
            sleepThread();
        return 0;
    }
    READABLE_FUNCTION_WAITS(waitMilliseconds, Verified);

    // markSaveTime: now, for waitSinceSaveMark.
    uint64_t markSaveTime()
    {
        const uint64_t now = timeMilliseconds();
        *g_saveMark = static_cast<uint32_t>(now);
        return now;
    }
    READABLE_FUNCTION(markSaveTime, Verified);

    // waitSinceSaveMark: so that a message stays up at least this long.
    int32_t waitSinceSaveMark(uint32_t milliseconds)
    {
        const uint64_t now = timeMilliseconds();
        const uint32_t elapsed = static_cast<uint32_t>(now) - *g_saveMark;
        if (elapsed < milliseconds)
            return game::tail(waitMilliseconds, milliseconds - elapsed);
        return 0;
    }
    READABLE_FUNCTION_WAITS(waitSinceSaveMark, Verified);

    // nullJob: the job that only waits and says it is done; no caller.
    int32_t nullJob()
    {
        ee::Local<SaveMessage> message;
        waitMilliseconds(*g_jobDelayMilliseconds);
        const ee::Ptr<SaveMessageQueue> queue = *g_saveMessageQueue;
        message->kind = kMessageDone;
        postSaveMessage(queue, message);
        return endSaveJob();
    }
    READABLE_FUNCTION_WAITS(nullJob, Draft);

    // askMemoryCardSlot: the slot menu, and the port from its answer.
    uint32_t askMemoryCardSlot()
    {
        ee::Local<SaveMessage> message;
        formatLocalizedText(kTextSlotSelect);
        const ee::Ptr<SaveMessageQueue> queue = *g_saveMessageQueue;
        message->kind = kMessageSlotMenu;
        message->value = sym::slotMenuData;
        postSaveMessage(queue, message);
        do
        {
            while (takeSaveMessage(*g_saveReplyQueue, message, 1) == 0)
            {
            }
        } while (message->kind != kMessageAnswer);
        const int32_t port = static_cast<int32_t>(message->value);
        if (port == -1)
            return kCardCancel;
        *g_cardPort = port;
        return kCardOk;
    }
    READABLE_FUNCTION_WAITS(askMemoryCardSlot, Verified);

    // listMemoryCardSaves: every directory on the card named the client's file name
    // and more: each whose file holds a whole save goes to the interface (its
    // first bytes are in the list buffer). A list ends with a message whose
    // name is 0 at the start.
    uint32_t listMemoryCardSaves(ee::Ptr<int32_t> foundCount)
    {
        ee::Local<char[64]> pattern;
        ee::Local<sce::sceMcTblGetDir> entry;
        ee::Local<SaveMessage> message;
        ee::Local<char[64]> path;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> result;
        ee::Local<int32_t> checksum;

        if (foundCount)
            *foundCount = 0;
        const ee::Ptr<SaveMessageQueue> queue = *g_saveMessageQueue;
        message->kind = kMessageSaveFound;
        message->value = 0; // the list starts
        message->listBuffer = 0;
        postSaveMessage(queue, message);
        sleepThread();
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, formatted);
        waitForCard(result);

        bool usable = false;
        if (*result == 0)
        {
            if (*formatted == 0)
                return kCardOk;
            sceMcChdir(*g_cardPort, kSlot, kRootPath, kNotWanted);
            waitForCard(result);
            usable = *result == 0;
        }
        if (!usable)
        {
            formatLocalizedText(kTextListingFailed);
            waitMilliseconds(kShowMilliseconds);
            return kCardFailed;
        }

        stringCopy(pattern, (*g_saveClient)->fileName());
        appendString(pattern, kAllFiles);
        sceMcGetDir(*g_cardPort, kSlot, pattern, sce::sceMcGetDirFirst, 1, entry);
        waitForCard(result);
        while (*result > 0)
        {
            const ee::Ptr<char> entryName(entry->EntryName);
            stringCopy(pastSlash(path), entryName);
            path[0] = '/';
            sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
            waitForCard(result);
            if (*result == 0)
            {
                sceMcOpen(*g_cardPort, kSlot, entryName, kOpenRead);
                waitForCard(result);
                const int32_t file = *result;
                if (file >= 0)
                {
                    sceMcRead(file, checksum, 4);
                    waitForCard(result);
                    sceMcRead(file, *g_saveListBuffer, (*g_saveClient)->listBufferBytes);
                    sceMcFlush(file);
                    waitForCard(result);
                    if (static_cast<uint32_t>(*result) == (*g_saveClient)->listBufferBytes)
                    {
                        if (foundCount)
                            *foundCount = *foundCount + 1;
                        const uint32_t buffer = *g_saveListBuffer;
                        const ee::Ptr<SaveMessageQueue> saves = *g_saveMessageQueue;
                        message->kind = kMessageSaveFound;
                        message->listBuffer = buffer;
                        message->value = ee::io::numberOf(entryName);
                        postSaveMessage(saves, message);
                        sleepThread();
                    }
                    sceMcClose(file);
                    waitForCard(result);
                }
            }
            sceMcGetDir(*g_cardPort, kSlot, pattern, sce::sceMcGetDirNext, 1, entry);
            waitForCard(result);
        }
        return kCardOk;
    }
    READABLE_FUNCTION_WAITS(listMemoryCardSaves, Verified);

    // chooseSaveFile: the interface's file menu up until the player answers or
    // the card goes; a file chosen copies its name to `chosenName`.
    uint32_t chooseSaveFile(ee::Ptr<char> chosenName)
    {
        ee::Local<SaveMessage> message;
        const ee::Ptr<SaveMessageQueue> queue = *g_saveMessageQueue;
        message->kind = kMessageFileMenu;
        message->value = 1;
        postSaveMessage(queue, message);
        for (;;)
        {
            if (memoryCardPresent() == 0)
            {
                while (takeSaveMessage(*g_saveReplyQueue, message, 0) != 0)
                {
                }
                const ee::Ptr<SaveMessageQueue> saves = *g_saveMessageQueue;
                message->kind = kMessageFileMenu;
                message->value = 0;
                postSaveMessage(saves, message);
                formatLocalizedText(kTextRemoved, *g_cardPort + 1);
                const int32_t answer = askSaveQuestion(kButtonsRetryCancelEscaped, 0, kMemoryCardFormatted);
                return answer == kSecondButton || answer == kAnswerBack ? kCardCancel : kCardAgain;
            }
            if (takeSaveMessage(*g_saveReplyQueue, message, 0) != 0)
            {
                if (message->kind == kMessageFileChosen)
                    stringCopy(chosenName, message->value);
                if (message->kind == kMessageAnswer)
                    return static_cast<int32_t>(message->value) == -1 ? kCardCancel : kCardOk;
            }
            waitMilliseconds(kPollMilliseconds);
        }
    }
    READABLE_FUNCTION_WAITS(chooseSaveFile, Verified);

    // confirmSaveOverwrite: the save's file on the card already? Then ask.
    uint32_t confirmSaveOverwrite(ee::Ptr<const char> fileName)
    {
        ee::Local<char[64]> path;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> result;
        startRootPath(path);
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, formatted);
        waitForCard(result);
        if (*result == 0)
        {
            if (*formatted == 0)
                return kCardOk;
            if (fileName)
                appendString(path, fileName);
            else
                appendString(path, (*g_saveClient)->fileName());
            sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
            waitForCard(result);
            if (*result != 0)
                return kCardOk;
            sceMcOpen(*g_cardPort, kSlot, pastSlash(path), kOpenWrite);
            waitForCard(result);
            const int32_t file = *result;
            if (file >= 0)
            {
                sceMcClose(file);
                waitForCard(result);
                waitSinceSaveMark(kBetweenStepsMilliseconds);
                const uint32_t title = (*g_saveClient)->title();
                formatLocalizedText(kTextOverwrite, title, *g_cardPort + 1);
                const int32_t answer = askSaveQuestion(kButtonsOverwriteCancel, 1, kMemoryCardPresent);
                if (answer == kSecondButton || answer == kAnswerBack)
                    return kCardCancel;
                if (answer == kFirstButton)
                    return kCardOk;
                return answer == kAnswerCardChanged ? kCardAgain : kCardFailed;
            }
            if (file == sceMcResNoEntry)
                return kCardOk;
        }
        formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
        waitMilliseconds(kShowMilliseconds);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(confirmSaveOverwrite, Verified);

    // writeMemoryCardSave: the client's data to the file named as the directory the
    // job is in, its checksum first.
    uint32_t writeMemoryCardSave()
    {
        ee::Local<char[64]> currentDirectory; // what sceMcChdir says we are in: "/name"
        ee::Local<int32_t> result;
        ee::Local<int32_t> checksum;
        const uint32_t data = (*g_saveClient)->data();
        const int32_t bytes = (*g_saveClient)->dataSize();
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result);
        if (*result != 0)
            formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
        else
        {
            markSaveTime();
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextSaving, title, *g_cardPort + 1);
            sceMcChdir(*g_cardPort, kSlot, kCurrentDirectory, currentDirectory);
            waitForCard(result);
            sceMcOpen(*g_cardPort, kSlot, pastSlash(currentDirectory), kOpenCreate);
            waitForCard(result);
            const int32_t file = *result;
            if (file >= 0)
            {
                *checksum = byteChecksum(data, bytes);
                sceMcWrite(file, checksum, 4);
                sceMcFlush(file);
                waitForCard(result);
                sceMcWrite(file, data, bytes);
                sceMcFlush(file);
                waitForCard(result);
                if (*result == bytes)
                {
                    sceMcClose(file);
                    waitForCard(result);
                    waitSinceSaveMark(kBetweenStepsMilliseconds);
                    formatLocalizedText(kTextSavingComplete);
                    waitMilliseconds(kShowMilliseconds);
                    return kCardOk;
                }
                waitSinceSaveMark(kBetweenStepsMilliseconds);
                if (*result < kCardGoneBelow)
                    formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
                else
                    formatLocalizedText(kTextSavingFailed, *g_cardPort);
                sceMcClose(file);
                waitForCard(result);
            }
            else
                formatLocalizedText(kTextSavingFailed);
        }
        waitMilliseconds(kShowMilliseconds);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(writeMemoryCardSave, Verified);

    // readMemoryCardSave: the same back, the checksum proved. `confirm` asks first;
    // `fileKnown`: a file that will not open is corrupt rather than missing.
    uint32_t readMemoryCardSave(uint32_t fileKnown, uint32_t confirm)
    {
        ee::Local<char[64]> currentDirectory; // what sceMcChdir says we are in: "/name"
        ee::Local<int32_t> result;
        ee::Local<int32_t> checksum;
        const int32_t bytes = (*g_saveClient)->dataSize();
        const uint32_t data = (*g_saveClient)->data();
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result);
        if (*result != 0)
        {
            formatLocalizedText(kTextLoadingFailed, *g_cardPort + 1);
            waitMilliseconds(kShowMilliseconds);
            return kCardFailed;
        }
        if (confirm != 0)
        {
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextConfirmLoad, title, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsYesNo, 1, kMemoryCardPresent);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kAnswerCardChanged)
                return kCardAgain;
        }
        markSaveTime();
        const uint32_t title = (*g_saveClient)->title();
        formatLocalizedText(kTextLoading, title, *g_cardPort + 1);
        sceMcChdir(*g_cardPort, kSlot, kCurrentDirectory, currentDirectory);
        waitForCard(result);
        sceMcOpen(*g_cardPort, kSlot, pastSlash(currentDirectory), kOpenRead);
        waitForCard(result);
        const int32_t file = *result;
        if (file < 0)
        {
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            // A bug kept: this path gives $MEMCARD_LOADING_FAILED no slot number,
            // so the text shows whatever number the clock's division left behind.
            const uint32_t failure = fileKnown != 0 ? kTextLoadingCorrupt : kTextLoadingFailed;
            formatLocalizedText(failure);
        }
        else
        {
            sceMcRead(file, checksum, 4);
            sceMcFlush(file);
            waitForCard(result);
            sceMcRead(file, data, bytes);
            sceMcFlush(file);
            waitForCard(result);
            if (*result != bytes)
            {
                waitSinceSaveMark(kBetweenStepsMilliseconds);
                if (*result < kCardGoneBelow)
                    formatLocalizedText(kTextLoadingFailed, *g_cardPort + 1);
                else
                    formatLocalizedText(kTextLoadingCorrupt, *g_cardPort);
                sceMcClose(file);
                waitForCard(result);
            }
            else
            {
                sceMcClose(file);
                waitForCard(result);
                if (byteChecksum(data, bytes) == *checksum)
                {
                    waitSinceSaveMark(kBetweenStepsMilliseconds);
                    formatLocalizedText(kTextLoadingComplete);
                    waitMilliseconds(kShowMilliseconds);
                    return kCardOk;
                }
                waitSinceSaveMark(kBetweenStepsMilliseconds);
                formatLocalizedText(kTextLoadingCorrupt);
            }
        }
        waitMilliseconds(kShowMilliseconds);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(readMemoryCardSave, Verified);

    // saveFileExists: the bytes of the client's save on the card, 0 if none.
    int32_t saveFileExists(ee::Ptr<SaveClient> client)
    {
        ee::Local<char[64]> path;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> result;
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, formatted);
        waitForCard(result);
        if (*result != 0 || *formatted == 0)
            return 0;
        stringCopy(pastSlash(path), client->fileName());
        path[0] = '/';
        sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
        waitForCard(result);
        if (*result != 0)
            return 0;
        sceMcOpen(*g_cardPort, kSlot, pastSlash(path), kOpenRead);
        waitForCard(result);
        const int32_t file = *result;
        if (file < 0)
            return 0;
        sceMcSeek(file, 0, kSeekEnd);
        waitForCard(result);
        const int32_t end = *result;
        sceMcClose(file);
        waitForCard(result);
        return end < 0 ? 0 : end;
    }
    READABLE_FUNCTION_WAITS(saveFileExists, Verified);

    // checkMemoryCardReady: a PS2 card in the slot? `ask` puts up the dialogs;
    // `checkFormat`: is it formatted?
    uint32_t checkMemoryCardReady(uint32_t ask, uint32_t checkFormat)
    {
        ee::Local<int32_t> cardType;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> command;
        ee::Local<int32_t> result;
        sceMcGetInfo(*g_cardPort, kSlot, cardType, kNotWanted, formatted);
        waitForCard(result, command);
        if (*result >= kCardGoneBelow && *cardType == kPlayStation2Card)
        {
            if (checkFormat == 0 || *formatted != 0)
                return kCardOk;
            if (ask == 0)
                return kCardFailed;
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            formatLocalizedText(kTextNeedFormatReadOnly, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryContinue, 0, kMemoryCardPresent);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardSkip;
            if (answer == kFirstButton || answer == kAnswerCardChanged)
                return kCardAgain;
            return kCardOk;
        }
        if (ask == 0)
            return kCardCancel;
        waitSinceSaveMark(kBetweenStepsMilliseconds);
        formatLocalizedText(kTextNoCard, *g_cardPort + 1);
        const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
        if (answer == kFirstButton || answer == kAnswerCardChanged)
            return kCardAgain;
        return kCardCancel;
    }
    READABLE_FUNCTION_WAITS(checkMemoryCardReady, Verified);

    // formatMemoryCard
    uint32_t formatMemoryCard()
    {
        ee::Local<int32_t> command;
        ee::Local<int32_t> result;
        formatLocalizedText(kTextFormatting, *g_cardPort + 1);
        waitMilliseconds(kFormatNoticeMilliseconds);
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result, command);
        if (*result == sceMcResNoFormat || *result == sceMcResChangedCard) // a card that is not this one
        {
            formatLocalizedText(kTextFormatFailedChange, *g_cardPort + 1);
            waitMilliseconds(kShowMilliseconds);
            return kCardCancel;
        }
        if (*result < kCardGoneBelow)
        {
            formatLocalizedText(kTextFormatFailedRemove, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton || answer == kAnswerCardChanged)
                return kCardAgain;
        }
        *result = sceMcFormat(*g_cardPort, kSlot);
        waitForCard(result, command);
        if (*result == 0)
        {
            formatLocalizedText(kTextFormatSuccess);
            waitMilliseconds(kShowMilliseconds);
            return kCardOk;
        }
        formatLocalizedText(kTextFormatFailed);
        waitMilliseconds(kShowMilliseconds);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(formatMemoryCard, Verified);

    // writeSaveFiles: the files around the save: the client's icon and icon.sys,
    // the browser's title broken at a space near the 16th character.
    uint32_t writeSaveFiles()
    {
        constexpr int32_t kLineCharacters = 16;
        constexpr uint16_t kWholeLine = kLineCharacters * sizeof(uint16_t); // the second line's offset: two bytes a character
        ee::Local<char[32]> title;
        ee::Local<int32_t> result;

        memoryFill(kIconSys, 0, kIconSysBytes);
        sce::mcIcon &icon = *g_iconSys;
        icon.head[0] = 'P';
        icon.head[1] = 'S';
        icon.head[2] = '2';
        icon.head[3] = 'D';
        icon.trans = 121;
        for (int light = 0; light < 3; ++light)
            for (int lane = 0; lane < 4; ++lane)
            {
                icon.lightDir[light][lane] = ee::Float(lane == light ? 1.0f : 0.0f);
                icon.lightCol[light][lane] = ee::Float(0.0f);
            }
        icon.lightAmbient[0] = ee::Float(1.0f);
        icon.lightAmbient[1] = ee::Float(1.0f);
        icon.lightAmbient[2] = ee::Float(1.0f);
        icon.lightAmbient[3] = ee::Float(0.0f);
        stringCopy(ee::Ptr<char>(icon.view), kIconFile);
        stringCopy(ee::Ptr<char>(icon.copy), kIconFile);
        stringCopy(ee::Ptr<char>(icon.del), kIconFile);
        stringCopyN(title, (*g_saveClient)->title(), 31);
        icon.nlOffset = kWholeLine;
        title[31] = '\0';

        // Word by word, the title into two lines of 16 characters.
        int32_t cursor = 0, wordStart = 0;
        int32_t lineLeft = kLineCharacters;
        int32_t lines = 0;
        for (;;)
        {
            while (title[cursor] != ' ' && title[cursor] != '\0')
                ++cursor;
            const char end = title[cursor];
            const int32_t wordLength = cursor - wordStart;
            if (lineLeft >= wordLength)
            {
                const int32_t after = lineLeft - wordLength;
                if (after != 0)
                    lineLeft = end != '\0' ? after - 1 : after;
                if (after == 0 || lineLeft == 0)
                {
                    icon.nlOffset = kWholeLine;
                    ++lines;
                    lineLeft = kLineCharacters;
                    if (lines == 2)
                        break;
                }
            }
            else if (lines == 0)
            {
                const int32_t after = kLineCharacters - wordLength;
                icon.nlOffset = static_cast<uint16_t>((kLineCharacters - 1 - lineLeft) << 1);
                lineLeft = after != 0 ? after - 1 : after;
                lines = 1;
                if (lineLeft == 0)
                {
                    icon.nlOffset = kWholeLine;
                    break;
                }
            }
            ++cursor;
            if (end == '\0')
                break;
            wordStart = cursor;
        }
        toSjisSaveName(title, ee::Ptr<uint16_t>(icon.title));
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, kNotWanted);
        waitForCard(result);
        if (*result != 0)
            formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
        else
        {
            sceMcOpen(*g_cardPort, kSlot, kIconFile, kOpenCreate);
            waitForCard(result);
            const int32_t iconFile = *result;
            if (iconFile >= 0)
            {
                const uint32_t iconData = (*g_saveClient)->icon();
                const int32_t iconBytes = (*g_saveClient)->iconSize();
                sceMcWrite(iconFile, iconData, iconBytes);
                waitForCard(result);
                sceMcClose(iconFile);
                waitForCard(result);
                sceMcOpen(*g_cardPort, kSlot, kIconSysFile, kOpenCreate);
                waitForCard(result);
                const int32_t sysFile = *result;
                if (sysFile >= 0)
                {
                    sceMcWrite(sysFile, kIconSys, kIconSysBytes);
                    waitForCard(result);
                    sceMcClose(sysFile);
                    waitForCard(result);
                    return kCardOk;
                }
            }
            formatLocalizedText(kTextSavingFailed);
        }
        waitMilliseconds(kShowMilliseconds);
        return kCardFailed;
    }
    READABLE_FUNCTION_WAITS(writeSaveFiles, Verified);

    int32_t saveBlocksMissing(uint32_t singleClient);

    uint32_t appendRequiredSpaceText(ee::Address text);

    // checkMemoryCardSpace: room on the card for one save (`singleClient`) or for all
    // of them? `ask`: tell the player and offer Retry or going on without.
    uint32_t checkMemoryCardSpace(uint32_t singleClient, uint32_t ask)
    {
        ee::Local<int32_t> freeKilobytes;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> command;
        ee::Local<int32_t> result;
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, freeKilobytes, formatted);
        waitForCard(result, command);
        if (*result == sceMcResNoFormat)
            return kCardAgain;
        if (*result < kCardGoneBelow)
        {
            if (ask == 0)
                return kCardCancel;
            formatLocalizedText(kTextRemoved, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton || answer == kAnswerCardChanged)
                return kCardAgain;
        }
        if (*formatted == 0)
            return kCardOk;
        const int32_t missing = saveBlocksMissing(singleClient);
        if (*freeKilobytes >= missing)
            return kCardOk;
        if (ask == 0)
            return kCardCancel;
        if (singleClient != 0)
        {
            waitSinceSaveMark(kSpaceMessageMilliseconds);
            const int32_t slotNumber = *g_cardPort + 1;
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextNeedSpaceOne, slotNumber, missing, title);
        }
        else
        {
            stringCopy(kNeedSpaceText, kTextNeedSpaceAll);
            appendRequiredSpaceText(kNeedSpaceText);
            waitSinceSaveMark(kSpaceMessageMilliseconds);
            formatLocalizedText(kNeedSpaceText, *g_cardPort + 1);
        }
        const int32_t answer = askSaveQuestion(kButtonsRetryContinueWithoutSaving, 0, kMemoryCardPresent);
        if (answer == kSecondButton || answer == kAnswerBack)
            return kCardSkip;
        if (answer == kFirstButton || answer == kAnswerCardChanged)
            return kCardAgain;
        return kCardOk;
    }
    READABLE_FUNCTION_WAITS(checkMemoryCardSpace, Verified);

    // saveClientFits: forEachSaveClient's callback: the client's missing
    // kilobytes added to `*missing`, past what its file already takes.
    uint32_t saveClientFits(ee::Ptr<SaveClient> client, ee::Ptr<int32_t> missing)
    {
        if (client->singleFile() == 0)
            return 1;
        const int32_t onCard = saveFileExists(client);
        if (onCard == 0)
        {
            const int32_t blocks = saveBlocksNeeded(client);
            *missing = *missing + blocks;
            return 1;
        }
        const int32_t needed = cardBytes(client->dataSize());
        if (static_cast<uint32_t>(onCard) < static_cast<uint32_t>(needed))
            *missing = *missing + static_cast<int32_t>(static_cast<uint32_t>(needed - onCard) >> 10);
        return 1;
    }
    READABLE_FUNCTION_WAITS(saveClientFits, Verified);

    // saveBlocksMissing: kilobytes still needed for the selected client or for all.
    int32_t saveBlocksMissing(uint32_t singleClient)
    {
        ee::Local<int32_t> missing;
        *missing = 0;
        if (singleClient == 0)
        {
            forEachSaveClient(kSaveClientFits, missing);
            return *missing;
        }
        const int32_t onCard = saveFileExists(*g_saveClient);
        const ee::Ptr<SaveClient> client = *g_saveClient;
        if (onCard == 0)
            return *missing + saveBlocksNeeded(client);
        const int32_t needed = cardBytes(client->dataSize());
        if (static_cast<uint32_t>(onCard) < static_cast<uint32_t>(needed))
            *missing = *missing + static_cast<int32_t>(static_cast<uint32_t>(needed - onCard) >> 10);
        return *missing;
    }
    READABLE_FUNCTION_WAITS(saveBlocksMissing, Verified);

    // saveToChosenFile: a new save: the first free directory of the client's name
    // and a number that fills 20 characters, its files written there. The
    // directory's name goes to `nameOut`. An unformatted card offers Format
    // first, and no such directory means a new one is made.
    int32_t saveToChosenFile(ee::Address nameOut)
    {
        ee::Local<char[64]> path;
        ee::Local<char[16]> directoryFormat; // "/%s%0" and the width: "/%s%015d"
        ee::Local<char[256]> needSpaceText;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> result;

        const auto accessError = [&]() {
            formatLocalizedText(kTextAccessError, *g_cardPort + 1);
            waitMilliseconds(kShowMilliseconds);
            return kCardCancel;
        };

        startRootPath(path);
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, formatted);
        bool justFormatted = false;
        waitForCard(result);
        if (*result != 0)
        {
            formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
            waitMilliseconds(kShowMilliseconds);
            return kCardFailed;
        }
        if (*formatted == 0)
        {
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            formatLocalizedText(kTextNeedFormat, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryFormatCancel, 0, kMemoryCardPresent);
            if (answer == kThirdButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton)
                return kCardAgain;
            if (answer == kSecondButton)
            {
                const int32_t step = formatMemoryCard();
                *result = step;
                justFormatted = true;
                if (step != kCardOk)
                    return step;
            }
            else if (answer == kAnswerCardChanged)
            {
                formatLocalizedText(kTextRemoved, *g_cardPort + 1);
                const int32_t again = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
                if (again == kSecondButton || again == kAnswerBack)
                    return kCardCancel;
                if (again == kFirstButton || again == kAnswerCardChanged)
                    return kCardAgain;
            }
        }

        // 10 to the number of digits the client's name leaves: that many
        // numbers to try (it wraps past nine digits, as the original's does).
        const int32_t digits = kDirectoryNameDigits - stringLength((*g_saveClient)->fileName());
        uint32_t numbers = 1;
        for (uint32_t i = 0; i != static_cast<uint32_t>(digits); ++i)
            numbers *= 10u;
        const int32_t tries = static_cast<int32_t>(numbers);

        for (int32_t number = 0; number < tries; ++number)
        {
            const ee::Ptr<const char> formatStart(kDirectoryFormatStart);
            for (int i = 0; i < 6; ++i)
                directoryFormat[i] = formatStart[i];
            memset(ee::Ptr<char>(directoryFormat) + 6, 0, 4);
            formatString(ee::Ptr<char>(directoryFormat) + 5, kWidthFormat, static_cast<uint64_t>(digits));
            const uint32_t name = (*g_saveClient)->fileName();
            formatString(path, directoryFormat, static_cast<uint64_t>(name), static_cast<uint64_t>(number));
            sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
            waitForCard(result);
            const int32_t found = *result;
            if (found == sceMcResNoFormat || found < kCardGoneBelow)
                return accessError();
            if (found != sceMcResNoEntry)
                continue; // a save has this number

            if (!justFormatted)
            {
                waitSinceSaveMark(kBetweenStepsMilliseconds);
                const uint32_t title = (*g_saveClient)->title();
                formatLocalizedText(kTextConfirmSave, title, *g_cardPort + 1);
                const int32_t answer = askSaveQuestion(kButtonsYesNo, 1, kMemoryCardPresent);
                if (answer == kSecondButton || answer == kAnswerBack)
                    return kCardCancel;
                if (answer == kAnswerCardChanged)
                {
                    formatLocalizedText(kTextRemoved, *g_cardPort + 1);
                    const int32_t again = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
                    if (again == kSecondButton || again == kAnswerBack)
                        return kCardCancel;
                    if (again == kFirstButton || again == kAnswerCardChanged)
                        return kCardAgain;
                }
            }
            markSaveTime();
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextSaving, title, *g_cardPort + 1);
            sceMcMkdir(*g_cardPort, kSlot, path);
            waitForCard(result);
            const int32_t made = *result;
            if (made == sceMcResFullDevice)
            {
                stringCopy(needSpaceText, kTextNeedSpace);
                appendRequiredSpaceText(needSpaceText);
                formatLocalizedText(needSpaceText, *g_cardPort + 1);
                const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardPresent);
                if (answer == kSecondButton || answer == kAnswerBack)
                    return kCardCancel;
                if (answer == kFirstButton || answer == kAnswerCardChanged)
                    return kCardAgain;
            }
            else if (made == sceMcResNoFormat || made < kCardGoneBelow || made == sceMcResNoEntry)
                return accessError();
            sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
            waitForCard(result);
            if (*result != 0)
                return accessError();
            if (nameOut != 0)
                stringCopy(nameOut, path);
            return writeSaveFiles();
        }

        formatLocalizedText(kTextNoNewSave);
        const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardPresent);
        return answer == kSecondButton || answer == kAnswerBack ? kCardCancel : kCardAgain;
    }
    READABLE_FUNCTION_WAITS(saveToChosenFile, Verified);

    // runMemoryCardTransfer: into the directory of `fileName` (or the client's name)
    // for a save (`saving`) or a load; `askMissing`: a missing file is
    // reported. The job's way in once a card is known.
    int32_t runMemoryCardTransfer(ee::Ptr<const char> fileName, uint32_t saving, uint32_t askMissing)
    {
        ee::Local<char[64]> path;
        ee::Local<int32_t> formatted;
        ee::Local<int32_t> result;
        bool justFormatted = false;

        const auto failed = [&](uint32_t milliseconds) {
            waitMilliseconds(milliseconds);
            return kCardFailed;
        };
        const auto retryOrCancelCard = [&]() {
            formatLocalizedText(kTextRemoved, *g_cardPort + 1);
            return askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardFormatted);
        };

        *result = kCardAgain; // what stands if the library has no command to sync
        startRootPath(path);
        sceMcGetInfo(*g_cardPort, kSlot, kNotWanted, kNotWanted, formatted);
        waitForCard(result);
        if (*result != 0 && *result != sceMcResChangedCard)
        {
            if (saving != 0)
                formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
            else
                formatLocalizedText(kTextLoadingFailed, *g_cardPort + 1);
            return failed(kBetweenStepsMilliseconds);
        }
        if (*formatted == 0)
        {
            if (saving == 0)
            {
                if (askMissing == 0)
                    return kCardCancel;
                const uint32_t title = (*g_saveClient)->title();
                formatLocalizedText(kTextFileNotFound, title, *g_cardPort + 1);
                const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardPresent);
                return answer == kFirstButton || answer == kAnswerCardChanged ? kCardAgain : kCardCancel;
            }
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            formatLocalizedText(kTextNeedFormat, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryFormatCancel, 0, kMemoryCardPresent);
            if (answer == kThirdButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton)
                return kCardAgain;
            if (answer == kSecondButton)
            {
                const int32_t step = formatMemoryCard();
                *result = step;
                if (step != kCardOk)
                    return step;
                justFormatted = true;
            }
            else if (answer == kAnswerCardChanged)
            {
                const int32_t again = retryOrCancelCard();
                if (again == kSecondButton || again == kAnswerBack)
                    return kCardCancel;
                if (again == kFirstButton || again == kAnswerCardChanged)
                    return kCardAgain;
            }
        }

        if (fileName)
            appendString(path, fileName);
        else
            appendString(path, (*g_saveClient)->fileName());
        sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
        waitForCard(result);
        const int32_t found = *result;
        if (found == sceMcResNoFormat || found < kCardGoneBelow)
        {
            if (saving != 0)
                formatLocalizedText(kTextSavingFailed, *g_cardPort + 1);
            else
                formatLocalizedText(kTextLoadingFailed, *g_cardPort + 1);
            return failed(kShowMilliseconds);
        }
        if (found != sceMcResNoEntry)
            return kCardOk;

        // No such directory: a load's file is missing, a save makes it.
        if (saving == 0)
        {
            if (askMissing == 0)
                return kCardFailed;
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextFileNotFound, title, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardPresent);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton || answer == kAnswerCardChanged)
                return kCardAgain;
            return kCardOk;
        }
        if (!justFormatted)
        {
            waitSinceSaveMark(kBetweenStepsMilliseconds);
            const uint32_t title = (*g_saveClient)->title();
            formatLocalizedText(kTextConfirmSave, title, *g_cardPort + 1);
            const int32_t answer = askSaveQuestion(kButtonsYesNo, 1, kMemoryCardPresent);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kAnswerCardChanged)
            {
                const int32_t again = retryOrCancelCard();
                if (again == kSecondButton || again == kAnswerBack)
                    return kCardCancel;
                if (again == kFirstButton || again == kAnswerCardChanged)
                    return kCardAgain;
            }
        }
        markSaveTime();
        const uint32_t title = (*g_saveClient)->title();
        formatLocalizedText(kTextSaving, title, *g_cardPort + 1);
        sceMcMkdir(*g_cardPort, kSlot, path);
        waitForCard(result);
        const int32_t made = *result;
        if (made == sceMcResFullDevice)
        {
            formatLocalizedText(kTextOutOfSpace);
            const int32_t answer = askSaveQuestion(kButtonsRetryCancel, 0, 0u);
            if (answer == kSecondButton || answer == kAnswerBack)
                return kCardCancel;
            if (answer == kFirstButton)
                return kCardAgain;
        }
        else if (made == sceMcResNoFormat || made < kCardGoneBelow || made == sceMcResNoEntry)
            return kCardFailed;
        sceMcChdir(*g_cardPort, kSlot, path, kNotWanted);
        waitForCard(result);
        if (*result != 0)
            return kCardAgain;
        return writeSaveFiles();
    }
    READABLE_FUNCTION_WAITS(runMemoryCardTransfer, Verified);

    // memoryCardStubTwo: returns 2 and nothing else; no caller.
    uint32_t memoryCardStubTwo()
    {
        return 2;
    }
    READABLE_FUNCTION(memoryCardStubTwo, Draft);

    // toSjisSaveName: ASCII to the big-endian Shift-JIS of icon.sys, 0 ended.
    void toSjisSaveName(ee::Ptr<const char> text, ee::Ptr<uint16_t> out)
    {
        const int32_t length = stringLength(text);
        for (int32_t i = 0; i < length; ++i)
        {
            const uint32_t sjis = asciiToSjis(static_cast<uint8_t>(text[i]));
            out[i] = static_cast<uint16_t>(((sjis >> 8) & 0xFFu) | ((sjis & 0xFFu) << 8));
        }
        out[length] = 0;
    }
    READABLE_FUNCTION(toSjisSaveName, Verified);

    namespace
    {
        // A character in both encodings. The digits and the two alphabets run
        // on from their first; the punctuation around them has a row each.
        struct SjisCharacter
        {
            uint16_t sjis;
            uint16_t ascii;
        };
        inline ee::Global<SjisCharacter[33], sym::asciiToSjisTable> g_sjisPunctuation;
        inline ee::Global<SjisCharacter[3], sym::sjisRanges> g_sjisRanges;
    }

    // asciiToSjis: one character: digits and letters by their ranges, the
    // punctuation between them by the table.
    uint32_t asciiToSjis(uint32_t character)
    {
        const int32_t c = static_cast<int32_t>(character & 0xFFu);
        const auto inRange = [c](int32_t range) {
            const SjisCharacter &first = (*g_sjisRanges)[range];
            return (first.sjis + c - first.ascii) & 0xFFFFu;
        };
        const auto punctuation = [c](int32_t run, int32_t rowsBefore) -> uint32_t { return (*g_sjisPunctuation)[c - run + rowsBefore].sjis; };
        if (c >= ' ' && c <= '/')
            return punctuation(' ', 0);
        if (c >= '0' && c <= '9')
            return inRange(0);
        if (c >= ':' && c <= '@')
            return punctuation(':', 16);
        if (c >= 'A' && c <= 'Z')
            return inRange(1);
        if (c >= '[' && c <= '`')
            return punctuation('[', 23);
        if (c >= 'a' && c <= 'z')
            return inRange(2);
        if (c >= '{' && c <= '~')
            return punctuation('{', 29);
        return 0;
    }
    READABLE_FUNCTION(asciiToSjis, Verified);

    // appendRequiredSpaceText: the needed-space message's list of every client.
    uint32_t appendRequiredSpaceText(ee::Address text)
    {
        appendString(text, kTextRequiredPrefix);
        const int32_t result = forEachSaveClient(kAppendClientRequiredSpace, text);
        appendString(text, kTextRequiredSuffix);
        return static_cast<uint32_t>(result);
    }
    READABLE_FUNCTION_WAITS(appendRequiredSpaceText, Verified);

    // appendClientRequiredSpace: forEachSaveClient's callback: a line for a client the card
    // has no complete file of.
    uint32_t appendClientRequiredSpace(ee::Ptr<SaveClient> client, uint32_t text)
    {
        ee::Local<char[0x150]> line;
        if (client->singleFile() == 0)
            return 1;
        const bool mayExist = client->singleFile() != 0;
        bool complete = false;
        if (mayExist)
        {
            const int32_t bytes = cardBytes(client->dataSize());
            complete = bytes == saveFileExists(client);
        }
        if (!complete)
        {
            expandStringKeys(ee::Ptr<char>(kRequiredEntryText), ee::Ptr<const char>(kTextRequiredEntry));
            const uint32_t title = client->title();
            const int32_t blocks = saveBlocksNeeded(client);
            formatString(line, kRequiredEntryText, static_cast<uint64_t>(title), static_cast<uint64_t>(blocks));
            appendString(text, line);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(appendClientRequiredSpace, Tested);

    // ---- the job bodies ---------------------------------------------------

    namespace
    {
        // A job's end: the save system told how it went, the thread gone.
        int32_t endJobWith(ee::Local<SaveMessage> &message, int32_t step)
        {
            const ee::Ptr<SaveMessageQueue> queue = *g_saveMessageQueue;
            message->kind = step;
            postSaveMessage(queue, message);
            return endSaveJob();
        }
    }

    // autoLoadJob (the boot's): which card a save can go on. A card in
    // either slot with room for the save: kCardOk. No card: the player is
    // asked. A card without room: what checkMemoryCardReady or
    // checkMemoryCardSpace asked and answered.
    int32_t autoLoadJob()
    {
        ee::Local<SaveMessage> message;
        ee::Local<int32_t[2]> cardThere; // per port, a card answers
        ee::Local<int32_t[2]> saveFits;  // and the save fits on it
        memset(cardThere, 0, 8);
        saveFits[0] = 1;
        saveFits[1] = 1;
        waitMilliseconds(*g_jobDelayMilliseconds);

        int32_t answer = 0;
        for (;;)
        {
            markSaveTime();
            formatLocalizedText(kTextNoText);
            *g_cardPort = 0;
            const int32_t first = checkMemoryCardReady(0, 0);
            *g_cardPort = 1;
            cardThere[0] = first == kCardOk;
            const int32_t second = checkMemoryCardReady(0, 0);
            cardThere[1] = second == kCardOk;
            waitSinceSaveMark(100);

            if (cardThere[0] == 0 && cardThere[1] == 0)
            {
                stringCopy(kNoCardText, kTextNoCardInserted);
                appendRequiredSpaceText(kNoCardText);
                formatLocalizedText(kNoCardText);
                const int32_t button = askSaveQuestion(kButtonsRetryContinueWithoutSaving, 0, kFindMemoryCard);
                if (button != kSecondButton && button != kAnswerBack)
                    continue;
                formatLocalizedText(kTextNoText);
                return endJobWith(message, kCardCancel);
            }
            const auto spaceOn = [&](int32_t &fits) {
                if (checkMemoryCardReady(0, 1) == kCardOk)
                    fits = checkMemoryCardSpace(0, 0) == kCardOk;
                else
                    fits = 0;
            };
            if (cardThere[0] != 0)
            {
                *g_cardPort = 0;
                spaceOn(saveFits[0]);
            }
            if (cardThere[1] != 0)
            {
                *g_cardPort = 1;
                spaceOn(saveFits[1]);
            }

            // A card it fits on: done. The thread ends here, and what follows
            // is reached only when a recording has the job return (see
            // job_recording.cpp).
            if ((cardThere[0] != 0 && saveFits[0] != 0) || (cardThere[1] != 0 && saveFits[1] != 0))
                endJobWith(message, kCardOk);

            // Otherwise the player is asked, slot 1 first. A card that does
            // not answer on the second one ends the walk apart.
            bool answered = false;
            if (cardThere[0] != 0 && saveFits[0] == 0)
            {
                *g_cardPort = 0;
                answer = checkMemoryCardReady(1, 1);
                if (answer == kCardOk)
                {
                    answer = checkMemoryCardSpace(0, 1);
                    answered = answer == kCardOk;
                }
                if (!answered)
                    answered = answer == kCardSkip;
            }
            if (!answered)
            {
                if (cardThere[1] == 0 || saveFits[1] != 0)
                    continue;
                *g_cardPort = 1;
                answer = checkMemoryCardReady(1, 1);
                if (answer != kCardOk)
                {
                    if (answer != kCardSkip)
                        continue;
                    break;
                }
                answer = checkMemoryCardSpace(0, 1);
                if (answer != kCardOk && answer != kCardSkip)
                    continue;
            }
            return endJobWith(message, answer);
        }
        return endJobWith(message, answer);
    }
    READABLE_FUNCTION_WAITS(autoLoadJob, Verified);

    // saveJob: the save: the slot asked for, the card checked, an
    // overwrite confirmed, space checked, the save's files written; again on
    // kCardAgain at any step.
    int32_t saveJob()
    {
        ee::Local<SaveMessage> message;
        waitMilliseconds(*g_jobDelayMilliseconds);
        int32_t step;
        do
        {
            step = askMemoryCardSlot();
            if (step != kCardOk)
                continue;
            do
            {
                markSaveTime();
                formatLocalizedText(kTextCheckingSlot, *g_cardPort + 1);
                waitSinceSaveMark(kSpaceMessageMilliseconds);
                step = checkMemoryCardReady(1, 0);
                if (step == kCardOk)
                    step = confirmSaveOverwrite(0u);
                if (step == kCardOk)
                    step = checkMemoryCardSpace(1, 1);
                if (step == kCardOk)
                    step = runMemoryCardTransfer(0u, 1, 1);
                if (step == kCardOk)
                    step = writeMemoryCardSave();
            } while (step == kCardAgain);
        } while (step == kCardAgain);
        return endJobWith(message, step);
    }
    READABLE_FUNCTION_WAITS(saveJob, Verified);

    // saveChoiceJob: a save the player chooses (the parks): the card's saves
    // listed, one chosen (a name: overwritten after a question; none: a new
    // one, space checked), the files written.
    int32_t saveChoiceJob()
    {
        ee::Local<SaveMessage> message;
        ee::Local<char[64]> chosenName;
        memset(chosenName, 0, 64);
        waitMilliseconds(*g_jobDelayMilliseconds);
        int32_t step;
        do
        {
            step = askMemoryCardSlot();
            if (step != kCardOk)
                continue;
            do
            {
                markSaveTime();
                formatLocalizedText(kTextCheckingSlot, *g_cardPort + 1);
                waitSinceSaveMark(kSpaceMessageMilliseconds);
                step = checkMemoryCardReady(1, 0);
                if (step == kCardOk)
                    step = listMemoryCardSaves(0u);
                if (step != kCardOk)
                    continue;
                step = chooseSaveFile(chosenName);
                if (step == kCardCancel) // no save chosen: the slot again
                {
                    step = kCardAgain;
                    break;
                }
                if (step != kCardOk)
                    continue;
                do
                {
                    if (chosenName[0] != '\0')
                    {
                        // An existing save: asked before it is overwritten.
                        step = runMemoryCardTransfer(chosenName, 1, 1);
                        if (step == kCardOk)
                            step = confirmSaveOverwrite(chosenName);
                    }
                    else
                    {
                        step = checkMemoryCardSpace(1, 1);
                        if (step == kCardOk)
                        {
                            markSaveTime();
                            step = saveToChosenFile(chosenName);
                        }
                    }
                    if (step == kCardOk)
                    {
                        markSaveTime();
                        step = writeMemoryCardSave();
                    }
                    formatLocalizedText(kTextNoText);
                } while (step == kCardAgain);
            } while (step == kCardAgain);
        } while (step == kCardAgain);
        return endJobWith(message, step);
    }
    READABLE_FUNCTION_WAITS(saveChoiceJob, Verified);

    // loadJob: the load the menus start: the slot asked for, the card
    // checked, the file found, its data read with the player's answers.
    int32_t loadJob()
    {
        ee::Local<SaveMessage> message;
        waitMilliseconds(*g_jobDelayMilliseconds);
        int32_t step;
        do
        {
            step = askMemoryCardSlot();
            if (step != kCardOk)
                continue;
            do
            {
                markSaveTime();
                formatLocalizedText(kTextCheckingSlot, *g_cardPort + 1);
                waitSinceSaveMark(kSpaceMessageMilliseconds);
                step = checkMemoryCardReady(1, 0);
                if (step == kCardOk)
                    step = runMemoryCardTransfer(0u, 0, 1);
                if (step == kCardOk)
                    step = readMemoryCardSave(1, 1);
            } while (step == kCardAgain);
        } while (step == kCardAgain);
        return endJobWith(message, step);
    }
    READABLE_FUNCTION_WAITS(loadJob, Verified);

    // silentLoadJob: the boot's load: each slot in turn, no question asked,
    // until one has the save; over again from the first on kCardAgain.
    int32_t silentLoadJob()
    {
        ee::Local<SaveMessage> message;
        int32_t step = kCardAgain;
        waitMilliseconds(*g_jobDelayMilliseconds);
        do
        {
            for (*g_cardPort = 0; static_cast<uint32_t>(*g_cardPort) < 2u; *g_cardPort = *g_cardPort + 1)
            {
                step = checkMemoryCardReady(0, 0);
                if (step == kCardOk)
                    step = runMemoryCardTransfer(0u, 0, 0);
                if (step == kCardOk)
                    step = readMemoryCardSave(0, 0);
                if (step == kCardOk)
                    break;
            }
        } while (step == kCardAgain);
        return endJobWith(message, step);
    }
    READABLE_FUNCTION_WAITS(silentLoadJob, Verified);

    // loadChoiceJob: a load the player chooses (the parks): the card's saves
    // listed; none: the player is told (Retry or Cancel); one chosen, read.
    int32_t loadChoiceJob()
    {
        ee::Local<SaveMessage> message;
        ee::Local<char[64]> chosenName;
        ee::Local<int32_t> foundCount;
        memset(chosenName, 0, 64);
        waitMilliseconds(*g_jobDelayMilliseconds);
        int32_t step;
        do
        {
            step = askMemoryCardSlot();
            if (step != kCardOk)
                continue;
            do
            {
                markSaveTime();
                formatLocalizedText(kTextCheckingSlot, *g_cardPort + 1);
                waitSinceSaveMark(kSpaceMessageMilliseconds);
                step = checkMemoryCardReady(1, 0);
                if (step == kCardOk)
                    step = listMemoryCardSaves(foundCount);
                if (step != kCardOk)
                    continue;
                if (*foundCount == 0)
                {
                    waitSinceSaveMark(kBetweenStepsMilliseconds);
                    const uint32_t title = (*g_saveClient)->title();
                    formatLocalizedText(kTextLibraryEmpty, title, *g_cardPort + 1);
                    const int32_t button = askSaveQuestion(kButtonsRetryCancel, 0, kMemoryCardPresent);
                    if (button == kSecondButton || button == kAnswerBack)
                        step = kCardCancel;
                    else if (button == kFirstButton || button == kAnswerCardChanged)
                        step = kCardAgain;
                    continue;
                }
                step = chooseSaveFile(chosenName);
                if (step == kCardOk && chosenName[0] != '\0')
                {
                    step = runMemoryCardTransfer(chosenName, 0, 1);
                    if (step == kCardOk)
                    {
                        waitSinceSaveMark(kBetweenStepsMilliseconds);
                        step = readMemoryCardSave(1, 1);
                    }
                }
                else if (step == kCardCancel) // no save chosen: the slot again
                {
                    step = kCardAgain;
                    break;
                }
            } while (step == kCardAgain);
        } while (step == kCardAgain);
        return endJobWith(message, step);
    }
    READABLE_FUNCTION_WAITS(loadChoiceJob, Verified);
}
