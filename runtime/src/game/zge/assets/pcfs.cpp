// The "pcfs" file device (registerFileDevices lists it): files on the disc
// through the IOP's cdrom0: file system (sceOpen and the rest), with a
// 10 KB read-ahead block each. Its methods are a FileDevice table
// (g_fileDeviceMethods, zge/assets/file.h): the ones that would change the
// disc refuse, the checks say yes, and open, close, read, write, seek and
// size work on a PcfsFile; the errors carry the original's rwf* names.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/pcfs.cpp.h"

namespace zge
{
    namespace
    {
        // An open pcfs file (the pool's records, 10260 bytes: dvd_device.cpp
        // makes the pool).
        constexpr uint32_t kReadAheadBytes = 0x2800;
        struct PcfsFile
        {
            int32_t descriptor;          // +0x0000 sceOpen's
            int32_t position;            // +0x0004 where the reader is in the file
            int32_t size;                // +0x0008 the file's size (its end when opened; writes move it on)
            uint8_t readAhead[kReadAheadBytes]; // +0x000C the block read from the disc after the bytes asked for
            uint32_t readAheadOffset;    // +0x280C where `position` is in readAhead; kReadAheadBytes: nothing buffered
            uint32_t _2810;
        };
        static_assert(offsetof(PcfsFile, readAhead) == 0xC && offsetof(PcfsFile, readAheadOffset) == 0x280C &&
                      sizeof(PcfsFile) == 10260);

        // The open files: their count, and the pool the records come from
        // (made with the DVD device's requests, initDvdRequestPool).
        inline ee::Global<int32_t, sym::g_pcfsOpenFiles> g_pcfsOpenFiles;
        inline ee::Global<ee::Ptr<Pool>, sym::g_dvdRequestPool> g_pcfsFilePool;

        // pcfsFileName's answer: the PATH attribute, copied.
        inline ee::Global<char, sym::g_pcfsFileName> g_pcfsFileName;

        // The open mode for FileMode's read and write bits ("rb", "rb", "wb",
        // "r+b") and the IOP's seek origin for FileSeekOrigin's.
        inline ee::Global<ee::Ptr<const char>[4], sym::g_openModeNames> g_openModeNames;
        inline ee::Global<uint32_t[3], sym::g_seekOrigins> g_seekOrigins;

        // The IOP's file calls (sce/ioman: flags as below, the origin 0 start,
        // 1 current, 2 end).
        constexpr uint32_t kReadOnly = 0x1, kWriteOnly = 0x2, kReadWrite = 0x3;
        constexpr uint32_t kCreate = 0x200, kTruncate = 0x400, kNoWait = 0x8000;
        constexpr uint32_t kOpenModeD = 0x20000000; // [unknown] what the mode letter 'd' asks for
        constexpr uint32_t kSeekSet = 0, kSeekEnd = 2;

        int32_t sceOpen(ee::Address path, uint32_t flags) { return ee::game<int32_t>(sym::sceOpen, path, flags); }
        void sceClose(int32_t descriptor) { ee::game(sym::sceClose, descriptor); }
        int32_t sceRead(int32_t descriptor, ee::Address dst, uint32_t bytes) { return ee::game<int32_t>(sym::sceRead, descriptor, dst, bytes); }
        int32_t sceWrite(int32_t descriptor, uint32_t src, uint32_t bytes) { return ee::game<int32_t>(sym::sceWrite, descriptor, src, bytes); }
        int32_t sceLseek(int32_t descriptor, int32_t offset, uint32_t origin) { return ee::game<int32_t>(sym::sceLseek, descriptor, offset, origin); }

        // A FileReport about a file: its subject the ZFile.
        struct FileReportOnFile
        {
            uint32_t context;
            ee::Ptr<ZFile> file;
            uint32_t succeeded;
            uint32_t value;
        };
        static_assert(sizeof(FileReportOnFile) == sizeof(FileReport) && offsetof(FileReportOnFile, file) == offsetof(FileReport, subject) &&
                      offsetof(FileReportOnFile, value) == offsetof(FileReport, value));

        // A file callback takes its report and nothing else.
        void callBack(ee::Ptr<FileCallback> callback, ee::Address report)
        {
            const ee::FunctionPtr<void(ee::Address report)> function{callback->function};
            function(report);
        }

        // A report filled in for the callback. Its value is the caller's to
        // set: an operation with no result leaves that word as it was.
        void fillReport(ee::Ptr<FileReport> report, ee::Ptr<FileCallback> callback, uint32_t device, uint32_t succeeded)
        {
            report->context = callback->context;
            report->subject = device;
            report->succeeded = succeeded;
        }
        void fillReport(ee::Ptr<FileReportOnFile> report, ee::Ptr<FileCallback> callback, ee::Ptr<ZFile> file, uint32_t succeeded)
        {
            report->context = callback->context;
            report->file = file;
            report->succeeded = succeeded;
        }

        // What a check that always holds does: its result word 1, the
        // callback told yes.
        uint32_t sayYes(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
        {
            ee::Local<FileReport> report;
            if (result)
                *result = 1;
            if (callback)
            {
                fillReport(report, callback, device, 1);
                report->value = 1;
                callBack(callback, report);
            }
            return 1;
        }

        uint32_t refuse(uint32_t message)
        {
            registerFileError(message);
            return 0;
        }
    }

    // pcfsDevice: its name.
    uint32_t pcfsDevice()
    {
        return sym::g_textPcfs;
    }
    READABLE_FUNCTION(pcfsDevice, Verified);

    // pcfsFileName: the path the attributes give: one, PATH, whose value is
    // the path (copied into a static buffer). Any other list: an error and 0.
    ee::Ptr<char> pcfsFileName(uint32_t /*device*/, uint32_t count, ee::Ptr<uint32_t> names, ee::Ptr<uint32_t> values)
    {
        if (count != 1 || compareStrings(names[0], sym::g_textPath) != 0)
        {
            registerFileError(sym::g_textInvalidAttributesForRenderWare);
            return ee::Ptr<char>();
        }
        char *name = g_pcfsFileName.get();
        stringCopy(name, values[0]);
        return name;
    }
    READABLE_FUNCTION(pcfsFileName, Verified);

    // pcfsCheckFormatted, pcfsCheckMedium: always.
    uint32_t pcfsCheckFormatted(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(pcfsCheckFormatted, Verified);

    uint32_t pcfsFormat() { return refuse(sym::g_textFunctionNotPermittedOnThis); }
    READABLE_FUNCTION(pcfsFormat, Verified);

    uint32_t pcfsCheckMedium(uint32_t device, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return sayYes(device, result, callback);
    }
    READABLE_FUNCTION(pcfsCheckMedium, Verified);

    // What the disc cannot do.
    uint32_t pcfsMount() { return refuse(sym::g_textFunctionNotPermittedOnThis); }
    READABLE_FUNCTION(pcfsMount, Verified);
    uint32_t pcfsCheckFreeSpace() { return refuse(sym::g_textFunctionNotImplementedForThis); }
    READABLE_FUNCTION(pcfsCheckFreeSpace, Verified);
    uint32_t pcfsFindDirectory() { return refuse(sym::g_textFunctionNotPermittedOnThis); }
    READABLE_FUNCTION(pcfsFindDirectory, Verified);
    uint32_t pcfsMakeDirectory() { return refuse(sym::g_textFunctionNotPermittedOnThis); }
    READABLE_FUNCTION(pcfsMakeDirectory, Verified);
    uint32_t pcfsRemoveDirectory() { return refuse(sym::g_textFunctionNotPermittedOnThis); }
    READABLE_FUNCTION(pcfsRemoveDirectory, Verified);

    uint32_t pcfsFileOpens(ee::Ptr<const char> path);

    // pcfsFindFile: whether the file opens (result word and the report's
    // value); the report itself always succeeds. 1.
    uint32_t pcfsFindFile(uint32_t device, ee::Ptr<const char> path, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReport> report;
        const uint32_t found = pcfsFileOpens(path) != 0 ? 1 : 0;
        if (result)
            *result = found;
        if (callback)
        {
            fillReport(report, callback, device, 1);
            report->value = found;
            callBack(callback, report);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(pcfsFindFile, Verified);

    // pcfsFileExists: the device's fileExists, the same as pcfsFindFile.
    uint32_t pcfsFileExists(uint32_t device, ee::Ptr<const char> path, ee::Ptr<uint32_t> result, ee::Ptr<FileCallback> callback)
    {
        return pcfsFindFile(device, path, result, callback);
    }
    READABLE_FUNCTION_WAITS(pcfsFileExists, Verified);

    uint32_t pcfsDeleteFile() { return refuse(sym::g_textFunctionNotImplementedForThis); }
    READABLE_FUNCTION(pcfsDeleteFile, Verified);

    uint32_t pcfsOpenFile(ee::Address path, ee::Address mode);
    int32_t pcfsCloseFile(ee::Ptr<PcfsFile> file);
    uint32_t pcfsReadFile(uint32_t dst, uint32_t elementSize, uint32_t count, ee::Ptr<PcfsFile> file);
    uint32_t pcfsWriteFile(uint32_t src, uint32_t elementSize, uint32_t count, ee::Ptr<PcfsFile> file);
    int32_t pcfsSeekFile(ee::Ptr<PcfsFile> file, uint64_t offset, uint32_t origin);
    int32_t pcfsFilePosition(ee::Ptr<PcfsFile> file);

    // rwfopen: the file opened in the mode its FileMode bits give; 0 and an
    // error if it does not open.
    uint32_t rwfopen(ee::Ptr<ZFile> file, ee::Ptr<const char> path, uint32_t mode, ee::Ptr<FileCallback> callback)
    {
        ee::Local<char[0x80]> name;
        ee::Local<FileReportOnFile> report;
        stringCopy(name, path);
        const uint32_t handle = pcfsOpenFile(name, (*g_openModeNames)[mode]);
        if (handle == 0)
        {
            if constexpr (game::readable::kIsBmx) // (ZGE4 registers no error for a file that would not open)
                return 0;
            return refuse(sym::g_textRwfopenFailed);
        }
        file->handle = handle;
        if (callback)
        {
            fillReport(report, callback, file, 1);
            callBack(callback, report);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(rwfopen, Verified);

    // rwfclose: the file closed; 0 and an error if that fails.
    uint32_t rwfclose(ee::Ptr<ZFile> file, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReportOnFile> report;
        if (pcfsCloseFile(ee::Ptr<PcfsFile>(file->handle)) != 0)
            return refuse(sym::g_textRwfcloseFailed);
        if (callback)
        {
            fillReport(report, callback, file, 1);
            callBack(callback, report);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(rwfclose, Verified);

    // pcfsRead: up to `bytes` read into dst; the bytes read (all of them:
    // the report succeeds). ZGE2 registers a file error when fewer came.
    uint32_t pcfsRead(ee::Ptr<ZFile> file, uint32_t dst, uint32_t bytes, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReportOnFile> report;
        const uint32_t read = pcfsReadFile(dst, 1, bytes, ee::Ptr<PcfsFile>(file->handle));
        if constexpr (game::readable::kIsDm2)
        {
            if (read != bytes)
                registerFileError(sym::dm2::g_textRwfreadFailed);
        }
        if (callback)
        {
            fillReport(report, callback, file, read == bytes ? 1 : 0);
            report->value = read;
            callBack(callback, report);
        }
        return read;
    }
    READABLE_FUNCTION_WAITS(pcfsRead, Verified);

    // rwfwrite: `bytes` written from src (an error when fewer are); the bytes
    // written.
    uint32_t rwfwrite(ee::Ptr<ZFile> file, uint32_t src, uint32_t bytes, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReportOnFile> report;
        const uint32_t written = pcfsWriteFile(src, 1, bytes, ee::Ptr<PcfsFile>(file->handle));
        if (written != bytes)
            registerFileError(sym::g_textRwfwriteFailed);
        if (callback)
        {
            fillReport(report, callback, file, written == bytes ? 1 : 0);
            report->value = written;
            callBack(callback, report);
        }
        return written;
    }
    READABLE_FUNCTION_WAITS(rwfwrite, Draft);

    // rwfseek: the file's position moved; the report's value the new position
    // only for a seek from the start (else 0). 0 and an error if it fails.
    uint32_t rwfseek(ee::Ptr<ZFile> file, uint32_t offset, uint32_t origin, ee::Ptr<FileCallback> callback)
    {
        ee::Local<FileReportOnFile> report;
        if (pcfsSeekFile(ee::Ptr<PcfsFile>(file->handle), uint64_t{offset}, (*g_seekOrigins)[origin]) != 0)
            return refuse(sym::g_textRwfseekFailed);
        if (callback)
        {
            fillReport(report, callback, file, 1);
            report->value = origin == kSeekFromStart ? offset : 0;
            callBack(callback, report);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(rwfseek, Verified);

    // pcfsPosition: the file's position (no callback).
    uint32_t pcfsPosition(ee::Ptr<ZFile> file, ee::Ptr<int32_t> position)
    {
        *position = pcfsFilePosition(ee::Ptr<PcfsFile>(file->handle));
        return 1;
    }
    READABLE_FUNCTION_WAITS(pcfsPosition, Draft);

    // pcfsSize: the file's size, by a seek to its end and back (which drops
    // its read-ahead block); the callback is not called.
    uint32_t pcfsSize(ee::Ptr<ZFile> file, ee::Ptr<int32_t> size)
    {
        const ee::Ptr<PcfsFile> opened(file->handle);
        const int32_t position = pcfsFilePosition(opened);
        pcfsSeekFile(opened, 0, kSeekEnd);
        *size = pcfsFilePosition(opened);
        pcfsSeekFile(opened, static_cast<uint64_t>(int64_t{position}), kSeekSet);
        return 1;
    }
    READABLE_FUNCTION_WAITS(pcfsSize, Verified);

    // pcfsOpenFlags: sceOpen's flags for an fopen-style mode: '+' read and
    // write, else 'r' read, else 'w' or 'a' write ('b' means nothing here);
    // 'w' creates and truncates, 'a' creates (and does not append), 'n' does
    // not wait. 0 for a mode with none of r, w, a, +.
    uint32_t pcfsOpenFlags(ee::Address mode)
    {
        const bool read = stringFindLast(mode, 'r') != nullptr;
        const bool write = stringFindLast(mode, 'w') != nullptr;
        const bool append = stringFindLast(mode, 'a') != nullptr;
        const bool update = stringFindLast(mode, '+') != nullptr;
        const bool noWait = stringFindLast(mode, 'n') != nullptr;
        const bool letterD = stringFindLast(mode, 'd') != nullptr;
        uint32_t flags;
        if (update)
            flags = kReadWrite;
        else if (read)
            flags = kReadOnly;
        else if (write || append)
            flags = kWriteOnly;
        else
            return 0;
        if (write)
            flags |= kCreate | kTruncate;
        if (append)
            flags |= kCreate;
        if (noWait)
            flags |= kNoWait;
        if (letterD)
            flags |= kOpenModeD;
        return flags;
    }
    READABLE_FUNCTION(pcfsOpenFlags, Verified);

    // pcfsOpen: the handle (its PcfsFile record) of a file on the disc ("cdrom0:\NAME;1": the version added when
    // missing, upper case, '/' turned to '\'), in a record of the pool, at
    // its start (a write-only mode without 'w' stays at its end), nothing
    // read ahead; 0 when it does not open. A mode that makes no flags keeps
    // the record taken (the original's: it is never given back).
    uint32_t pcfsOpen(ee::Address name, ee::Address mode)
    {
        ee::Local<char[0x100]> path;
        formatString(path, sym::g_textCdrom0, name);
        path[0xFD] = 0;
        if (!stringFindLast(path, ';'))
            appendString(path, sym::g_textVersion1);
        stringToUpper(ee::Ptr<char>(path) + 7); // after "cdrom0:"
        path[0xFF] = 0;
        for (ee::Ptr<char> c = path; *c != 0; ++c)
            if (*c == '/')
                *c = '\\';

        const uint32_t handle = poolAlloc(*g_pcfsFilePool);
        if (handle == 0)
            return 0;
        const ee::Ptr<PcfsFile> file(handle);
        const uint32_t flags = pcfsOpenFlags(mode);
        if (flags == 0)
            return 0;
        file->descriptor = sceOpen(path, flags);
        if (file->descriptor >= 0)
        {
            file->size = file->position = sceLseek(file->descriptor, 0, kSeekEnd);
            if (file->position >= 0)
            {
                const bool appendOnly = (flags & (kCreate | kTruncate)) == kCreate;
                if (!appendOnly)
                    file->position = sceLseek(file->descriptor, 0, kSeekSet);
                if (appendOnly || file->position >= 0)
                {
                    file->readAheadOffset = kReadAheadBytes;
                    *g_pcfsOpenFiles += 1;
                    return handle;
                }
            }
            sceClose(file->descriptor);
        }
        poolFree(*g_pcfsFilePool, handle);
        return 0;
    }
    READABLE_FUNCTION_WAITS(pcfsOpen, Verified);

    // pcfsOpenFile: pcfsOpen.
    uint32_t pcfsOpenFile(ee::Address path, ee::Address mode)
    {
        return pcfsOpen(path, mode);
    }
    READABLE_FUNCTION_WAITS(pcfsOpenFile, Verified);

    // pcfsCloseFile: the file closed and its record given back; 0, or -1 for
    // none (or with no file open).
    int32_t pcfsCloseFile(ee::Ptr<PcfsFile> file)
    {
        if (!file || *g_pcfsOpenFiles == 0)
            return -1;
        sceClose(file->descriptor);
        poolFree(*g_pcfsFilePool, file);
        *g_pcfsOpenFiles -= 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(pcfsCloseFile, Verified);

    // pcfsFileOpens: whether the file opens for reading (it is closed again).
    uint32_t pcfsFileOpens(ee::Ptr<const char> path)
    {
        const uint32_t handle = pcfsOpenFile(path, ee::Ptr<const char>(sym::g_textReadMode));
        if (handle == 0)
            return 0;
        pcfsCloseFile(ee::Ptr<PcfsFile>(handle));
        return 1;
    }
    READABLE_FUNCTION_WAITS(pcfsFileOpens, Verified);

    // pcfsReadFile: fread: count elements of elementSize into dst, no further
    // than the file's end; the elements read. What the read-ahead block holds
    // comes from it first; the rest is read from the disc, straight into dst
    // when it is a block or more (the next block then read ahead), else by
    // way of a new read-ahead block. A failed direct read counts as none.
    uint32_t pcfsReadFile(uint32_t dst, uint32_t elementSize, uint32_t count, ee::Ptr<PcfsFile> file)
    {
        int32_t wanted = static_cast<int32_t>(elementSize * count);
        if (file->size < file->position + wanted)
            wanted = file->size - file->position;
        int32_t copied = 0;
        if (file->readAheadOffset < kReadAheadBytes && wanted > 0)
        {
            const uint32_t buffered = kReadAheadBytes - file->readAheadOffset;
            copied = static_cast<int32_t>(static_cast<uint32_t>(wanted) < buffered ? static_cast<uint32_t>(wanted) : buffered);
            memoryCopy(dst, ee::Ptr<uint8_t>(file->readAhead) + file->readAheadOffset, copied);
            dst += copied;
            file->readAheadOffset += copied;
            file->position += copied;
        }
        int32_t rest = wanted - copied;
        if (rest != 0)
        {
            const ee::Ptr<uint8_t> readAhead(file->readAhead);
            if (static_cast<uint32_t>(rest) >= kReadAheadBytes)
            {
                const int32_t read = sceRead(file->descriptor, dst, rest);
                rest = read >= 0 ? read : 0;
                sceRead(file->descriptor, readAhead, kReadAheadBytes);
                file->readAheadOffset = 0;
            }
            else
            {
                sceRead(file->descriptor, readAhead, kReadAheadBytes);
                memoryCopy(dst, readAhead, rest);
                file->readAheadOffset = rest;
            }
            copied += rest;
            file->position += rest;
        }
        // (A zero elementSize traps in the original; the game never passes one.)
        return elementSize != 0 ? static_cast<uint32_t>(copied) / elementSize : 0;
    }
    READABLE_FUNCTION_WAITS(pcfsReadFile, Verified);

    // pcfsWriteFile: fwrite: count elements of elementSize from src straight
    // to the disc file (the read-ahead block is left as it is); the elements
    // written, 0 if the write fails.
    uint32_t pcfsWriteFile(uint32_t src, uint32_t elementSize, uint32_t count, ee::Ptr<PcfsFile> file)
    {
        const int32_t written = sceWrite(file->descriptor, src, elementSize * count);
        if (written == -1)
            return 0;
        file->position += written;
        if (file->size < file->position)
            file->size = file->position;
        // (A zero elementSize returns 0 here; the original traps after that test.)
        return elementSize != 0 ? static_cast<uint32_t>(written) / elementSize : 0;
    }
    READABLE_FUNCTION_WAITS(pcfsWriteFile, Draft);

    // pcfsSeekFile: fseek; 0, or -1 (an origin it does not know, or a seek the
    // disc refuses: the position is then kept). A seek from the current
    // position that lands in the read-ahead block moves in it; any other drops
    // the block and seeks on the disc. The offset is 64 bits: a backward
    // seek its callers pass as a 32-bit word does not land in the block.
    int32_t pcfsSeekFile(ee::Ptr<PcfsFile> file, uint64_t offset, uint32_t origin)
    {
        const int32_t position = file->position;
        const uint32_t offsetInBlock = file->readAheadOffset;
        file->readAheadOffset = kReadAheadBytes;
        const int32_t blockStart = position - static_cast<int32_t>(offsetInBlock);
        const bool nothingBuffered = offsetInBlock == kReadAheadBytes;
        const int32_t offset32 = static_cast<int32_t>(offset);
        switch (origin)
        {
        case kSeekFromCurrent:
        {
            const int64_t target = int64_t{position} + static_cast<int64_t>(offset);
            if (target >= blockStart && target < int64_t{blockStart + static_cast<int32_t>(kReadAheadBytes)})
            {
                file->position = position + offset32;
                file->readAheadOffset = static_cast<uint32_t>(file->position - blockStart);
            }
            else
                file->position = sceLseek(file->descriptor, position + offset32, kSeekSet);
            break;
        }
        case kSeekFromStart:
            file->position = sceLseek(file->descriptor, offset32, kSeekSet);
            break;
        case kSeekFromEnd:
            file->position = sceLseek(file->descriptor, offset32, kSeekEnd);
            break;
        default:
            return -1;
        }
        if (nothingBuffered)
            file->readAheadOffset = kReadAheadBytes;
        if (file->position == -1)
        {
            file->position = position;
            file->readAheadOffset = kReadAheadBytes;
            return -1;
        }
        return 0;
    }
    READABLE_FUNCTION(pcfsSeekFile, Verified);

    // pcfsFilePosition: ftell.
    int32_t pcfsFilePosition(ee::Ptr<PcfsFile> file)
    {
        return file->position;
    }
    READABLE_FUNCTION(pcfsFilePosition, Verified);
}
