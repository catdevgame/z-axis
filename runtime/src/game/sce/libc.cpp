// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdlib/strtol.c carries the notice
// below; the string/, stdlib/ and errno/ files ported here carry none.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// Copyright (c) 1990 The Regents of the University of California.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. All advertising materials mentioning features or use of this software
//    must display the following acknowledgement:
//    This product includes software developed by the University of
//    California, Berkeley and its contributors.
// 4. Neither the name of the University nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

// libc: newlib 1.8.2's C library as the game links it (_calloc_r-__fixdfdi;
// the same code in DM2 and BMX XXX), the functions of it written in C there.
//
// Version pinned against the candidate releases (docs/READABLE.md, round
// 15): mallocr.c's MALLOC_ALIGNMENT 16 and strchr's aligned-first form came
// with 1.8.2; 1.9.0's __svfscanf leading-zero code (atol, sprintf) is not
// there, and strrchr is the byte loop 1.10.0 replaced with strchr. The EE's
// long is 64 bits (so strtol's arithmetic goes through libgcc's __muldi3,
// __udivdi3 and __umoddi3).
//
// string/*.c's speed paths (strlen, strcat, strcpy, strcmp, strchr, strncmp,
// strncpy, memchr, memmove) are ported from 1.8.2's source with the EE's
// long, 8 bytes (LBLOCKSIZE 8, DETECTNULL on 64 bits). The EE build adds a
// step that newlib 1.8.2 has no source for (a Cygnus/SCE addition; no ee-newlib
// source is on disk): where every address the loop walks is 16-aligned it
// works a quadword (lq/sq, 16 bytes) at a time, with DETECTNULL done byte by
// byte over the quadword (psubb, pnor, pand) and two quadwords compared word by
// word (psubw). That step is reproduced from the machine code, each function's
// where its comment says, and only changes how far ahead the loop reads: a
// quadword holds a zero byte exactly when one of its two longs does, so the
// step finds what the long loop finds, and what each function returns and
// writes is newlib's. The library suite (tests/src/ps2_library_tests.cpp)
// runs each against the game's own on the same inputs.
//
// malloc's family, memcpy, memset, printf's family, fflush, fread and mbtowc
// are the runtime's (config.game.toml's stubs; not_ported.tsv).
//

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "sce/newlib.h"
#include "sce/libc.h"

#include <cstdint>
#include "sym/sce/libc.cpp.h"

namespace sce::libc
{
    namespace
    {
        // _impure_ptr: the struct _reent of the one thread newlib knows (_REENT).
        inline ee::Global<uint32_t, sym::g_impurePtr> g_impurePtr;

        constexpr uint32_t kMemalignR = sym::_memalign_r; // _memalign_r (the runtime's)
        constexpr uint32_t kMallocR = sym::_malloc_r;   // _malloc_r (the runtime's)
        constexpr uint32_t kReallocR = sym::_realloc_r;  // _realloc_r (the runtime's)
        constexpr uint32_t kFreeR = sym::_free_r;     // _free_r (the runtime's)

        // ctype.h: _ctype_ (ctype/ctype_.c), indexed by the character + 1 so that EOF (-1)
        // reads entry 0. The compiled code indexes it with the signed char.
        constexpr uint32_t kCtypeTable = sym::ctypeTable;
        enum CtypeBits : uint8_t
        {
            kUpper = 0x01, // _U
            kLower = 0x02, // _L
            kDigit = 0x04, // _N
            kSpace = 0x08, // _S
        };
        uint8_t ctype(int c) { return *ee::Ptr<uint8_t>(kCtypeTable + 1u + static_cast<uint32_t>(c)); }
        bool isUpper(int c) { return (ctype(c) & kUpper) != 0; }
        bool isLower(int c) { return (ctype(c) & kLower) != 0; }
        bool isDigit(int c) { return (ctype(c) & kDigit) != 0; }
        bool isSpace(int c) { return (ctype(c) & kSpace) != 0; }
        bool isAlpha(int c) { return (ctype(c) & (kUpper | kLower)) != 0; }
        int toLower(int c) { return isUpper(c) ? c - 'A' + 'a' : c; }
        int toUpper(int c) { return isLower(c) ? c - 'a' + 'A' : c; }

        int8_t schar(uint32_t p) { return *ee::Ptr<int8_t>(p); }
        uint8_t uchar(uint32_t p) { return *ee::Ptr<uint8_t>(p); }

        constexpr int32_t kErange = 34;
        constexpr int64_t kLongMax = INT64_MAX; // the EE's long
        constexpr int64_t kLongMin = INT64_MIN;
    }

    // The runtime's (above) that are host code which neither waits nor calls the game: called from a readable
    // function on a fiber, they run on that fiber (ee/readable.h LibraryLeaf).
    READABLE_LIBRARY_LEAF(sym::memcpy);
    READABLE_LIBRARY_LEAF(sym::memset);
    READABLE_LIBRARY_LEAF(sym::_malloc_r);
    READABLE_LIBRARY_LEAF(sym::_memalign_r);
    READABLE_LIBRARY_LEAF(sym::_calloc_r);
    READABLE_LIBRARY_LEAF(sym::_realloc_r);
    READABLE_LIBRARY_LEAF(sym::_free_r);

    // __errno (errno/errno.c): &_REENT->_errno, the first field of struct _reent.
    uint32_t __errno()
    {
        return *g_impurePtr;
    }
    READABLE_FUNCTION(__errno, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t _memalign_r(uint32_t reent, uint32_t alignment, uint32_t bytes) { return ee::game<uint32_t>(kMemalignR, reent, alignment, bytes); }
    }

    // memalign, malloc, free, realloc (stdlib/malign.c, malloc.c, realloc.c): the
    // reentrant versions with _REENT.
    uint32_t memalign(uint32_t align, uint32_t nbytes)
    {
        return _memalign_r(*g_impurePtr, align, nbytes);
    }
    READABLE_FUNCTION(memalign, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t _malloc_r(uint32_t reent, uint32_t bytes) { return ee::game<uint32_t>(kMallocR, reent, bytes); }
    }

    uint32_t malloc(uint32_t nbytes)
    {
        return _malloc_r(*g_impurePtr, nbytes);
    }
    READABLE_FUNCTION(malloc, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void _free_r(uint32_t reent, uint32_t mem) { ee::tailCall(kFreeR, reent, mem); }
        }
    }

    void free(uint32_t aptr)
    {
        tail::_free_r(*g_impurePtr, aptr);
    }
    READABLE_FUNCTION(free, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t _realloc_r(uint32_t reent, uint32_t oldMem, uint32_t bytes) { return ee::game<uint32_t>(kReallocR, reent, oldMem, bytes); }
    }

    uint32_t realloc(uint32_t ap, uint32_t nbytes)
    {
        return _realloc_r(*g_impurePtr, ap, nbytes);
    }
    READABLE_FUNCTION(realloc, Verified);

    // strcasecmp (string/strcasecmp.c): compares with tolower on both sides.
    int32_t strcasecmp(uint32_t left, uint32_t right)
    {
        if (schar(left) != '\0')
        {
            for (;;)
            {
                if (toLower(schar(left)) != toLower(schar(right)))
                    break;
                ++left;
                ++right;
                if (schar(left) == '\0')
                    break;
            }
        }
        return toLower(uchar(left)) - toLower(uchar(right));
    }
    READABLE_FUNCTION(strcasecmp, Verified);

    // strlwr (string/strlwr.c): lower-cases the string in place; returns it.
    uint32_t strlwr(uint32_t a)
    {
        const uint32_t ret = a;
        if (schar(a) != '\0')
        {
            for (;;)
            {
                if (isUpper(schar(a)))
                    *ee::Ptr<int8_t>(a) = static_cast<int8_t>(toLower(schar(a)));
                ++a;
                if (schar(a) == '\0')
                    break;
            }
        }
        return ret;
    }
    READABLE_FUNCTION(strlwr, Verified);

    // strupr (string/strupr.c): upper-cases the string in place; returns it.
    uint32_t strupr(uint32_t a)
    {
        const uint32_t ret = a;
        if (schar(a) != '\0')
        {
            for (;;)
            {
                if (isLower(schar(a)))
                    *ee::Ptr<int8_t>(a) = static_cast<int8_t>(toUpper(schar(a)));
                ++a;
                if (schar(a) == '\0')
                    break;
            }
        }
        return ret;
    }
    READABLE_FUNCTION(strupr, Verified);

    // _strtol_r (stdlib/strtol.c): a long (64 bits on the EE) from a string in a base
    // (0: by its prefix), saturating with ERANGE; *endptr past the digits.
    int64_t _strtol_r(uint32_t rptr, uint32_t nptr, uint32_t endptr, int32_t base)
    {
        uint32_t s = nptr;
        int c;
        bool neg = false;

        // Skip white space and pick up a leading +/- sign if any.
        for (;;)
        {
            c = schar(s++);
            if (!isSpace(c))
                break;
        }
        if (c == '-')
        {
            neg = true;
            c = schar(s);
            ++s;
        }
        else
        {
            if (c == '+')
            {
                c = schar(s);
                ++s;
            }
        }
        // Base 0 allows 0x for hex and 0 for octal, else decimal; base 16 allows 0x.
        if (base == 0 || base == 16)
        {
            if (c == '0' && (schar(s) == 'x' || schar(s) == 'X'))
            {
                c = schar(s + 1);
                s += 2;
                base = 16;
            }
        }
        if (base == 0)
            base = c == '0' ? 8 : 10;

        // The cutoff between legal and illegal numbers: the largest legal value divided by
        // the base, the last digit's limit in cutlim.
        const uint64_t ubase = static_cast<uint64_t>(static_cast<int64_t>(base));
        uint64_t cutoff = neg ? static_cast<uint64_t>(kLongMin) : static_cast<uint64_t>(kLongMax);
        const int cutlim = static_cast<int32_t>(libgcc::__umoddi3(cutoff, ubase));
        cutoff = libgcc::__udivdi3(cutoff, ubase);
        uint64_t acc = 0;
        int any = 0; // digits consumed; negative on overflow
        for (;;)
        {
            if (isDigit(c))
                c -= '0';
            else
            {
                if (isAlpha(c))
                    c -= isUpper(c) ? 'A' - 10 : 'a' - 10;
                else
                    break;
            }
            if (c >= base)
                break;
            if (any >= 0 && acc > cutoff)
                any = -1;
            else if (any < 0 || (acc == cutoff && c > cutlim))
                any = -1;
            else
            {
                any = 1;
                acc = libgcc::__muldi3(acc, ubase) + static_cast<uint64_t>(static_cast<int64_t>(c));
            }
            c = schar(s++);
        }
        if (any < 0)
        {
            acc = static_cast<uint64_t>(neg ? kLongMin : kLongMax);
            *ee::Ptr<int32_t>(rptr) = kErange; // rptr->_errno
        }
        else
        {
            if (neg)
                acc = 0 - acc;
        }
        if (endptr != 0)
            *ee::Ptr<uint32_t>(endptr) = any ? s - 1 : nptr;
        return static_cast<int64_t>(acc);
    }
    READABLE_FUNCTION(_strtol_r, Verified);

    // strtol (stdlib/strtol.c): _strtol_r with _REENT.
    int64_t strtol(ee::Address s, ee::Address ptr, int32_t base)
    {
        return _strtol_r(*g_impurePtr, s, ptr, base);
    }
    READABLE_FUNCTION(strtol, Verified);

    // atoi (stdlib/atoi.c; DM2 links it, for the load screen's motion file numbers; AI and BMX XXX do not):
    // strtol in base 10 with no end pointer, cut to an int.
    inline int32_t atoi(ee::Address s)
    {
        return static_cast<int32_t>(strtol(s, 0u, 10));
    }
    READABLE_FUNCTION_ONLY(dm2, atoi);

    // string/strrchr.c. The byte loop (1.10.0 replaced it with strchr).
    ee::Ptr<const char> strrchr(ee::Ptr<const char> s, int32_t i)
    {
        const char c = static_cast<char>(i);
        ee::Ptr<const char> last;
        for (; *s != '\0'; ++s)
        {
            if (*s == c)
                last = s;
        }
        if (*s == c)
            last = s;
        return last;
    }
    READABLE_FUNCTION(strrchr, Verified);

    // ---- string/*.c's word loops, with the EE's long and its quadword step ------------------

    namespace
    {
        constexpr uint32_t kLongBytes = 8;      // LBLOCKSIZE, sizeof (long) on the EE
        constexpr uint32_t kQuadwordBytes = 16; // the EE build's step

        // UNALIGNED: an address off a block of `bytes` (8 or 16).
        bool unaligned(uint32_t address, uint32_t bytes) { return (address & (bytes - 1)) != 0; }

        uint64_t longAt(uint32_t address) { return *ee::Ptr<const uint64_t>(address); }
        void copyLong(uint32_t to, uint32_t from) { *ee::Ptr<uint64_t>(to) = longAt(from); }

        // DETECTNULL: nonzero if the long holds a zero byte.
        bool detectNull(uint64_t x) { return ((x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull) != 0; }
        // DETECTCHAR (strchr.c, memchr.c): DETECTNULL of the long xor the character in every byte.
        bool detectChar(uint64_t x, uint64_t mask) { return detectNull(x ^ mask); }
        // The character in every byte of a long, as the sources build it.
        uint64_t charMask(uint8_t c)
        {
            uint64_t mask = 0;
            for (uint32_t j = 0; j < kLongBytes; ++j)
                mask = (mask << 8) | c;
            return mask;
        }

        // The quadword step's tests: the machine code tests the 16 bytes at once, which is
        // the test of each of its two longs.
        bool quadwordHasNull(uint32_t address)
        {
            return detectNull(longAt(address)) || detectNull(longAt(address + kLongBytes));
        }
        bool quadwordHasChar(uint32_t address, uint64_t mask)
        {
            return detectChar(longAt(address), mask) || detectChar(longAt(address + kLongBytes), mask);
        }
        bool quadwordsEqual(uint32_t a, uint32_t b)
        {
            return longAt(a) == longAt(b) && longAt(a + kLongBytes) == longAt(b + kLongBytes);
        }
        // A quadword copied (lq, then sq): both halves read before either is written.
        void copyQuadword(uint32_t to, uint32_t from)
        {
            const uint64_t low = longAt(from);
            const uint64_t high = longAt(from + kLongBytes);
            *ee::Ptr<uint64_t>(to) = low;
            *ee::Ptr<uint64_t>(to + kLongBytes) = high;
        }

        // The skip to the terminating null that strlen and strcat share: if the string is
        // long-aligned, whole blocks while none holds a null (a quadword at a time where it
        // is 16-aligned), then the bytes of the block that holds it.
        uint32_t skipToNull(uint32_t s)
        {
            if (!unaligned(s, kLongBytes))
            {
                if (!unaligned(s, kQuadwordBytes))
                {
                    while (!quadwordHasNull(s))
                        s += kQuadwordBytes;
                }
                else
                {
                    while (!detectNull(longAt(s)))
                        s += kLongBytes;
                }
            }
            while (schar(s) != '\0')
                ++s;
            return s;
        }
    }

    // strlen (string/strlen.c): the null found a block at a time, then byte by byte.
    uint32_t strlen(ee::Address str)
    {
        const uint32_t start = str;
        return skipToNull(start) - start;
    }
    READABLE_FUNCTION(strlen, Verified);

    // strcpy (string/strcpy.c): if both strings are long-aligned, whole blocks while the
    // source's holds no null (quadwords where both are 16-aligned), then bytes up to and
    // with the null. dst0.
    uint32_t strcpy(ee::Address dst0, ee::Address src0)
    {
        uint32_t dst = dst0;
        uint32_t src = src0;
        if (!unaligned(src | dst, kLongBytes))
        {
            if (!unaligned(src | dst, kQuadwordBytes))
            {
                while (!quadwordHasNull(src))
                {
                    copyQuadword(dst, src);
                    dst += kQuadwordBytes;
                    src += kQuadwordBytes;
                }
            }
            else
            {
                while (!detectNull(longAt(src)))
                {
                    copyLong(dst, src);
                    dst += kLongBytes;
                    src += kLongBytes;
                }
            }
        }
        for (;;)
        {
            const uint8_t c = uchar(src++);
            *ee::Ptr<uint8_t>(dst++) = c;
            if (c == 0)
                break;
        }
        return dst0;
    }
    READABLE_FUNCTION(strcpy, Verified);

    // strcat (string/strcat.c): s1's data skipped as strlen does, then strcpy to its
    // trailing null. s1.
    uint32_t strcat(ee::Address s1, ee::Address s2)
    {
        strcpy(skipToNull(s1), s2);
        return s1;
    }
    READABLE_FUNCTION(strcat, Verified);

    // strcmp (string/strcmp.c): if both are long-aligned, equal blocks passed (quadwords
    // where both are 16-aligned): equal with a null in them, the strings are equal. From a
    // block that differs, or unaligned from the start, byte by byte.
    int32_t strcmp(ee::Address left, ee::Address right)
    {
        uint32_t s1 = left;
        uint32_t s2 = right;
        if (!unaligned(s1 | s2, kLongBytes))
        {
            if (!unaligned(s1 | s2, kQuadwordBytes))
            {
                while (quadwordsEqual(s1, s2))
                {
                    if (quadwordHasNull(s1))
                        return 0;
                    s1 += kQuadwordBytes;
                    s2 += kQuadwordBytes;
                }
            }
            else
            {
                while (longAt(s1) == longAt(s2))
                {
                    if (detectNull(longAt(s1)))
                        return 0;
                    s1 += kLongBytes;
                    s2 += kLongBytes;
                }
            }
        }
        while (schar(s1) != '\0' && schar(s1) == schar(s2))
        {
            ++s1;
            ++s2;
        }
        return uchar(s1) - uchar(s2);
    }
    READABLE_FUNCTION(strcmp, Verified);

    // strchr (string/strchr.c): if s is long-aligned, blocks while one holds neither a null
    // nor the character (quadwords where s is 16-aligned), then bytes. The character's
    // address, or 0.
    uint32_t strchr(ee::Address text, int32_t i)
    {
        uint32_t s = text;
        const uint8_t c = static_cast<uint8_t>(i);
        if (!unaligned(s, kLongBytes))
        {
            const uint64_t mask = charMask(c);
            if (!unaligned(s, kQuadwordBytes))
            {
                while (!quadwordHasNull(s) && !quadwordHasChar(s, mask))
                    s += kQuadwordBytes;
            }
            else
            {
                while (!detectNull(longAt(s)) && !detectChar(longAt(s), mask))
                    s += kLongBytes;
            }
        }
        while (uchar(s) != 0 && uchar(s) != c)
            ++s;
        if (uchar(s) == c)
            return s;
        return 0;
    }
    READABLE_FUNCTION(strchr, Verified);

    // strncmp (string/strncmp.c): as strcmp, at most n bytes. The block is a quadword where
    // both are 16-aligned and n is at least 16 (whose loop goes on to the bytes once fewer
    // than 16 are left), else a long while at least 8 are left.
    int32_t strncmp(ee::Address left, ee::Address right, uint32_t n)
    {
        uint32_t s1 = left;
        uint32_t s2 = right;
        if (n == 0)
            return 0;
        if (!unaligned(s1 | s2, kLongBytes))
        {
            if (!unaligned(s1 | s2, kQuadwordBytes) && n >= kQuadwordBytes)
            {
                while (n >= kQuadwordBytes && quadwordsEqual(s1, s2))
                {
                    n -= kQuadwordBytes;
                    // Out of bytes, or a null: equal, since the blocks are.
                    if (n == 0 || quadwordHasNull(s1))
                        return 0;
                    s1 += kQuadwordBytes;
                    s2 += kQuadwordBytes;
                }
            }
            else
            {
                while (n >= kLongBytes && longAt(s1) == longAt(s2))
                {
                    n -= kLongBytes;
                    if (n == 0 || detectNull(longAt(s1)))
                        return 0;
                    s1 += kLongBytes;
                    s2 += kLongBytes;
                }
            }
        }
        while (n-- > 0 && schar(s1) == schar(s2))
        {
            if (n == 0 || schar(s1) == '\0')
                return 0;
            ++s1;
            ++s2;
        }
        return uchar(s1) - uchar(s2);
    }
    READABLE_FUNCTION(strncmp, Verified);

    // strncpy (string/strncpy.c): if both are long-aligned and count is at least a block,
    // blocks while the source's holds no null, then bytes up to and with the null, then
    // nulls to count. Where both are 16-aligned the block is a quadword, but only for the
    // first: the machine code's quadword loop goes on into the long loop from its second
    // test, so the rest go a long at a time. The bytes written are the same either way. dst0.
    uint32_t strncpy(ee::Address dst0, ee::Address src0, uint32_t count)
    {
        uint32_t dst = dst0;
        uint32_t src = src0;
        bool longs = false; // on to the long loop
        if (!unaligned(src | dst, kLongBytes))
        {
            if (!unaligned(src | dst, kQuadwordBytes))
            {
                if (count >= kQuadwordBytes && !quadwordHasNull(src))
                {
                    copyQuadword(dst, src);
                    count -= kQuadwordBytes;
                    dst += kQuadwordBytes;
                    src += kQuadwordBytes;
                    longs = count >= kQuadwordBytes && !quadwordHasNull(src);
                }
            }
            else
                longs = count >= kLongBytes && !detectNull(longAt(src));
        }
        while (longs)
        {
            count -= kLongBytes;
            copyLong(dst, src);
            dst += kLongBytes;
            src += kLongBytes;
            longs = count >= kLongBytes && !detectNull(longAt(src));
        }
        while (count > 0)
        {
            --count;
            const uint8_t c = uchar(src++);
            *ee::Ptr<uint8_t>(dst++) = c;
            if (c == 0)
                break;
        }
        while (count-- > 0)
            *ee::Ptr<uint8_t>(dst++) = 0;
        return dst0;
    }
    READABLE_FUNCTION(strncpy, Verified);

    // memchr (string/memchr.c): the first of length bytes equal to c, or 0. The EE build's
    // block is the quadword alone (no long loop): where src is 16-aligned and at least 16
    // bytes are left, a quadword at a time, searched byte by byte once it holds c.
    uint32_t memchr(ee::Address source, int32_t c, uint32_t length)
    {
        uint32_t src = source;
        const uint8_t wanted = static_cast<uint8_t>(c);
        if (length >= kQuadwordBytes && !unaligned(src, kQuadwordBytes))
        {
            const uint64_t mask = charMask(wanted);
            while (length >= kQuadwordBytes)
            {
                if (quadwordHasChar(src, mask))
                {
                    for (uint32_t j = 0; j < kQuadwordBytes; ++j)
                    {
                        if (uchar(src) == wanted)
                            return src;
                        ++src;
                    }
                }
                length -= kQuadwordBytes;
                src += kQuadwordBytes;
            }
        }
        while (length-- > 0)
        {
            if (uchar(src) == wanted)
                return src;
            ++src;
        }
        return 0;
    }
    READABLE_FUNCTION(memchr, Verified);

    // memmove (string/memmove.c): backwards byte by byte when dst overlaps the end of src;
    // else forwards, in BIGBLOCKSIZE blocks (four longs: two quadwords on the EE, where both
    // are 16-aligned and at least 32 bytes are to go), then longs, then bytes. dst.
    uint32_t memmove(ee::Address dstVoid, ee::Address srcVoid, uint32_t length)
    {
        constexpr uint32_t kBigBlockBytes = kLongBytes << 2;
        uint32_t dst = dstVoid;
        uint32_t src = srcVoid;
        if (src < dst && dst < src + length)
        {
            // Destructive overlap: copy backwards.
            src += length;
            dst += length;
            while (length-- > 0)
                *ee::Ptr<uint8_t>(--dst) = uchar(--src);
        }
        else
        {
            if (length >= kBigBlockBytes && !unaligned(src | dst, kQuadwordBytes))
            {
                while (length >= kBigBlockBytes)
                {
                    copyQuadword(dst, src);
                    copyQuadword(dst + kQuadwordBytes, src + kQuadwordBytes);
                    dst += kBigBlockBytes;
                    src += kBigBlockBytes;
                    length -= kBigBlockBytes;
                }
                while (length >= kLongBytes)
                {
                    copyLong(dst, src);
                    dst += kLongBytes;
                    src += kLongBytes;
                    length -= kLongBytes;
                }
            }
            while (length-- > 0)
                *ee::Ptr<uint8_t>(dst++) = uchar(src++);
        }
        return dstVoid;
    }
    READABLE_FUNCTION(memmove, Verified);

    // _strtoul_r (stdlib/strtoul.c): as _strtol_r, unsigned, and ULONG_MAX is 64 bits here.
    uint64_t _strtoul_r(uint32_t rptr, uint32_t nptr, uint32_t endptr, int32_t base)
    {
        uint32_t s = nptr;
        int c;
        do
        {
            c = schar(s++);
        } while (isSpace(c));
        bool neg = false;
        if (c == '-')
        {
            neg = true;
            c = schar(s++);
        }
        else if (c == '+')
            c = schar(s++);
        if ((base == 0 || base == 16) && c == '0')
        {
            const int x = schar(s);
            if (x == 'x' || x == 'X')
            {
                c = schar(s + 1u);
                s += 2u;
                base = 16;
            }
        }
        if (base == 0)
            base = c == '0' ? 8 : 10;
        constexpr uint64_t kUnsignedLongMax = ~0ull;
        const uint64_t ubase = static_cast<uint64_t>(static_cast<int64_t>(base));
        const uint64_t cutoff = libgcc::__udivdi3(kUnsignedLongMax, ubase);
        const int cutlim = static_cast<int32_t>(libgcc::__umoddi3(kUnsignedLongMax, ubase));
        uint64_t acc = 0;
        int any = 0; // digits consumed; negative on overflow
        for (;;)
        {
            if (isDigit(c))
                c -= '0';
            else if (isAlpha(c))
                c -= isUpper(c) ? 'A' - 10 : 'a' - 10;
            else
                break;
            if (c >= base)
                break;
            if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
                any = -1;
            else
            {
                any = 1;
                acc = libgcc::__muldi3(acc, ubase) + static_cast<uint64_t>(static_cast<int64_t>(c));
            }
            c = schar(s++);
        }
        if (any < 0)
        {
            acc = kUnsignedLongMax;
            *ee::Ptr<int32_t>(rptr) = kErange; // rptr->_errno
        }
        else if (neg)
            acc = 0 - acc;
        if (endptr != 0)
            *ee::Ptr<uint32_t>(endptr) = any ? s - 1 : nptr;
        return acc;
    }
    READABLE_FUNCTION(_strtoul_r, Verified);

    // strtoul, strtod, atof, calloc: the _r forms on _REENT.
    uint64_t strtoul(uint32_t s, uint32_t ptr, int32_t base)
    {
        return _strtoul_r(*g_impurePtr, s, ptr, base);
    }
    READABLE_FUNCTION(strtoul, Verified);

    namespace
    {
        uint64_t strtodR(uint32_t reent, uint32_t text, uint32_t end) { return ee::game<uint64_t>(sym::_strtod_r, reent, text, end); }
        uint32_t callocR(uint32_t reent, uint32_t count, uint32_t bytes) { return ee::game<uint32_t>(sym::_calloc_r, reent, count, bytes); }
    }

    uint64_t strtod(ee::Address text, ee::Address end)
    {
        return strtodR(*g_impurePtr, text, end);
    }
    READABLE_FUNCTION(strtod, Verified);

    uint64_t atof(ee::Address text)
    {
        return strtod(text, 0u);
    }
    READABLE_FUNCTION(atof, Verified);

    uint32_t calloc(uint32_t count, uint32_t bytes)
    {
        return callocR(*g_impurePtr, count, bytes);
    }
    READABLE_FUNCTION(calloc, Verified);

    // string/strncasecmp.c.
    int32_t strncasecmp(uint32_t left, uint32_t right, uint32_t n)
    {
        if (n == 0u)
            return 0;
        while (n-- != 0u && toLower(schar(left)) == toLower(schar(right)))
        {
            if (n == 0u || schar(left) == '\0' || schar(right) == '\0')
                break;
            ++left;
            ++right;
        }
        return toLower(*ee::Ptr<uint8_t>(left)) - toLower(*ee::Ptr<uint8_t>(right));
    }
    READABLE_FUNCTION(strncasecmp, Draft);

    // strtodf in stdlib/strtod.c.
    ee::Float strtof(ee::Address text, ee::Address end)
    {
        return libgcc::dptofp(strtod(text, end));
    }
    READABLE_FUNCTION(strtof, Draft);

    // locale/locale.c: only the "C" locale and the empty name are known. The name of the locale in
    // force, or 0 for any other.
    uint32_t _setlocale_r(ee::Ptr<Reent> reent, int32_t category, uint32_t locale)
    {
        if (locale != 0u)
        {
            if (strcmp(locale, sym::g_textCLocale) != 0 && strcmp(locale, sym::g_emptyLocaleName) != 0)
                return 0u;
            reent->currentLocale = ee::Ptr<const char>(locale);
            reent->currentCategory = category;
        }
        return sym::g_textCLocale;
    }
    READABLE_FUNCTION(_setlocale_r, Draft);

    uint32_t setlocale(int32_t category, uint32_t locale)
    {
        return _setlocale_r(ee::Ptr<Reent>(*g_impurePtr), category, locale);
    }
    READABLE_FUNCTION(setlocale, Draft);

    // _localeconv_r and localeconv (locale/locale.c): the one locale's conventions, whatever the
    // _reent.
    ee::Ptr<const LocaleConventions> _localeconv_r(ee::Ptr<Reent> /*reent*/)
    {
        return ee::Ptr<const LocaleConventions>(sym::g_cLocaleConventions);
    }
    READABLE_FUNCTION(_localeconv_r, Verified);

    ee::Ptr<const LocaleConventions> localeconv()
    {
        return _localeconv_r(ee::Ptr<Reent>(*g_impurePtr));
    }
    READABLE_FUNCTION(localeconv, Verified);
}
