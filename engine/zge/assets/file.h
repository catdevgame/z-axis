#pragma once

// The engine's files (zfile.c, cancelFileRequests-openMemoryFile and the two functions after
// it; zge/assets/file.cpp). A path names its
// device by its first four characters ("dvd0:\ASSETS\LEVELS\TUT\TUT.TXR",
// "mc00:\ZAxis\globals.dat", "rd00:..." a file in memory); a path without
// one goes to the default device. Each device is a table of game functions
// (FileDevice): the platform's "dvd0", "pcfs" and "mc00" (registerFileDevices
// lists them) and the RAM disk "rd00" (ramDiskDevice). Every operation takes a
// callback last (a FileCallback, or 0): with one, and the request queue on,
// the operation is queued and the request task does it later and calls back
// with a FileReport; without one the device does it at once.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/file.h.h"

namespace zge { struct Task; } // (zge/platform/tasks.h)

namespace zge
{
    // An operation's callback: function(FileReport *report), the report's
    // context set to this context.
    struct FileCallback
    {
        uint32_t function; // +0x00
        uint32_t context;  // +0x04
    };

    // What a device tells a callback when it has done an operation.
    struct FileReport
    {
        uint32_t context;   // +0x00 the callback's
        ee::Address subject; // +0x04 the file (a ZFile), or the device (a FileDevice)
        uint32_t succeeded; // +0x08 1: done (a read or write: all that was asked)
        uint32_t value;     // +0x0C the result: bytes read or written, the position, 1
    };
    static_assert(sizeof(FileReport) == 0x10);

    struct ZFile
    {
        uint32_t handle;   // +0x00 the device's handle (0 until opened; "rd00": its MemoryStream)
        uint32_t device;   // +0x04 its FileDevice
        char path[0x80];   // +0x08 as the game opened it ("dvd0:\ASSETS\LEVELS\TUT\TUT.TXR")
        uint32_t position; // +0x88 where the next read or write starts (zfile keeps its own)
    };
    static_assert(offsetof(ZFile, path) == 0x08 && offsetof(ZFile, position) == 0x88 && sizeof(ZFile) == 0x8C);

    // A device's functions. The ones before `open` take the device first, a
    // path (on the device, its name taken off) where they need one, and a
    // result word set to 1 or 0 (the report's `succeeded` says the same).
    // A device without an operation has one that reports "Function not
    // supported on this device" and returns 0.
    struct FileDevice
    {
        uint32_t name;            // +0x00 () -> its four-character name ("dvd0")
        uint32_t fileName;        // +0x04 (device, count, attribute names, values) -> a path made of
                                  //       attributes ("rd00": ADDRESS and SIZE, "rd00:%#10x:%#10x")
        uint32_t checkFormatted;  // +0x08 (device, &result, callback)
        uint32_t format;          // +0x0C (device, callback)
        uint32_t checkMedium;     // +0x10 (device, &result, callback): the card or disc still the one mounted
        uint32_t mount;           // +0x14 (device, "port:slot" ("*u": unmount), &result, callback)
        uint32_t checkFreeSpace;  // +0x18 (device, bytes, &result, callback)
        uint32_t findDirectory;   // +0x1C (device, path, &result, callback)
        uint32_t makeDirectory;   // +0x20 (device, path, &result, callback)
        uint32_t removeDirectory; // +0x24 (device, path, &result, callback)
        uint32_t findFile;        // +0x28 (device, path, &result, callback)
        uint32_t fileExists;      // +0x2C (device, path, &result, callback)
        uint32_t deleteFile;      // +0x30 (device, path, callback)
        uint32_t open;            // +0x34 (file, path, mode, callback) -> 0: failed
        uint32_t close;           // +0x38 (file, callback) -> 0: failed
        uint32_t read;            // +0x3C (file, dst, bytes, callback) -> bytes read
        uint32_t write;           // +0x40 (file, src, bytes, callback) -> bytes written
        uint32_t seek;            // +0x44 (file, offset, FileSeekOrigin, callback)
        uint32_t position;        // +0x48 (file, &position) (no caller: zfile keeps the position itself)
        uint32_t size;            // +0x4C (file, &bytes, callback)
        uint32_t create;          // +0x50 (file, path, mode, attributes, callback) (no device has one)
    };
    static_assert(offsetof(FileDevice, open) == 0x34 && offsetof(FileDevice, size) == 0x4C &&
                  offsetof(FileDevice, create) == 0x50);

    // The operations, as the request queue numbers them (the request task's
    // jump table).
    enum FileOperation : uint32_t
    {
        kFileOpCheckFormatted = 0,
        kFileOpFormat = 1,
        kFileOpCheckMedium = 2,
        kFileOpMount = 3,
        kFileOpCheckFreeSpace = 4,
        kFileOpFindDirectory = 5,
        kFileOpMakeDirectory = 6,
        kFileOpRemoveDirectory = 7,
        kFileOpFindFile = 8,
        kFileOpExists = 9,
        kFileOpDelete = 10,
        kFileOpOpen = 11,
        kFileOpClose = 12,
        kFileOpRead = 13,
        kFileOpWrite = 14,
        kFileOpSeek = 15,
        kFileOpPosition = 16,
        kFileOpSize = 17,
        kFileOpCreate = 18, // (createFile queues its request as kFileOpOpen: the original's)
        kFileOpCount = 19,
    };

    // openFile's mode.
    enum FileMode : uint32_t
    {
        kFileModeRead = 1u << 0,
        kFileModeWrite = 1u << 1,
    };

    enum FileSeekOrigin : uint32_t
    {
        kSeekFromStart = 0,
        kSeekFromCurrent = 1,
        kSeekFromEnd = 2,
    };

    // A queued operation (the request pool's records).
    struct FileRequest
    {
        uint32_t operation;    // +0x00 FileOperation
        uint32_t arguments[4]; // +0x04 the operation's, before its callback
        FileCallback callback; // +0x14 a copy of the caller's (it need not stay)
    };
    static_assert(sizeof(FileRequest) == 0x1C);

    // An operation the request task has started: the device calls `relay`
    // (relayFileReport, its context this record), which keeps the report; the
    // task's next run hands it to `callback`.
    struct FileRequestRelay
    {
        FileCallback relay;    // +0x00
        FileCallback callback; // +0x08 the request's
        FileReport report;     // +0x10
        uint32_t reported;     // +0x20 1: the device has called back
    };
    static_assert(offsetof(FileRequestRelay, report) == 0x10 && sizeof(FileRequestRelay) == 0x24);

    struct FileDeviceList
    {
        uint32_t count;       // +0x00
        uint32_t devices[16]; // +0x04 FileDevice tables
    };

    // zfile's state (g_fileSystemReady).
    struct FileSystem
    {
        uint32_t ready;             // +0x00
        FileDeviceList devices;     // +0x04
        uint32_t defaultDevice;     // +0x48 a path without a device's name goes to it
        char lastError[0x40];       // +0x4C registerFileError's text
        uint32_t filePool;          // +0x8C ZFile records (8 a block; freed: closeFileRecord)
        ee::Ptr<Task> requestTask;  // +0x90 the task running runFileRequests (made on the first queued request)
        uint32_t relayPool;         // +0x94 FileRequestRelay records
        uint32_t requestPool;       // +0x98 FileRequest records
    };
    static_assert(offsetof(FileSystem, defaultDevice) == 0x48 && offsetof(FileSystem, lastError) == 0x4C &&
                  offsetof(FileSystem, filePool) == 0x8C && offsetof(FileSystem, requestPool) == 0x98);

    constexpr uint32_t kFileSystemAddress = sym::g_fileSystemReady;
    inline ee::Global<FileSystem, kFileSystemAddress> g_fileSystem;

    // What openFile gives back. Opened at once (no callback, or the request
    // queue off): the file, or none when the device could not open it
    // ("Error opening file."). Queued (a callback given, the queue on): only
    // that it was queued; the request task opens the file later and the
    // callback's report carries it.
    struct OpenFileResult
    {
        static constexpr uint32_t kQueued = 1;

        uint32_t value; // 0: not opened, kQueued, else the file

        OpenFileResult() : value(0) {} // not opened
        OpenFileResult(ee::Ptr<ZFile> opened) : value(opened.address) {}
        explicit OpenFileResult(uint32_t word) : value(word) {}
        static OpenFileResult queuedRequest() { return OpenFileResult(kQueued); }

        bool queued() const { return value == kQueued; }
        // The file opened at once; none if it failed or was queued (a call
        // without a callback is never queued).
        ee::Ptr<ZFile> file() const { return ee::Ptr<ZFile>(queued() ? 0u : value); }
        // The word itself, for the sweep's drafts, which hold call results as words.
        explicit operator uint32_t() const { return value; }
        explicit operator int32_t() const { return static_cast<int32_t>(value); }
    };
    static_assert(sizeof(OpenFileResult) == 4);
}

namespace ee
{
    // openFile's result travels as the word it is.
    template <>
    struct Abi<zge::OpenFileResult, void>
    {
        static constexpr bool isFloat = false;
        static zge::OpenFileResult fromReg(uint64_t r) { return zge::OpenFileResult(static_cast<uint32_t>(r)); }
        static uint64_t toReg(zge::OpenFileResult result) { return Abi<uint32_t>::toReg(result.value); }
    };
}

namespace zge
{
    // fileRead: `bytes` read into dst from the file's position.
    int32_t fileRead(ee::Ptr<ZFile> file, ee::Address dst, uint32_t bytes, uint32_t callback);
    // openFile: a file opened (FileMode); closeFile: one closed.
    OpenFileResult openFile(ee::Address path, uint32_t mode, uint32_t callback);
    // openMemoryFile: `bytes` of memory opened as a file on the RAM disk, for
    // reading and writing; none if it could not be.
    ee::Ptr<ZFile> openMemoryFile(ee::Address address, uint32_t bytes);
    uint32_t closeFile(ee::Ptr<ZFile> file, uint32_t callback);
}

namespace zge
{
    // (file.cpp)
    uint32_t registerFileError(ee::Address text);
    uint32_t initFileSystem();
    uint32_t shutDownFileSystem();
    void fileSystemReportHook();
    uint32_t findDevice(ee::Address path);
    ee::Ptr<const char> deviceName(uint32_t device);
    uint32_t checkDeviceFormatted(uint32_t device, ee::Address result, uint32_t callback);
    // checkDeviceMedium, checkDeviceFreeSpace: whether a device has a medium
    // in it, and room for `bytes`: 1 when the check ran, what it found in result.
    uint32_t checkDeviceMedium(uint32_t device, ee::Address result, uint32_t callback);
    uint32_t checkDeviceFreeSpace(uint32_t device, uint32_t bytes, ee::Address result, uint32_t callback);
    void makeDirectory(ee::Address path, ee::Address result, uint32_t callback);
    void deleteFile(ee::Address path, uint32_t callback);
    uint32_t formatDevice(uint32_t device, uint32_t callback);
    uint32_t mountDevice(uint32_t device, ee::Address portSlot, ee::Address result, uint32_t callback);
    void findDirectory(ee::Address path, ee::Address result, uint32_t callback);
    uint32_t findFile(ee::Address path, ee::Address result, uint32_t callback);
    int32_t fileWrite(ee::Ptr<ZFile> file, ee::Address src, uint32_t bytes, uint32_t callback);
    uint32_t fileSeek(ee::Ptr<ZFile> file, uint32_t offset, uint32_t origin, uint32_t callback);
    uint32_t filePosition(ee::Ptr<ZFile> file, ee::Address position, uint32_t callback);
    uint32_t fileSize(ee::Ptr<ZFile> file, ee::Address bytes, uint32_t callback);
    uint32_t loadFile(ee::Address path, ee::Address bytesOut);
    // loadFileBytes: the first `bytes` of a file read into a heap block; 0 if fewer could be read.
    uint32_t loadFileBytes(ee::Address path, uint32_t bytes);
    // fileRequestsInFlight: the requests started and not yet reported (file.cpp; ZGE4's updateZgui counts them).
    int32_t fileRequestsInFlight();
    uint32_t freeLoadedFile(ee::Address data);
    uint32_t readFileLine(ee::Ptr<ZFile> file, ee::Address dst, uint32_t size);
    uint32_t joinPath(ee::Address directory, ee::Address name);
    uint32_t pathDirectory(ee::Address path, ee::Address dst);
    uint32_t pathExtension(ee::Address path, ee::Address dst);
    // pathBaseName: a path's file name without its extension (after its last
    // separator, to its last dot), put in dst; dst, or 0.
    uint32_t pathBaseName(ee::Address path, ee::Address dst);
    uint32_t isMemoryFile(ee::Ptr<ZFile> file);
    uint32_t defaultFileDevice();
    ee::Ptr<const char> filePath(ee::Ptr<ZFile> file);
}

namespace zge
{
    // (dvd_device.cpp)
    void checkDvdResult(uint32_t succeeded, uint32_t subject, uint32_t resultOut, uint32_t callback, uint32_t text);
    void initDvdRequestPool();
    void shutdownDvdRequestPool();
    uint32_t dvdReadError();
    uint32_t clearDvdBusy();
    uint32_t dvdInit();
}

namespace zge
{
    // (file_devices.cpp)
    uint32_t registerFileDevices(ee::Ptr<FileDeviceList> list);
    uint32_t shutDownFileDevices();
}

namespace zge
{
    // (ram_disk.cpp)
    int32_t memoryStreamRead(ee::Ptr<ZFile> file, uint32_t dst, uint32_t bytes, ee::Ptr<FileCallback> callback);
    int32_t initRamDisk();
    uint32_t shutdownRamDisk();
}
