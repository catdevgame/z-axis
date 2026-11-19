#pragma once

// newlib 1.8.2's C library functions that one file of it calls in another and the engine calls by name
// (sce/libc*.cpp, sce/mprec.cpp: newlib.h has their records and mprec's numbers). The EE's long is 64 bits.

#include "sce/newlib.h"

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace sce::libc
{
    // errno/errno.c: the address of errno.
    uint32_t __errno();

    // stdlib (libc.cpp, libc_malloc.cpp).
    uint32_t malloc(uint32_t nbytes);
    void free(uint32_t aptr);
    void malloc_stats();
    void __malloc_lock(ee::Address ptr);   // stdlib/mlock.c: empty in this build
    void __malloc_unlock(ee::Address ptr);
    int64_t strtol(ee::Address s, ee::Address ptr, int32_t base);
    int32_t atoi(ee::Address s); // DM2 only: reach it from DM2's code
    uint64_t atof(ee::Address text);

    // string (libc.cpp).
    int32_t strcasecmp(uint32_t left, uint32_t right);
    int32_t strncasecmp(uint32_t left, uint32_t right, uint32_t n);
    uint32_t strlwr(uint32_t a);
    uint32_t strupr(uint32_t a);
    ee::Ptr<const char> strrchr(ee::Ptr<const char> s, int32_t i);
    // string/*.c's word loops with the EE's quadword step (libc.cpp): an address or 0, a length, a comparison.
    uint32_t strlen(ee::Address str);
    uint32_t strcpy(ee::Address dst0, ee::Address src0);
    uint32_t strcat(ee::Address s1, ee::Address s2);
    int32_t strcmp(ee::Address left, ee::Address right);
    uint32_t strchr(ee::Address text, int32_t i);
    int32_t strncmp(ee::Address left, ee::Address right, uint32_t n);
    uint32_t strncpy(ee::Address dst0, ee::Address src0, uint32_t count);
    uint32_t memchr(ee::Address source, int32_t c, uint32_t length);
    uint32_t memmove(ee::Address dstVoid, ee::Address srcVoid, uint32_t length);

    // locale (libc.cpp).
    ee::Ptr<const LocaleConventions> localeconv();

    // stdio (libc_stdio.cpp, libc_malloc.cpp). fiprintf and sscanf are variadic: a call names the words its
    // format reads (ee::VarArg).
    int32_t fiprintf(ee::Ptr<File> fp, ee::Address format, ee::VarArg word1 = {}, ee::VarArg word2 = {},
                     ee::VarArg word3 = {}, ee::VarArg word4 = {}, ee::VarArg word5 = {}, ee::VarArg word6 = {});
    int32_t sscanf(ee::Ptr<uint8_t> text, ee::Address format, ee::VarArg word1 = {}, ee::VarArg word2 = {},
                   ee::VarArg word3 = {}, ee::VarArg word4 = {}, ee::VarArg word5 = {}, ee::VarArg word6 = {});
    uint32_t fread(ee::Ptr<uint8_t> buf, uint32_t size, uint32_t count, ee::Ptr<File> fp);
    int32_t ungetc(int32_t c, ee::Ptr<File> fp);
    int32_t fflush(ee::Ptr<File> fp);
    int32_t __srefill(ee::Ptr<File> fp);
    int32_t __swsetup(ee::Ptr<File> fp);
    int32_t __sfvwrite(ee::Ptr<File> fp, ee::Ptr<IoRequest> request);
    ee::Ptr<FileBlock> __sfmoreglue(ee::Ptr<Reent> reent, int32_t n);

    // stdlib/dtoa.c (libc_strtod.cpp).
    ee::Ptr<char> _dtoa_r(ee::Ptr<Reent> reent, uint64_t value, int32_t mode, int32_t ndigits, ee::Ptr<int32_t> decpt,
                          ee::Ptr<int32_t> sign, ee::Ptr<ee::Ptr<char>> rve);
}
