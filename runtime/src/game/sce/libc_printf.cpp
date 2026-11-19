// Licence: ported from newlib 1.8.2 (https://sourceware.org/pub/newlib/newlib-1.8.2.tar.gz): stdio/vfprintf.c
// carries the notice below.
// The port is distributed under this project's GPL-3.0 (LICENSE), with these notices
// kept as their licences require; NOTICE lists every third-party source.
//
// Copyright (c) 1990 The Regents of the University of California.
// All rights reserved.
//
// This code is derived from software contributed to Berkeley by
// Chris Torek.
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

// stdio/vfprintf.c, newlib 1.8.2, which the game links twice: built with INTEGER_ONLY as fiprintf's
// formatter (vfiprintf, no floating point), and in full as printf's (vfprintf, with %e, %f and %g
// through cvt, exponent and _dtoa_r). Each build has its own copies of the file's static helpers
// and constant strings; here one body serves both (formatTo, by the build's traits). printf,
// sprintf and their kin are the runtime's, so the full build is dead in the game (Draft).
// DM2's and BMX XXX's are close: the same source, other inlining. An argument takes a 64-bit slot
// on the va_list, and the EE's long is 64 bits; a double travels as its 64-bit pattern.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/newlib.h"
#include "sce/libc.h"
#include "sce/libgcc.h"
#include "sce/libm.h"

#include <cstdint>
#include <type_traits>
#include "sym/sce/libc_printf.cpp.h"

namespace sce::libc
{
    namespace
    {
        inline ee::Global<ee::Ptr<Reent>, sym::g_impurePtr> g_impurePtr;
        inline ee::Global<int32_t, sym::g_mbCurMax> g_mbCurMax;

        // The flags of one conversion.
        constexpr int kAlternate = 0x001, kHexPrefix = 0x002, kLeftAdjust = 0x004, kLongInt = 0x010;
        constexpr int kQuadInt = 0x020, kShortInt = 0x040, kZeroPad = 0x080;
        constexpr int kLongDouble = 0x008, kFloatingPoint = 0x100; // LONGDBL (a double on the EE), FPT

        constexpr int kVectorCount = 8;    // NIOV: pieces gathered before a write
        constexpr int kPadBytes = 16;      // PADSIZE
        constexpr int kDefaultPrecision = 6; // DEFPREC
        constexpr int kMaxExponent = 308;    // MAXEXP (floatio.h)
        constexpr int kMaxFraction = 39;     // MAXFRACT

        enum Base { kNoBase, kOctal, kDecimal, kHex };

        template <typename T>
        ee::Ptr<uint8_t> bytesOf(ee::Ptr<T> text) { return reinterpret_cast<uint8_t *>(const_cast<char *>(text.get())); }

        int32_t nextCharacter(ee::Address wc, ee::Ptr<const char> format, ee::Address state)
        {
            return ee::game<int32_t>(sym::_mbtowc_r, *g_impurePtr, wc, format, *g_mbCurMax, state);
        }
        // vfiprintf and vfprintf by address, handed to printUnbuffered as its printer.
        int32_t vfiprintfOn(ee::Address fp, ee::Ptr<const char> format, uint32_t arguments) { return ee::game<int32_t>(sym::vfiprintf, fp, format, arguments); }
        int32_t vfprintfOn(ee::Address fp, ee::Ptr<const char> format, uint32_t arguments) { return ee::game<int32_t>(sym::vfprintf, fp, format, arguments); }

        // The floating point build's: libm's tests, libgcc's compare and subtract, locale and dtoa.
        constexpr uint64_t kZeroDouble = 0u;

        // __sprint and __sbprintf: each build has its own copy of these static helpers.
        int32_t writeGathered(ee::Ptr<File> fp, ee::Ptr<IoRequest> request)
        {
            if (request->residual == 0)
            {
                request->vectorCount = 0;
                return 0;
            }
            const int32_t error = __sfvwrite(fp, request);
            request->residual = 0;
            request->vectorCount = 0;
            return error;
        }
        template <int32_t (*Formatter)(ee::Address, ee::Ptr<const char>, uint32_t)>
        int32_t printUnbuffered(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
        {
            ee::Local<File> fake;
            ee::Local<uint8_t[kBufferSize]> buffer;
            const ee::Ptr<uint8_t> bytes(buffer.address());
            fake->reent = fp->reent;
            fake->flags = fp->flags & ~kUnbuffered;
            fake->descriptor = fp->descriptor;
            fake->cookie = fp->cookie;
            fake->writeFunction = fp->writeFunction;
            fake->bufferBase = bytes;
            fake->position = bytes;
            fake->bufferSize = kBufferSize;
            fake->writeSpace = kBufferSize;
            fake->lineBufferSize = 0;
            int32_t result = Formatter(fake, format, arguments);
            if (result >= 0 && fflush(fake) != 0)
                result = kEof;
            if ((fake->flags & kHadError) != 0)
                fp->flags |= kHadError;
            return result;
        }
    }

    // __sprint: writes what the request has gathered, and empties it.
    int32_t __sprint2(ee::Ptr<File> fp, ee::Ptr<IoRequest> request)
    {
        return writeGathered(fp, request);
    }
    READABLE_FUNCTION_WAITS(__sprint2, Verified);

    int32_t __sprint(ee::Ptr<File> fp, ee::Ptr<IoRequest> request)
    {
        return writeGathered(fp, request);
    }
    READABLE_FUNCTION_WAITS(__sprint, Verified);

    // __sbprintf: printing to an unbuffered file goes through a fake buffered FILE on the stack, so a
    // line reaches it in one write.
    int32_t __sbprintf2(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        return printUnbuffered<vfiprintfOn>(fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(__sbprintf2, Verified);

    int32_t __sbprintf(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        return printUnbuffered<vfprintfOn>(fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(__sbprintf, Draft);

    // cvt: value's digits for %e, %f or %g from _dtoa_r: ndigits of them after the point for %f
    // (mode 3), that many significant ones for %g and one more for %e (mode 2, the digit before the
    // point and ndigits after). The sign goes to *sign, the decimal exponent to *decpt and the digit
    // count to *length; the digits are padded with zeroes to the precision unless %g without #.
    ee::Ptr<char> cvt(ee::Ptr<Reent> data, uint64_t value, int32_t ndigits, int32_t flags, ee::Ptr<char> sign, ee::Ptr<int32_t> decpt,
                       int32_t ch, ee::Ptr<int32_t> length)
    {
        ee::Local<int32_t> dsgn;
        ee::Local<ee::Ptr<char>> rve;
        int32_t mode;
        if (ch == 'f')
            mode = 3;
        else
        {
            if (ch == 'e' || ch == 'E')
                ++ndigits;
            mode = 2;
        }
        if ((value >> 63) != 0u) // the sign bit: below zero, and -0.0
        {
            value = libgcc::dpsub(kZeroDouble, value);
            *sign = '-';
        }
        else
            *sign = '\0';
        const ee::Ptr<char> digits = _dtoa_r(data, value, mode, ndigits, decpt, dsgn, rve);
        if ((ch != 'g' && ch != 'G') || (flags & kAlternate) != 0) // print trailing zeroes
        {
            ee::Ptr<char> bp = digits + ndigits;
            if (ch == 'f')
            {
                if (*digits == '0' && libgcc::dpcmp(value, kZeroDouble) != 0)
                    *decpt = -ndigits + 1;
                bp += *decpt;
            }
            if (libgcc::dpcmp(value, kZeroDouble) == 0) // kludge for __dtoa irregularity
                *rve = bp;
            while ((*rve).get() < bp.get())
            {
                **rve = '0';
                ++*rve;
            }
        }
        *length = static_cast<int32_t>((*rve).get() - digits.get());
        return digits;
    }
    READABLE_FUNCTION(cvt, Verified);

    // exponent: %e's exponent written at p0: the letter, the sign and at least two digits. Its
    // length.
    int32_t exponent(ee::Ptr<char> p0, int32_t exp, int32_t fmtch)
    {
        ee::Local<char[kMaxExponent]> expbuf;
        const ee::Ptr<char> end = ee::Ptr<char>(expbuf) + kMaxExponent;
        ee::Ptr<char> p = p0;
        *p = static_cast<char>(fmtch);
        ++p;
        if (exp < 0)
        {
            exp = -exp;
            *p = '-';
        }
        else
            *p = '+';
        ++p;
        ee::Ptr<char> t = end;
        if (exp > 9)
        {
            do
            {
                t -= 1;
                *t = static_cast<char>(exp % 10 + '0');
            } while ((exp /= 10) > 9);
            t -= 1;
            *t = static_cast<char>(exp + '0');
            for (; t.get() < end.get(); ++t, ++p)
                *p = *t;
        }
        else
        {
            *p = '0';
            ++p;
            *p = static_cast<char>(exp + '0');
            ++p;
        }
        return static_cast<int32_t>(p.get() - p0.get());
    }
    READABLE_FUNCTION(exponent, Verified);

    namespace
    {
        // What differs between the two builds of vfprintf.c: the floating point, the size of the
        // number buffer (BUF), and the copies of the static helpers and constant strings.
        struct IntegerOnlyBuild
        {
            static constexpr bool kFloats = false;
            static constexpr int kNumberBytes = 40;
            static constexpr uint32_t kBlanks = sym::g_printfBlanks2, kZeroes = sym::g_printfZeroes2;
            static constexpr uint32_t kLowerDigits = sym::g_text0123456789abcdef2, kUpperDigits = sym::g_text0123456789abcdef4;
            static constexpr uint32_t kNullText = sym::g_textNull2;
            static int32_t sprint(ee::Ptr<File> fp, ee::Ptr<IoRequest> request) { return __sprint2(fp, request); }
            static int32_t printUnbuffered(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments) { return __sbprintf2(fp, format, arguments); }
        };
        struct FloatingPointBuild
        {
            static constexpr bool kFloats = true;
            static constexpr int kNumberBytes = kMaxExponent + kMaxFraction + 1; // and the decimal point
            static constexpr uint32_t kBlanks = sym::g_printfBlanks, kZeroes = sym::g_printfZeroes;
            static constexpr uint32_t kLowerDigits = sym::g_text0123456789abcdef, kUpperDigits = sym::g_text0123456789abcdef3;
            static constexpr uint32_t kNullText = sym::g_textNull;
            static int32_t sprint(ee::Ptr<File> fp, ee::Ptr<IoRequest> request) { return __sprint(fp, request); }
            static int32_t printUnbuffered(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments) { return __sbprintf(fp, format, arguments); }
        };
        struct NotInThisBuild {};
        // A local only the floating point build has.
        template <typename Build, typename T>
        using FloatLocal = std::conditional_t<Build::kFloats, ee::Local<T>, NotInThisBuild>;
    }

    // _vfprintf_r, either build: the format's text, and each conversion padded to its field.
    template <typename Build>
    int32_t formatTo(ee::Ptr<Reent> data, ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        ee::Local<IoRequest> request;
        ee::Local<IoVector[kVectorCount]> vectors;
        ee::Local<char[Build::kNumberBytes]> number;
        ee::Local<char[2]> prefix;
        ee::Local<char> sign;
        ee::Local<int32_t> wc;
        ee::Local<int32_t> state;
        FloatLocal<Build, char> softsign;      // the sign cvt found
        FloatLocal<Build, int32_t> expt;       // the decimal exponent
        FloatLocal<Build, int32_t> ndig;       // the digits cvt gave
        FloatLocal<Build, char[7]> expstr;     // %e's exponent
        const ee::Ptr<char> numberStart(number.address());
        const ee::Ptr<char> numberEnd = numberStart + Build::kNumberBytes;
        ee::Ptr<const char> decimalPoint;
        if constexpr (Build::kFloats)
            decimalPoint = localeconv()->decimalPoint;

        *state = 0;
        if (((fp->flags & kCanWrite) == 0 || !fp->bufferBase) && __swsetup(fp) != 0)
            return kEof;
        if ((fp->flags & (kUnbuffered | kCanWrite | kReadWrite)) == (kUnbuffered | kCanWrite) && fp->descriptor >= 0)
            return Build::printUnbuffered(fp, format, arguments);

        ee::Ptr<IoVector> vector(vectors.address());
        request->vectors = vector;
        request->residual = 0;
        request->vectorCount = 0;
        int32_t result = 0;

        const auto finished = [&]() { return (fp->flags & kHadError) != 0 ? kEof : result; };
        // PRINT: gathers a piece. False when the write that makes room fails.
        const auto print = [&](ee::Ptr<uint8_t> base, int32_t length) {
            vector->base = base;
            vector->length = static_cast<uint32_t>(length);
            request->residual += length;
            ++vector;
            request->vectorCount += 1;
            if (request->vectorCount >= kVectorCount)
            {
                if (Build::sprint(fp, request) != 0)
                    return false;
                vector = ee::Ptr<IoVector>(vectors.address());
            }
            return true;
        };
        // PAD: count blanks or zeroes, a block of 16 at a time.
        const auto pad = [&](int32_t count, ee::Ptr<uint8_t> with) {
            if (count <= 0)
                return true;
            for (; count > kPadBytes; count -= kPadBytes)
            {
                if (!print(with, kPadBytes))
                    return false;
            }
            return print(with, count);
        };
        // FLUSH
        const auto flush = [&]() {
            if (request->residual != 0 && Build::sprint(fp, request) != 0)
                return false;
            request->vectorCount = 0;
            vector = ee::Ptr<IoVector>(vectors.address());
            return true;
        };
        const auto nextWord = [&arguments]() {
            const uint64_t word = *ee::Ptr<uint64_t>(arguments);
            arguments += sizeof(uint64_t);
            return word;
        };
        // SARG and UARG as the library was built, without long long (_NO_LONGLONG): %q and %ll set
        // QUADINT, which nothing reads, so they take an int as a plain %d does. A long is 64 bits.
        const auto signedArgument = [&](int flags) -> int64_t {
            const uint64_t word = nextWord();
            if ((flags & kLongInt) != 0)
                return static_cast<int64_t>(word);
            if ((flags & kShortInt) != 0)
                return static_cast<int16_t>(word);
            return static_cast<int32_t>(word);
        };
        const auto unsignedArgument = [&](int flags) -> uint64_t {
            const uint64_t word = nextWord();
            if ((flags & kLongInt) != 0)
                return word;
            if ((flags & kShortInt) != 0)
                return static_cast<uint16_t>(word);
            return static_cast<uint32_t>(word);
        };
        const auto isDigit = [](int c) { return static_cast<uint32_t>(c - '0') <= 9u; };
        const ee::Ptr<uint8_t> kBlanks(Build::kBlanks), kZeroes(Build::kZeroes);

        for (;;)
        {
            // The text up to the next %.
            const ee::Ptr<const char> text = format;
            int32_t n;
            for (;;)
            {
                n = nextCharacter(wc, format, state);
                if (n <= 0)
                    break;
                format += n;
                if (*wc == '%')
                {
                    format -= 1;
                    break;
                }
            }
            const int32_t textLength = static_cast<int32_t>(format.get() - text.get());
            if (textLength != 0)
            {
                if (!print(bytesOf(text), textLength))
                    return finished();
                result += textLength;
            }
            if (n <= 0)
            {
                flush();
                return finished();
            }
            ++format;

            // The conversion: flags, width and precision, then its letter.
            int flags = 0;
            int32_t dprec = 0, width = 0, prec = -1;
            *sign = '\0';
            uint64_t value = 0u;
            Base base = kNoBase;
            bool isNumber = false;
            uint32_t digits = Build::kLowerDigits;
            uint64_t doubleValue = 0u;  // %e, %f, %g's argument
            int32_t exponentSize = 0;   // expsize
            ee::Ptr<char> cp;
            int32_t size = 0;
            bool storedCount = false;
            int ch = *format;
            ++format;
            const auto nextFormat = [&format, &ch]() {
                ch = *format;
                ++format;
            };
            for (bool reading = true; reading;)
            {
                reading = false;
                switch (ch)
                {
                case ' ':
                    if (*sign == '\0')
                        *sign = ' ';
                    reading = true;
                    break;
                case '#':
                    flags |= kAlternate;
                    reading = true;
                    break;
                case '*':
                    width = static_cast<int32_t>(nextWord());
                    if (width < 0)
                    {
                        width = -width;
                        flags |= kLeftAdjust;
                    }
                    reading = true;
                    break;
                case '-':
                    flags |= kLeftAdjust;
                    reading = true;
                    break;
                case '+':
                    *sign = '+';
                    reading = true;
                    break;
                case '.':
                    nextFormat();
                    if (ch == '*')
                    {
                        const int32_t given = static_cast<int32_t>(nextWord());
                        prec = given < 0 ? -1 : given;
                        reading = true;
                        break;
                    }
                    {
                        int32_t precision = 0;
                        while (isDigit(ch))
                        {
                            precision = 10 * precision + ch - '0';
                            nextFormat();
                        }
                        prec = precision < 0 ? -1 : precision;
                    }
                    reading = true;
                    continue;
                case '0':
                    flags |= kZeroPad;
                    reading = true;
                    break;
                case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    width = 0;
                    do
                    {
                        width = 10 * width + ch - '0';
                        nextFormat();
                    } while (isDigit(ch));
                    reading = true;
                    continue;
                case 'h':
                    flags |= kShortInt;
                    reading = true;
                    break;
                case 'l':
                    if (*format == 'l')
                    {
                        ++format;
                        flags |= kQuadInt;
                    }
                    else
                        flags |= kLongInt;
                    reading = true;
                    break;
                case 'q':
                    flags |= kQuadInt;
                    reading = true;
                    break;
                case 'c':
                    numberStart[0] = static_cast<char>(nextWord());
                    cp = numberStart;
                    size = 1;
                    *sign = '\0';
                    break;
                case 'D':
                    flags |= kLongInt;
                    [[fallthrough]];
                case 'd':
                case 'i':
                {
                    const int64_t signedValue = signedArgument(flags);
                    value = static_cast<uint64_t>(signedValue);
                    if (signedValue < 0)
                    {
                        value = 0u - value;
                        *sign = '-';
                    }
                    base = kDecimal;
                    isNumber = true;
                    break;
                }
                case 'n':
                {
                    const uint32_t to = static_cast<uint32_t>(nextWord());
                    if ((flags & kLongInt) != 0) // (not QUADINT, as above)
                        *ee::Ptr<int64_t>(to) = result;
                    else if ((flags & kShortInt) != 0)
                        *ee::Ptr<int16_t>(to) = static_cast<int16_t>(result);
                    else
                        *ee::Ptr<int32_t>(to) = result;
                    storedCount = true;
                    break;
                }
                case 'O':
                    flags |= kLongInt;
                    [[fallthrough]];
                case 'o':
                    value = unsignedArgument(flags);
                    base = kOctal;
                    *sign = '\0';
                    isNumber = true;
                    break;
                case 'p':
                    value = static_cast<uint32_t>(nextWord());
                    base = kHex;
                    digits = Build::kLowerDigits;
                    flags |= kHexPrefix;
                    ch = 'x';
                    *sign = '\0';
                    isNumber = true;
                    break;
                case 's':
                    cp = ee::Ptr<char>(static_cast<uint32_t>(nextWord()));
                    if (!cp)
                        cp = ee::Ptr<char>(Build::kNullText);
                    if (prec >= 0)
                    {
                        const ee::Ptr<char> end(memchr(cp, 0, static_cast<uint32_t>(prec)));
                        if (end)
                        {
                            size = static_cast<int32_t>(end.get() - cp.get());
                            if (size > prec)
                                size = prec;
                        }
                        else
                            size = prec;
                    }
                    else
                        size = static_cast<int32_t>(strlen(cp));
                    *sign = '\0';
                    break;
                case 'U':
                    flags |= kLongInt;
                    [[fallthrough]];
                case 'u':
                    value = unsignedArgument(flags);
                    base = kDecimal;
                    *sign = '\0';
                    isNumber = true;
                    break;
                case 'X':
                case 'x':
                    digits = ch == 'X' ? Build::kUpperDigits : Build::kLowerDigits;
                    value = unsignedArgument(flags);
                    base = kHex;
                    if ((flags & kAlternate) != 0 && value != 0u)
                        flags |= kHexPrefix;
                    *sign = '\0';
                    isNumber = true;
                    break;
                case 'e':
                case 'E':
                case 'f':
                case 'g':
                case 'G':
                    if constexpr (Build::kFloats)
                    {
                        if (prec == -1)
                            prec = kDefaultPrecision;
                        else if ((ch == 'g' || ch == 'G') && prec == 0)
                            prec = 1;
                        doubleValue = nextWord(); // with L or without: the EE's long double is a double
                        // do this before tricky precision changes
                        if (libm::isinf(doubleValue) != 0)
                        {
                            if (libgcc::dpcmp(doubleValue, kZeroDouble) < 0)
                                *sign = '-';
                            cp = ee::Ptr<char>(sym::g_textInf);
                            size = 3;
                            break;
                        }
                        if (libm::isnan(doubleValue) != 0)
                        {
                            cp = ee::Ptr<char>(sym::g_textNaN);
                            size = 3;
                            break;
                        }
                        flags |= kFloatingPoint;
                        cp = cvt(data, doubleValue, prec, flags, softsign, expt, ch, ndig);
                        if (ch == 'g' || ch == 'G')
                        {
                            if (*expt <= -4 || *expt > prec)
                                ch = ch == 'g' ? 'e' : 'E';
                            else
                                ch = 'g';
                        }
                        if (ch <= 'e') // 'e' or 'E'
                        {
                            --*expt;
                            exponentSize = exponent(expstr, *expt, ch);
                            size = exponentSize + *ndig;
                            if (*ndig > 1 || (flags & kAlternate) != 0)
                                ++size;
                        }
                        else if (ch == 'f')
                        {
                            if (*expt > 0)
                            {
                                size = *expt;
                                if (prec != 0 || (flags & kAlternate) != 0)
                                    size += prec + 1;
                            }
                            else // "0.X"
                                size = prec + 2;
                        }
                        else if (*expt >= *ndig) // fixed %g
                        {
                            size = *expt;
                            if ((flags & kAlternate) != 0)
                                ++size;
                        }
                        else
                            size = *ndig + (*expt > 0 ? 1 : 2 - *expt);
                        if (*softsign != '\0')
                            *sign = '-';
                        break;
                    }
                    [[fallthrough]];
                case 'L':
                    if constexpr (Build::kFloats)
                    {
                        flags |= kLongDouble;
                        reading = true;
                        break;
                    }
                    [[fallthrough]];
                default:
                    if (ch == '\0')
                    {
                        flush();
                        return finished();
                    }
                    numberStart[0] = static_cast<char>(ch);
                    cp = numberStart;
                    size = 1;
                    *sign = '\0';
                    break;
                }
                if (reading)
                    nextFormat();
            }
            if (storedCount)
                continue;

            if (isNumber)
            {
                dprec = prec;
                if (dprec >= 0)
                    flags &= ~kZeroPad;
                cp = numberEnd;
                if (value != 0u || prec != 0)
                {
                    const ee::Ptr<char> digitOf(digits);
                    const auto put = [&cp](char c) {
                        cp -= 1;
                        *cp = c;
                    };
                    switch (base)
                    {
                    case kOctal:
                        do
                        {
                            put(static_cast<char>((value & 7u) + '0'));
                            value >>= 3;
                        } while (value != 0u);
                        if ((flags & kAlternate) != 0 && *cp != '0')
                            put('0');
                        break;
                    case kDecimal:
                        while (value >= 10u)
                        {
                            put(static_cast<char>(libgcc::__umoddi3(value, 10u) + '0'));
                            value = libgcc::__udivdi3(value, 10u);
                        }
                        put(static_cast<char>(value + '0'));
                        break;
                    default:
                        do
                        {
                            put(digitOf[value & 15u]);
                            value >>= 4;
                        } while (value != 0u);
                        break;
                    }
                }
                size = static_cast<int32_t>(numberEnd.get() - cp.get());
            }

            // The field: padding, sign or prefix, zeroes, the text, padding.
            int32_t realSize = dprec > size ? dprec : size;
            if (*sign != '\0')
                ++realSize;
            else if ((flags & kHexPrefix) != 0)
                realSize += 2;
            if ((flags & (kLeftAdjust | kZeroPad)) == 0 && !pad(width - realSize, kBlanks))
                return finished();
            if (*sign != '\0')
            {
                if (!print(ee::Ptr<uint8_t>(sign.address()), 1))
                    return finished();
            }
            else if ((flags & kHexPrefix) != 0)
            {
                prefix[0] = '0';
                prefix[1] = static_cast<char>(ch);
                if (!print(ee::Ptr<uint8_t>(prefix.address()), 2))
                    return finished();
            }
            if ((flags & (kLeftAdjust | kZeroPad)) == kZeroPad && !pad(width - realSize, kZeroes))
                return finished();
            if (!pad(dprec - size, kZeroes))
                return finished();
            if constexpr (Build::kFloats)
            {
                if ((flags & kFloatingPoint) != 0)
                {
                    // Glue the number together from cvt's digits, the point and zeroes.
                    const ee::Ptr<uint8_t> zero(sym::g_textZeroDigit), point(sym::g_textPoint);
                    const ee::Ptr<uint8_t> localePoint = bytesOf(decimalPoint);
                    const auto printFloat = [&]() {
                        if (ch >= 'f') // 'f' or 'g'
                        {
                            if (libgcc::dpcmp(doubleValue, kZeroDouble) == 0) // kludge for __dtoa irregularity
                            {
                                if (!print(zero, 1))
                                    return false;
                                if (*expt < *ndig || (flags & kAlternate) != 0)
                                    return print(localePoint, 1) && pad(*ndig - 1, kZeroes);
                                return true;
                            }
                            if (*expt <= 0)
                                return print(zero, 1) && print(localePoint, 1) && pad(-*expt, kZeroes) && print(bytesOf(cp), *ndig);
                            if (*expt >= *ndig)
                                return print(bytesOf(cp), *ndig) && pad(*expt - *ndig, kZeroes) && ((flags & kAlternate) == 0 || print(point, 1));
                            return print(bytesOf(cp), *expt) && print(point, 1) && print(bytesOf(cp + *expt), *ndig - *expt);
                        }
                        // 'e' or 'E'
                        if (*ndig > 1 || (flags & kAlternate) != 0)
                        {
                            prefix[0] = *cp;
                            prefix[1] = '.';
                            if (!print(ee::Ptr<uint8_t>(prefix.address()), 2))
                                return false;
                            // Bug kept: the source's `_double || flags & ALT == 0` reads as
                            // `flags & (ALT == 0)`, which is 0, so only the value decides: a zero
                            // prints zeroes for its digits, even with #.
                            if (libgcc::dpcmp(doubleValue, kZeroDouble) != 0)
                            {
                                if (!print(bytesOf(cp + 1), *ndig - 1))
                                    return false;
                            }
                            else if (!pad(*ndig - 1, kZeroes)) // 0.[0..], a __dtoa irregularity
                                return false;
                        }
                        else if (!print(bytesOf(cp), 1)) // XeYYY
                            return false;
                        return print(ee::Ptr<uint8_t>(expstr.address()), exponentSize);
                    };
                    if (!printFloat())
                        return finished();
                }
                else if (!print(bytesOf(cp), size))
                    return finished();
            }
            else if (!print(bytesOf(cp), size))
                return finished();
            if ((flags & kLeftAdjust) != 0 && !pad(width - realSize, kBlanks))
                return finished();
            result += width > realSize ? width : realSize;
            if (!flush())
                return finished();
        }
    }

    int32_t _vfiprintf_r(ee::Ptr<Reent> data, ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        return formatTo<IntegerOnlyBuild>(data, fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(_vfiprintf_r, Verified);

    int32_t _vfprintf_r(ee::Ptr<Reent> data, ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        return formatTo<FloatingPointBuild>(data, fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(_vfprintf_r, Verified);

    void __sinit(ee::Ptr<Reent> reent); // (libc_stdio.cpp)

    namespace
    {
        // CHECK_INIT (stdio/local.h): the FILE's _reent, _impure_ptr's if it has none, set up before it prints.
        void checkInit(ee::Ptr<File> fp)
        {
            if (!fp->reent)
                fp->reent = *g_impurePtr;
            if (fp->reent->didInit == 0)
                __sinit(fp->reent);
        }
    }

    int32_t vfiprintf(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        checkInit(fp);
        return _vfiprintf_r(fp->reent, fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(vfiprintf, Verified);

    // vfprintf: _vfprintf_r on the FILE's own _reent.
    int32_t vfprintf(ee::Ptr<File> fp, ee::Ptr<const char> format, uint32_t arguments)
    {
        checkInit(fp);
        return _vfprintf_r(fp->reent, fp, format, arguments);
    }
    READABLE_FUNCTION_WAITS(vfprintf, Verified);
}
