// The engine's files (zfile.c, cancelFileRequests-openMemoryFile, and isMemoryFile and
// filePath after it; zge/assets/file.h lays the records out): the devices,
// the request queue and its task, the file operations, whole-file loads,
// line reads and path pieces. The RAM disk's read (its device's) is at the
// end.
//
// Original bugs kept: registerFileError keeps a text only when one is
// already kept (it tests the buffer the wrong way round, so on the PS2 no
// error is ever kept); createFile queues its request as an open (the
// request task's table has a create case, 18, that nothing queues);
// cancelFileRequests hands the pool a request's link where it wants the
// record; loadFile gives back 0 (and frees what it read) for a file whose
// size is a multiple of 0x800 bytes, the last read returning nothing.

#include "file.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/assets/file.cpp.h"

namespace zge
{

    int32_t queueFileRequest(uint32_t operation, const uint32_t *callbackArgument, ee::Address a, ee::Address b,
                             ee::Address c, ee::Address d);

    namespace
    {
        // The upper half of zfile's state's address and of its strings', which
        // the original keeps in saved registers (the callees save them).
        constexpr uint32_t kDeviceListAddress = kFileSystemAddress + offsetof(FileSystem, devices);
        constexpr uint32_t kLastErrorAddress = kFileSystemAddress + offsetof(FileSystem, lastError);

        // 1: an operation with a callback is queued (off while the request
        // task does the queued ones, so they run at once).
        inline ee::Global<uint32_t, sym::g_queueFileRequests> g_queueFileRequests;
        // The RAM disk device as openMemoryFile last found it (-1 at first).
        inline ee::Global<uint32_t, sym::g_ramDiskDevice> g_ramDiskDevice;
        // takeFileError's copy of the last error.
        constexpr uint32_t kTakenErrorBuffer = sym::takenErrorBuffer;
        // resolveDevicePath's and joinPath's buffers.
        constexpr uint32_t kDevicePathBuffer = sym::devicePathBuffer;
        constexpr uint32_t kJoinedPathBuffer = sym::joinedPathBuffer;

        constexpr uint32_t kRamDiskName = sym::ramDiskName2;   // "rd00"
        constexpr uint32_t kDriveS = sym::driveS;        // "s:"
        constexpr uint32_t kPathRoot = sym::pathRoot;      // "" (the PS2's root: no prefix)
        constexpr uint32_t kDeviceSeparator = sym::deviceSeparator; // ":"
        constexpr uint32_t kAddressAttribute = sym::addressAttribute; // "ADDRESS"
        constexpr uint32_t kSizeAttribute = sym::sizeAttribute;    // "SIZE"
        // "rd00" as a word (isMemoryFile compares a path's first four bytes).
        inline ee::Global<uint32_t, sym::g_ramDiskNameWord> g_ramDiskNameWord;
        constexpr uint32_t kErrorOpening = sym::errorOpening;  // "Error opening file."
        constexpr uint32_t kErrorCreating = sym::errorCreating; // "Error creating file."
        constexpr uint32_t kErrorClosing = sym::errorClosing;  // "Error when trying to close file through device."
        constexpr uint32_t kSourceName = sym::sourceName;    // "../ngengine/ZGE3/zfile.c"

        constexpr uint32_t kRamDiskDevice = sym::ramDiskDevice; // "rd00"'s FileDevice
        constexpr uint32_t kLineFeed = 10;
        constexpr uint32_t kExtensionDot = '.';
        constexpr uint32_t kLoadChunkBytes = 0x800;
        constexpr uint32_t kMaxPathLength = 128;
        constexpr uint32_t kMaxExtensionLength = 4;    // (the dot not counted; under 4 characters)
        constexpr uint32_t kMaxErrorLength = 0x3F;
        constexpr uint32_t kDeviceNameLength = 4;
        constexpr uint32_t kRelaysPerBlock = 0x20;
        constexpr uint32_t kRequestsPerBlock = 0x20;
        constexpr uint32_t kFilesPerBlock = 8;
        // The request task: newTask (flags, function, 0, 1, source
        // file, line) -> a task run every frame.
        constexpr uint32_t kRequestTaskFlags = game::readable::perGame(0x106u, 0x105u, 0x106u);
        constexpr uint32_t kRequestTaskLine = game::readable::perGame(0x192u, 0x18Bu, 0x192u); // ZGE2's zfile.c is older

        // Called through the dispatcher as the original calls them.
        constexpr uint32_t kListPlatformDevices = sym::registerFileDevices; // (&device list) -> 1 ("dvd0", "pcfs", "mc00" added)
        constexpr uint32_t kShutDownFileDevices = sym::shutDownFileDevices;
        constexpr uint32_t kHeapFree = sym::heapFree;     // (block)
        constexpr uint32_t kNewPool = sym::createPool;      // (record bytes, records a block, free callback) -> a pool
        constexpr uint32_t kDestroyPool = sym::destroyPool;  // (pool)
        constexpr uint32_t kPoolAlloc = sym::poolAlloc;    // (pool) -> a record
        constexpr uint32_t kPoolOldest = sym::poolLast;   // (pool) -> the link of its oldest record in use, or 0
        constexpr uint32_t kPoolNewer = sym::poolPrevious;    // (link) -> the next newer record's link, or 0
        constexpr uint32_t kPoolCount = sym::poolCount;    // (pool) -> records in use
        constexpr uint32_t kInitRamDisk = sym::initRamDisk;  // () -> 1 (its streams' pool made)
        constexpr uint32_t kShutDownRamDisk = sym::shutdownRamDisk;
        // This file's.
        constexpr uint32_t kRelayFileReport = sym::relayFileReport;
        constexpr uint32_t kRunFileRequests = sym::runFileRequests;
        constexpr uint32_t kQueueFileRequest = sym::queueFileRequest;
        constexpr uint32_t kResolveDevicePath = sym::resolveDevicePath;
        constexpr uint32_t kCloseFileRecord = sym::closeFileRecord;
        constexpr uint32_t kRegisterFileError = sym::registerFileError;
        constexpr uint32_t kFileDevices = sym::fileDevices;
        constexpr uint32_t kDeviceName = sym::deviceName;
        constexpr uint32_t kDeviceFileName = sym::deviceFileName;
        constexpr uint32_t kFindDevice = sym::findDevice;
        constexpr uint32_t kDefaultFileDevice = sym::defaultFileDevice;
        constexpr uint32_t kSetDefaultFileDevice = sym::setDefaultFileDevice;
        constexpr uint32_t kCheckDeviceFormatted = sym::checkDeviceFormatted;
        constexpr uint32_t kFormatDevice = sym::formatDevice;
        constexpr uint32_t kCheckDeviceMedium = sym::checkDeviceMedium;
        constexpr uint32_t kMountDevice = sym::mountDevice;
        constexpr uint32_t kCheckDeviceFreeSpace = sym::checkDeviceFreeSpace;
        constexpr uint32_t kFindDirectory = sym::findDirectory;
        constexpr uint32_t kMakeDirectory = sym::makeDirectory;
        constexpr uint32_t kRemoveDirectory = sym::removeDirectory;
        constexpr uint32_t kFindFile = sym::findFile;
        constexpr uint32_t kFileExists = sym::fileExists;
        constexpr uint32_t kDeleteFile = sym::deleteFile;
        constexpr uint32_t kOpenFile = sym::openFile;
        constexpr uint32_t kCreateFile = sym::createFile;
        constexpr uint32_t kCloseFile = sym::closeFile;
        constexpr uint32_t kFileWrite = sym::fileWrite;
        constexpr uint32_t kFileSeek = sym::fileSeek;
        constexpr uint32_t kFilePosition = sym::filePosition;
        constexpr uint32_t kFileSize = sym::fileSize;
        constexpr uint32_t kPathSeparator = sym::pathSeparator;
        constexpr uint32_t kFilePath = sym::filePath;

        FileDevice &deviceAt(uint32_t device) { return *ee::at<FileDevice>(device); }
        FileRequest &requestAt(uint32_t request) { return *ee::at<FileRequest>(request); }
        FileRequestRelay &relayAt(uint32_t relay) { return *ee::at<FileRequestRelay>(relay); }
        ee::Ptr<Pool> poolAt(uint32_t pool) { return ee::Ptr<Pool>(pool); }
        int8_t charAt(uint32_t address) { return *ee::Ptr<int8_t>(address); }
        void callFileCallback(uint32_t function, ee::Ptr<FileReport> report) { ee::game(function, report); }

        // A device's method: FileDevice says what each one takes.
        template <typename R = void, typename... A>
        R callDevice(uint32_t method, const A &...arguments) { return ee::game<R>(method, arguments...); }

        // What an operation on a path keeps while it asks the device.
        struct DeviceCallLocals
        {
            uint32_t callback; // the operation's; queueFileRequest takes it by address
            uint32_t device;   // resolveDevicePath's answer
        };

        // openMemoryFile's description of a file to the RAM disk's fileName.
        struct MemoryFileAttributes
        {
            uint32_t names[2];  // +0x00 "ADDRESS", "SIZE"
            uint32_t gap08[2];
            uint32_t values[2]; // +0x10
        };
    }

    // ---- the request queue -------------------------------------------------

    // cancelFileRequests (no caller): the queued reads, writes and seeks of a file
    // taken off the queue; 1 if there were any. (The pool is handed the
    // request's link, not the request: the original's.)
    uint32_t cancelFileRequests(uint32_t file)
    {
        FileSystem &files = *g_fileSystem;
        uint32_t cancelled = 0;
        for (uint32_t link = poolLast(&poolAt(files.requestPool)->used); link != 0;)
        {
            const uint32_t newer = poolPrevious(ee::Ptr<PoolLink>(link));
            const FileRequest &request = *poolRecordAs<FileRequest>(link);
            // A queued read, write or seek of this file. (Freed by its link,
            // where runFileRequests frees a record.)
            if (request.operation >= kFileOpRead && request.operation < kFileOpPosition && request.arguments[0] == file)
            {
                cancelled = 1;
                poolFree(poolAt(files.requestPool), link);
            }
            link = newer;
        }
        return cancelled;
    }
    READABLE_FUNCTION(cancelFileRequests, Verified);

    // fileRequestsInFlight (no caller): the requests started and not yet reported.
    int32_t fileRequestsInFlight()
    {
        return poolCount(poolAt(g_fileSystem->relayPool));
    }
    READABLE_FUNCTION(fileRequestsInFlight, Verified);

    // relayFileReport: a started request's relay: the device's report kept for
    // the request task (the report's context is the relay record).
    uint32_t relayFileReport(ee::Ptr<FileReport> report)
    {
        const uint32_t relayAddress = report->context;
        FileRequestRelay &relay = relayAt(relayAddress);
        std::memcpy(&relay.report, report.get(), sizeof(FileReport));
        relay.reported = 1;
        return relayAddress;
    }
    READABLE_FUNCTION(relayFileReport, Verified);

    uint32_t checkDeviceMedium(uint32_t device, ee::Address result, uint32_t callback);

    uint32_t checkDeviceFreeSpace(uint32_t device, uint32_t bytes, ee::Address result, uint32_t callback);

    void makeDirectory(ee::Address path, ee::Address result, uint32_t callback);

    void removeDirectory(ee::Address path, ee::Address result, uint32_t callback);

    void fileExists(ee::Address path, ee::Address result, uint32_t callback);

    void deleteFile(ee::Address path, uint32_t callback);

    void createFile(ee::Address path, uint32_t mode, uint32_t attributes, uint32_t callback);

    // runFileRequests: the request task's function, every frame while there
    // are requests. The reports the devices have made go to their callbacks;
    // then, with no request in flight, the queued requests are done in order
    // (the queue off, so they run at once), until one that takes time
    // (format, the medium check, mount, the free space check, read, write)
    // has started. With nothing in flight or queued, the task deletes itself.
    namespace
    {
        // A queued operation started, the relay its callback. Whether the
        // next request can start in the same run: a format, a medium check, a
        // mount, a free-space check, a read and a write have the task wait.
        bool startFileRequest(const FileRequest &request, uint32_t relay)
        {
            const uint32_t *arguments = request.arguments;
            switch (request.operation)
            {
            case kFileOpCheckFormatted:
                checkDeviceFormatted(arguments[0], arguments[1], relay);
                return true;
            case kFileOpFormat:
                formatDevice(arguments[0], relay);
                return false;
            case kFileOpCheckMedium:
                checkDeviceMedium(arguments[0], arguments[1], relay);
                return false;
            case kFileOpMount:
                mountDevice(arguments[0], arguments[1], arguments[2], relay);
                return false;
            case kFileOpCheckFreeSpace:
                checkDeviceFreeSpace(arguments[0], arguments[1], arguments[2], relay);
                return false;
            case kFileOpFindDirectory:
                findDirectory(arguments[0], arguments[1], relay);
                return true;
            case kFileOpMakeDirectory:
                makeDirectory(arguments[0], arguments[1], relay);
                return true;
            case kFileOpRemoveDirectory:
                removeDirectory(arguments[0], arguments[1], relay);
                return true;
            case kFileOpFindFile:
                findFile(arguments[0], arguments[1], relay);
                return true;
            case kFileOpExists:
                fileExists(arguments[0], arguments[1], relay);
                return true;
            case kFileOpDelete:
                deleteFile(arguments[0], relay);
                return true;
            case kFileOpOpen:
                openFile(arguments[0], arguments[1], relay);
                return true;
            case kFileOpCreate:
                if constexpr (game::readable::kIsDm2) // (ZGE2 has no create request: past its 18)
                    return false;
                else
                {
                    createFile(arguments[0], arguments[1], arguments[2], relay);
                    return true;
                }
            case kFileOpClose:
                closeFile(ee::Ptr<ZFile>(arguments[0]), relay);
                return true;
            case kFileOpRead:
                fileRead(ee::Ptr<ZFile>(arguments[0]), arguments[1], arguments[2], relay);
                return false;
            case kFileOpWrite:
                fileWrite(ee::Ptr<ZFile>(arguments[0]), arguments[1], arguments[2], relay);
                return false;
            case kFileOpSeek:
                fileSeek(ee::Ptr<ZFile>(arguments[0]), arguments[1], arguments[2], relay);
                return true;
            case kFileOpPosition:
                filePosition(ee::Ptr<ZFile>(arguments[0]), arguments[1], relay);
                return true;
            case kFileOpSize:
                fileSize(ee::Ptr<ZFile>(arguments[0]), arguments[1], relay);
                return true;
            default:
                return false;
            }
        }
    }

    uint32_t runFileRequests()
    {
        FileSystem &files = *g_fileSystem;
        // Reports the devices have given go to their callbacks.
        for (uint32_t link = poolLast(&poolAt(files.relayPool)->used); link != 0;)
        {
            const uint32_t relayAddress = poolRecord(link);
            FileRequestRelay &relay = relayAt(relayAddress);
            if (relay.reported)
            {
                relay.report.context = relay.callback.context;
                callFileCallback(relay.callback.function, &relay.report);
                link = poolPrevious(ee::Ptr<PoolLink>(link));
                poolFree(poolAt(files.relayPool), relayAddress);
            }
            else
                link = poolPrevious(ee::Ptr<PoolLink>(link));
        }
        if (poolCount(poolAt(files.relayPool)) != 0)
            return 1;

        // Nothing under way: start what is queued. The operations run at once
        // while queueing is off.
        *g_queueFileRequests = 0;
        for (uint32_t link = poolLast(&poolAt(files.requestPool)->used); link != 0; link = poolLast(&poolAt(files.requestPool)->used))
        {
            const uint32_t requestAddress = poolRecord(link);
            const uint32_t relayRecord = poolAlloc(poolAt(files.relayPool));
            FileRequestRelay &relay = relayAt(relayRecord);
            const FileRequest &request = requestAt(requestAddress);
            relay.relay.context = relayRecord;
            relay.relay.function = kRelayFileReport;
            relay.callback = request.callback;
            relay.reported = 0;
            const bool runNext = startFileRequest(request, relayRecord);
            poolFree(poolAt(files.requestPool), requestAddress);
            if (!runNext)
                break;
        }
        *g_queueFileRequests = 1;
        if (poolCount(poolAt(files.relayPool)) == 0 && poolCount(poolAt(files.requestPool)) == 0)
        {
            deleteTask(files.requestTask);
            files.requestTask = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(runFileRequests, Verified);

    // queueFileRequest: with the queue on and a callback given (*callbackArgument,
    // the caller's argument), the operation is queued for the request task
    // (made on the first) and 1 returned; else 0: the caller does it now.
    int32_t queueFileRequest(uint32_t operation, const uint32_t *callbackArgument, ee::Address a, ee::Address b,
                             ee::Address c, ee::Address d)
    {
        FileSystem &files = *g_fileSystem;
        if (*callbackArgument == 0 || *g_queueFileRequests == 0)
            return 0;
        if (files.requestTask == 0)
            files.requestTask = newTask(kRequestTaskFlags, kRunFileRequests, 0, 1, kSourceName, kRequestTaskLine);
        FileRequest &request = requestAt(poolAlloc(poolAt(files.requestPool)));
        request.operation = operation;
        request.arguments[0] = a;
        request.arguments[1] = b;
        request.arguments[2] = c;
        request.arguments[3] = d;
        request.callback = *ee::Ptr<FileCallback>(*callbackArgument);
        return 1;
    }
    READABLE_FUNCTION_WAITS(queueFileRequest, Verified);

    // ---- devices -----------------------------------------------------------

    ee::Ptr<const char> deviceName(uint32_t device);

    // resolveDevicePath: a path's device (put in *deviceOut) and the path on it:
    // the device's name and its colon taken off, the root prefix put before
    // (on the PS2 an empty one) unless the device is the RAM disk or the
    // path starts "s:" (zfile's buffer); 0 for a device without a name.
    namespace
    {
        // resolveDevicePath's copy of the path past the device's name (ZGE2 keeps none: no room in its frame).
        struct alignas(16) DevicePathCopy
        {
            uint8_t bytes[0x140];
            static constexpr uint32_t frameBytes() { return game::readable::perGame<uint32_t>(sizeof(bytes), 0u, sizeof(bytes)); }
        };
    }

    uint32_t resolveDevicePath(ee::Address path, ee::Address deviceOut)
    {
        ee::Local<DevicePathCopy> localPath;
        const uint32_t device = findDevice(path);
        *ee::Ptr<uint32_t>(deviceOut) = device;
        const ee::Ptr<const char> name = deviceName(device);
        if (!name)
            return 0;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 hands on the path itself, past the device's name and its colon: no root prefix, no buffer.
            if (compareStringsN(path, name, kDeviceNameLength) == 0 && charAt(path + kDeviceNameLength) == ':')
                return path + kDeviceNameLength + 1;
            return path;
        }
        if (compareStringsN(path, name, kDeviceNameLength) == 0 && charAt(path + kDeviceNameLength) == ':')
            stringCopy(localPath, path + kDeviceNameLength + 1);
        else
            stringCopy(localPath, path);
        *ee::Ptr<uint8_t>(kDevicePathBuffer) = 0;
        if (compareStringsN(name, kRamDiskName, kDeviceNameLength) != 0)
        {
            if (compareStringsN(localPath, kDriveS, 2) != 0)
                appendString(kDevicePathBuffer, kPathRoot);
        }
        appendString(kDevicePathBuffer, localPath);
        return kDevicePathBuffer;
    }
    READABLE_FUNCTION_WAITS(resolveDevicePath, Verified);

    // closeFileRecord: the file pool's free callback: the file closed. 1.
    uint32_t closeFileRecord(ee::Ptr<ZFile> file)
    {
        closeFile(file, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(closeFileRecord, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        uint32_t stringCopy(uint32_t dst, uint32_t src) { return game::tail(zge::stringCopy, dst, src); }
        }
    }

    // registerFileError (zfileRegisterErrorMessage, the other builds' text): the
    // text kept as the last error if it is short enough, but only when an
    // error is already kept (the original's test; see the top).
    uint32_t registerFileError(ee::Address text)
    {
        const int32_t kept = charAt(kLastErrorAddress);
        if (kept != 0)
        {
            const uint32_t fits = static_cast<uint32_t>(stringLength(text)) < kMaxErrorLength ? 1u : 0u;
            if (!fits)
                return fits;
            return tail::stringCopy(kLastErrorAddress, text); // the original's tail call
        }
        return static_cast<uint32_t>(kept);
    }
    READABLE_FUNCTION(registerFileError, Verified);

    void setDefaultFileDevice(uint32_t device);

    // initFileSystem: zfile started: the platform's devices listed and the
    // RAM disk after them, the pools made, the first device the default.
    // 0 if the devices or the file pool could not be had.
    uint32_t initFileSystem()
    {
        FileSystem &files = *g_fileSystem;
        if (registerFileDevices(&files.devices) == 0)
            return 0;
        if (initRamDisk() == 1)
        {
            const uint32_t count = files.devices.count;
            files.devices.devices[count] = kRamDiskDevice;
            files.devices.count = count + 1;
        }
        files.filePool = createPool(sizeof(ZFile), kFilesPerBlock, kCloseFileRecord);
        if (!files.filePool)
            return 0;
        files.relayPool = createPool(sizeof(FileRequestRelay), kRelaysPerBlock, 0);
        const uint32_t requestPool = createPool(sizeof(FileRequest), kRequestsPerBlock, 0);
        const uint32_t firstDevice = files.devices.devices[0];
        files.ready = 1;
        files.requestPool = requestPool;
        files.requestTask = 0;
        setDefaultFileDevice(firstDevice);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initFileSystem, Verified);

    // shutDownFileSystem: zfile stopped (once started): its pools destroyed (the
    // files' closes them), the RAM disk's and the platform's devices shut
    // down, no default device. 1.
    uint32_t shutDownFileSystem()
    {
        FileSystem &files = *g_fileSystem;
        if (files.ready)
        {
            destroyPool(poolAt(files.relayPool));
            destroyPool(poolAt(files.requestPool));
            destroyPool(poolAt(files.filePool));
            shutdownRamDisk();
            shutDownFileDevices();
            files.filePool = 0;
            setDefaultFileDevice(~0u);
            files.ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownFileSystem, Verified);

    // fileDevices: the device list (count, then the devices).
    uint32_t fileDevices()
    {
        return kDeviceListAddress;
    }
    READABLE_FUNCTION(fileDevices, Verified);

    // The game functions findDevice calls, typed. (This block stands for the
    // header the binding tool writes.)
    namespace device_calls
    {
        uint32_t stringLength(ee::Ptr<const char> text) { return static_cast<uint32_t>(zge::stringLength(text)); }
        void stringCopy(ee::Ptr<char> to, ee::Ptr<const char> from) { zge::stringCopy(to, from); }
        void appendString(ee::Ptr<char> to, ee::Ptr<const char> from) { zge::appendString(to, from); }
        int32_t compareStringsN(ee::Ptr<const char> a, ee::Ptr<char> b, uint32_t bytes) { return zge::compareStringsN(a, b, bytes); }
        constexpr ee::Ptr<const char> kDeviceSeparatorText{kDeviceSeparator};
    }

    // Empty; reportEngineObjects.
    void fileSystemReportHook()
    {
    }
    READABLE_FUNCTION(fileSystemReportHook, Verified);

    // deviceName, deviceFileName: a device's name; a path on it made of
    // attributes (count, their names, their values).
    ee::Ptr<const char> deviceName(uint32_t device)
    {
        return callDevice<ee::Ptr<const char>>(deviceAt(device).name, device);
    }
    READABLE_FUNCTION(deviceName, Verified);

    uint32_t deviceFileName(uint32_t device, uint32_t count, ee::Address names, ee::Address values)
    {
        return callDevice<uint32_t>(deviceAt(device).fileName, device, count, names, values);
    }
    READABLE_FUNCTION(deviceFileName, Verified);

    // The device a path names (its first four characters a device's name; the
    // colon after them is not compared), else the default device.
    uint32_t findDevice(ee::Address path)
    {
        using namespace device_calls;
        if (stringLength(path) >= kDeviceNameLength)
        {
            for (uint32_t i = 0; i < g_fileSystem->devices.count; ++i)
            {
                const ee::Ptr<const char> name = deviceName(g_fileSystem->devices.devices[i]);
                if (!name)
                    continue;

                ee::Local<char[16]> prefix; // the device's name and ":"
                stringCopy(prefix, name);
                appendString(prefix, kDeviceSeparatorText);
                if (compareStringsN(path, prefix, kDeviceNameLength) == 0)
                    return g_fileSystem->devices.devices[i];
            }
        }
        return defaultFileDevice();
    }
    READABLE_FUNCTION_WAITS(findDevice, Verified);

    // defaultFileDevice, setDefaultFileDevice: the default device; set.
    uint32_t defaultFileDevice()
    {
        return g_fileSystem->defaultDevice;
    }
    READABLE_FUNCTION(defaultFileDevice, Verified);

    void setDefaultFileDevice(uint32_t device)
    {
        g_fileSystem->defaultDevice = device;
    }
    READABLE_FUNCTION(setDefaultFileDevice, Verified);

    // takeFileError (no caller): the last error, taken (a copy of it, the
    // kept text cleared), or 0.
    uint32_t takeFileError()
    {
        if (charAt(kLastErrorAddress) == 0)
            return 0;
        stringCopy(kTakenErrorBuffer, kLastErrorAddress);
        *ee::Ptr<uint8_t>(kLastErrorAddress) = 0;
        return kTakenErrorBuffer;
    }
    READABLE_FUNCTION(takeFileError, Verified);

    // ---- the device operations ---------------------------------------------
    //
    // Each queues itself when it can (queueFileRequest), else calls its
    // device's function; the callback is the caller's last argument, kept in
    // the frame for the queue to read.

    namespace
    {
        // The call of a path operation's device function: the path resolved
        // (the device it names kept in the locals), then the device's
        // method (device, path on it, `result`, callback).
        template <typename R = void>
        R callPathOperation(ee::Ptr<DeviceCallLocals> call, uint32_t FileDevice::*method, uint32_t path, uint32_t result)
        {
            const uint32_t devicePath = resolveDevicePath(path, &call->device);
            const uint32_t device = call->device;
            return callDevice<R>(deviceAt(device).*method, device, devicePath, result, call->callback);
        }
    }

    // checkDeviceFormatted: whether the device's medium is formatted (*result).
    uint32_t checkDeviceFormatted(uint32_t device, ee::Address result, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpCheckFormatted, callbackSlot, device, result, callback, 0) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(device).checkFormatted, device, result, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(checkDeviceFormatted, Verified);

    // formatDevice: the device's medium formatted.
    uint32_t formatDevice(uint32_t device, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpFormat, callbackSlot, device, callback, 0, 0) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(device).format, device, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(formatDevice, Verified);

    // checkDeviceMedium: whether the device's medium is still the one mounted.
    uint32_t checkDeviceMedium(uint32_t device, ee::Address result, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpCheckMedium, callbackSlot, device, result, callback, 0) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(device).checkMedium, device, result, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(checkDeviceMedium, Verified);

    // mountDevice: the device mounted on a port and slot ("0:0").
    uint32_t mountDevice(uint32_t device, ee::Address portSlot, ee::Address result, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpMount, callbackSlot, device, portSlot, result, callback) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(device).mount, device, portSlot, result, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(mountDevice, Verified);

    // checkDeviceFreeSpace: whether the device has `bytes` free.
    uint32_t checkDeviceFreeSpace(uint32_t device, uint32_t bytes, ee::Address result, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpCheckFreeSpace, callbackSlot, device, bytes, result, callback) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(device).checkFreeSpace, device, bytes, result, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(checkDeviceFreeSpace, Verified);

    // findDirectory .. fileExists: whether a directory is there; one made;
    // one removed; whether a file is found (the device's result returned);
    // whether one is there.
    void findDirectory(ee::Address path, ee::Address result, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpFindDirectory, &call->callback, path, result, callback, 0) == 0)
            callPathOperation(call, &FileDevice::findDirectory, path, result);
    }
    READABLE_FUNCTION_WAITS(findDirectory, Verified);

    void makeDirectory(ee::Address path, ee::Address result, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpMakeDirectory, &call->callback, path, result, callback, 0) == 0)
            callPathOperation(call, &FileDevice::makeDirectory, path, result);
    }
    READABLE_FUNCTION_WAITS(makeDirectory, Verified);

    void removeDirectory(ee::Address path, ee::Address result, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpRemoveDirectory, &call->callback, path, result, callback, 0) == 0)
            callPathOperation(call, &FileDevice::removeDirectory, path, result);
    }
    READABLE_FUNCTION_WAITS(removeDirectory, Verified);

    uint32_t findFile(ee::Address path, ee::Address result, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpFindFile, &call->callback, path, result, callback, 0) == 0)
            return callPathOperation<uint32_t>(call, &FileDevice::findFile, path, result);
        return 1;
    }
    READABLE_FUNCTION_WAITS(findFile, Verified);

    void fileExists(ee::Address path, ee::Address result, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpExists, &call->callback, path, result, callback, 0) == 0)
            callPathOperation(call, &FileDevice::fileExists, path, result);
    }
    READABLE_FUNCTION_WAITS(fileExists, Verified);

    // deleteFile: a file deleted.
    void deleteFile(ee::Address path, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpDelete, &call->callback, path, callback, 0, 0) == 0)
        {
            const uint32_t devicePath = resolveDevicePath(path, &call->device);
            callDevice(deviceAt(call->device).deleteFile, call->device, devicePath, call->callback);
        }
    }
    READABLE_FUNCTION_WAITS(deleteFile, Verified);

    // ---- files -------------------------------------------------------------

    // openFile: a file opened (FileMode): a record from the file pool
    // with the path as given, the device's open on the path on it. No file
    // if the device could not ("Error opening file."); queued with a
    // callback and the request queue on (OpenFileResult).
    namespace
    {
        ee::Ptr<ZFile> newFileRecord(uint32_t path, uint32_t device)
        {
            const ee::Ptr<ZFile> file(poolAlloc(poolAt(g_fileSystem->filePool)));
            file->handle = 0;
            file->position = 0;
            file->device = device;
            stringCopy(file->path, path);
            return file;
        }
    }

    OpenFileResult openFile(ee::Address path, uint32_t mode, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpOpen, &call->callback, path, mode, callback, 0) != 0)
            return OpenFileResult::queuedRequest();
        const uint32_t devicePath = resolveDevicePath(path, &call->device);
        const ee::Ptr<ZFile> file = newFileRecord(path, call->device);
        if (callDevice<uint32_t>(deviceAt(call->device).open, file, devicePath, mode, call->callback) == 0)
        {
            // ZGE4's error names the file ("Error opening file:%s").
            if constexpr (game::readable::kIsBmx)
                registerFileError(formatScratch(sym::bmx::errorOpeningFileNamed, path));
            else
                registerFileError(kErrorOpening);
            poolFree(poolAt(g_fileSystem->filePool), file);
            return {};
        }
        return file;
    }
    READABLE_FUNCTION_WAITS(openFile, Verified);

    // createFile (the request task its only caller): a file created
    // through the device's create ("Error creating file."; no device has
    // one). Its request is queued as an open (the original's).
    void createFile(ee::Address path, uint32_t mode, uint32_t attributes, uint32_t callback)
    {
        ee::Local<DeviceCallLocals> call;
        call->callback = callback;
        if (queueFileRequest(kFileOpOpen, &call->callback, path, mode, attributes, callback) != 0)
            return;
        const uint32_t devicePath = resolveDevicePath(path, &call->device);
        const ee::Ptr<ZFile> file = newFileRecord(path, call->device);
        if (callDevice<uint32_t>(deviceAt(call->device).create, file, devicePath, mode, attributes, call->callback) == 0)
        {
            registerFileError(kErrorCreating);
            poolFree(poolAt(g_fileSystem->filePool), file);
        }
    }
    READABLE_FUNCTION_WAITS(createFile, Draft);

    // closeFile: a file closed through its device and its record given
    // back. 0 if the device could not ("Error when trying to close file
    // through device.", the record kept).
    uint32_t closeFile(ee::Ptr<ZFile> file, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpClose, callbackSlot, file, callback, 0, 0) != 0)
            return 1;
        if (callDevice<uint32_t>(deviceAt(file->device).close, file, *callbackSlot) == 0)
        {
            registerFileError(kErrorClosing);
            return 0;
        }
        poolFree(poolAt(g_fileSystem->filePool), file);
        return 1;
    }
    READABLE_FUNCTION_WAITS(closeFile, Verified);

    // fileRead: `bytes` read into dst through the file's device (0
    // asked: 0); the file's position moves on by what it read. Queued: `bytes`.
    int32_t fileRead(ee::Ptr<ZFile> file, ee::Address dst, uint32_t bytes, uint32_t callback)
    {
        ee::Local<uint32_t> callbackArgument;
        *callbackArgument = callback;
        if (!bytes)
            return 0;
        if (queueFileRequest(kFileOpRead, callbackArgument, file, dst, bytes, callback) != 0)
            return static_cast<int32_t>(bytes);
        const int32_t got = callDevice<int32_t>(deviceAt(file->device).read, file, dst, bytes, *callbackArgument);
        file->position += static_cast<uint32_t>(got);
        return got;
    }
    READABLE_FUNCTION_WAITS(fileRead, Verified);

    // fileWrite: `bytes` written from src through the file's device (0
    // asked: 0); the position moves on by what was asked, before the write.
    // Queued: `bytes`.
    int32_t fileWrite(ee::Ptr<ZFile> file, ee::Address src, uint32_t bytes, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (bytes == 0)
            return 0;
        if (queueFileRequest(kFileOpWrite, callbackSlot, file, src, bytes, callback) != 0)
            return static_cast<int32_t>(bytes);
        const uint32_t write = deviceAt(file->device).write;
        file->position += bytes;
        return callDevice<int32_t>(write, file, src, bytes, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(fileWrite, Verified);

    // fileSeek: the file's position moved (FileSeekOrigin; from the end:
    // the position kept as -1), then the device's seek. Queued: 1.
    uint32_t fileSeek(ee::Ptr<ZFile> file, uint32_t offset, uint32_t origin, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        uint32_t position = 0; // (an origin that is none of the three: 0)
        if (origin == kSeekFromCurrent)
            position = file->position + offset;
        else if (origin == kSeekFromStart)
            position = offset;
        else if (origin == kSeekFromEnd)
            position = ~0u;
        if (queueFileRequest(kFileOpSeek, callbackSlot, file, offset, origin, callback) != 0)
            return 1;
        file->position = position;
        return callDevice<uint32_t>(deviceAt(file->device).seek, file, offset, origin, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(fileSeek, Verified);

    // filePosition: the file's position (zfile's own) put in *position. 1.
    uint32_t filePosition(ee::Ptr<ZFile> file, ee::Address position, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpPosition, callbackSlot, file, position, callback, 0) == 0 && position != 0)
            *ee::Ptr<uint32_t>(position) = file->position;
        return 1;
    }
    READABLE_FUNCTION_WAITS(filePosition, Verified);

    // fileSize: the file's size put in *bytes, by its device. Queued: 1.
    uint32_t fileSize(ee::Ptr<ZFile> file, ee::Address bytes, uint32_t callback)
    {
        ee::Local<uint32_t> callbackSlot;
        *callbackSlot = callback;
        if (queueFileRequest(kFileOpSize, callbackSlot, file, bytes, callback, 0) != 0)
            return 1;
        return callDevice<uint32_t>(deviceAt(file->device).size, file, bytes, *callbackSlot);
    }
    READABLE_FUNCTION_WAITS(fileSize, Verified);

    // fileDeviceName (no caller): the name of a file's device.
    ee::Ptr<const char> fileDeviceName(ee::Ptr<ZFile> file)
    {
        return deviceName(file->device);
    }
    READABLE_FUNCTION_WAITS(fileDeviceName, Verified);

    // loadFile: a whole file read into a heap block, 0x800 bytes at a
    // time, the block grown as it goes and cut to the size at the end (put
    // in *bytesOut, if given); 0 if it could not be read (and for a size
    // that is a multiple of 0x800: see the top). freeLoadedFile frees it.
    uint32_t loadFile(ee::Address path, ee::Address bytesOut)
    {
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
            return 0;
        uint32_t loaded = 0;
        uint32_t data = heapAllocate(kLoadChunkBytes);
        if (data)
        {
            uint32_t dst = data;
            uint32_t bytes = 0;
            for (;;)
            {
                const int32_t got = fileRead(file, dst, kLoadChunkBytes, 0);
                bytes += static_cast<uint32_t>(got);
                if (got == 0)
                    break;
                if (static_cast<uint32_t>(got) == kLoadChunkBytes)
                {
                    // A whole chunk: room for the next.
                    data = reallocate(data, bytes + kLoadChunkBytes);
                    if (!data)
                        break;
                    dst = data + bytes;
                }
                else
                {
                    // The end: the block cut to the size.
                    loaded = reallocate(data, bytes);
                    if (loaded && bytesOut)
                        *ee::Ptr<uint32_t>(bytesOut) = bytes;
                    break;
                }
            }
            if (!loaded)
                heapFree(data);
        }
        closeFile(file, 0);
        return loaded;
    }
    READABLE_FUNCTION_WAITS(loadFile, Verified);

    // loadFileBytes (no caller): the first `bytes` of a file read into a heap
    // block; 0 if fewer could be read.
    uint32_t loadFileBytes(ee::Address path, uint32_t bytes)
    {
        uint32_t loaded = 0;
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
            return loaded;
        const uint32_t data = heapAllocate(bytes);
        if (data)
        {
            if (static_cast<uint32_t>(fileRead(file, data, bytes, 0)) == bytes)
                loaded = data;
            else
                heapFree(data);
        }
        closeFile(file, 0);
        return loaded;
    }
    READABLE_FUNCTION_WAITS(loadFileBytes, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        uint32_t heapFree(uint32_t block) { return ee::tailCall<uint32_t>(kHeapFree, block); }
        }
    }

    // freeLoadedFile: what loadFile gave, freed.
    uint32_t freeLoadedFile(ee::Address data)
    {
        return tail::heapFree(data); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(freeLoadedFile, Verified);

    // readFileLine: a line read into dst a byte at a time (at most size - 1
    // bytes, the line feed kept), then a NUL; dst, or 0 if nothing was read.
    uint32_t readFileLine(ee::Ptr<ZFile> file, ee::Address dst, uint32_t size)
    {
        if (size == 0)
            return 0;
        uint32_t cursor = dst;
        uint32_t left = size - 1;
        int32_t last = 0;
        while (left != 0)
        {
            if (fileRead(ee::Ptr<ZFile>(file), cursor, 1, 0) != 1)
                left = 0;
            else
            {
                last = charAt(cursor);
                --left;
                ++cursor;
            }
            if (static_cast<uint32_t>(last) == kLineFeed)
                break;
        }
        *ee::Ptr<uint8_t>(cursor) = 0;
        if (charAt(dst) == 0)
            return 0;
        return dst;
    }
    READABLE_FUNCTION_WAITS(readFileLine, Verified);

    // ---- paths -------------------------------------------------------------

    // joinPath: a directory and a name joined (zfile's buffer).
    uint32_t joinPath(ee::Address directory, ee::Address name)
    {
        stringCopy(kJoinedPathBuffer, directory);
        appendString(kJoinedPathBuffer, name);
        return kJoinedPathBuffer;
    }
    READABLE_FUNCTION(joinPath, Verified);

    // pathSeparator: the path separator ('\').
    uint32_t pathSeparator()
    {
        return '\\';
    }
    READABLE_FUNCTION(pathSeparator, Verified);

    // pathDirectory: a path's directory, to its last separator (kept), put in
    // dst; dst, or 0 for no path or one of 128 characters or more. (Without
    // a separator: an empty directory.)
    namespace
    {
        // Where the path's last separator is, from `index` back; -1: none.
        int32_t lastSeparator(uint32_t path, int32_t index)
        {
            while (index >= 0 && charAt(path + static_cast<uint32_t>(index)) != static_cast<int32_t>(pathSeparator()))
                --index;
            return index;
        }
    }

    uint32_t pathDirectory(ee::Address path, ee::Address dst)
    {
        if (path == 0)
            return 0;
        const int32_t length = stringLength(path);
        if (length >= static_cast<int32_t>(kMaxPathLength))
            return 0;
        const int32_t separator = lastSeparator(path, length);
        stringCopyN(dst, path, static_cast<uint32_t>(separator + 1));
        *ee::Ptr<uint8_t>(static_cast<uint32_t>(separator) + dst + 1) = 0;
        return dst;
    }
    READABLE_FUNCTION(pathDirectory, Verified);

    // pathExtension: a path's extension (after its last dot), put in dst if
    // shorter than four characters; dst, or 0. (Without a dot: the whole
    // path, if as short.)
    uint32_t pathExtension(ee::Address path, ee::Address dst)
    {
        if (path == 0)
            return 0;
        int32_t index = stringLength(path);
        if (index >= static_cast<int32_t>(kMaxPathLength))
            return 0;
        // What follows the last dot; with no dot, the whole path.
        while (index >= 0 && charAt(path + static_cast<uint32_t>(index)) != static_cast<int8_t>(kExtensionDot))
            --index;
        const uint32_t extension = path + static_cast<uint32_t>(index) + 1;
        if (static_cast<uint32_t>(stringLength(extension)) >= kMaxExtensionLength)
            return 0;
        stringCopy(dst, extension);
        return dst;
    }
    READABLE_FUNCTION(pathExtension, Verified);

    // pathBaseName (no caller): a path's file name without its extension
    // (after its last separator, to its last dot), put in dst; dst, or 0.
    uint32_t pathBaseName(ee::Address path, ee::Address dst)
    {
        if (path == 0)
            return 0;
        const int32_t length = stringLength(path);
        if (length >= static_cast<int32_t>(kMaxPathLength))
            return 0;
        // The name after the last separator, its extension cut off.
        stringCopy(dst, path + static_cast<uint32_t>(lastSeparator(path, length)) + 1);
        for (int32_t index = stringLength(dst); index >= 0; --index)
        {
            const uint32_t at = dst + static_cast<uint32_t>(index);
            if (charAt(at) == static_cast<int8_t>(kExtensionDot))
            {
                *ee::Ptr<uint8_t>(at) = 0;
                break;
            }
        }
        return dst;
    }
    READABLE_FUNCTION(pathBaseName, Verified);

    // openMemoryFile: a file over memory (`bytes` at `address`) opened on the
    // RAM disk (found by its name; its name made of the ADDRESS and SIZE
    // attributes), for reading and writing.
    ee::Ptr<ZFile> openMemoryFile(ee::Address address, uint32_t bytes)
    {
        ee::Local<MemoryFileAttributes> attributes;
        const ee::Ptr<FileDeviceList> devices(fileDevices());
        for (uint32_t i = 0; i < devices->count; ++i)
        {
            if (compareStrings(deviceName(devices->devices[i]), kRamDiskName) == 0)
            {
                *g_ramDiskDevice = devices->devices[i];
                break;
            }
        }
        const uint32_t device = *g_ramDiskDevice;
        attributes->values[0] = address;
        attributes->values[1] = bytes;
        attributes->names[0] = kAddressAttribute;
        attributes->names[1] = kSizeAttribute;
        const uint32_t path = deviceFileName(device, 2, attributes->names, attributes->values);
        // ZGE2 opens a memory file for reading only.
        constexpr uint32_t kMemoryFileMode =
            game::readable::perGame<uint32_t>(kFileModeRead | kFileModeWrite, kFileModeRead, kFileModeRead | kFileModeWrite);
        return openFile(path, kMemoryFileMode, 0).file();
    }
    READABLE_FUNCTION_WAITS(openMemoryFile, Verified);

    // isMemoryFile: whether a file is on the RAM disk (its path starts "rd00",
    // compared as one word).
    uint32_t isMemoryFile(ee::Ptr<ZFile> file)
    {
        const ee::Ptr<const char> path = filePath(file);
        return std::memcmp(path.get(), &*g_ramDiskNameWord, sizeof(uint32_t)) == 0 ? 1u : 0u;
    }
    READABLE_FUNCTION(isMemoryFile, Verified);

    // filePath: a file's path, as it was opened.
    ee::Ptr<const char> filePath(ee::Ptr<ZFile> file)
    {
        return file->path;
    }
    READABLE_FUNCTION(filePath, Verified);
}
