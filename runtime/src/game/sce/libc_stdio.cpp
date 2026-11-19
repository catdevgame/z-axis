// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): the stdio/ files
// carry the notice below; printf.c, fiprintf.c, mbtowc_r.c and reent/ carry none.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// Copyright (c) 1990 The Regents of the University of California.
// All rights reserved.
//
// Redistribution and use in source and binary forms are permitted
// provided that the above copyright notice and this paragraph are
// duplicated in all such forms and that any documentation,
// advertising materials, and other materials related to such
// distribution and use acknowledge that the software was developed
// by the University of California, Berkeley.  The name of the
// University may not be used to endorse or promote products derived
// from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

// libc, the part the runtime replaces (config.game.toml's stubs, so generated
// code has a host handler here and no recompiled function): newlib 1.8.2's
// printf family's front ends, fflush, fread and _mbtowc_r
// as the game links them, ported from that source (stdio/printf.c, sprintf.c,
// vsprintf.c, fflush.c, fread.c, stdlib/mbtowc_r.c without MB_CAPABLE) with the
// EE build's sizes (long is 64 bits). memcpy and memset are not here: the EE
// build's carry a Cygnus/SCE quadword step with no source on disk, and the
// runtime's stubs are the game's (not_ported.tsv); the code here calls them.
//
// Those are Draft: with no recompiled function there is nothing for ps2xFnCheck
// to compare them with, and the runtime's handlers stay the game's. Past them is
// the FILE machinery the game does run, from stdio/stdio.c, findfp.c, fwalk.c,
// makebuf.c, wsetup.c, refill.c, ungetc.c and reent/*r.c. The allocator is
// libc_malloc.cpp.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libc.h"

#include <cstdint>
#include "sym/sce/libc_stdio.cpp.h"

namespace sce::libc
{
    namespace
    {
        inline ee::Global<ee::Ptr<Reent>, sym::g_impurePtr> g_impurePtr; // _impure_ptr, _REENT
        inline ee::Global<int32_t, sym::g_errno> g_errno;                // errno, as the system calls leave it

        // A string as a FILE, as sprintf and vsprintf make one: a write-only
        // buffer with no limit, flagged __SWR | __SSTR.
        void openString(File *f, ee::Ptr<uint8_t> str, ee::Ptr<Reent> reent)
        {
            f->position = str;
            f->bufferBase = str;
            f->flags = kCanWrite | kIsString;
            f->writeSpace = 0x7FFFFFFF;
            f->bufferSize = 0x7FFFFFFF;
            f->reent = reent;
        }
        void openString(File *f, ee::Ptr<uint8_t> str) { openString(f, str, *g_impurePtr); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t vfprintfR(ee::Ptr<Reent> data, ee::Ptr<File> fp, uint32_t fmt0, ee::Address ap) { return ee::game<int32_t>(sym::_vfprintf_r, data, fp, fmt0, ap); }
        int32_t vfprintf(ee::Address fp, uint32_t fmt0, ee::Address ap) { return ee::game<int32_t>(sym::vfprintf, fp, fmt0, ap); }
        // memcpy: the runtime's (a stub in config.game.toml).
        void memcpy(ee::Ptr<uint8_t> to, ee::Ptr<uint8_t> from, uint32_t bytes) { ee::game(sym::memcpy, to, from, bytes); }
    }

    // stdio/printf.c: the arguments after the fixed ones go to the stack, the
    // va_list that _vfprintf_r walks (64-bit slots, a register each).
    int32_t _printf_r(ee::Ptr<Reent> reent, uint32_t format, uint64_t word1, uint64_t word2, uint64_t word3, uint64_t word4, uint64_t word5, uint64_t word6)
    {
        ee::Local<uint64_t[6]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5, word6};
        for (int i = 0; i < 6; ++i)
            arguments[i] = values[i];
        return vfprintfR(reent, reent->standardOut, format, arguments);
    }

    int32_t printf(uint32_t format, uint64_t word1, uint64_t word2, uint64_t word3, uint64_t word4, uint64_t word5, uint64_t word6, uint64_t word7)
    {
        ee::Local<uint64_t[7]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5, word6, word7};
        for (int i = 0; i < 7; ++i)
            arguments[i] = values[i];
        const ee::Ptr<Reent> reent = *g_impurePtr;
        const ee::Ptr<File> out = reent->standardOut;
        out->reent = reent; // _REENT in stdout's FILE, as the compiled code leaves it
        return vfprintf(out, format, arguments);
    }

    // stdio/sprintf.c.
    int32_t sprintf(ee::Ptr<uint8_t> str, uint32_t format, uint64_t word1, uint64_t word2, uint64_t word3, uint64_t word4, uint64_t word5, uint64_t word6)
    {
        ee::Local<File> f;
        openString(&*f, str);
        ee::Local<uint64_t[6]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5, word6};
        for (int i = 0; i < 6; ++i)
            arguments[i] = values[i];
        const int32_t written = vfprintf(f, format, arguments);
        *f->position = 0;
        return written;
    }

    // stdio/sprintf.c's _sprintf_r: sprintf on the given _reent. Five argument words come in
    // registers after the three fixed ones.
    int32_t _sprintf_r(ee::Ptr<Reent> reent, ee::Ptr<uint8_t> str, uint32_t format, ee::VarArg word1, ee::VarArg word2, ee::VarArg word3,
                       ee::VarArg word4, ee::VarArg word5)
    {
        ee::Local<File> f;
        openString(&*f, str, reent);
        ee::Local<uint64_t[5]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5};
        for (int i = 0; i < 5; ++i)
            arguments[i] = values[i];
        const int32_t written = vfprintf(f, format, arguments);
        *f->position = 0;
        return written;
    }
    READABLE_FUNCTION_WAITS(_sprintf_r, Verified);

    // stdio/vsprintf.c.
    int32_t vsprintf(ee::Ptr<uint8_t> str, uint32_t format, uint32_t ap)
    {
        ee::Local<File> f;
        openString(&*f, str);
        const int32_t written = vfprintf(f, format, ap);
        *f->position = 0;
        return written;
    }

    // reent/*r.c: the system calls with their errno kept in the _reent. On the EE these go to Sony's
    // stubs in kernel.cpp and the runtime's read and write.
    namespace
    {
        int32_t systemRead(int32_t fd, ee::Ptr<uint8_t> buffer, int32_t count) { return ee::game<int32_t>(sym::read, fd, buffer, count); }
        int32_t systemWrite(int32_t fd, ee::Ptr<uint8_t> buffer, int32_t count) { return ee::game<int32_t>(sym::write, fd, buffer, count); }
        int32_t systemClose(int32_t fd) { return ee::game<int32_t>(sym::_close, fd); }
        int32_t systemSeek(int32_t fd, int32_t offset, int32_t whence) { return ee::game<int32_t>(sym::_lseek, fd, offset, whence); }
        int32_t systemStat(int32_t fd, ee::Ptr<FileStatus> status) { return ee::game<int32_t>(sym::_fstat, fd, status); }

        void keepError(ee::Ptr<Reent> reent, int32_t result)
        {
            if (result == -1 && *g_errno != 0)
                reent->errorNumber = *g_errno;
        }
    }

    int32_t _read_r(ee::Ptr<Reent> reent, int32_t fd, ee::Ptr<uint8_t> buffer, int32_t count)
    {
        *g_errno = 0;
        const int32_t result = systemRead(fd, buffer, count);
        keepError(reent, result);
        return result;
    }
    READABLE_FUNCTION(_read_r, Verified);

    int32_t _write_r(ee::Ptr<Reent> reent, int32_t fd, ee::Ptr<uint8_t> buffer, int32_t count)
    {
        *g_errno = 0;
        const int32_t result = systemWrite(fd, buffer, count);
        keepError(reent, result);
        return result;
    }
    READABLE_FUNCTION(_write_r, Verified);

    int32_t _close_r(ee::Ptr<Reent> reent, int32_t fd)
    {
        *g_errno = 0;
        const int32_t result = systemClose(fd);
        keepError(reent, result);
        return result;
    }
    READABLE_FUNCTION(_close_r, Verified);

    int32_t _lseek_r(ee::Ptr<Reent> reent, int32_t fd, int32_t offset, int32_t whence)
    {
        *g_errno = 0;
        const int32_t result = systemSeek(fd, offset, whence);
        keepError(reent, result);
        return result;
    }
    READABLE_FUNCTION(_lseek_r, Verified);

    int32_t _fstat_r(ee::Ptr<Reent> reent, int32_t fd, ee::Ptr<FileStatus> status)
    {
        *g_errno = 0;
        const int32_t result = systemStat(fd, status);
        keepError(reent, result);
        return result;
    }
    READABLE_FUNCTION(_fstat_r, Verified);

    // stdio/stdio.c: the four functions a FILE on a descriptor starts with.
    int32_t __sread(ee::Ptr<File> fp, ee::Ptr<uint8_t> buffer, int32_t count)
    {
        const int32_t result = _read_r(fp->reent, fp->descriptor, buffer, count);
        if (result >= 0)
            fp->seekOffset += result;
        else
            fp->flags &= ~kOffsetCorrect;
        return result;
    }
    READABLE_FUNCTION(__sread, Verified);

    int32_t __swrite(ee::Ptr<File> fp, ee::Ptr<uint8_t> buffer, int32_t count)
    {
        if ((fp->flags & kAppend) != 0)
            _lseek_r(fp->reent, fp->descriptor, 0, 2); // SEEK_END
        fp->flags &= ~kOffsetCorrect;
        return _write_r(fp->reent, fp->descriptor, buffer, count);
    }
    READABLE_FUNCTION(__swrite, Verified);

    int32_t __sseek(ee::Ptr<File> fp, int32_t offset, int32_t whence)
    {
        const int32_t result = _lseek_r(fp->reent, fp->descriptor, offset, whence);
        if (result == -1)
            fp->flags &= ~kOffsetCorrect;
        else
        {
            fp->flags |= kOffsetCorrect;
            fp->seekOffset = result;
        }
        return result;
    }
    READABLE_FUNCTION(__sseek, Verified);

    int32_t __sclose(ee::Ptr<File> fp)
    {
        return _close_r(fp->reent, fp->descriptor);
    }
    READABLE_FUNCTION(__sclose, Verified);

    // stdio/fwalk.c: calls function on every FILE in use, in every block of them.
    int32_t _fwalk(ee::Ptr<Reent> reent, FileFunction function)
    {
        int32_t result = 0;
        for (ee::Ptr<FileBlock> block = &reent->glue; block; block = block->next)
        {
            ee::Ptr<File> fp = block->files;
            for (int32_t left = block->fileCount - 1; left >= 0; --left, ++fp)
            {
                if (fp->flags != 0)
                    result |= function(fp);
            }
        }
        return result;
    }
    READABLE_FUNCTION(_fwalk, Verified);

    // stdio/findfp.c.
    void std(ee::Ptr<File> fp, int32_t flags, int32_t descriptor, ee::Ptr<Reent> reent)
    {
        fp->position = ee::Ptr<uint8_t>();
        fp->readSpace = 0;
        fp->writeSpace = 0;
        fp->flags = static_cast<int16_t>(flags);
        fp->descriptor = static_cast<int16_t>(descriptor);
        fp->bufferBase = ee::Ptr<uint8_t>();
        fp->bufferSize = 0;
        fp->lineBufferSize = 0;
        fp->cookie = fp;
        fp->readFunction = sym::__sread;
        fp->writeFunction = sym::__swrite;
        fp->seekFunction = sym::__sseek;
        fp->closeFunction = sym::__sclose;
        fp->reent = reent;
    }
    READABLE_FUNCTION(std, Verified);

    void _cleanup_r(ee::Ptr<Reent> reent)
    {
        game::tail(_fwalk, reent, sym::fflush);
    }
    READABLE_FUNCTION(_cleanup_r, Verified);

    void __sinit(ee::Ptr<Reent> reent)
    {
        reent->cleanup = sym::_cleanup_r;
        reent->didInit = 1;
        std(&reent->stdFiles[0], kCanRead, 0, reent);
        std(&reent->stdFiles[1], kCanWrite | kLineBuffered, 1, reent);
        std(&reent->stdFiles[2], kCanWrite | kUnbuffered, 2, reent);
        reent->glue.next = ee::Ptr<FileBlock>();
        reent->glue.fileCount = 3;
        reent->glue.files = &reent->stdFiles[0];
    }
    READABLE_FUNCTION(__sinit, Verified);

    namespace
    {
        constexpr int32_t kOutOfMemory = 12; // ENOMEM
        constexpr int32_t kFilesPerBlock = 4; // NDYNAMIC: FILEs in each block added after the first
    }

    // __sfp (stdio/findfp.c): a free FILE for fopen and its kin, from the blocks of them, adding a
    // block when all are in use. Its flags are 1, which reserves it; the caller sets the real
    // ones. 0 with ENOMEM when no block can be had.
    ee::Ptr<File> __sfp(ee::Ptr<Reent> reent)
    {
        if (!reent->didInit)
            __sinit(reent);
        for (ee::Ptr<FileBlock> block = &reent->glue;; block = block->next)
        {
            ee::Ptr<File> fp = block->files;
            for (int32_t n = block->fileCount; --n >= 0; ++fp)
            {
                if (fp->flags != 0)
                    continue;
                fp->flags = 1;                         // reserve this slot
                fp->position = ee::Ptr<uint8_t>();     // no current pointer
                fp->writeSpace = 0;                    // nothing to read or write
                fp->readSpace = 0;
                fp->bufferBase = ee::Ptr<uint8_t>();   // no buffer
                fp->bufferSize = 0;
                fp->lineBufferSize = 0;                // not line buffered
                fp->descriptor = -1;                   // no file
                fp->ungetBase = ee::Ptr<uint8_t>();    // no ungetc buffer
                fp->ungetSize = 0;
                fp->lineBase = ee::Ptr<uint8_t>();     // no line buffer
                fp->lineSize = 0;
                fp->reent = reent;
                return fp;
            }
            if (!block->next && !(block->next = __sfmoreglue(reent, kFilesPerBlock)))
                break;
        }
        reent->errorNumber = kOutOfMemory;
        return ee::Ptr<File>();
    }
    READABLE_FUNCTION(__sfp, Draft);

    // _cleanup (stdio/findfp.c): _cleanup_r on _REENT, which flushes every file.
    void _cleanup()
    {
        game::tail(_cleanup_r, *g_impurePtr);
    }
    READABLE_FUNCTION(_cleanup, Draft);

    namespace
    {
        // CHECK_INIT (stdio/local.h): a FILE belongs to a _reent, and that one is set up.
        void checkInit(ee::Ptr<File> fp)
        {
            if (!fp->reent)
                fp->reent = *g_impurePtr;
            if (fp->reent->didInit == 0)
                __sinit(fp->reent);
        }

        ee::Ptr<uint8_t> mallocR(ee::Ptr<Reent> reent, uint32_t bytes) { return ee::game<ee::Ptr<uint8_t>>(sym::_malloc_r, reent, bytes); }
        ee::Ptr<uint8_t> reallocR(ee::Ptr<Reent> reent, ee::Ptr<uint8_t> block, uint32_t bytes) { return ee::game<ee::Ptr<uint8_t>>(sym::_realloc_r, reent, block, bytes); }
        void freeR(ee::Ptr<Reent> reent, ee::Ptr<uint8_t> block) { ee::game(sym::_free_r, reent, block); }
        int32_t isatty(int32_t fd) { return ee::game<int32_t>(sym::isatty, fd); }

        // FREEUB (stdio/local.h): lets go of ungetc's buffer, unless it is the one inside the FILE.
        void freeUngetBuffer(ee::Ptr<File> fp)
        {
            if (fp->ungetBase.get() != fp->ungetBytes)
                freeR(fp->reent, fp->ungetBase);
            fp->ungetBase = ee::Ptr<uint8_t>();
        }
    }

    // stdio/makebuf.c: gives a FILE its buffer. The EE's build has no st_blksize, so always BUFSIZ.
    void __smakebuf(ee::Ptr<File> fp)
    {
        const ee::Ptr<uint8_t> ownByte = &fp->getcByte;
        if ((fp->flags & kUnbuffered) != 0)
        {
            fp->bufferBase = ownByte;
            fp->position = ownByte;
            fp->bufferSize = 1;
            return;
        }
        ee::Local<FileStatus> status;
        bool couldBeTty = false;
        if (fp->descriptor < 0 || _fstat_r(fp->reent, fp->descriptor, status) < 0)
            fp->flags |= kNoSeekOptimise;
        else
        {
            couldBeTty = (status->mode & kFileTypeMask) == kCharacterDevice;
            if ((status->mode & kFileTypeMask) == kRegularFile && fp->seekFunction == SeekFunction(sym::__sseek))
            {
                fp->flags |= kSeekOptimise;
                fp->blockSize = kBufferSize;
            }
            else
                fp->flags |= kNoSeekOptimise;
        }
        const ee::Ptr<uint8_t> buffer = mallocR(fp->reent, kBufferSize);
        if (!buffer)
        {
            fp->flags |= kUnbuffered;
            fp->bufferBase = ownByte;
            fp->position = ownByte;
            fp->bufferSize = 1;
        }
        else
        {
            fp->reent->cleanup = sym::_cleanup_r;
            fp->flags |= kBufferMalloced;
            fp->bufferBase = buffer;
            fp->position = buffer;
            fp->bufferSize = kBufferSize;
            if (couldBeTty && isatty(fp->descriptor))
                fp->flags |= kLineBuffered;
        }
    }
    READABLE_FUNCTION(__smakebuf, Verified);

    // stdio/wsetup.c: makes a FILE ready to write. 0, or EOF if it cannot be written.
    int32_t __swsetup(ee::Ptr<File> fp)
    {
        checkInit(fp);
        if ((fp->flags & kCanWrite) == 0)
        {
            if ((fp->flags & kReadWrite) == 0)
                return kEof;
            if ((fp->flags & kCanRead) != 0)
            {
                if (fp->ungetBase)
                    freeUngetBuffer(fp);
                fp->flags &= ~(kCanRead | kAtEof);
                fp->readSpace = 0;
                fp->position = fp->bufferBase;
            }
            fp->flags |= kCanWrite;
        }
        if (!fp->bufferBase)
            __smakebuf(fp);
        if ((fp->flags & kLineBuffered) != 0)
        {
            fp->writeSpace = 0;
            fp->lineBufferSize = -fp->bufferSize;
        }
        else
            fp->writeSpace = (fp->flags & kUnbuffered) != 0 ? 0 : fp->bufferSize;
        return 0;
    }
    READABLE_FUNCTION(__swsetup, Verified);

    // stdio/refill.c's lflush. newlib means to flush only line-buffered files open for writing, but its
    // test is `(flags & (__SLBF | __SWR)) == __SLBF | __SWR`, which C reads as `(... == __SLBF) | __SWR`,
    // always true: so it flushes every file it is given. The bug is the library's and stays.
    int32_t lflush(ee::Ptr<File> fp)
    {
        return fflush(fp);
    }
    READABLE_FUNCTION(lflush, Verified);

    // stdio/refill.c: fills a FILE's read buffer. 0, or EOF at the end of the file or on an error.
    int32_t __srefill(ee::Ptr<File> fp)
    {
        checkInit(fp);
        fp->readSpace = 0;
        if ((fp->flags & kAtEof) != 0)
            return kEof;
        if ((fp->flags & kCanRead) == 0)
        {
            if ((fp->flags & kReadWrite) == 0)
                return kEof;
            if ((fp->flags & kCanWrite) != 0)
            {
                if (fflush(fp) != 0)
                    return kEof;
                fp->flags &= ~kCanWrite;
                fp->writeSpace = 0;
                fp->lineBufferSize = 0;
            }
            fp->flags |= kCanRead;
        }
        else if (fp->ungetBase)
        {
            freeUngetBuffer(fp);
            fp->readSpace = fp->savedReadSpace;
            if (fp->readSpace != 0)
            {
                fp->position = fp->savedPosition;
                return 0;
            }
        }
        if (!fp->bufferBase)
            __smakebuf(fp);
        if ((fp->flags & (kLineBuffered | kUnbuffered)) != 0)
            _fwalk(fp->reent, sym::lflush);
        fp->position = fp->bufferBase;
        fp->readSpace = fp->readFunction(fp->cookie, fp->position, fp->bufferSize);
        fp->flags &= ~kLineModified;
        if (fp->readSpace > 0)
            return 0;
        if (fp->readSpace == 0)
            fp->flags |= kAtEof;
        else
        {
            fp->readSpace = 0;
            fp->flags |= kHadError;
        }
        return kEof;
    }
    READABLE_FUNCTION_WAITS(__srefill, Verified);

    // stdio/ungetc.c: makes room for one more pushed-back byte, at the end of a larger buffer.
    int32_t __submore(ee::Ptr<File> fp)
    {
        if (fp->ungetBase.get() == fp->ungetBytes)
        {
            const ee::Ptr<uint8_t> buffer = mallocR(fp->reent, kBufferSize);
            if (!buffer)
                return kEof;
            fp->ungetBase = buffer;
            fp->ungetSize = kBufferSize;
            const ee::Ptr<uint8_t> bytes = buffer + kBufferSize - sizeof(fp->ungetBytes);
            for (int i = static_cast<int>(sizeof(fp->ungetBytes)) - 1; i >= 0; --i)
                bytes[i] = fp->ungetBytes[i];
            fp->position = bytes;
            return 0;
        }
        const int32_t size = fp->ungetSize;
        const ee::Ptr<uint8_t> bytes = reallocR(fp->reent, fp->ungetBase, static_cast<uint32_t>(size) << 1);
        if (!bytes)
            return kEof;
        memcpy(bytes + size, bytes, static_cast<uint32_t>(size));
        fp->position = bytes + size;
        fp->ungetBase = bytes;
        fp->ungetSize = size << 1;
        return 0;
    }
    READABLE_FUNCTION(__submore, Verified);

    // stdio/ungetc.c.
    int32_t ungetc(int32_t c, ee::Ptr<File> fp)
    {
        if (c == kEof)
            return kEof;
        checkInit(fp);
        fp->flags &= ~kAtEof;
        if ((fp->flags & kCanRead) == 0)
        {
            if ((fp->flags & kReadWrite) == 0)
                return kEof;
            if ((fp->flags & kCanWrite) != 0)
            {
                if (fflush(fp) != 0)
                    return kEof;
                fp->flags &= ~kCanWrite;
                fp->writeSpace = 0;
                fp->lineBufferSize = 0;
            }
            fp->flags |= kCanRead;
        }
        c = static_cast<uint8_t>(c);
        if (fp->ungetBase)
        {
            if (fp->readSpace >= fp->ungetSize && __submore(fp) != 0)
                return kEof;
            fp->position -= 1;
            *fp->position = static_cast<uint8_t>(c);
            fp->readSpace += 1;
            return c;
        }
        if (fp->bufferBase && fp->position.get() > fp->bufferBase.get() && fp->position[-1] == c)
        {
            fp->position -= 1;
            fp->readSpace += 1;
            return c;
        }
        fp->savedReadSpace = fp->readSpace;
        fp->savedPosition = fp->position;
        fp->ungetBase = fp->ungetBytes;
        fp->ungetSize = sizeof(fp->ungetBytes);
        fp->ungetBytes[sizeof(fp->ungetBytes) - 1] = static_cast<uint8_t>(c);
        fp->position = &fp->ungetBytes[sizeof(fp->ungetBytes) - 1];
        fp->readSpace = 1;
        return c;
    }
    READABLE_FUNCTION(ungetc, Verified);

    // stdio/fiprintf.c and sscanf.c: the arguments after the fixed ones go to the stack, the va_list.
    namespace
    {
        int32_t vfiprintf(ee::Ptr<File> fp, ee::Address format, ee::Address arguments) { return ee::game<int32_t>(sym::vfiprintf, fp, format, arguments); }
        int32_t svfscanf(ee::Address fp, ee::Address format, ee::Address arguments) { return ee::game<int32_t>(sym::__svfscanf, fp, format, arguments); }
    }

    int32_t fiprintf(ee::Ptr<File> fp, ee::Address format, ee::VarArg word1, ee::VarArg word2, ee::VarArg word3, ee::VarArg word4,
                     ee::VarArg word5, ee::VarArg word6)
    {
        ee::Local<uint64_t[6]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5, word6};
        for (int i = 0; i < 6; ++i)
            arguments[i] = values[i];
        return vfiprintf(fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(fiprintf, Verified);

    // sscanf.c's eofread: what a string's FILE reads when it runs out, which is nothing.
    int32_t eofread(ee::Address /*cookie*/, ee::Ptr<char> /*buf*/, int32_t /*len*/)
    {
        return 0;
    }
    READABLE_FUNCTION(eofread, Verified);

    int32_t sscanf(ee::Ptr<uint8_t> text, ee::Address format, ee::VarArg word1, ee::VarArg word2, ee::VarArg word3, ee::VarArg word4,
                   ee::VarArg word5, ee::VarArg word6)
    {
        ee::Local<File> f;
        ee::Local<uint64_t[6]> arguments;
        const uint64_t values[] = {word1, word2, word3, word4, word5, word6};
        for (int i = 0; i < 6; ++i)
            arguments[i] = values[i];
        f->flags = kCanRead;
        f->bufferBase = text;
        f->position = text;
        const int32_t length = static_cast<int32_t>(strlen(text));
        f->bufferSize = length;
        f->readSpace = length;
        f->reent = *g_impurePtr;
        f->readFunction = sym::eofread;
        f->ungetBase = ee::Ptr<uint8_t>();
        f->lineBase = ee::Ptr<uint8_t>();
        return svfscanf(f, format, arguments);
    }
    READABLE_FUNCTION_WAITS(sscanf, Verified);

    namespace
    {
        uint32_t smaller(uint32_t a, uint32_t b) { return a < b ? a : b; }
    }

    // stdio/fvwrite.c: writes the request's pieces, by how the FILE is buffered. Unbuffered, each piece
    // goes straight out, BUFSIZ at a time. Fully buffered (a string's FILE too), the buffer fills and
    // flushes, and a piece at least its size bypasses it. Line buffered, the same, with a flush after
    // each newline. 0, or EOF with the FILE's error flag set.
    int32_t __sfvwrite(ee::Ptr<File> fp, ee::Ptr<IoRequest> request)
    {
        if (request->residual == 0)
            return 0;
        if (((fp->flags & kCanWrite) == 0 || !fp->bufferBase) && __swsetup(fp) != 0)
            return kEof;
        ee::Ptr<IoVector> vector = request->vectors;
        ee::Ptr<uint8_t> p;
        uint32_t length = 0u;
        const auto copyIn = [&](int32_t bytes) {
            memmove(fp->position, p, static_cast<uint32_t>(bytes));
            fp->position += bytes;
        };
        const auto nextPiece = [&]() {
            p = vector->base;
            length = vector->length;
            ++vector;
        };
        const auto failed = [&]() {
            fp->flags |= kHadError;
            return kEof;
        };
        const auto buffered = [&]() { return fp->position.get() > fp->bufferBase.get(); };
        int32_t written;
        if ((fp->flags & kUnbuffered) != 0)
        {
            do
            {
                while (length == 0u)
                    nextPiece();
                written = fp->writeFunction(fp->cookie, p, static_cast<int32_t>(smaller(length, kBufferSize)));
                if (written <= 0)
                    return failed();
                p += written;
                length -= static_cast<uint32_t>(written);
                request->residual -= written;
            } while (request->residual != 0);
        }
        else if ((fp->flags & kLineBuffered) == 0)
        {
            do
            {
                while (length == 0u)
                    nextPiece();
                written = fp->writeSpace;
                if ((fp->flags & kIsString) != 0)
                {
                    if (length < static_cast<uint32_t>(written))
                        written = static_cast<int32_t>(length);
                    memmove(fp->position, p, static_cast<uint32_t>(written));
                    fp->writeSpace -= written;
                    fp->position += written;
                    written = static_cast<int32_t>(length); // pretend it all went: a string has no error
                }
                else if (buffered() && length > static_cast<uint32_t>(written))
                {
                    copyIn(written);
                    if (fflush(fp) != 0)
                        return failed();
                }
                else if (length >= static_cast<uint32_t>(fp->bufferSize))
                {
                    written = fp->writeFunction(fp->cookie, p, fp->bufferSize);
                    if (written <= 0)
                        return failed();
                }
                else
                {
                    written = static_cast<int32_t>(length);
                    memmove(fp->position, p, static_cast<uint32_t>(written));
                    fp->writeSpace -= written;
                    fp->position += written;
                }
                p += written;
                length -= static_cast<uint32_t>(written);
                request->residual -= written;
            } while (request->residual != 0);
        }
        else
        {
            bool newlineKnown = false;
            int32_t newlineDistance = 0;
            do
            {
                while (length == 0u)
                {
                    newlineKnown = false;
                    nextPiece();
                }
                if (!newlineKnown)
                {
                    const ee::Ptr<uint8_t> newline(memchr(p, '\n', length));
                    newlineDistance = newline ? static_cast<int32_t>(newline.get() + 1 - p.get()) : static_cast<int32_t>(length + 1u);
                    newlineKnown = true;
                }
                const int32_t bytes = static_cast<int32_t>(smaller(length, static_cast<uint32_t>(newlineDistance)));
                written = fp->writeSpace + fp->bufferSize;
                if (buffered() && bytes > written)
                {
                    copyIn(written);
                    if (fflush(fp) != 0)
                        return failed();
                }
                else if (bytes >= fp->bufferSize)
                {
                    written = fp->writeFunction(fp->cookie, p, fp->bufferSize);
                    if (written <= 0)
                        return failed();
                }
                else
                {
                    written = bytes;
                    memmove(fp->position, p, static_cast<uint32_t>(written));
                    fp->writeSpace -= written;
                    fp->position += written;
                }
                newlineDistance -= written;
                if (newlineDistance == 0)
                {
                    if (fflush(fp) != 0)
                        return failed();
                    newlineKnown = false;
                }
                p += written;
                length -= static_cast<uint32_t>(written);
                request->residual -= written;
            } while (request->residual != 0);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(__sfvwrite, Verified);

    // stdio/fflush.c: a single file, or (fp null) every file.
    int32_t fflush(ee::Ptr<File> fp)
    {
        if (!fp)
            return _fwalk(*g_impurePtr, sym::fflush);
        checkInit(fp);
        const int t = fp->flags;
        ee::Ptr<uint8_t> p = fp->bufferBase;
        if ((t & kCanWrite) == 0 || !p)
            return 0;
        int32_t n = static_cast<int32_t>(fp->position.get() - p.get()); // write this much
        fp->position = p;
        fp->writeSpace = t & (kLineBuffered | kUnbuffered) ? 0 : fp->bufferSize;
        while (n > 0)
        {
            const int32_t wrote = fp->writeFunction(fp->cookie, p, n);
            if (wrote <= 0)
            {
                fp->flags |= kHadError;
                return kEof;
            }
            p += wrote;
            n -= wrote;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(fflush, Draft);

    // stdio/fread.c.
    uint32_t fread(ee::Ptr<uint8_t> buf, uint32_t size, uint32_t count, ee::Ptr<File> fp)
    {
        uint32_t resid = count * size;
        if (resid == 0u)
            return 0u;
        if (fp->readSpace < 0)
            fp->readSpace = 0;
        const uint32_t total = resid;
        ee::Ptr<uint8_t> p = buf;
        while (resid > static_cast<uint32_t>(fp->readSpace))
        {
            const uint32_t r = static_cast<uint32_t>(fp->readSpace);
            memcpy(p, fp->position, r);
            fp->position += r;
            p += r;
            resid -= r;
            if (__srefill(fp) != 0)
                return (total - resid) / size; // no more input: a partial result
        }
        memcpy(p, fp->position, resid);
        fp->readSpace -= static_cast<int32_t>(resid);
        fp->position += resid;
        return count;
    }
    READABLE_FUNCTION_WAITS(fread, Draft);

    // stdlib/mbtowc_r.c without MB_CAPABLE: every byte is a character.
    int32_t _mbtowc_r(uint32_t /*reent*/, ee::Ptr<uint32_t> pwc, ee::Ptr<const uint8_t> s, uint32_t n)
    {
        if (!s)
            return 0;
        if (n == 0u)
            return -1;
        const uint8_t c = *s;
        if (pwc)
            *pwc = c;
        return c != 0 ? 1 : 0;
    }
}
