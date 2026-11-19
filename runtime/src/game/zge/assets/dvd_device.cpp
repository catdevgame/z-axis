// The engine's file device on the disc ("dvd0", g_dvdDeviceTable; zge/assets/file.h
// has the device's table): a client of an IOP module over SIF RPC (server
// 0x45; its completions come back through an EE server of ours, 0x46). One
// operation at a time, under a semaphore: an operation with a callback
// returns at once and its end function (the check...() here) gives the lock
// back; one without waits for the IOP. Reads go through a 32 KB buffer of
// whole sectors when they fit, straight to the destination when they do not
// (through the buffer's copy on the IOP side if they are not aligned).
//
// This is the device's PS2 form, which the readable game proves against the
// original. The host runs its own form of the functions that talk to the IOP
// (zge/host/disc_device.cpp: the game's files on the host); [host] disc = ps2
// (ZAXIS_DISC=ps2, PS2X_DISC=cdvd) runs this one over the emulated IOP, the
// only route to these. The rest of the table (the device's name, the checks
// with nothing to ask the IOP) is the same in both.

#include "dvd_client.h"
#include "file.h"

#include "ee/function_ptr.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/dvd_device.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kSectorBytes = 0x800;

        // What the IOP module is asked, by function number.
        enum DvdFunction : uint32_t
        {
            kDvdFnStart = 0,
            kDvdFnMount = 1,
            kDvdFnStat = 3,
            kDvdFnOpen = 4,
            kDvdFnClose = 5,
            kDvdFnRead = 6,
            kDvdFnSeek = 8,
            kDvdFnTell = 9,
            kDvdFnReadBuffered = 10,
        };
        constexpr uint32_t kReplyBytes = 0x80, kPathBytes = 0x100;
        constexpr uint32_t kRpcServerId = 0x45, kCompletionServerId = 0x46;
        constexpr uint32_t kRpcNoWait = 1;

        // How a read tells that it is done.
        enum DvdReadMode : uint32_t
        {
            kReadReportsCallback = 1, // the completion server calls it
            kReadSignalsSema = 2,     // readSema is signalled
            kReadThenCallback = 3,    // of the buffer: the completion server copies, then calls it
        };

        constexpr uint32_t kDvd = g_dvd.address;
        constexpr uint32_t kDvdRpc = kDvd + offsetof(DvdClient, rpc);
        constexpr uint32_t kDvdBuffer = kDvd + offsetof(DvdClient, buffer);
        constexpr uint32_t kDvdPath = kDvd + offsetof(DvdClient, path);
        constexpr uint32_t kDvdReply = kDvd + offsetof(DvdClient, reply);
        constexpr uint32_t kDvdRequestFile = kDvd + offsetof(DvdClient, requestFile);
        constexpr uint32_t kDvdSeekRequest = kDvd + offsetof(DvdClient, seekFile);
        constexpr uint32_t kDvdReadRequest = kDvd + offsetof(DvdClient, read);
        constexpr uint32_t kDvdServerQueue = kDvd + offsetof(DvdClient, serverQueue);
        constexpr uint32_t kDvdServerData = kDvd + offsetof(DvdClient, serverData);
        constexpr uint32_t kDvdServerStack = kDvd + offsetof(DvdClient, serverStack);
        constexpr uint32_t kDvdServerBuffer = kDvd + offsetof(DvdClient, serverBuffer);

        // What the completion server gets from the IOP (the RPC's data).
        enum DvdCompletionKind : uint32_t
        {
            kCompletionRead = 1,
            kCompletionWoken = 2,
            kCompletionBufferRead = 3,
            kCompletionBuffered = 4,
        };
        struct DvdCompletion
        {
            uint32_t file;        // +0x00
            uint32_t value;       // +0x04
            uint32_t kind;        // +0x08
            uint32_t notify;      // +0x0C the callback, or the semaphore
            uint32_t destination; // +0x10
            uint32_t _14;         // +0x14
            uint32_t offset;      // +0x18 in the buffer
        };
        static_assert(offsetof(DvdCompletion, offset) == 0x18);

        inline ee::Global<uint32_t, sym::g_dvdRequestPool> g_dvdRequestPool;
        constexpr uint32_t kRequestPoolRecordBytes = 10260, kRequestPoolRecords = 8;

        constexpr uint32_t kDvdDevice = sym::g_dvdDeviceTable; // the FileDevice
        constexpr uint32_t kCheckDvdResult = sym::checkDvdResult;
        constexpr uint32_t kCheckDvdFileResult = sym::checkDvdFileResult;
        constexpr uint32_t kCheckDvdPresent = sym::checkDvdPresent;
        constexpr uint32_t kCheckDvdAccess = sym::checkDvdAccess;
        constexpr uint32_t kCheckDvdOpen = sym::checkDvdOpen;
        constexpr uint32_t kCheckDvdSeek = sym::checkDvdSeek;
        constexpr uint32_t kCheckDvdFileInfo = sym::checkDvdFileInfo;
        constexpr uint32_t kDvdCompletionServer = sym::dvdCompletionServer;
        constexpr uint32_t kDvdIopRequest = sym::dvdIopRequest;
        constexpr uint32_t kSceCdDiskReady = sym::sceCdDiskReady;
        constexpr uint32_t kCreateThread = sym::createThread;
        constexpr uint32_t kStartThread = sym::startThread;
        constexpr uint32_t kChangeThreadPriority = sym::changeThreadPriority;
        constexpr uint32_t kGetThreadId = sym::getThreadId;
        constexpr uint32_t kCreateSema = sym::createSema;
        constexpr uint32_t kSignalSema = sym::signalSema;
        constexpr uint32_t kWaitSema = sym::waitSema;
        constexpr uint32_t kFlushCache = sym::flushCache;
        constexpr uint32_t kSifInitRpc = sym::sifInitRpc;
        constexpr uint32_t kSifBindRpc = sym::sifBindRpc;
        constexpr uint32_t kSifCheckStatRpc = sym::sifCheckStatRpc;
        constexpr uint32_t kSifSetRpcQueue = sym::sifSetRpcQueue;
        constexpr uint32_t kSifRegisterRpc = sym::sifRegisterRpc;
        constexpr uint32_t kSifRpcLoop = sym::sifRpcLoop;
        constexpr uint32_t kSceFsReset = sym::sceFsReset;

        constexpr uint32_t kTextNone = sym::textNone; // ""
        constexpr uint32_t kTextNoCd = sym::g_textNoCdFound;
        constexpr uint32_t kTextDvd0 = sym::g_textDvd0;
        constexpr uint32_t kTextMiscError = sym::g_textMiscErrorOccuredDuringAccess;
        constexpr uint32_t kTextOpenError = sym::g_textErrorOpeningFile;
        constexpr uint32_t kTextSeekError = sym::g_textErrorWhielSeeking;
        constexpr uint32_t kTextInfoError = sym::g_textErrorRetreivingFileInformation;
        constexpr uint32_t kTextReadError = sym::g_textErrorWhileReadingDataFromFile;
        constexpr uint32_t kTextBindFailed = sym::g_textPs2CdDvdSystemFailedToBind;
        constexpr uint32_t kTextCannotFormat = sym::g_textThisDeviceCannotBeFormated;
        constexpr uint32_t kTextNotSupported = sym::g_textFunctionNotSupportedForThis;
        constexpr uint32_t kTextNotMounted = sym::g_textDeviceNotMounted;
        constexpr uint32_t kTextReadOnly = sym::g_textOnlyReadOnlyModeSupportedFor;
        constexpr uint32_t kTextWriteNotImplemented = sym::g_textFunctionNotImplementedForThis2;

        // Registers the original holds at its calls that are not this code's.
        constexpr uint32_t kGlobalPointer = sym::gp;

        // The lock, as each operation takes and gives it;
        void takeLock()
        {
            const uint32_t lock = g_dvd->lock;
            --g_dvd->lockCount;
            ee::call(kWaitSema, lock);
        }
        void giveLock()
        {
            const uint32_t lock = g_dvd->lock;
            ++g_dvd->lockCount;
            ee::call(kSignalSema, lock);
        }

        // Until the client's last call is done, counting the spins.
        void waitForClient(uint32_t client = kDvdRpc)
        {
            for (;;)
            {
                if (ee::call<int32_t>(kSifCheckStatRpc, client) == 0)
                    break;
                ++g_dvd->rpcWaitSpins;
            }
        }

        // The common form of the operations that have nothing to ask the IOP:
        // once nothing is running, the check with this result.
        template <typename Succeeded>
        void reportWhenIdle(uint32_t device, uint32_t resultOut, uint32_t callback, Succeeded succeeded, uint32_t text, bool hasResult)
        {
            takeLock();
            giveLock();
            checkDvdResult(succeeded(), device, resultOut, callback, text);
        }
    }

    // initDvdRequestPool
    void initDvdRequestPool()
    {
        *g_dvdRequestPool = createPool(kRequestPoolRecordBytes, kRequestPoolRecords, 0u);
    }
    READABLE_FUNCTION_WAITS(initDvdRequestPool, Verified);

    // shutdownDvdRequestPool
    void shutdownDvdRequestPool()
    {
        destroyPool(ee::Ptr<Pool>(*g_dvdRequestPool));
    }
    READABLE_FUNCTION_WAITS(shutdownDvdRequestPool, Draft);

    // dvdReadError: the lock's semaphore.
    uint32_t dvdReadError() { return g_dvd->lock; }
    READABLE_FUNCTION(dvdReadError, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // SignalSema takes the semaphore alone; the rest is what the original
        // has in the registers, handed on until readable_args_ok.tsv has rows
        // for these three calls.
        template <typename... Rest>
        void signalSema(uint32_t lock, const Rest &...rest) { ee::game(kSignalSema, lock, rest...); }
        // A file callback takes its report and nothing else: the game has two,
        // ignoreFileReport and relayFileReport, and the original's other
        // registers hold what it was doing before the call.
        void callFileCallback(uint32_t function, ee::Ptr<FileReport> report)
        {
            const ee::FunctionPtr<void(ee::Ptr<FileReport>)> callback{function};
            callback(report);
        }
        // What an operation keeps on its stack around an RPC.
        struct DvdCallLocals
        {
            uint32_t rpcEndParameter; // the RPC's ninth argument: on the stack
            uint32_t gap04[3];
            FileReport report;        // +0x10 for its callback
            uint32_t wholeInBuffer;   // +0x20 dvdRead: nothing to ask the IOP
        };
        static_assert(offsetof(DvdCallLocals, report) == 0x10 && offsetof(DvdCallLocals, wholeInBuffer) == 0x20);

        // The kernel's SemaParam and ThreadParam.
        struct SemaParameters
        {
            uint32_t count;        // +0x00
            uint32_t maxCount;     // +0x04
            uint32_t initialCount; // +0x08
            uint32_t waitingThreads;
            uint32_t attributes;
            uint32_t option;
            uint32_t gap18[2];
        };
        static_assert(sizeof(SemaParameters) == 0x20);
        struct ThreadParameters
        {
            uint32_t status;          // +0x00
            uint32_t entry;           // +0x04
            uint32_t stack;           // +0x08
            uint32_t stackBytes;      // +0x0C
            uint32_t globalPointer;   // +0x10
            uint32_t initialPriority; // +0x14
            uint32_t rest[6];
        };
        static_assert(sizeof(ThreadParameters) == 0x30);
        struct DvdInitLocals
        {
            uint32_t rpcEndParameter;
            uint32_t gap04[3];
            ThreadParameters server;  // +0x10
            SemaParameters readSema;  // +0x40
            SemaParameters lockSema;  // +0x60
        };
        static_assert(offsetof(DvdInitLocals, readSema) == 0x40 && offsetof(DvdInitLocals, lockSema) == 0x60);
    }

    // checkDvdResult: an operation's end that holds the lock: the result word,
    // a failure's text kept, the lock given back, the callback's report.
    // Without a callback the lock stays with the waiting caller.
    void checkDvdResult(uint32_t succeeded, uint32_t subject, uint32_t resultOut, uint32_t callback, uint32_t text)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<FileCallback> record(callback);
        if (resultOut != 0)
            *ee::Ptr<uint32_t>(resultOut) = succeeded != 0 ? 1 : 0;
        if (callback == 0)
            return;
        if (succeeded == 0)
            registerFileError(text);
        const uint32_t context = record->context;
        const uint32_t lock = g_dvd->lock;
        ++g_dvd->lockCount;
        report->subject = subject;
        report->context = context;
        report->succeeded = succeeded != 0 ? 1 : 0;
        report->value = succeeded != 0 ? 1 : 0;
        if (succeeded != 0)
            signalSema(lock, 1, context);
        else
            signalSema(lock, context);
        callFileCallback(record->function, report);
    }
    READABLE_FUNCTION(checkDvdResult, Verified);

    // checkDvdFileResult: the same for what ends without the lock: a file's, the
    // value reported. A file's lock is given back by its own end.
    void checkDvdFileResult(uint32_t succeeded, uint32_t value, uint32_t resultOut, uint32_t callback, uint32_t text)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<FileCallback> record(callback);
        if (resultOut != 0)
            *ee::Ptr<uint32_t>(resultOut) = succeeded != 0 ? 1 : 0;
        if (callback == 0)
            return;
        if (succeeded == 0)
        {
            registerFileError(text);
            report->value = value;
            report->context = record->context;
            report->succeeded = 0;
        }
        else
        {
            report->succeeded = 1;
            report->value = value;
            report->context = record->context;
            report->subject = value;
        }
        callFileCallback(record->function, report);
    }
    READABLE_FUNCTION(checkDvdFileResult, Verified);

    // checkDvdPresent: a mount's end.
    void checkDvdPresent()
    {
        const int32_t reply = g_dvd->reply;
        const uint32_t callback = g_dvd->resultCallback, resultOut = g_dvd->resultOut;
        g_dvd->mounted = static_cast<uint32_t>(reply);
        game::tail(checkDvdResult, reply, kDvdDevice, resultOut, callback, kTextNoCd);
    }
    READABLE_FUNCTION_WAITS(checkDvdPresent, Verified);

    // checkDvdAccess: a stat's or a close's end.
    void checkDvdAccess()
    {
        game::tail(checkDvdResult, g_dvd->reply, kDvdDevice, g_dvd->resultOut, g_dvd->resultCallback, kTextMiscError);
    }
    READABLE_FUNCTION_WAITS(checkDvdAccess, Verified);

    // checkDvdAccessInverted
    void checkDvdAccessInverted()
    {
        game::tail(checkDvdResult, static_cast<int32_t>(0u < static_cast<uint32_t>(~g_dvd->reply)), kDvdDevice, g_dvd->resultOut, g_dvd->resultCallback, kTextMiscError);
    }
    READABLE_FUNCTION_WAITS(checkDvdAccessInverted, Draft);

    // checkDvdOpen: an open's end: the IOP's handle and the device in the file.
    void checkDvdOpen()
    {
        ZFile &file = *ee::Ptr<ZFile>(g_dvd->callbackFile);
        file.handle = static_cast<uint32_t>(g_dvd->reply);
        file.device = kDvdDevice;
        game::tail(checkDvdResult, static_cast<int32_t>(0u < static_cast<uint32_t>(g_dvd->reply)), g_dvd->callbackFile, g_dvd->resultOut, g_dvd->resultCallback, kTextOpenError);
    }
    READABLE_FUNCTION_WAITS(checkDvdOpen, Verified);

    // checkDvdSeek: the file position's end.
    void checkDvdSeek()
    {
        game::tail(checkDvdFileResult, static_cast<int32_t>(0u < static_cast<uint32_t>(g_dvd->fileSize)), g_dvd->reply, g_dvd->resultOut, g_dvd->resultCallback, kTextSeekError);
    }
    READABLE_FUNCTION_WAITS(checkDvdSeek, Draft);

    // checkDvdFileInfo: the file size's end.
    void checkDvdFileInfo()
    {
        const int32_t size = g_dvd->fileSize;
        game::tail(checkDvdFileResult, static_cast<int32_t>(0u < static_cast<uint32_t>(size)), size, g_dvd->resultOut, g_dvd->resultCallback, kTextInfoError);
    }
    READABLE_FUNCTION_WAITS(checkDvdFileInfo, Verified);

    // checkDvdRead: a read's end.
    void checkDvdRead()
    {
        const int32_t bytes = g_dvd->reply;
        game::tail(checkDvdFileResult, static_cast<int32_t>(0u < static_cast<uint32_t>(bytes)), bytes, g_dvd->resultOut, g_dvd->resultCallback, kTextReadError);
    }
    READABLE_FUNCTION_WAITS(checkDvdRead, Draft);

    // clearDvdBusy: 1 if the device was up, which it no longer is.
    uint32_t clearDvdBusy()
    {
        if (g_dvd->initialised == 0)
            return 0;
        g_dvd->initialised = 0;
        return 1;
    }
    READABLE_FUNCTION(clearDvdBusy, Verified);

    // dvdDevice: its name.
    uint32_t dvdDevice() { return kTextDvd0; }
    READABLE_FUNCTION(dvdDevice, Verified);

    // dvdSetAttributes
    uint32_t dvdSetAttributes() { return 0; }
    READABLE_FUNCTION(dvdSetAttributes, Verified);

    // dvdIsFormatted
    uint32_t dvdIsFormatted(uint32_t device, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return 1; }, kTextNone, true);
        return 1;
    }
    READABLE_FUNCTION_WAITS(dvdIsFormatted, Verified);

    // dvdFormat
    uint32_t dvdFormat(uint32_t device, uint32_t callback)
    {
        reportWhenIdle(device, 0, callback, [] { return 0; }, kTextCannotFormat, false);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdFormat, Verified);

    // dvdIsPresent
    uint32_t dvdIsPresent(uint32_t device, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return g_dvd->mounted; }, kTextNoCd, true);
        return 1;
    }
    READABLE_FUNCTION_WAITS(dvdIsPresent, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceCdDiskReady(uint32_t value) { ee::game(kSceCdDiskReady, value); }
        void sceFsReset() { ee::game(kSceFsReset); }
    }

    // dvdMount: the mount: asked of the IOP again; its end sets `mounted`.
    uint32_t dvdMount(uint32_t, uint32_t, uint32_t resultOut, uint32_t callback)
    {
        ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
        rpcEndParameter = 0;
        takeLock();
        giveLock();
        g_dvd->resultCallback = callback;
        g_dvd->resultOut = resultOut;
        const uint32_t mode = resultOut < 1u ? kRpcNoWait : 0;
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnMount, mode, kTextNone, 0, kDvdReply, kReplyBytes, kCheckDvdPresent);
        sceCdDiskReady(0);
        sceFsReset();
        return 1;
    }
    READABLE_FUNCTION_WAITS(dvdMount, Draft);

    // dvdHasFreeSpace
    uint32_t dvdHasFreeSpace(uint32_t device, uint32_t, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return 0; }, kTextNotSupported, true);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdHasFreeSpace, Verified);

    // dvdListDirectory
    uint32_t dvdListDirectory(uint32_t device, uint32_t, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return 0; }, kTextNotSupported, true);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdListDirectory, Verified);

    // dvdMakeDirectory
    uint32_t dvdMakeDirectory(uint32_t device, uint32_t, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return 0; }, kTextNotSupported, true);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdMakeDirectory, Verified);

    // dvdRemoveDirectory
    uint32_t dvdRemoveDirectory(uint32_t device, uint32_t, uint32_t resultOut, uint32_t callback)
    {
        reportWhenIdle(device, resultOut, callback, [] { return 0; }, kTextNotSupported, true);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdRemoveDirectory, Verified);

    // dvdStat: does a path exist: the IOP's answer goes to the end function.
    uint32_t dvdStat(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
        rpcEndParameter = 0;
        takeLock();
        giveLock();
        uint32_t result = 0;
        if (g_dvd->mounted == 0)
        {
            checkDvdResult(0, device, resultOut, callback, kTextNotMounted);
        }
        else
        {
            g_dvd->resultCallback = callback;
            g_dvd->resultOut = resultOut;
            stringCopy(kDvdPath, path);
            stringToUpper(kDvdPath);
            const uint32_t mode = resultOut < 1u ? kRpcNoWait : 0;
            waitForClient();
            callIopRpc(kDvdRpc, kDvdFnStat, mode, kDvdPath, kPathBytes, kDvdReply, kReplyBytes, kCheckDvdAccess);
            result = 1;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(dvdStat, Draft);

    // dvdFindFileAt: both of the device's file look-ups are the stat.
    int32_t dvdFindFileAt(uint32_t device, uint32_t path, uint32_t resultOut, uint32_t callback)
    {
        return static_cast<int32_t>(dvdStat(device, path, resultOut, callback));
    }
    READABLE_FUNCTION_WAITS(dvdFindFileAt, Verified);

    // dvdRemoveFile
    uint32_t dvdRemoveFile(uint32_t device, uint32_t, uint32_t callback)
    {
        reportWhenIdle(device, 0, callback, [] { return 0; }, kTextNotSupported, false);
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdRemoveFile, Verified);

    // dvdOpen: read-only; asking otherwise reports the failure, and opens
    // anyway. Without a callback the lock stays for the wait.
    uint32_t dvdOpen(uint32_t file, uint32_t path, uint32_t mode, uint32_t callback)
    {
        ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
        rpcEndParameter = 0;
        takeLock();
        giveLock();
        if (g_dvd->mounted == 0)
        {
            checkDvdResult(0, file, 0, callback, kTextNotMounted);
            return 0;
        }
        if (mode != kFileModeRead)
        {
            checkDvdResult(0, file, 0, callback, kTextReadOnly);
        }
        takeLock();
        g_dvd->resultOut = 0;
        g_dvd->resultCallback = callback;
        g_dvd->callbackFile = file;
        stringCopy(kDvdPath, path);
        stringToUpper(kDvdPath);
        g_dvd->openMode = mode;
        g_dvd->bufferFile = 0;
        g_dvd->bufferValid = 0;
        const uint32_t rpcMode = callback != 0 ? kRpcNoWait : 0;
        waitForClient();
        uint32_t endFunction = kCheckDvdOpen;
        if (callback == 0)
            endFunction = 0;
        callIopRpc(kDvdRpc, kDvdFnOpen, rpcMode, kDvdPath, kPathBytes, kDvdReply, kReplyBytes, endFunction);
        uint32_t result = 1;
        if (callback == 0)
        {
            giveLock();
            const int32_t handle = g_dvd->reply;
            ZFile &opened = *ee::Ptr<ZFile>(file);
            if (handle == 0)
            {
                opened.handle = 0;
                result = 0;
            }
            else
            {
                opened.handle = static_cast<uint32_t>(handle);
                opened.device = kDvdDevice;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(dvdOpen, Verified);

    // dvdClose
    int32_t dvdClose(uint32_t file, uint32_t callback)
    {
        ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
        rpcEndParameter = 0;
        takeLock();
        giveLock();
        if (g_dvd->mounted == 0)
        {
            checkDvdResult(0, file, 0, callback, kTextNotMounted);
            return 0;
        }
        const uint32_t rpcMode = callback != 0 ? kRpcNoWait : 0;
        takeLock();
        g_dvd->resultOut = 0;
        g_dvd->resultCallback = callback;
        g_dvd->callbackFile = file;
        g_dvd->requestFile = ee::Ptr<ZFile>(file)->handle;
        waitForClient();
        uint32_t endFunction = kCheckDvdAccess;
        if (callback == 0)
            endFunction = 0;
        callIopRpc(kDvdRpc, kDvdFnClose, rpcMode, kDvdRequestFile, sizeof(uint32_t), kDvdReply, kReplyBytes, endFunction);
        int32_t result = 1;
        if (callback == 0)
        {
            giveLock();
            result = static_cast<int32_t>(0u < static_cast<uint32_t>(g_dvd->reply));
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(dvdClose, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
        void waitSema(uint32_t readSema) { ee::game(kWaitSema, readSema); }
    }

    // dvdRead: a read, from the IOP's position in the file, clipped to its
    // end. If the buffer holds all of it, a copy. What does not fit the
    // buffer goes straight to the destination, after what the buffer has of
    // its start; it fits: the buffer is loaded with its sectors first, the
    // start it already has copied off. Returns the bytes, -1 when a callback
    // will have them, 0 when the disc has gone.
    int32_t dvdRead(uint32_t file, uint32_t destination, uint32_t size, uint32_t callback)
    {
        ee::Local<DvdCallLocals> locals;
        locals->rpcEndParameter = 0;
        uint32_t &wholeInBuffer = locals->wholeInBuffer;
        FileReport &report = locals->report;
        const auto handle = [&] { return ee::Ptr<ZFile>(file)->handle; };

        // Where the read stands: the file position, the destination, the
        // bytes left and what has been copied from the buffer.
        uint32_t position = 0;
        uint32_t dst = destination;
        uint32_t left = size;
        uint32_t copied = 0;

        wholeInBuffer = 0;
        takeLock();
        flushCache(0);
        g_dvd->requestFile = handle();
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnTell, 0u, kDvdRequestFile, sizeof(uint32_t), kDvdReply, kReplyBytes, 0u);

        position = static_cast<uint32_t>(g_dvd->reply);
        const uint32_t fileSize = static_cast<uint32_t>(g_dvd->fileSize);
        uint32_t sectorStart = position >> 11;
        const uint32_t alignedStart = sectorStart << 11;
        left = fileSize < position + size ? fileSize - position : size;
        const uint32_t alignedBytes = (left + (position - alignedStart) + kSectorBytes - 1) >> 11 << 11;
        const bool fitsBuffer = !(kDvdBufferBytes < alignedBytes);

        const auto bufferHolds = [&] { return g_dvd->bufferFile == handle(); };
        if (g_dvd->bufferValid != 1 || bufferHolds())
        {
            if (g_dvd->bufferValid == 1)
            {
                const uint32_t bufferStart = g_dvd->bufferPosition;
                wholeInBuffer = 0;
                if (alignedStart >= bufferStart)
                    wholeInBuffer = bufferStart + kDvdBufferBytes < alignedStart + alignedBytes ? 0 : 1;
            }
        }

        const auto notMounted = [&]
        {
            checkDvdResult(0, file, 0, callback, kTextNotMounted);
            return 0;
        };

        if (wholeInBuffer == 0)
        {
            if (!fitsBuffer)
            {
                const uint32_t bufferStart = g_dvd->bufferPosition;
                const uint32_t bufferEnd = bufferStart + kDvdBufferBytes;
                uint32_t viaBuffer = 0;
                if (callback == 0)
                {
                    if (g_dvd->bufferValid != 0 && bufferHolds() && alignedStart >= bufferStart && position < bufferEnd)
                    {
                        copied = bufferEnd - position;
                        const uint32_t offset = position - bufferStart;
                        if (copied < left)
                        {
                            position += copied;
                            memoryCopy(dst, offset + kDvdBuffer, copied);
                            dst += copied;
                            left -= copied;
                            flushCache(0);
                            g_dvd->seekOffset = bufferEnd;
                            g_dvd->seekOrigin = 0;
                            g_dvd->seekFile = handle();
                            waitForClient();
                            callIopRpc(kDvdRpc, kDvdFnSeek, 0u, kDvdSeekRequest, 12u, 0u, 0u, 0u);
                        }
                    }
                }
                if ((dst & 0xFu) != 0 || (left & 0xFu) != 0 || (position & 0xFu) != 0)
                    viaBuffer = 1;
                if (g_dvd->mounted == 0)
                    return notMounted();
                g_dvd->resultOut = 0;
                g_dvd->resultCallback = callback;
                g_dvd->callbackFile = file;
                DvdReadRequest &request = g_dvd->read;
                request.destination = dst;
                request.file = handle();
                request.bytes = left;
                request.viaBuffer = viaBuffer;
                if (viaBuffer != 0)
                {
                    g_dvd->bufferValid = 0;
                    request.buffer = kDvdBuffer;
                    g_dvd->bufferFile = 0;
                }
                if (callback == 0)
                {
                    request.mode = kReadSignalsSema;
                    request.notify = g_dvd->readSema;
                }
                else
                {
                    request.notify = callback;
                    request.mode = kReadReportsCallback;
                }
                waitForClient();
                callIopRpc(kDvdRpc, kDvdFnRead, 0u, kDvdReadRequest, static_cast<uint32_t>(sizeof(DvdReadRequest)), kDvdReply, kReplyBytes, 0u);
                if (callback == 0)
                {
                    waitSema(g_dvd->readSema);
                    giveLock();
                    return g_dvd->reply + static_cast<int32_t>(copied);
                }
            }
            else
            {
                uint32_t bufferStart = sectorStart << 11;
                if (callback == 0 && g_dvd->bufferValid != 0 && bufferHolds())
                {
                    const uint32_t heldStart = g_dvd->bufferPosition;
                    const uint32_t heldEnd = heldStart + kDvdBufferBytes;
                    if (alignedStart >= heldStart && position < heldEnd)
                    {
                        if (!(heldEnd >= alignedStart + alignedBytes))
                        {
                            copied = heldEnd - position;
                            const uint32_t offset = position - heldStart;
                            if (copied < left)
                            {
                                position += copied;
                                memoryCopy(dst, offset + kDvdBuffer, copied);
                                dst += copied;
                                left -= copied;
                                sectorStart = position >> 11;
                            }
                        }
                    }
                    bufferStart = sectorStart << 11;
                }
                g_dvd->bufferValid = 0;
                g_dvd->bufferPosition = bufferStart;
                g_dvd->resultOut = 0;
                g_dvd->bufferFile = handle();
                g_dvd->resultCallback = callback;
                g_dvd->callbackFile = file;
                DvdBufferRequest &request = g_dvd->bufferRead;
                request.position = bufferStart;
                request.file = handle();
                request.mode = callback != 0 ? kReadThenCallback : kReadSignalsSema;
                uint32_t notify = callback;
                if (callback == 0)
                    notify = g_dvd->readSema;
                request.notify = notify;
                request.buffer = kDvdBuffer;
                request.end = position + left;
                g_dvd->readBytes = left;
                g_dvd->readPosition = position;
                g_dvd->readDestination = dst;
                waitForClient();
                callIopRpc(kDvdRpc, kDvdFnReadBuffered, 0u, kDvd + offsetof(DvdClient, bufferRead), static_cast<uint32_t>(sizeof(DvdBufferRequest)), 0u, 0u, 0u);
                if (callback != 0)
                    return 0;
                wholeInBuffer = 1;
                waitSema(g_dvd->readSema);
                g_dvd->bufferValid = 1;
            }
        }

        if (wholeInBuffer != 1)
            return -1;

        // The rest out of the buffer, the IOP told where the file is now.
        if (g_dvd->mounted == 0)
            return notMounted();
        const uint32_t fromBuffer = position - g_dvd->bufferPosition;
        memoryCopy(dst, fromBuffer + kDvdBuffer, left);
        flushCache(0);
        g_dvd->seekOrigin = 0;
        g_dvd->seekOffset = position + left;
        g_dvd->seekFile = handle();
        const uint32_t total = left + copied;
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnSeek, 0u, kDvdSeekRequest, 12u, 0u, 0u, 0u);
        giveLock();
        if (callback != 0)
        {
            const FileCallback &record = *ee::Ptr<FileCallback>(callback);
            report.subject = file;
            report.value = left;
            report.succeeded = 1;
            report.context = record.context;
            callFileCallback(record.function, &report);
        }
        return static_cast<int32_t>(total);
    }
    READABLE_FUNCTION_WAITS(dvdRead, Verified);

    // dvdWrite: the disc is not written.
    uint32_t dvdWrite()
    {
        registerFileError(kTextWriteNotImplemented);
        return static_cast<uint32_t>(-1);
    }
    READABLE_FUNCTION(dvdWrite, Draft);

    // dvdSeek: 1 if it went through; a callback is told at once.
    int32_t dvdSeek(uint32_t file, uint32_t offset, uint32_t origin, uint32_t callback)
    {
        ee::Local<DvdCallLocals> locals;
        locals->rpcEndParameter = 0;
        FileReport &report = locals->report;
        if (g_dvd->mounted == 0)
        {
            checkDvdResult(0, file, 0, callback, kTextNotMounted);
            return 0;
        }
        takeLock();
        g_dvd->resultOut = 0;
        g_dvd->resultCallback = callback;
        g_dvd->callbackFile = file;
        g_dvd->seekOrigin = origin;
        g_dvd->seekFile = ee::Ptr<ZFile>(file)->handle;
        g_dvd->seekOffset = offset;
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnSeek, 0u, kDvdSeekRequest, 12u, kDvdReply, kReplyBytes, 0u);
        int32_t result = 1;
        if (callback == 0)
        {
            giveLock();
            result = static_cast<int32_t>(0u < static_cast<uint32_t>(~g_dvd->reply));
        }
        else
        {
            const FileCallback &record = *ee::Ptr<FileCallback>(callback);
            const uint32_t context = record.context, lock = g_dvd->lock;
            report.subject = file;
            report.value = 1;
            report.context = context;
            ++g_dvd->lockCount;
            report.succeeded = 1;
            signalSema(lock, 1, context);
            callFileCallback(record.function, &report);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(dvdSeek, Verified);

    namespace
    {
        // dvdFilePosition and dvdFileSize: the IOP's tell gives both.
        int32_t askFileInfo(uint32_t file, uint32_t out, uint32_t callback, uint32_t endFunction, bool position)
        {
            ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
            rpcEndParameter = 0;
            if (g_dvd->mounted == 0)
            {
                checkDvdResult(0, file, 0, callback, kTextNotMounted);
                return 0;
            }
            const uint32_t rpcMode = callback != 0 ? kRpcNoWait : 0;
            takeLock();
            g_dvd->resultOut = 0;
            g_dvd->resultCallback = callback;
            g_dvd->callbackFile = file;
            g_dvd->requestFile = ee::Ptr<ZFile>(file)->handle;
            waitForClient();
            uint32_t end = endFunction;
            if (callback == 0)
                end = 0;
            callIopRpc(kDvdRpc, kDvdFnTell, rpcMode, kDvdRequestFile, sizeof(uint32_t), kDvdReply, kReplyBytes, end);
            int32_t result = 1;
            if (callback == 0)
            {
                giveLock();
                *ee::Ptr<int32_t>(out) = position ? g_dvd->reply : g_dvd->fileSize;
                result = static_cast<int32_t>(0u < static_cast<uint32_t>(g_dvd->fileSize));
            }
            return result;
        }
    }

    // dvdFilePosition
    int32_t dvdFilePosition(uint32_t file, uint32_t positionOut, uint32_t callback)
    {
        return askFileInfo(file, positionOut, callback, kCheckDvdSeek, true);
    }
    READABLE_FUNCTION_WAITS(dvdFilePosition, Draft);

    // dvdFileSize
    int32_t dvdFileSize(uint32_t file, uint32_t sizeOut, uint32_t callback)
    {
        return askFileInfo(file, sizeOut, callback, kCheckDvdFileInfo, false);
    }
    READABLE_FUNCTION_WAITS(dvdFileSize, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sifInitRpc(uint32_t value) { ee::game(kSifInitRpc, value); }
        int32_t getThreadId() { return ee::game<int32_t>(kGetThreadId); }
        void sifSetRpcQueue(uint32_t dvdServerQueue, uint32_t thread) { ee::game(kSifSetRpcQueue, dvdServerQueue, thread); }
        void sifRegisterRpc(uint32_t serverData, uint32_t command, uint32_t function, uint32_t buffer, uint32_t cancelFunction, uint32_t cancelBuffer, uint32_t queue) { ee::game(kSifRegisterRpc, serverData, command, function, buffer, cancelFunction, cancelBuffer, queue); }
        namespace tail
        {
        int32_t sifRpcLoop(uint32_t dvdServerQueue) { return ee::tailCall<int32_t>(kSifRpcLoop, dvdServerQueue); }
        }
    }

    // dvdCompletionServer: the completion server's thread: it registers, tells the
    // IOP module to start, and serves.
    int32_t dvdCompletionServer()
    {
        ee::Local<uint32_t> rpcEndParameter; // the RPC's ninth argument: on the stack
        rpcEndParameter = 0;
        sifInitRpc(0);
        const int32_t thread = getThreadId();
        sifSetRpcQueue(kDvdServerQueue, thread);
        sifRegisterRpc(kDvdServerData, kCompletionServerId, kDvdIopRequest, kDvdServerBuffer, 0, 0, kDvdServerQueue);
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnStart, 0u, kTextNone, 0u, kDvdReply, kReplyBytes, 0u);
        return tail::sifRpcLoop(kDvdServerQueue);
    }
    READABLE_FUNCTION_WAITS(dvdCompletionServer, Verified);

    // dvdIopRequest: the EE server's function: what the IOP tells when a read is
    // done. Function 0 only. Always 0.
    uint32_t dvdIopRequest(uint32_t function, ee::Ptr<DvdCompletion> completion)
    {
        ee::Local<DvdCallLocals> locals;
        locals->rpcEndParameter = 0;
        FileReport &report = locals->report;
        flushCache(0);
        if (function == 0)
        {
            const uint32_t kind = completion->kind;
            if (kind == kCompletionWoken)
            {
                g_dvd->reply = static_cast<int32_t>(completion->value);
                signalSema(completion->notify);
            }
            else if (kind == kCompletionRead || kind == kCompletionBufferRead)
            {
                if (kind == kCompletionRead)
                {
                    report.succeeded = kind;
                    report.subject = completion->file;
                    report.value = completion->value;
                }
                else
                {
                    memoryCopy(g_dvd->readDestination, g_dvd->readPosition - g_dvd->bufferPosition + kDvdBuffer, g_dvd->readBytes);
                    flushCache(0);
                    g_dvd->seekOrigin = 0;
                    g_dvd->seekFile = ee::Ptr<ZFile>(g_dvd->callbackFile)->handle;
                    g_dvd->seekOffset = g_dvd->readPosition + g_dvd->readBytes;
                    waitForClient();
                    callIopRpc(kDvdRpc, kDvdFnSeek, 0u, kDvdSeekRequest, 12u, 0u, 0u, 0u);
                    report.subject = completion->file;
                    report.succeeded = 1;
                    report.value = g_dvd->readBytes;
                }
                const ee::Ptr<FileCallback> record(completion->notify);
                report.context = record->context;
                ++g_dvd->lockCount;
                signalSema(g_dvd->lock);
                callFileCallback(record->function, &report);
            }
            else if (kind == kCompletionBuffered)
            {
                g_dvd->bufferValid = 0;
                g_dvd->bufferFile = 0;
                memoryCopy(completion->destination, completion->offset + kDvdBuffer, completion->value);
                flushCache(0);
            }
            else
                return 0;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(dvdIopRequest, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t sifBindRpc(uint32_t client, uint32_t id, uint32_t mode) { return ee::game<int32_t>(kSifBindRpc, client, id, mode); }
        int32_t sifCheckStatRpc(uint32_t client) { return ee::game<int32_t>(kSifCheckStatRpc, client); }
        uint32_t createSema(ee::Ptr<SemaParameters> parameters) { return ee::game<uint32_t>(kCreateSema, parameters); }
        uint32_t createThread(ee::Ptr<ThreadParameters> parameters, uint32_t serverStackBytes, uint32_t dvdServerStack, uint32_t globalPointer) { return ee::game<uint32_t>(kCreateThread, parameters, serverStackBytes, dvdServerStack, globalPointer); }
        void startThread(uint32_t thread, uint32_t argument) { ee::game(kStartThread, thread, argument); }
        void changeThreadPriority(uint32_t self, uint32_t clientPriority) { ee::game(kChangeThreadPriority, self, clientPriority); }
    }

    // dvdInit: the device up: bound to the IOP module once it answers,
    // the semaphores, the completion server's thread (which starts the
    // module) at a priority above this one's, then the first mount.
    uint32_t dvdInit()
    {
        ee::Local<DvdInitLocals> locals;
        locals->rpcEndParameter = 0;
        g_dvd->initialised = 1;
        g_dvd->mounted = 0;
        sifInitRpc(0);
        for (;;)
        {
            if (sifBindRpc(kDvdRpc, kRpcServerId, 1) < 0)
            {
                registerFileError(kTextBindFailed);
                return 0;
            }
            for (;;)
            {
                const int32_t busy = sifCheckStatRpc(kDvdRpc);
                if (busy == 0)
                    break;
            }
            if (g_dvd->server != 0)
                break;
            int32_t delay = 0x00010000;
            for (;;)
            {
                if (--delay != -1)
                    continue;
                break;
            }
        }

        // The read semaphore, empty, and the lock, free.
        locals->readSema.maxCount = 1;
        locals->readSema.initialCount = 0;
        g_dvd->readSema = createSema(&locals->readSema);
        locals->lockSema.maxCount = 1;
        locals->lockSema.initialCount = 1;
        g_dvd->lock = createSema(&locals->lockSema);
        locals->server.entry = kDvdCompletionServer;
        locals->server.stack = kDvdServerStack;
        locals->server.stackBytes = kDvdServerStackBytes;
        locals->server.initialPriority = kDvdServerPriority;
        locals->server.globalPointer = kGlobalPointer;
        g_dvd->lockCount = 1;
        const uint32_t thread = createThread(&locals->server, kDvdServerStackBytes, kDvdServerStack, kGlobalPointer);
        g_dvd->serverThread = thread;
        startThread(thread, 0);
        const int32_t self = getThreadId();
        changeThreadPriority(self, kDvdClientPriority);
        g_dvd->resultCallback = 0;
        g_dvd->bufferValid = 0;
        g_dvd->bufferFile = 0;
        g_dvd->bufferPosition = 0;
        g_dvd->resultOut = 0;
        waitForClient();
        callIopRpc(kDvdRpc, kDvdFnMount, 0u, kTextNone, 0u, kDvdReply, kReplyBytes, kCheckDvdPresent);
        sceCdDiskReady(0);
        sceFsReset();
        return 1;
    }
    READABLE_FUNCTION_WAITS(dvdInit, Verified);
}
