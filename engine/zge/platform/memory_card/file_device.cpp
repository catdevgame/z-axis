// The "mc00" file device (the second part of zge/ps2/Ps2MemCard.c, saveBlocksNeeded ..
// were the sweep's drafts): zfile's FileDevice for the memory card,
// its table at deviceTable (zge/assets/file.h). libmc only starts a command and
// sceMcSync ends it, so an operation is two functions here: the device method
// starts the command, and its "done" handler turns the result into a FileReport
// or a text in zfile's last error. With a callback the sync is polled from a
// task (startMemoryCardCall, memoryCardPollTask); without one the method spins
// on sceMcSync itself and the handler runs before it returns. One command at a
// time: `busy` refuses another with "MemCard call already in progress".
//
// sceMcSync's result: 0 or more done; -1 the card was changed, -2 not
// formatted, -3 no space, -4 no such entry, -5 no permission (in use, or a mode
// the file was not opened in), -6 the directory is not empty, -7 too many files
// open, -8 the write area failed; anything else: the card is gone and the mount
// with it.

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/platform/save_system.h"
#include "zge/platform/memory_card/memory_card.h"
#include "sce/libmc.h"
#include "zge/assets/file.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/memory_card/file_device.cpp.h"

namespace zge
{
    namespace
    {
        // The device's state (cardAddress).
        struct MemoryCard
        {
            int32_t present;        // +0x00 set by initMemoryCardDevice
            int32_t busy;           // +0x04 a libmc command is running
            int32_t mounted;        // +0x08
            int32_t port;           // +0x0C
            int32_t slot;           // +0x10
            int32_t type;           // +0x14 sceMcGetInfo's
            int32_t format;         // +0x18 sceMcGetInfo's: 1 formatted
            uint32_t freeKilobytes; // +0x1C sceMcGetInfo's
            ee::Ptr<Task> task;     // +0x20 polls sceMcSync when the operation has a callback
            uint32_t resultOut;     // +0x24 the operation's result word
            uint32_t callback;      // +0x28 its FileCallback
            uint32_t doneHandler;   // +0x2C the sync's handler: (result, subject)
            int32_t bytesDone;      // +0x30 a read's or a write's, a seek's position
            uint8_t _034[0x0C];
            uint8_t directoryEntries[0x400]; // +0x40 sceMcGetDir's table: 16 entries of 0x40 bytes
        };
        static_assert(offsetof(MemoryCard, freeKilobytes) == 0x1C && offsetof(MemoryCard, bytesDone) == 0x30 &&
                      offsetof(MemoryCard, directoryEntries) == 0x40);
        constexpr uint32_t kCardAddress = sym::cardAddress;
        inline ee::Global<MemoryCard, kCardAddress> g_card;

        constexpr uint32_t kDeviceTable = sym::deviceTable;  // the FileDevice; the subject of its reports
        constexpr uint32_t kDeviceName = sym::g_textMc002;   // "mc00"
        constexpr uint32_t kNoText = sym::noText;       // ""
        constexpr uint32_t kPathBuffer = sym::pathBuffer;   // memoryCardPath's result
        constexpr uint32_t kFileNameBuffer = sym::fileNameBuffer; // memoryCardSetAttributes's
        constexpr uint32_t kOpenModes = sym::openModes;    // FileMode -> sceMcOpen's mode
        constexpr int32_t kDirectoryEntries = 16;       // sceMcGetDir's table

        inline ee::Global<uint32_t, sym::g_bytesWanted> g_bytesWanted; // memoryCardHasFreeSpace's, for its handler

        using sce::sceMcResDeniedPermit;
        using sce::sceMcResFailReplace;
        using sce::sceMcResFullDevice;
        using sce::sceMcResNoEntry;
        using sce::sceMcResNoFormat;
        using sce::sceMcResNotEmpty;
        using sce::sceMcResUpLimitHandle;

        // A file open on the card, as zfile holds it.
        struct CardFile
        {
            int32_t descriptor; // sceMcOpen's
        };

        // The handlers, and the poll task the operations with a callback run on.
        constexpr uint32_t kFormatDone = sym::memoryCardFormatDone;
        constexpr uint32_t kFormattedDone = sym::memoryCardFormattedDone;
        constexpr uint32_t kPresentDone = sym::memoryCardPresentDone;
        constexpr uint32_t kMountDone = sym::memoryCardMountDone;
        constexpr uint32_t kFreeSpaceDone = sym::memoryCardFreeSpaceDone;
        constexpr uint32_t kListDone = sym::memoryCardListDone;
        constexpr uint32_t kMakeDirectoryDone = sym::memoryCardMakeDirectoryDone;
        constexpr uint32_t kRemoveDone = sym::memoryCardRemoveDone;
        constexpr uint32_t kOpenDone = sym::memoryCardOpenDone;
        constexpr uint32_t kCloseDone = sym::memoryCardCloseDone;
        constexpr uint32_t kWriteDone = sym::memoryCardWriteDone;
        constexpr uint32_t kReadDone = sym::memoryCardReadDone;
        constexpr uint32_t kSeekDone = sym::memoryCardSeekDone;
        constexpr uint32_t kMemoryCardPollTask = sym::memoryCardPollTask;
        constexpr uint32_t kPollTaskPriority = kStageAfterFrame;
        constexpr int32_t kPollTaskSourceLine = game::readable::perGame(684, 683);

        constexpr uint32_t kTextFormatFailed = sym::g_textFailedToFormatMemoryCard;     // "Failed to format memory card"
        constexpr uint32_t kTextNoCardOrUnformatted = sym::g_textNoMemoryCardFoundOrItIsNot; // "No memory card found or it is not formated"
        constexpr uint32_t kTextCardChanged = sym::g_textNoMemoryCardFoundOrItWasSubstitute;      // "No memory card found or it was substituted"
        constexpr uint32_t kTextNoCardToMount = sym::g_textNoMemcardFoundToMountOnSpecified;    // "No memcard found to mount on specified port/slot"
        constexpr uint32_t kTextNoFreeSpace = sym::g_textMemoryCardDoesNotHaveEnough;      // "Memory card does not have enough free space"
        constexpr uint32_t kTextMountLost = sym::g_textDifferentMemoryCardInserted;        // "Different memory card inserted, mount lost"
        constexpr uint32_t kTextNoDirectory = sym::g_textDirectoryNotFound;      // "Directory not found"
        constexpr uint32_t kTextNotFormatted = sym::g_textMemoryCardIsNotFormated;     // "Memory card is not formated!"
        constexpr uint32_t kTextFullForDirectory = sym::g_textMemoryCardIsFullCannotCreate; // "Memory card is full, cannot create directory!"
        constexpr uint32_t kTextBadDirectoryPath = sym::g_textInvalidPathCannotCreateDirectory; // "Invalid path, cannot create directory!"
        constexpr uint32_t kTextBadDeletePath = sym::g_textInvalidPathOrFileCannotDelete;    // "Invalid path or file, cannot delete!"
        constexpr uint32_t kTextInUseDelete = sym::g_textFileOrDirectoryCurrentlyIn;      // "File or directory currently in use, cannot delete!"
        constexpr uint32_t kTextNotEmpty = sym::g_textSubDirectoryIsNotEmptyCannot;         // "Sub-directory is not empty, cannot delete!"
        constexpr uint32_t kTextFullForOpen = sym::g_textNotEnoughFreeSpaceOnTheMemory;      // "Not enough free space on the memory card!"
        constexpr uint32_t kTextBadOpenPath = sym::g_textInvalidPathOrFileCannotOpen;      // "Invalid path or file, cannot open!"
        constexpr uint32_t kTextInUseOpen = sym::g_textFileOrDirectoryCurrentlyIn2;        // "File or directory currently in use, cannot open!"
        constexpr uint32_t kTextTooManyFiles = sym::g_textTooManySimultaniouslyOpened;     // "Too many simultaniously opened files!"
        constexpr uint32_t kTextBadHandleClose = sym::g_textInvalidFileHandleCannotClose;   // "Invalid file handle, cannot close!"
        constexpr uint32_t kTextFullForWrite = sym::g_textNotEnoughSpaceToWriteToFile;     // "Not enough space to write to file"
        constexpr uint32_t kTextBadHandleWrite = sym::g_textInvalidFileHandleCannotWrite;   // "Invalid file handle, cannot write!"
        constexpr uint32_t kTextNotWritable = sym::g_textFileNotOpenInWriteMode;      // "File not open in write mode"
        constexpr uint32_t kTextWriteAreaFailed = sym::g_textMemoryCardWriteToSubrogation;  // "Memory card write to subrogation area failed. (whatever that means)!"
        constexpr uint32_t kTextBadHandleRead = sym::g_textInvalidFileHandleCannotRead;    // "Invalid file handle, cannot read!"
        constexpr uint32_t kTextNotReadable = sym::g_textFileNotOpenInReadMode;      // "File not open in read mode"
        constexpr uint32_t kTextBadHandleSeek = sym::g_textInvalidFileHandleCannotSeek;    // "Invalid file handle, cannot seek!"
        constexpr uint32_t kSourceFile = sym::g_textNgengineZge3Ps2Ps2MemCardC;           // "../ngengine/ZGE3/ps2/Ps2MemCard.c"
        constexpr uint32_t kTextNoAttributes = sym::g_textPs2MemCardSystemDoesNotRequire;     // "PS2 MemCard system does not require any attributes"
        constexpr uint32_t kTextReentered = sym::g_textMemCardCallAlreadyInProgress;        // "MemCard call already in progress, cannot reenter"
        constexpr uint32_t kTextNotMounted = sym::g_textMemoryCardDeviceNotMounted;       // "Memory Card device not mounted"
        constexpr uint32_t kTextNoCardMounted = sym::g_textNoMemoryCardNotMounted;    // "No memory card not mounted"

        // The game functions below call: ee/plain.h. libmc's first: each of these
        // starts a command, or says at once that it could not (not 0).
        void sceMcInit() { ee::game(sym::sceMcInit); }
        int32_t sceMcSync(int32_t mode, ee::Address command, ee::Address result) { return ee::game<int32_t>(sym::sceMcSync, mode, command, result); }
        int32_t sceMcGetInfo(int32_t port, int32_t slot, ee::Address type, ee::Address free, ee::Address format) { return ee::game<int32_t>(sym::sceMcGetInfo, port, slot, type, free, format); }
        int32_t sceMcFormat(int32_t port, int32_t slot) { return ee::game<int32_t>(sym::sceMcFormat, port, slot); }
        int32_t sceMcUnformat(int32_t port, int32_t slot) { return ee::game<int32_t>(sym::sceMcUnformat, port, slot); }
        int32_t sceMcGetDir(int32_t port, int32_t slot, uint32_t path, int32_t mode, int32_t maxEntries, ee::Address table) { return ee::game<int32_t>(sym::sceMcGetDir, port, slot, path, mode, maxEntries, table); }
        int32_t sceMcMkdir(int32_t port, int32_t slot, uint32_t path) { return ee::game<int32_t>(sym::sceMcMkdir, port, slot, path); }
        int32_t sceMcDelete(int32_t port, int32_t slot, uint32_t path) { return ee::game<int32_t>(sym::sceMcDelete, port, slot, path); }
        int32_t sceMcOpen(int32_t port, int32_t slot, uint32_t path, int32_t mode) { return ee::game<int32_t>(sym::sceMcOpen, port, slot, path, mode); }
        int32_t sceMcClose(int32_t descriptor) { return ee::game<int32_t>(sym::sceMcClose, descriptor); }
        int32_t sceMcRead(int32_t descriptor, uint32_t buffer, int32_t bytes) { return ee::game<int32_t>(sym::sceMcRead, descriptor, buffer, bytes); }
        int32_t sceMcWrite(int32_t descriptor, uint32_t buffer, int32_t bytes) { return ee::game<int32_t>(sym::sceMcWrite, descriptor, buffer, bytes); }
        int32_t sceMcSeek(int32_t descriptor, int32_t offset, int32_t origin) { return ee::game<int32_t>(sym::sceMcSeek, descriptor, offset, origin); }
        void runCallback(uint32_t function, ee::Address report) { ee::game(function, report); }
        void runHandler(uint32_t handler, int32_t result, uint32_t subject) { ee::game(handler, result, subject); }

        // A handler's last act: the original's tail calls of the report functions.
        template <typename Subject>
        int32_t succeed(Subject subject)
        {
            return ee::tailCall<int32_t>(sym::reportMemoryCardResult, 1, subject, g_card->resultOut, g_card->callback, kNoText);
        }
        template <typename Subject>
        int32_t fail(Subject subject, uint32_t text)
        {
            return ee::tailCall<int32_t>(sym::reportMemoryCardResult, 0, subject, g_card->resultOut, g_card->callback, text);
        }
        // The card is gone: the mount with it.
        template <typename Subject>
        int32_t failUnmounted(Subject subject, uint32_t text)
        {
            const uint32_t callback = g_card->callback, resultOut = g_card->resultOut;
            g_card->mounted = 0;
            return ee::tailCall<int32_t>(sym::reportMemoryCardResult, 0, subject, resultOut, callback, text);
        }
        int32_t succeedIo(ee::Ptr<CardFile> file)
        {
            return ee::tailCall<int32_t>(sym::reportMemoryCardIoResult, 1, file, g_card->resultOut, g_card->callback, kNoText);
        }
        int32_t failIo(ee::Ptr<CardFile> file, uint32_t text)
        {
            return ee::tailCall<int32_t>(sym::reportMemoryCardIoResult, 0, file, g_card->resultOut, g_card->callback, text);
        }
        int32_t failIoUnmounted(ee::Ptr<CardFile> file)
        {
            const uint32_t callback = g_card->callback, resultOut = g_card->resultOut;
            g_card->mounted = 0;
            return ee::tailCall<int32_t>(sym::reportMemoryCardIoResult, 0, file, resultOut, callback, kTextNoCardToMount);
        }

        bool busy() { return g_card->busy == 1; }

        // sceMcGetInfo about the mounted card, into the device's state.
        int32_t refreshCardInfo(int32_t port, int32_t slot)
        {
            MemoryCard &card = *g_card;
            return sceMcGetInfo(port, slot, &card.type, &card.freeKilobytes, &card.format);
        }

        // Bytes as whole kilobytes, rounded up.
        int32_t wholeKilobytes(int32_t bytes) { return static_cast<uint32_t>(bytes + 1023) >> 10; }
    }

    // saveBlocksNeeded: the 1 KB blocks a save needs: its icon's size and its
    // data's (with four bytes more) rounded up, plus 5.
    int32_t saveBlocksNeeded(ee::Ptr<SaveClient> client)
    {
        const int32_t iconBlocks = wholeKilobytes(client->iconSize());
        const int32_t dataBlocks = wholeKilobytes(client->dataSize() + 4);
        return iconBlocks + dataBlocks + 5;
    }
    READABLE_FUNCTION(saveBlocksNeeded, Verified);

    // reportMemoryCardResult: an operation's end: the result word, a failure's text in
    // zfile's last error (only with a callback), the report to the callback.
    void reportMemoryCardResult(uint32_t succeeded, ee::Address subject, uint32_t resultOut, uint32_t callback, uint32_t text)
    {
        ee::Local<FileReport> report;
        if (succeeded == 0)
        {
            if (resultOut != 0)
                *ee::Ptr<uint32_t>(resultOut) = 0;
            if (callback == 0)
                return;
            registerFileError(text);
            const FileCallback &target = *ee::Ptr<FileCallback>(callback);
            const uint32_t function = target.function;
            report->subject = subject;
            report->context = target.context;
            report->succeeded = 0;
            report->value = 0;
            runCallback(function, report);
            return;
        }
        if (resultOut != 0)
            *ee::Ptr<uint32_t>(resultOut) = 1;
        if (callback != 0)
        {
            const FileCallback &target = *ee::Ptr<FileCallback>(callback);
            const uint32_t function = target.function;
            report->subject = subject;
            report->value = 1;
            report->context = target.context;
            report->succeeded = 1;
            runCallback(function, report);
        }
    }
    READABLE_FUNCTION(reportMemoryCardResult, Verified);

    // reportMemoryCardIoResult: the same for a read, write or seek: no result word, the
    // report's value the bytes done.
    void reportMemoryCardIoResult(uint32_t succeeded, ee::Address file, uint32_t, uint32_t callback, uint32_t text)
    {
        ee::Local<FileReport> report;
        if (callback == 0)
            return;
        const FileCallback &target = *ee::Ptr<FileCallback>(callback);
        if (succeeded == 0)
        {
            registerFileError(text);
            const uint32_t function = target.function;
            report->subject = file;
            report->context = target.context;
            report->succeeded = 0;
            report->value = 0;
            runCallback(function, report);
            return;
        }
        const uint32_t function = target.function;
        report->subject = file;
        report->succeeded = 1;
        report->value = g_card->bytesDone;
        report->context = target.context;
        runCallback(function, report);
    }
    READABLE_FUNCTION(reportMemoryCardIoResult, Verified);

    // memoryCardFormatDone: sceMcFormat's handler. A failed format drops the mount.
    int32_t memoryCardFormatDone(uint32_t mcResult)
    {
        g_card->busy = 0;
        if (mcResult == 0)
        {
            g_card->format = 1;
            return succeed(kDeviceTable);
        }
        return failUnmounted(kDeviceTable, kTextFormatFailed);
    }
    READABLE_FUNCTION_WAITS(memoryCardFormatDone, Verified);

    // memoryCardFormattedDone: sceMcGetInfo's handler of checkFormatted: the same card
    // and formatted. (Keeps the mount either way.)
    int32_t memoryCardFormattedDone(uint32_t mcResult)
    {
        g_card->busy = 0;
        if (mcResult == 0)
        {
            if (g_card->format == 1)
                return succeed(kDeviceTable);
        }
        return fail(kDeviceTable, kTextNoCardOrUnformatted);
    }
    READABLE_FUNCTION_WAITS(memoryCardFormattedDone, Verified);

    // memoryCardPresentDone: sceMcGetInfo's handler of checkMedium: still the same card.
    int32_t memoryCardPresentDone(uint32_t mcResult)
    {
        g_card->busy = 0;
        if (mcResult == 0)
            return succeed(kDeviceTable);
        return failUnmounted(kDeviceTable, kTextCardChanged);
    }
    READABLE_FUNCTION_WAITS(memoryCardPresentDone, Verified);

    // memoryCardMountDone: sceMcGetInfo's handler of mount: 0, -1 or -2 (the same
    // card, another one, an unformatted one): a card there, mounted.
    int32_t memoryCardMountDone(uint32_t mcResult)
    {
        g_card->busy = 0;
        if (static_cast<int32_t>(mcResult) >= sceMcResNoFormat && static_cast<int32_t>(mcResult) <= 0)
        {
            g_card->mounted = 1;
            return succeed(kDeviceTable);
        }
        return fail(kDeviceTable, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardMountDone, Verified);

    // memoryCardFreeSpaceDone: sceMcGetInfo's handler of checkFreeSpace: the kilobytes
    // the bytes wanted need against the card's.
    int32_t memoryCardFreeSpaceDone(uint32_t mcResult, ee::Ptr<uint32_t> bytesWanted)
    {
        g_card->busy = 0;
        if (mcResult == 0)
        {
            if (g_card->freeKilobytes < (*bytesWanted >> 10))
                return fail(kDeviceTable, kTextNoFreeSpace);
            return succeed(kDeviceTable);
        }
        if (mcResult + 2 < 2u) // the card was changed, or is not formatted: another card
            return failUnmounted(kDeviceTable, kTextMountLost);
        return failUnmounted(kDeviceTable, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardFreeSpaceDone, Verified);

    // memoryCardListDone: sceMcGetDir's handler: entries found. None, or -4: no
    // such directory.
    int32_t memoryCardListDone(uint32_t mcResult)
    {
        g_card->busy = 0;
        if (static_cast<int32_t>(mcResult) > 0)
            return succeed(kDeviceTable);
        if (mcResult == 0 || static_cast<int32_t>(mcResult) == sceMcResNoEntry)
            return fail(kDeviceTable, kTextNoDirectory);
        if (static_cast<int32_t>(mcResult) == sceMcResNoFormat)
            return fail(kDeviceTable, kTextNotFormatted);
        return failUnmounted(kDeviceTable, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardListDone, Verified);

    // memoryCardMakeDirectoryDone: sceMcMkdir's handler.
    int32_t memoryCardMakeDirectoryDone(uint32_t mcResult)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->busy = 0;
        if (code == 0)
            return succeed(kDeviceTable);
        if (code == sceMcResNoFormat)
            return fail(kDeviceTable, kTextNotFormatted);
        if (code == sceMcResFullDevice)
            return fail(kDeviceTable, kTextFullForDirectory);
        if (code == sceMcResNoEntry)
            return fail(kDeviceTable, kTextBadDirectoryPath);
        return failUnmounted(kDeviceTable, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardMakeDirectoryDone, Verified);

    // memoryCardRemoveDone: sceMcDelete's handler.
    int32_t memoryCardRemoveDone(uint32_t mcResult)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->busy = 0;
        if (code == 0)
            return succeed(kDeviceTable);
        if (code == sceMcResNoFormat)
            return fail(kDeviceTable, kTextNotFormatted);
        if (code == sceMcResNoEntry)
            return fail(kDeviceTable, kTextBadDeletePath);
        if (code == sceMcResDeniedPermit)
            return fail(kDeviceTable, kTextInUseDelete);
        if (code == sceMcResNotEmpty)
            return fail(kDeviceTable, kTextNotEmpty);
        return failUnmounted(kDeviceTable, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardRemoveDone, Verified);

    // memoryCardOpenDone: sceMcOpen's handler: the file's handle is the result. A
    // failure reports no subject, and the file stays as it was (-1 from
    // memoryCardOpen, without a callback).
    int32_t memoryCardOpenDone(uint32_t mcResult, ee::Ptr<CardFile> file)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->busy = 0;
        if (code == 0)
        {
            file->descriptor = 0;
            return succeed(file);
        }
        if (code == sceMcResNoFormat)
            return fail(0, kTextNotFormatted);
        if (code == sceMcResFullDevice)
            return fail(0, kTextFullForOpen);
        if (code == sceMcResNoEntry)
            return fail(0, kTextBadOpenPath);
        if (code == sceMcResDeniedPermit)
            return fail(0, kTextInUseOpen);
        if (code == sceMcResUpLimitHandle)
            return fail(0, kTextTooManyFiles);
        return failUnmounted(0, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardOpenDone, Verified);

    // memoryCardCloseDone: sceMcClose's handler.
    int32_t memoryCardCloseDone(uint32_t mcResult, ee::Ptr<CardFile> file)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->busy = 0;
        if (code == 0)
            return succeed(file);
        if (code == sceMcResNoFormat)
            return fail(file, kTextNotFormatted);
        if (code == sceMcResNoEntry)
            return fail(file, kTextBadHandleClose);
        return failUnmounted(file, kTextNoCardToMount);
    }
    READABLE_FUNCTION_WAITS(memoryCardCloseDone, Verified);

    // memoryCardWriteDone: sceMcWrite's handler: the bytes written, -1 until known.
    int32_t memoryCardWriteDone(uint32_t mcResult, ee::Ptr<CardFile> file)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->bytesDone = -1;
        g_card->busy = 0;
        if (code >= 0)
        {
            g_card->bytesDone = code;
            return succeedIo(file);
        }
        if (code == sceMcResNoFormat)
            return failIo(file, kTextNotFormatted);
        if (code == sceMcResFullDevice)
            return failIo(file, kTextFullForWrite);
        if (code == sceMcResNoEntry)
            return failIo(file, kTextBadHandleWrite);
        if (code == sceMcResDeniedPermit)
            return failIo(file, kTextNotWritable);
        if (code == sceMcResFailReplace)
            return failIo(file, kTextWriteAreaFailed);
        return failIoUnmounted(file);
    }
    READABLE_FUNCTION_WAITS(memoryCardWriteDone, Verified);

    // memoryCardReadDone: sceMcRead's handler: the bytes read, -1 until known.
    int32_t memoryCardReadDone(uint32_t mcResult, ee::Ptr<CardFile> file)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->bytesDone = -1;
        g_card->busy = 0;
        if (code >= 0)
        {
            g_card->bytesDone = code;
            return succeedIo(file);
        }
        if (code == sceMcResNoFormat)
            return failIo(file, kTextNotFormatted);
        if (code == sceMcResNoEntry)
            return failIo(file, kTextBadHandleRead);
        if (code == sceMcResDeniedPermit)
            return failIo(file, kTextNotReadable);
        return failIoUnmounted(file);
    }
    READABLE_FUNCTION_WAITS(memoryCardReadDone, Verified);

    // memoryCardSeekDone: sceMcSeek's handler: the position.
    int32_t memoryCardSeekDone(uint32_t mcResult, ee::Ptr<CardFile> file)
    {
        const int32_t code = static_cast<int32_t>(mcResult);
        g_card->busy = 0;
        g_card->bytesDone = 0;
        if (code >= 0)
        {
            g_card->bytesDone = code;
            return succeedIo(file);
        }
        if (code == sceMcResNoFormat)
            return failIo(file, kTextNotFormatted);
        if (code == sceMcResNoEntry)
            return failIo(file, kTextBadHandleSeek);
        return failIoUnmounted(file);
    }
    READABLE_FUNCTION_WAITS(memoryCardSeekDone, Verified);

    // memoryCardPath: a path as libmc wants it, in a static buffer: its
    // backslashes made slashes (the string's length asked at every character,
    // as the original does).
    uint32_t memoryCardPath(uint32_t path)
    {
        stringCopy(kPathBuffer, path);
        const ee::Ptr<char> copy(kPathBuffer);
        for (uint32_t i = 0; i < static_cast<uint32_t>(stringLength(kPathBuffer)); ++i)
        {
            if (copy[i] == '\\')
                copy[i] = '/';
        }
        return kPathBuffer;
    }
    READABLE_FUNCTION(memoryCardPath, Verified);

    // memoryCardPollTask: the task of an operation with a callback: when sceMcSync
    // says the command ended, the task goes and the handler runs.
    uint32_t memoryCardPollTask(uint32_t subject)
    {
        ee::Local<int32_t> commandResult;
        if (sceMcSync(sce::sceMcSyncPoll, 0, commandResult) != 0)
        {
            deleteTask(g_card->task);
            const uint32_t handler = g_card->doneHandler;
            g_card->busy = 0;
            if (handler != 0)
                runHandler(handler, *commandResult, subject);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(memoryCardPollTask, Verified);

    // memoryCardSyncResult: one poll of sceMcSync for an operation without a task:
    // 1 when it ended (its handler has run).
    uint32_t memoryCardSyncResult(uint32_t subject, ee::Ptr<int32_t> commandResult)
    {
        if (sceMcSync(sce::sceMcSyncPoll, 0, commandResult) == 0)
            return 0;
        const uint32_t handler = g_card->doneHandler;
        if (handler != 0)
            runHandler(handler, *commandResult, subject);
        return 1;
    }
    READABLE_FUNCTION_WAITS(memoryCardSyncResult, Verified);

    // startMemoryCardCall: a command started: its handler kept, then with a callback
    // a task that polls, without one a spin until it ended.
    uint32_t startMemoryCardCall(uint32_t doneHandler, uint32_t callback, uint32_t resultOut, ee::Address subject)
    {
        ee::Local<int32_t> commandResult;
        g_card->doneHandler = doneHandler;
        g_card->resultOut = resultOut;
        g_card->callback = callback;
        if (callback != 0)
        {
            g_card->busy = 1;
            g_card->task = newTask(kPollTaskPriority, kMemoryCardPollTask, subject, 0, kSourceFile, kPollTaskSourceLine);
            return 1;
        }
        while (memoryCardSyncResult(subject, commandResult) == 0)
        {
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(startMemoryCardCall, Verified);

    // initMemoryCardDevice (the platform's device list).
    uint32_t initMemoryCardDevice()
    {
        g_card->present = 1;
        g_card->mounted = 0;
        sceMcInit();
        return 1;
    }
    READABLE_FUNCTION(initMemoryCardDevice, Verified);

    // shutdownMemoryCardDevice: no caller.
    uint32_t shutdownMemoryCardDevice()
    {
        if (g_card->present == 0)
            return 1;
        g_card->present = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownMemoryCardDevice, Draft);

    // memoryCardDevice: FileDevice::name.
    uint32_t memoryCardDevice()
    {
        return kDeviceName;
    }
    READABLE_FUNCTION(memoryCardDevice, Verified);

    // memoryCardSetAttributes: FileDevice::fileName: a card file's name has no attributes.
    uint32_t memoryCardSetAttributes(uint32_t, uint32_t count)
    {
        if (count != 0)
        {
            registerFileError(kTextNoAttributes);
            return 0;
        }
        stringCopy(kFileNameBuffer, kNoText);
        return kFileNameBuffer;
    }
    READABLE_FUNCTION(memoryCardSetAttributes, Verified);

    // memoryCardIsFormatted: FileDevice::checkFormatted.
    uint32_t memoryCardIsFormatted(uint32_t device, uint32_t resultOut, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, resultOut, callback, kTextNotMounted);
            else
            {
                if (refreshCardInfo(g_card->port, g_card->slot) == 0)
                {
                    return startMemoryCardCall(kFormattedDone, callback, resultOut, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardIsFormatted, Verified);

    // memoryCardFormat: FileDevice::format.
    uint32_t memoryCardFormat(uint32_t device, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                if (sceMcFormat(g_card->port, g_card->slot) == 0)
                {
                    return startMemoryCardCall(kFormatDone, callback, 0, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardFormat, Verified);

    // memoryCardIsPresent: FileDevice::checkMedium.
    uint32_t memoryCardIsPresent(uint32_t device, uint32_t resultOut, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, resultOut, callback, kTextNotMounted);
            else
            {
                if (refreshCardInfo(g_card->port, g_card->slot) == 0)
                {
                    return startMemoryCardCall(kPresentDone, callback, resultOut, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardIsPresent, Verified);

    // memoryCardUnformat: FileDevice::mount: "port:slot", or "*u" with a mounted
    // card to unmount it (libmc's Unformat, a command with no handler).
    uint32_t memoryCardUnformat(uint32_t, ee::Ptr<const char> where, uint32_t resultOut, uint32_t callback)
    {
        ee::Local<int32_t> commandResult;
        if (busy())
        {
            registerFileError(kTextReentered);
            return 0;
        }
        if (where[0] == '*' && where[1] == 'u' && g_card->mounted == 1)
        {
            if (sceMcUnformat(g_card->port, g_card->slot) != 0)
                return 0;
            sceMcSync(sce::sceMcSyncWait, 0, commandResult);
            return 1;
        }
        g_card->mounted = 0;
        const int32_t port = where[0] - '0';
        g_card->port = port;
        const int32_t slot = where[2] - '0';
        g_card->slot = slot;
        if (refreshCardInfo(port, slot) != 0)
            return 0;
        return startMemoryCardCall(kMountDone, callback, resultOut, 0);
    }
    READABLE_FUNCTION_WAITS(memoryCardUnformat, Verified);

    // memoryCardHasFreeSpace: FileDevice::checkFreeSpace.
    uint32_t memoryCardHasFreeSpace(uint32_t device, uint32_t bytes, uint32_t resultOut, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            const int32_t mounted = g_card->mounted;
            *g_bytesWanted = bytes;
            if (mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                if (refreshCardInfo(g_card->port, g_card->slot) == 0)
                {
                    return startMemoryCardCall(kFreeSpaceDone, callback, resultOut, sym::g_bytesWanted);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardHasFreeSpace, Verified);

    namespace
    {
        // findDirectory and findFile: sceMcGetDir on the path.
        uint32_t listCardPath(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
        {
            if (busy())
                registerFileError(kTextReentered);
            else if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                const uint32_t cardPath = memoryCardPath(path);
                if (sceMcGetDir(g_card->port, g_card->slot, cardPath, sce::sceMcGetDirFirst, kDirectoryEntries, &g_card->directoryEntries) == 0)
                    return startMemoryCardCall(kListDone, callback, resultOut, 0);
            }
            return 0;
        }
    }

    // memoryCardListDirectory: FileDevice::findDirectory.
    uint32_t memoryCardListDirectory(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        return listCardPath(device, path, resultOut, callback);
    }
    READABLE_FUNCTION_WAITS(memoryCardListDirectory, Verified);

    // memoryCardMakeDirectory: FileDevice::makeDirectory.
    uint32_t memoryCardMakeDirectory(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                const uint32_t cardPath = memoryCardPath(path);
                if (sceMcMkdir(g_card->port, g_card->slot, cardPath) == 0)
                {
                    return startMemoryCardCall(kMakeDirectoryDone, callback, resultOut, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardMakeDirectory, Verified);

    // memoryCardRemoveDirectory: FileDevice::removeDirectory. (The path is asked for
    // before the mount is looked at: this one alone.)
    uint32_t memoryCardRemoveDirectory(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            const uint32_t cardPath = memoryCardPath(path);
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                if (sceMcDelete(g_card->port, g_card->slot, cardPath) == 0)
                {
                    return startMemoryCardCall(kRemoveDone, callback, resultOut, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardRemoveDirectory, Verified);

    // memoryCardFindFile: FileDevice::findFile.
    uint32_t memoryCardFindFile(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        return listCardPath(device, path, resultOut, callback);
    }
    READABLE_FUNCTION_WAITS(memoryCardFindFile, Verified);

    // memoryCardFindFileAt: FileDevice::fileExists.
    int32_t memoryCardFindFileAt(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        const uint32_t cardPath = memoryCardPath(path);
        return static_cast<int32_t>(memoryCardFindFile(device, cardPath, resultOut, callback));
    }
    READABLE_FUNCTION_WAITS(memoryCardFindFileAt, Verified);

    // memoryCardRemoveFile: FileDevice::deleteFile.
    uint32_t memoryCardRemoveFile(uint32_t device, uint32_t path, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, device, 0, callback, kTextNoCardMounted);
            else
            {
                const uint32_t cardPath = memoryCardPath(path);
                if (sceMcDelete(g_card->port, g_card->slot, cardPath) == 0)
                {
                    return startMemoryCardCall(kRemoveDone, callback, 0, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardRemoveFile, Verified);

    // memoryCardOpen: FileDevice::open: the file's handle set by the handler; 0
    // when it failed. A mode that writes creates a file that is not there.
    int32_t memoryCardOpen(ee::Ptr<CardFile> file, uint32_t path, uint32_t mode, uint32_t callback)
    {
        ee::Local<char[0x80]> pathCopy;
        ee::Local<int32_t> syncCommand;
        ee::Local<int32_t> syncResult;
        if (busy())
        {
            registerFileError(kTextReentered);
            return 0;
        }
        if (g_card->mounted == 0)
        {
            reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            return 0;
        }
        const uint32_t cardPath = memoryCardPath(path);
        stringCopy(pathCopy, cardPath);
        int32_t mcMode = ee::Ptr<int32_t>(kOpenModes)[mode];
        if (mode >= 2u)
        {
            sceMcGetDir(g_card->port, g_card->slot, cardPath, sce::sceMcGetDirFirst, kDirectoryEntries, &g_card->directoryEntries);
            sceMcSync(sce::sceMcSyncWait, syncCommand, syncResult);
            if (*syncResult <= 0)
                mcMode |= sce::sceMcFileCreateFile;
        }
        int32_t result = 0;
        if (sceMcOpen(g_card->port, g_card->slot, cardPath, mcMode) == 0)
        {
            if (callback == 0)
                file->descriptor = -1;
            result = startMemoryCardCall(kOpenDone, callback, 0, file);
            if (result == 1 && file->descriptor == -1)
                result = 0;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(memoryCardOpen, Verified);

    // memoryCardClose: FileDevice::close.
    uint32_t memoryCardClose(ee::Ptr<CardFile> file, uint32_t callback)
    {
        if (busy())
            registerFileError(kTextReentered);
        else
        {
            if (g_card->mounted == 0)
                reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            else
            {
                if (sceMcClose(file->descriptor) == 0)
                {
                    return startMemoryCardCall(kCloseDone, callback, 0, 0);
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(memoryCardClose, Verified);

    // memoryCardRead: FileDevice::read: the bytes read, with a callback 0 for now;
    // -1 when the command did not start.
    uint32_t memoryCardRead(ee::Ptr<CardFile> file, uint32_t buffer, int32_t bytes, uint32_t callback)
    {
        if (g_card->mounted == 0)
        {
            reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            return 0xFFFFFFFFu;
        }
        g_card->bytesDone = 0;
        if (sceMcRead(file->descriptor, buffer, bytes) != 0)
            return 0xFFFFFFFFu;
        startMemoryCardCall(kReadDone, callback, 0, file);
        return g_card->bytesDone;
    }
    READABLE_FUNCTION_WAITS(memoryCardRead, Verified);

    // memoryCardWrite: FileDevice::write.
    uint32_t memoryCardWrite(ee::Ptr<CardFile> file, uint32_t buffer, int32_t bytes, uint32_t callback)
    {
        if (g_card->mounted == 0)
        {
            reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            return 0xFFFFFFFFu;
        }
        g_card->bytesDone = 0;
        if (sceMcWrite(file->descriptor, buffer, bytes) != 0)
            return 0xFFFFFFFFu;
        startMemoryCardCall(kWriteDone, callback, 0, file);
        return g_card->bytesDone;
    }
    READABLE_FUNCTION_WAITS(memoryCardWrite, Verified);

    // memoryCardSeek: FileDevice::seek: 1 when it started; the handler keeps the
    // position.
    uint32_t memoryCardSeek(ee::Ptr<CardFile> file, int32_t offset, int32_t origin, uint32_t callback)
    {
        if (g_card->mounted == 0)
        {
            reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            return 0;
        }
        g_card->bytesDone = 0;
        uint32_t result = 0;
        if (sceMcSeek(file->descriptor, offset, origin) == 0)
        {
            startMemoryCardCall(kSeekDone, callback, 0, file);
            result = 1;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(memoryCardSeek, Verified);

    namespace
    {
        constexpr int32_t kSeekFromHere = 1; // libmc's SEEK_CUR
    }

    // memoryCardTell: FileDevice::position: the file's position asked of the
    // card (a seek by 0 from where it is); 1 when it started. Without a
    // callback the command has ended by then and positionOut (when given)
    // gets the position; with one, it gets the 0 the handler has yet to
    // replace.
    uint32_t memoryCardTell(ee::Ptr<CardFile> file, ee::Ptr<int32_t> positionOut, uint32_t callback)
    {
        if (g_card->mounted == 0)
        {
            reportMemoryCardResult(0, file, 0, callback, kTextNoCardMounted);
            return 0;
        }
        g_card->bytesDone = 0;
        if (sceMcSeek(file->descriptor, 0, kSeekFromHere) != 0)
            return 0;
        startMemoryCardCall(kSeekDone, callback, 0, file);
        if (positionOut)
            *positionOut = g_card->bytesDone;
        return 1;
    }
    READABLE_FUNCTION_WAITS(memoryCardTell, Verified);

    namespace
    {
        constexpr uint32_t kTextNotForThisDevice = sym::g_textFunctionNotImplementedForThis3; // "Function not implemented for this device"
    }

    // memoryCardSize: FileDevice::size, which the card does not answer.
    uint32_t memoryCardSize()
    {
        registerFileError(kTextNotForThisDevice);
        return 0;
    }
    READABLE_FUNCTION(memoryCardSize, Verified);
}
