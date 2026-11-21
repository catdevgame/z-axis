// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdio/vfscanf.c and
// sscanf.c carry the notice below.
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

// stdio/vfscanf.c and sscanf's front end, newlib 1.8.2: what the game's level and menu text parsers
// run. The same code in DM2 and BMX XXX. No multibyte support here: 1.8.2 has the wchar_t variables
// but a character is a byte. A pointer on the va_list takes a 64-bit slot.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libc.h"
#include "sce/libgcc.h"

#include <cstdint>
#include "sym/sce/libc_scanf.cpp.h"

namespace sce::libc
{
    namespace
    {
        // The flags of one conversion (LONG .. NZDIGITS).
        constexpr int kLong = 0x01, kLongDouble = 0x02, kShort = 0x04, kSuppress = 0x08, kPointer = 0x10, kNoSkip = 0x20;
        constexpr int kSignOk = 0x40, kNoDigits = 0x80;
        constexpr int kPointOk = 0x100, kExponentOk = 0x200;      // floating point
        constexpr int kPrefixOk = 0x100, kNoNonZeroDigits = 0x200; // integers

        enum Conversion { kChar, kCharacterClass, kString, kInteger, kFloat, kNotConverted };

        constexpr int kSpace = 0x08, kUpper = 0x01; // ctype.h's _S, _U
        bool isSpace(int c) { return (ee::Ptr<uint8_t>(sym::ctypeTable)[static_cast<uint32_t>(c + 1)] & kSpace) != 0; }
        bool isUpper(int c) { return (ee::Ptr<uint8_t>(sym::ctypeTable)[static_cast<uint32_t>(c + 1)] & kUpper) != 0; }

        constexpr uint32_t kNumberBufferBytes = 350u; // BUF: MAXEXP + MAXFRACT + sign, point, NUL

        using ConvertInteger = ee::FunctionPtr<uint64_t(ee::Ptr<char>, uint32_t, int32_t)>; // strtol or strtoul

        // A base seen a digit from 1 to 9 in is what the digit says when it was "pick one".
        constexpr int16_t kBaseFix[17] = {10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

        bool bufferEmpty(ee::Ptr<File> fp) { return fp->readSpace <= 0 && __srefill(fp) != 0; }

        // One character read off: EOF is reported by a refill's failing.
        bool moveOn(ee::Ptr<File> fp)
        {
            fp->readSpace -= 1;
            if (fp->readSpace > 0)
            {
                ++fp->position;
                return true;
            }
            return __srefill(fp) == 0;
        }
    }

    // __sccl: reads a %[...] scanset at fmt into tab, 256 bytes of 1 for a character in the set.
    // Past its closing bracket; at a NUL if there is none.
    ee::Ptr<const char> __sccl(ee::Ptr<char> tab, ee::Ptr<const char> fmt)
    {
        const auto next = [&fmt]() {
            const int value = *fmt;
            ++fmt;
            return value;
        };
        int c = next();
        int v = 0;
        if (c == '^')
        {
            v = 1;
            c = next();
        }
        for (int n = 0; n < 256; ++n)
            tab[n] = static_cast<char>(v);
        if (c == 0)
            return fmt - 1;
        v = 1 - v;
        for (;;)
        {
            tab[c] = static_cast<char>(v);
            int n;
            for (;;)
            {
                n = next();
                if (n != '-')
                    break;
                n = *fmt;
                if (n == ']' || n < c)
                {
                    n = '-';
                    break;
                }
                ++fmt;
                do
                {
                    ++c;
                    tab[c] = static_cast<char>(v);
                } while (c < n);
            }
            if (n == 0)
                return fmt - 1;
            if (n == ']')
                return fmt;
            c = n;
        }
    }
    READABLE_FUNCTION(__sccl, Verified);

    int32_t __svfscanf(ee::Ptr<File> fp, ee::Ptr<const char> fmt, uint32_t arguments)
    {
        ee::Local<char[256]> scanset;
        ee::Local<char[kNumberBufferBytes]> numberText;
        const ee::Ptr<char> buffer(numberText.address());
        int32_t assigned = 0;
        int32_t read = 0;
        int base = 0;
        ConvertInteger convert;

        const auto nextPointer = [&arguments]() {
            const uint32_t pointer = *ee::Ptr<uint32_t>(arguments);
            arguments += sizeof(uint64_t);
            return pointer;
        };
        const auto inputFailure = [&assigned]() { return assigned != 0 ? assigned : -1; };

        for (;;)
        {
            const int wc = *fmt;
            ++fmt;
            if (wc == 0)
                return assigned;
            if (isSpace(wc))
            {
                for (;;)
                {
                    if (bufferEmpty(fp))
                        return assigned;
                    if (!isSpace(*fp->position))
                        break;
                    ++read;
                    fp->readSpace -= 1;
                    ++fp->position;
                }
                continue;
            }

            // The conversion, past its flags and width. A literal character is its own.
            uint32_t width = 0u;
            int flags = 0;
            int c = 0;
            Conversion conversion = kNotConverted;
            bool literal = wc != '%';
            bool stored = false;
            while (!literal && conversion == kNotConverted && !stored)
            {
                c = *fmt;
                ++fmt;
                switch (c)
                {
                case '%':
                    literal = true;
                    break;
                case '*':
                    flags |= kSuppress;
                    break;
                case 'l':
                    flags |= kLong;
                    break;
                case 'L':
                    flags |= kLongDouble;
                    break;
                case 'h':
                    flags |= kShort;
                    break;
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    width = width * 10u + static_cast<uint32_t>(c) - '0';
                    break;
                case 'D':
                    flags |= kLong;
                    [[fallthrough]];
                case 'd':
                    conversion = kInteger;
                    convert = sym::strtol;
                    base = 10;
                    break;
                case 'i':
                    conversion = kInteger;
                    convert = sym::strtol;
                    base = 0;
                    break;
                case 'O':
                    flags |= kLong;
                    [[fallthrough]];
                case 'o':
                    conversion = kInteger;
                    convert = sym::strtoul;
                    base = 8;
                    break;
                case 'u':
                    conversion = kInteger;
                    convert = sym::strtoul;
                    base = 10;
                    break;
                case 'X':
                case 'x':
                    flags |= kPrefixOk;
                    conversion = kInteger;
                    convert = sym::strtoul;
                    base = 16;
                    break;
                case 'E': case 'G': case 'e': case 'f': case 'g':
                    conversion = kFloat;
                    break;
                case 's':
                    conversion = kString;
                    break;
                case '[':
                    fmt = __sccl(scanset, fmt);
                    flags |= kNoSkip;
                    conversion = kCharacterClass;
                    break;
                case 'c':
                    flags |= kNoSkip;
                    conversion = kChar;
                    break;
                case 'p':
                    flags |= kPointer | kPrefixOk;
                    conversion = kInteger;
                    convert = sym::strtoul;
                    base = 16;
                    break;
                case 'n':
                    if ((flags & kSuppress) == 0)
                    {
                        if ((flags & kShort) != 0)
                            *ee::Ptr<int16_t>(nextPointer()) = static_cast<int16_t>(read);
                        else if ((flags & kLong) != 0)
                            *ee::Ptr<int64_t>(nextPointer()) = read;
                        else
                            *ee::Ptr<int32_t>(nextPointer()) = read;
                    }
                    stored = true;
                    break;
                case '\0':
                    return kEof;
                default:
                    if (isUpper(c))
                        flags |= kLong;
                    conversion = kInteger;
                    convert = sym::strtol;
                    base = 10;
                    break;
                }
            }
            if (stored)
                continue;
            if (literal)
            {
                if (bufferEmpty(fp))
                    return inputFailure();
                if (*fp->position != fmt[-1])
                    return assigned;
                fp->readSpace -= 1;
                ++fp->position;
                ++read;
                continue;
            }

            if (bufferEmpty(fp))
                return inputFailure();
            if ((flags & kNoSkip) == 0)
            {
                while (isSpace(*fp->position))
                {
                    ++read;
                    if (!moveOn(fp))
                        return inputFailure();
                }
            }

            switch (conversion)
            {
            case kChar:
                if (width == 0u)
                    width = 1u;
                if ((flags & kSuppress) != 0)
                {
                    uint32_t sum = 0u;
                    for (;;)
                    {
                        const int32_t n = fp->readSpace;
                        if (static_cast<uint32_t>(n) < width)
                        {
                            sum += static_cast<uint32_t>(n);
                            width -= static_cast<uint32_t>(n);
                            fp->position += n;
                            if (__srefill(fp) != 0)
                            {
                                if (sum == 0u)
                                    return inputFailure();
                                break;
                            }
                        }
                        else
                        {
                            sum += width;
                            fp->readSpace -= static_cast<int32_t>(width);
                            fp->position += width;
                            break;
                        }
                    }
                    read += static_cast<int32_t>(sum);
                }
                else
                {
                    const uint32_t got = fread(ee::Ptr<uint8_t>(nextPointer()), 1u, width, fp);
                    if (got == 0u)
                        return inputFailure();
                    read += static_cast<int32_t>(got);
                    ++assigned;
                }
                break;

            case kCharacterClass:
            {
                if (width == 0u)
                    width = ~0u;
                int32_t n = 0;
                if ((flags & kSuppress) != 0)
                {
                    while (scanset[*fp->position] != 0)
                    {
                        ++n;
                        fp->readSpace -= 1;
                        ++fp->position;
                        if (--width == 0u)
                            break;
                        if (bufferEmpty(fp))
                        {
                            if (n == 0)
                                return inputFailure();
                            break;
                        }
                    }
                    if (n == 0)
                        return assigned;
                }
                else
                {
                    const ee::Ptr<uint8_t> first(nextPointer());
                    ee::Ptr<uint8_t> p = first;
                    while (scanset[*fp->position] != 0)
                    {
                        fp->readSpace -= 1;
                        *p = *fp->position;
                        ++p;
                        ++fp->position;
                        if (--width == 0u)
                            break;
                        if (bufferEmpty(fp))
                        {
                            if (p.get() == first.get())
                                return inputFailure();
                            break;
                        }
                    }
                    n = static_cast<int32_t>(p.get() - first.get());
                    if (n == 0)
                        return assigned;
                    *p = 0;
                    ++assigned;
                }
                read += n;
                break;
            }

            case kString:
                if (width == 0u)
                    width = ~0u;
                if ((flags & kSuppress) != 0)
                {
                    int32_t n = 0;
                    while (!isSpace(*fp->position))
                    {
                        ++n;
                        fp->readSpace -= 1;
                        ++fp->position;
                        if (--width == 0u || bufferEmpty(fp))
                            break;
                    }
                    read += n;
                }
                else
                {
                    const ee::Ptr<uint8_t> first(nextPointer());
                    ee::Ptr<uint8_t> p = first;
                    while (!isSpace(*fp->position))
                    {
                        fp->readSpace -= 1;
                        *p = *fp->position;
                        ++p;
                        ++fp->position;
                        if (--width == 0u || bufferEmpty(fp))
                            break;
                    }
                    *p = 0;
                    read += static_cast<int32_t>(p.get() - first.get());
                    ++assigned;
                }
                break;

            case kInteger:
            {
                if (--width > kNumberBufferBytes - 2u)
                    width = kNumberBufferBytes - 2u;
                ++width;
                flags |= kSignOk | kNoDigits | kNoNonZeroDigits;
                ee::Ptr<char> p = buffer;
                for (; width != 0u; --width)
                {
                    c = *fp->position;
                    bool accepted = false;
                    if (c == '0')
                    {
                        if (base == 0)
                        {
                            base = 8;
                            flags |= kPrefixOk;
                        }
                        if ((flags & kNoNonZeroDigits) != 0)
                            flags &= ~(kSignOk | kNoNonZeroDigits | kNoDigits);
                        else
                            flags &= ~(kSignOk | kPrefixOk | kNoDigits);
                        accepted = true;
                    }
                    else if (c >= '1' && c <= '7')
                    {
                        base = kBaseFix[base];
                        flags &= ~(kSignOk | kPrefixOk | kNoDigits);
                        accepted = true;
                    }
                    else if (c == '8' || c == '9')
                    {
                        base = kBaseFix[base];
                        if (base > 8)
                        {
                            flags &= ~(kSignOk | kPrefixOk | kNoDigits);
                            accepted = true;
                        }
                    }
                    else if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                    {
                        if (base > 10)
                        {
                            flags &= ~(kSignOk | kPrefixOk | kNoDigits);
                            accepted = true;
                        }
                    }
                    else if (c == '+' || c == '-')
                    {
                        if ((flags & kSignOk) != 0)
                        {
                            flags &= ~kSignOk;
                            accepted = true;
                        }
                    }
                    else if (c == 'x' || c == 'X')
                    {
                        if ((flags & kPrefixOk) != 0 && p.get() == (buffer + 1).get())
                        {
                            base = 16;
                            flags &= ~kPrefixOk;
                            accepted = true;
                        }
                    }
                    if (!accepted)
                        break;
                    *p = static_cast<char>(c);
                    ++p;
                    if (!moveOn(fp))
                        break;
                }
                if ((flags & kNoDigits) != 0)
                {
                    if (p.get() > buffer.get())
                    {
                        p -= 1;
                        ungetc(*p, fp);
                    }
                    return assigned;
                }
                c = p[-1];
                if (c == 'x' || c == 'X')
                {
                    p -= 1;
                    ungetc(c, fp);
                }
                if ((flags & kSuppress) == 0)
                {
                    *p = 0;
                    const uint64_t result = convert(buffer, 0u, base);
                    if ((flags & kPointer) != 0)
                        *ee::Ptr<uint32_t>(nextPointer()) = static_cast<uint32_t>(result);
                    else if ((flags & kShort) != 0)
                        *ee::Ptr<int16_t>(nextPointer()) = static_cast<int16_t>(result);
                    else if ((flags & kLong) != 0)
                        *ee::Ptr<uint64_t>(nextPointer()) = result;
                    else
                        *ee::Ptr<int32_t>(nextPointer()) = static_cast<int32_t>(result);
                    ++assigned;
                }
                read += static_cast<int32_t>(p.get() - buffer.get());
                break;
            }

            case kFloat:
            {
                if (--width > kNumberBufferBytes - 2u)
                    width = kNumberBufferBytes - 2u;
                ++width;
                flags |= kSignOk | kNoDigits | kPointOk | kExponentOk;
                ee::Ptr<char> p = buffer;
                for (; width != 0u; --width)
                {
                    c = *fp->position;
                    bool accepted = false;
                    if (c >= '0' && c <= '9')
                    {
                        flags &= ~(kSignOk | kNoDigits);
                        accepted = true;
                    }
                    else if (c == '+' || c == '-')
                    {
                        if ((flags & kSignOk) != 0)
                        {
                            flags &= ~kSignOk;
                            accepted = true;
                        }
                    }
                    else if (c == '.')
                    {
                        if ((flags & kPointOk) != 0)
                        {
                            flags &= ~(kSignOk | kPointOk);
                            accepted = true;
                        }
                    }
                    else if (c == 'e' || c == 'E')
                    {
                        if ((flags & (kNoDigits | kExponentOk)) == kExponentOk)
                        {
                            flags = (flags & ~(kExponentOk | kPointOk)) | kSignOk | kNoDigits;
                            accepted = true;
                        }
                    }
                    if (!accepted)
                        break;
                    *p = static_cast<char>(c);
                    ++p;
                    if (!moveOn(fp))
                        break;
                }
                if ((flags & kNoDigits) != 0)
                {
                    if ((flags & kExponentOk) != 0)
                    {
                        while (p.get() > buffer.get())
                        {
                            p -= 1;
                            ungetc(*p, fp);
                        }
                        return assigned;
                    }
                    // An exponent with no digits: its e, and its sign if it had one, go back.
                    p -= 1;
                    c = *p;
                    if (c != 'e' && c != 'E')
                    {
                        ungetc(c, fp);
                        p -= 1;
                        c = *p;
                    }
                    ungetc(c, fp);
                }
                if ((flags & kSuppress) == 0)
                {
                    *p = 0;
                    const uint64_t result = atof(buffer);
                    if ((flags & (kLong | kLongDouble)) != 0)
                        *ee::Ptr<uint64_t>(nextPointer()) = result;
                    else
                        *ee::Ptr<ee::Float>(nextPointer()) = libgcc::dptofp(result);
                    ++assigned;
                }
                read += static_cast<int32_t>(p.get() - buffer.get());
                break;
            }

            default:
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(__svfscanf, Verified);
}
