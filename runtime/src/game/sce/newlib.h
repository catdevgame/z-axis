#pragma once

// Licence: the records and numbers here are newlib 1.8.2's (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz):
// stdio/local.h and stdlib/mprec.h carry the notices below; sys/reent.h carries none.
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
//
// The author of this software is David M. Gay.
//
// Copyright (c) 1991 by AT&T.
//
// Permission to use, copy, modify, and distribute this software for any
// purpose without fee is hereby granted, provided that this entire notice
// is included in all copies of any software which is or includes a copy
// or modification of this software and in all copies of the supporting
// documentation for such software.
//
// THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
// WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR AT&T MAKES ANY
// REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
// OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.

// newlib 1.8.2's stdio and reentrancy records (sys/reent.h) as the game's EE
// build lays them out: pointers are 32 bits, long and off_t 64. The field
// comments give newlib's own names.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace sce::libc
{
    struct Reent;
    struct File;

    using TransferFunction = ee::FunctionPtr<int32_t(ee::Ptr<File>, ee::Ptr<uint8_t>, int32_t)>; // read or write from a cookie
    using SeekFunction = ee::FunctionPtr<int32_t(ee::Ptr<File>, int32_t, int32_t)>;
    using FileFunction = ee::FunctionPtr<int32_t(ee::Ptr<File>)>; // _fwalk's, on each FILE in use; a FILE's close

    // __SLBF .. __SMOD, FILE's flags.
    constexpr int kLineBuffered = 0x0001, kUnbuffered = 0x0002, kCanRead = 0x0004, kCanWrite = 0x0008, kReadWrite = 0x0010;
    constexpr int kAtEof = 0x0020, kHadError = 0x0040, kBufferMalloced = 0x0080, kAppend = 0x0100, kIsString = 0x0200;
    constexpr int kSeekOptimise = 0x0400, kNoSeekOptimise = 0x0800, kOffsetCorrect = 0x1000, kLineModified = 0x2000;
    constexpr int32_t kEof = -1;
    constexpr int32_t kBufferSize = 1024; // BUFSIZ

    // struct __sFILE.
    struct File
    {
        ee::Ptr<uint8_t> position;    // _p, the current place in the buffer
        int32_t readSpace;            // _r, read space left
        int32_t writeSpace;           // _w, write space left
        int16_t flags;                // _flags
        int16_t descriptor;           // _file
        ee::Ptr<uint8_t> bufferBase;  // _bf._base
        int32_t bufferSize;           // _bf._size
        int32_t lineBufferSize;       // _lbfsize, 0 or minus the buffer's size
        ee::Ptr<File> cookie;         // _cookie, handed to the four functions below
        TransferFunction readFunction;  // _read
        TransferFunction writeFunction; // _write
        SeekFunction seekFunction;      // _seek
        FileFunction closeFunction;     // _close
        ee::Ptr<uint8_t> ungetBase;   // _ub._base, ungetc's buffer
        int32_t ungetSize;            // _ub._size
        ee::Ptr<uint8_t> savedPosition; // _up, the place while ungetc's buffer is in use
        int32_t savedReadSpace;       // _ur
        uint8_t ungetBytes[3];        // _ubuf
        uint8_t getcByte;             // _nbuf, the buffer of an unbuffered file
        ee::Ptr<uint8_t> lineBase;    // _lb._base, fgetline's buffer
        int32_t lineSize;             // _lb._size
        int32_t blockSize;            // _blksize
        int32_t seekOffset;           // _offset
        ee::Ptr<Reent> reent;         // _data
    };
    static_assert(offsetof(File, flags) == 0x0C && offsetof(File, readFunction) == 0x20 && offsetof(File, ungetBase) == 0x30 &&
                  offsetof(File, ungetBytes) == 0x40 && offsetof(File, seekOffset) == 0x50 && sizeof(File) == 0x58);

    // struct __siov and __suio (stdio/fvwrite.h): what to write, in pieces.
    struct IoVector
    {
        ee::Ptr<uint8_t> base;  // iov_base
        uint32_t length;        // iov_len
    };
    struct IoRequest
    {
        ee::Ptr<IoVector> vectors; // uio_iov
        int32_t vectorCount;       // uio_iovcnt
        int32_t residual;          // uio_resid, bytes left to write
    };
    static_assert(sizeof(IoVector) == 8 && sizeof(IoRequest) == 12);

    // struct stat (sys/stat.h).
    struct FileStatus
    {
        int16_t device;        // st_dev
        uint16_t inode;        // st_ino
        uint32_t mode;         // st_mode
        uint16_t linkCount;    // st_nlink
        uint16_t user;         // st_uid
        uint16_t group;        // st_gid
        int16_t specialDevice; // st_rdev
        int64_t size;          // st_size
        int64_t accessTime;    // st_atime
        int64_t spare1;        // st_spare1
        int64_t modifyTime;    // st_mtime
        int64_t spare2;        // st_spare2
        int64_t changeTime;    // st_ctime
        int64_t spare3;        // st_spare3
        int64_t blockSize;     // st_blksize
        int64_t blocks;        // st_blocks
        int64_t spare4[2];     // st_spare4
    };
    static_assert(offsetof(FileStatus, mode) == 4 && offsetof(FileStatus, blockSize) == 0x48 && sizeof(FileStatus) == 0x68);
    constexpr uint32_t kFileTypeMask = 0xF000u, kCharacterDevice = 0x2000u, kRegularFile = 0x8000u; // S_IFMT, S_IFCHR, S_IFREG

    // struct _glue: a block of FILEs, chained from Reent::glue.
    struct FileBlock
    {
        ee::Ptr<FileBlock> next;      // _next
        int32_t fileCount;            // _niobs
        ee::Ptr<File> files;          // _iobs
    };
    static_assert(sizeof(FileBlock) == 0x0C);

    // struct _Bigint (stdlib/mprec.h): a number in x[0..wds-1], low word first. Its size is 2 to the k
    // words; x has room for that.
    struct Bigint
    {
        ee::Ptr<Bigint> next;         // _next, the free list of its size
        int32_t k;                    // _k
        int32_t maxWords;             // _maxwds
        int32_t sign;                 // _sign
        int32_t wordCount;            // _wds
        uint32_t x[1];                // _x
    };
    static_assert(offsetof(Bigint, x) == 0x14);

    // struct lconv (locale.h): the "C" locale's conventions, which localeconv reports.
    struct LocaleConventions
    {
        ee::Ptr<const char> decimalPoint;          // decimal_point, "."
        ee::Ptr<const char> thousandsSeparator;    // thousands_sep
        ee::Ptr<const char> grouping;              // grouping
        ee::Ptr<const char> internationalCurrency; // int_curr_symbol
        ee::Ptr<const char> currencySymbol;        // currency_symbol
        ee::Ptr<const char> moneyDecimalPoint;     // mon_decimal_point
        ee::Ptr<const char> moneyThousandsSeparator; // mon_thousands_sep
        ee::Ptr<const char> moneyGrouping;         // mon_grouping
        ee::Ptr<const char> positiveSign;          // positive_sign
        ee::Ptr<const char> negativeSign;          // negative_sign
        char internationalFractionDigits;          // int_frac_digits
        char fractionDigits;                       // frac_digits
        char positiveSymbolPrecedes;               // p_cs_precedes
        char positiveSeparatedBySpace;             // p_sep_by_space
        char negativeSymbolPrecedes;               // n_cs_precedes
        char negativeSeparatedBySpace;             // n_sep_by_space
        char positiveSignPosition;                 // p_sign_posn
        char negativeSignPosition;                 // n_sign_posn
    };
    static_assert(sizeof(LocaleConventions) == 0x30);

    // struct _reent, the state of the C library for the one thread it knows.
    struct Reent
    {
        int32_t errorNumber;          // _errno
        ee::Ptr<File> standardIn;     // _stdin
        ee::Ptr<File> standardOut;    // _stdout
        ee::Ptr<File> standardErr;    // _stderr
        int32_t scanfChar;            // _inc
        char emergency[25];           // _emergency
        int32_t currentCategory;      // _current_category
        ee::Ptr<const char> currentLocale; // _current_locale
        int32_t didInit;              // __sdidinit
        ee::FunctionPtr<void(ee::Ptr<Reent>)> cleanup; // __cleanup
        ee::Ptr<Bigint> dtoaResult;   // _result
        int32_t dtoaResultK;          // _result_k
        ee::Ptr<Bigint> powersOf5;    // _p5s
        ee::Ptr<ee::Ptr<Bigint>> bigintFreeList; // _freelist
        int32_t floatBufferLength;    // _cvtlen
        ee::Ptr<char> floatBuffer;    // _cvtbuf
        uint8_t unionAndAtexit[0x180];// _new, _atexit, _atexit0, _sig_func
        FileBlock glue;               // __sglue
        File stdFiles[3];             // __sf
    };
    static_assert(offsetof(Reent, didInit) == 0x38 && offsetof(Reent, bigintFreeList) == 0x4C && offsetof(Reent, glue) == 0x1D8 &&
                  offsetof(Reent, stdFiles) == 0x1E4);

    // stdlib/mprec.c's numbers (mprec.cpp). A double travels as its 64-bit pattern.
    ee::Ptr<Bigint> _Balloc(ee::Ptr<Reent> reent, int32_t k);
    void _Bfree(ee::Ptr<Reent> reent, ee::Ptr<Bigint> v);
    ee::Ptr<Bigint> _multadd(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t m, int32_t a);
    ee::Ptr<Bigint> _s2b(ee::Ptr<Reent> reent, ee::Ptr<const char> s, int32_t nd0, int32_t nd, uint32_t y9);
    int32_t _hi0bits(uint32_t x);
    ee::Ptr<Bigint> _i2b(ee::Ptr<Reent> reent, int32_t value);
    ee::Ptr<Bigint> _multiply(ee::Ptr<Reent> reent, ee::Ptr<Bigint> a, ee::Ptr<Bigint> b);
    ee::Ptr<Bigint> _pow5mult(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t k);
    ee::Ptr<Bigint> _lshift(ee::Ptr<Reent> reent, ee::Ptr<Bigint> b, int32_t k);
    int32_t __mcmp(ee::Ptr<Bigint> a, ee::Ptr<Bigint> b);
    ee::Ptr<Bigint> __mdiff(ee::Ptr<Reent> reent, ee::Ptr<Bigint> a, ee::Ptr<Bigint> b);
    uint64_t _ulp(uint64_t x);
    ee::Ptr<Bigint> _d2b(ee::Ptr<Reent> reent, uint64_t d, ee::Ptr<int32_t> e, ee::Ptr<int32_t> bits);
    uint64_t _ratio(ee::Ptr<Bigint> a, ee::Ptr<Bigint> b);
}
