// The RAM disk, device "rd00" (the table at ramDiskDevice, zfile's FileDevice;
// BMX XXX PS2 word for word): a file is a block of EE memory, named by its
// address and size ("rd00:ADDRESS:SIZE", both in hex). Nothing is on a disk, so
// the checks all say yes, and the directory and delete operations refuse.

#include "file.h"
#include "game_address.h"

#include "ee/function_ptr.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/ram_disk.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kDeviceName = sym::g_textRd00;            // "rd00"
        constexpr uint32_t kAttributeAddress = sym::g_textAddress;      // "ADDRESS"
        constexpr uint32_t kAttributeSize = sym::g_textSize;         // "SIZE"
        constexpr uint32_t kFileNameFormat = sym::g_textRd002;        // "rd00:%#10x:%#10x"
        constexpr uint32_t kBadAttributes = sym::badAttributes;         // "Invalid RAM disk file name attributes"
        constexpr uint32_t kNotSupported = sym::g_textFunctionNotSupportedOnThis;          // "Function not supported on this device"
        constexpr uint32_t kNotPermitted = sym::g_textFunctionNotPermittedOnThis2;          // "Function not permitted on this device"
        constexpr uint32_t kBadFileName = sym::g_textInvalidFileName;           // "Invalid file name"
        constexpr uint32_t kBadRamFileName = sym::g_textInvalidFileNameGivenForRam;        // "Invalid file name given for RAM disk"
        constexpr uint32_t kAccessViolation = sym::g_textFileAccessViolation;       // "File access violation"
        constexpr uint32_t kSeekPastEnd = sym::seekPastEnd;           // "Seek past end of file"
        constexpr int32_t kNameSeparator = ':';
        constexpr uint32_t kHexadecimal = 16;

        // A file's stream, the ZFile's handle (a pool record).
        struct MemoryStream
        {
            uint32_t position; // +0x00
            uint32_t size;     // +0x04
            uint32_t mode;     // +0x08 StreamMode (the rest of the word is the record's old contents)
            uint32_t data;     // +0x0C
        };
        static_assert(sizeof(MemoryStream) == 0x10);

        enum StreamMode : uint32_t
        {
            kStreamWritable = 1u << 0,
            kStreamReadable = 1u << 1,
        };

        inline ee::Global<uint32_t, sym::g_ramDiskFilePool> g_ramDiskFilePool;
        // ramDiskFileName's answer ("rd00:" and the two numbers).
        inline ee::Global<char, sym::g_ramDiskFileName> g_ramDiskFileName;

        // A file callback takes its report and nothing else: the game has two,
        // ignoreFileReport and relayFileReport, and the original's other
        // registers hold what it was doing before the call.
        void callFileCallback(uint32_t function, ee::Ptr<FileReport> report)
        {
            const ee::FunctionPtr<void(ee::Ptr<FileReport>)> callback{function};
            callback(report);
        }

        // A file of this device: the handle word every device has
        // (ZFile::handle) holds its stream.
        struct RamDiskFile
        {
            ee::Ptr<MemoryStream> stream; // +0x00 ZFile::handle
        };
        static_assert(offsetof(ZFile, handle) == 0);
        ee::Ptr<RamDiskFile> ramDiskFile(ee::Ptr<ZFile> file) { return file.cast<RamDiskFile>(); }

        // A report filled in for the callback. Its value is the caller's to
        // set: an operation with no result leaves that word as it was.
        void fillReport(ee::Ptr<FileReport> report, ee::Ptr<FileCallback> callback, ee::Address subject, uint32_t succeeded)
        {
            report->context = callback->context;
            report->subject = subject;
            report->succeeded = succeeded;
        }

        // The report of an operation with nothing to do: its result word
        // set, the callback told.
        uint32_t sayYes(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
        {
            ee::Local<FileReport> report;
            if (result)
                *result = 1;
            if (callback)
            {
                fillReport(report, callback, device, 1);
                report->value = 1;
                callFileCallback(callback->function, report);
            }
            return 1;
        }

        uint32_t refuse(uint32_t message)
        {
            registerFileError(message);
            return 0;
        }
    }

    // ramDiskName.
    uint32_t ramDiskName() { return kDeviceName; }
    READABLE_FUNCTION(ramDiskName, Verified);

    // ramDiskFileName: the path of the memory block the two attributes
    // ("ADDRESS", "SIZE") give, in a static buffer. Any other list: an error
    // and 0.
    ee::Ptr<char> ramDiskFileName(uint32_t /*device*/, uint32_t count, uint32_t namesAddress, uint32_t valuesAddress)
    {
        const ee::Ptr<uint32_t> names(namesAddress);
        const ee::Ptr<int32_t> values(valuesAddress);
        if (count != 2u || compareStrings(names[0], kAttributeAddress) != 0 || compareStrings(names[1], kAttributeSize) != 0)
        {
            registerFileError(kBadAttributes);
            return ee::Ptr<char>();
        }
        char *out = g_ramDiskFileName.get();
        formatString(out, kFileNameFormat, values[0], values[1]);
        return out;
    }
    READABLE_FUNCTION(ramDiskFileName, Verified);

    // ramDiskCheckFormatted.
    uint32_t ramDiskCheckFormatted(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(ramDiskCheckFormatted, Verified);

    // ramDiskFormat.
    uint32_t ramDiskFormat() { return refuse(kNotSupported); }
    READABLE_FUNCTION(ramDiskFormat, Verified);

    // ramDiskCheckMedium.
    uint32_t ramDiskCheckMedium(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(ramDiskCheckMedium, Verified);

    // ramDiskMount.
    uint32_t ramDiskMount(uint32_t device, uint32_t /*name*/, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(ramDiskMount, Verified);

    // ramDiskCheckFreeSpace.
    uint32_t ramDiskCheckFreeSpace(uint32_t device, uint32_t /*bytes*/, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(ramDiskCheckFreeSpace, Verified);

    // ramDiskFindDirectory.
    uint32_t ramDiskFindDirectory() { return refuse(kNotPermitted); }
    READABLE_FUNCTION(ramDiskFindDirectory, Verified);

    // ramDiskMakeDirectory.
    uint32_t ramDiskMakeDirectory() { return refuse(kNotPermitted); }
    READABLE_FUNCTION(ramDiskMakeDirectory, Verified);

    // ramDiskRemoveDirectory.
    uint32_t ramDiskRemoveDirectory() { return refuse(kNotPermitted); }
    READABLE_FUNCTION(ramDiskRemoveDirectory, Verified);

    // ramDiskFindFile: every path is a file here.
    uint32_t ramDiskFindFile(uint32_t device, uint32_t /*path*/, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(ramDiskFindFile, Verified);

    // ramDiskFileExists: the same; its arguments go on in their registers.
    uint32_t ramDiskFileExists(uint32_t device, uint32_t path, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return ramDiskFindFile(device, path, result, callback);
    }
    READABLE_FUNCTION_WAITS(ramDiskFileExists, Verified);

    // ramDiskDeleteFile.
    uint32_t ramDiskDeleteFile() { return refuse(kNotSupported); }
    READABLE_FUNCTION(ramDiskDeleteFile, Verified);

    // ramDiskOpen: a file opened on the memory block its path names
    // ("rd00:ADDRESS:SIZE"; the device's name is off by now, so `path` starts
    // at the address). The mode's read and write bits go into the stream the
    // other way round. An address or size that reads as 0 is reported, and
    // the stream is made anyway.
    uint32_t ramDiskOpen(ee::Ptr<ZFile> file, uint32_t path, uint32_t mode, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        // The path is "address:size", both in hexadecimal.
        const uint64_t address = stringToInt(path, kHexadecimal);
        const uint32_t separator = stringFind(path, kNameSeparator);
        if (separator == 0)
        {
            registerFileError(kBadFileName);
            return 0;
        }
        const uint64_t size = stringToInt(separator + 1, kHexadecimal);
        if (size == 0 || address == 0)
            registerFileError(kBadRamFileName);
        const ee::Ptr<MemoryStream> stream(poolAlloc(ee::Ptr<Pool>(*g_ramDiskFilePool)));
        stream->data = static_cast<uint32_t>(address);
        stream->size = static_cast<uint32_t>(size);
        stream->mode = (stream->mode & ~(kStreamReadable | kStreamWritable)) | (mode & kFileModeRead ? kStreamReadable : 0u) |
                       (mode & kFileModeWrite ? kStreamWritable : 0u);
        stream->position = 0;
        ramDiskFile(file)->stream = stream;
        if (callback)
        {
            fillReport(report, callback, file, 1);
            callFileCallback(callback->function, report);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(ramDiskOpen, Verified);

    // ramDiskClose.
    uint32_t ramDiskClose(ee::Ptr<ZFile> file, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<MemoryStream> stream = ramDiskFile(file)->stream;
        stream->position = 0;
        stream->size = 0;
        stream->data = 0;
        poolFree(ee::Ptr<Pool>(*g_ramDiskFilePool), stream);
        if (callback)
        {
            fillReport(report, callback, file, 1);
            callFileCallback(callback->function, report);
        }
        return 1;
    }
    READABLE_FUNCTION(ramDiskClose, Verified);

    // memoryStreamRead: what is left of `bytes` from the stream's position copied
    // to dst, the position moved on, the callback (if any) told; the bytes
    // read. A stream not open for reading: "File access violation" and 0.
    int32_t memoryStreamRead(ee::Ptr<ZFile> file, uint32_t dst, uint32_t bytes, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<MemoryStream> stream = ramDiskFile(file)->stream;
        if (!(stream->mode & kStreamReadable))
        {
            registerFileError(kAccessViolation);
            return 0;
        }
        const uint32_t position = stream->position;
        const uint32_t count = stream->size >= position + bytes ? bytes : stream->size - position;
        if (!count)
            return 0;
        memoryCopy(dst, stream->data + position, count);
        stream->position += count;
        if (callback)
        {
            fillReport(report, callback, file, count == bytes ? 1u : 0u);
            report->value = count;
            callFileCallback(callback->function, report);
        }
        return static_cast<int32_t>(count);
    }
    READABLE_FUNCTION(memoryStreamRead, Verified);

    // ramDiskWrite: the same the other way.
    int32_t ramDiskWrite(ee::Ptr<ZFile> file, uint32_t src, uint32_t bytes, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<MemoryStream> stream = ramDiskFile(file)->stream;
        if (!(stream->mode & kStreamWritable))
        {
            registerFileError(kAccessViolation);
            return 0;
        }
        const uint32_t position = stream->position;
        const uint32_t size = stream->size;
        const uint32_t count = size >= position + bytes ? bytes : size - position;
        if (count == 0)
            return 0;
        const uint32_t data = stream->data;
        memoryCopy(data + position, src, count);
        stream->position += count;
        if (callback)
        {
            const uint32_t function = callback->function;
            fillReport(report, callback, file, count == bytes ? 1u : 0u);
            report->value = count;
            callFileCallback(callback->function, report);
        }
        return static_cast<int32_t>(count);
    }
    READABLE_FUNCTION(ramDiskWrite, Verified);

    // ramDiskSeek. From the end the offset is subtracted, unchecked; an
    // unknown origin leaves the position and reports success.
    uint32_t ramDiskSeek(ee::Ptr<ZFile> file, uint32_t offset, uint32_t origin, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        const ee::Ptr<MemoryStream> stream = ramDiskFile(file)->stream;
        auto seekPastEnd = [] {
            registerFileError(kSeekPastEnd);
            return 0u;
        };
        if (origin == kSeekFromCurrent)
        {
            const uint32_t position = stream->position + offset;
            if (stream->size < position)
                return seekPastEnd();
            stream->position = position;
        }
        else if (origin == kSeekFromStart)
        {
            if (stream->size < offset)
                return seekPastEnd();
            stream->position = offset;
        }
        else if (origin == kSeekFromEnd)
            stream->position = stream->size - offset;
        if (callback)
        {
            fillReport(report, callback, file, 1);
            report->value = stream->position;
            callFileCallback(callback->function, report);
        }
        return 1;
    }
    READABLE_FUNCTION(ramDiskSeek, Verified);

    // ramDiskPosition (FileDevice::position: zfile keeps its own, so no caller).
    uint32_t ramDiskPosition(ee::Ptr<ZFile> file, ee::Ptr<uint32_t> position)
    {
        *position = ramDiskFile(file)->stream->position;
        return 1;
    }
    READABLE_FUNCTION(ramDiskPosition, Draft);

    // ramDiskSize (the callback is not called).
    uint32_t ramDiskSize(ee::Ptr<ZFile> file, ee::Ptr<uint32_t> bytes)
    {
        *bytes = ramDiskFile(file)->stream->size;
        return 1;
    }
    READABLE_FUNCTION(ramDiskSize, Verified);

    // initRamDisk.
    int32_t initRamDisk()
    {
        const uint32_t pool = createPool(0x10, 0x10, 0);
        *g_ramDiskFilePool = pool;
        return pool != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(initRamDisk, Verified);

    // shutdownRamDisk.
    uint32_t shutdownRamDisk()
    {
        destroyPool(ee::Ptr<Pool>(*g_ramDiskFilePool));
        *g_ramDiskFilePool = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownRamDisk, Verified);
}
