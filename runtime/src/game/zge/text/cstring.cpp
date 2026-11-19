// The engine's wrappers around the C library's memory and string routines
// (memoryFill, memoryCopy in the math block, stringCopy-stringCompareNoCase in the
// text block; DM2 and BMX XXX have them word for word): each calls the
// newlib routine (sce/libc.cpp) and hands back its result. The engine's
// code calls these, not the library.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "sce/libc.h"

#include <cstdint>
#include "sym/zge/text/cstring.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kLibcMemcpy = sym::memcpy;
        constexpr uint32_t kLibcMemset = sym::memset;
        constexpr uint32_t kStringLength = sym::stringLength; // (this file's)
    }

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t memset(uint32_t m, uint32_t c, uint32_t n) { return ee::game<uint32_t>(kLibcMemset, m, c, n); }
    }

    // memoryFill: memset(dst, byte, bytes); returns dst. ZGE4 fills a word
    // at a time when dst and bytes are both multiples of 4 (memset
    // otherwise). Kept bug: its word loop tests the count after the first
    // store, so an aligned fill of 0 bytes never ends.
    uint32_t memoryFill(ee::Address dst, int32_t byte, uint32_t bytes)
    {
        if constexpr (game::readable::kIsBmx)
        {
            const uint32_t to = static_cast<uint32_t>(dst);
            if (((to | bytes) & 3u) == 0)
            {
                const uint32_t b = static_cast<uint32_t>(byte) & 0xFFu;
                const uint32_t word = b << 24 | b | b << 8 | b << 16;
                uint32_t words = bytes >> 2;
                uint32_t at = to;
                do
                {
                    *ee::Ptr<uint32_t>(at) = word;
                    at += 4;
                } while (--words != 0);
                return to;
            }
        }
        return memset(dst, byte, bytes);
    }
    READABLE_FUNCTION(memoryFill, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t memcpy(uint32_t dst0, uint32_t src0, uint32_t len0) { return ee::game<uint32_t>(kLibcMemcpy, dst0, src0, len0); }
    }

    // memoryCopy: memcpy(dst, src, bytes); returns dst.
    uint32_t memoryCopy(ee::Address dst, ee::Address src, uint32_t bytes)
    {
        return memcpy(dst, src, bytes);
    }
    READABLE_FUNCTION(memoryCopy, Verified);

    // memoryCopyAligned (was memoryCompareWords): bytes copied a word at a
    // time when the two addresses and the count are all multiples of 4, a
    // half-word at a time when they are of 2, else a byte at a time
    // (nothing for a count of 0, or above 0x7FFFFFFF). Returns where the
    // copy ended less the count: dst, or dst - bytes when nothing was copied.
    uint32_t memoryCopyAligned(uint32_t dst, uint32_t src, uint32_t bytes)
    {
        const uint32_t alignment = dst | src | bytes;
        if ((alignment & 3u) == 0)
        {
            const uint32_t words = bytes >> 2;
            const ee::Ptr<uint32_t> to(dst), from(src);
            for (uint32_t i = 0; i < words; ++i)
                to[i] = from[i];
            return dst + words * 4u - bytes;
        }
        if ((alignment & 1u) == 0)
        {
            const uint32_t halves = bytes >> 1;
            const ee::Ptr<uint16_t> to(dst), from(src);
            for (uint32_t i = 0; i < halves; ++i)
                to[i] = from[i];
            return dst + halves * 2u - bytes;
        }
        const int32_t count = static_cast<int32_t>(bytes);
        const ee::Ptr<uint8_t> to(dst), from(src);
        for (int32_t i = 0; i < count; ++i)
            to[i] = from[i];
        return dst + (count > 0 ? bytes : 0u) - bytes;
    }
    READABLE_FUNCTION(memoryCopyAligned, Draft);

    // memorySetReturning: memset(dst, value, bytes); returns dst (not
    // memset's result, which is the same).
    uint32_t memorySetReturning(uint32_t dst, uint32_t value, uint32_t bytes)
    {
        memset(dst, value, bytes);
        return dst;
    }
    READABLE_FUNCTION(memorySetReturning, Draft);

    // memoryMove (was memoryCompare): memmove: ranges that do not overlap go
    // to memoryCopyAligned (its result returned); overlapping ones are copied
    // a byte at a time, from the front when dst is below src, from the back
    // when above. Returns dst otherwise.
    uint32_t memoryMove(uint32_t dst, uint32_t src, uint32_t bytes)
    {
        if (bytes == 0)
            return dst;
        const uint32_t dstEnd = dst + bytes;
        const uint32_t srcEnd = src + bytes;
        if (!(src < dstEnd) || !(dst < srcEnd))
            return memoryCopyAligned(dst, src, bytes);
        // (A count above 0x80000000 copies nothing, as the original's
        // signed countdown from bytes - 1 does.)
        const uint32_t count = static_cast<int32_t>(bytes - 1) >= 0 ? bytes : 0u;
        if (dst < src)
        {
            const ee::Ptr<uint8_t> to(dst), from(src);
            for (uint32_t i = 0; i < count; ++i)
                to[i] = from[i];
        }
        else if (src < dst)
        {
            for (uint32_t i = 1; i <= count; ++i)
                *(ee::Ptr<uint8_t>(dstEnd) - i) = *(ee::Ptr<uint8_t>(srcEnd) - i);
        }
        return dst;
    }
    READABLE_FUNCTION(memoryMove, Draft);

    // stringCopy: strcpy(dst, src); returns dst.
    uint32_t stringCopy(ee::Address dst, ee::Address src)
    {
        return sce::libc::strcpy(dst, src);
    }
    READABLE_FUNCTION(stringCopy, Verified);

    // stringFind: strchr(text, c): the first c in text, or 0.
    uint32_t stringFind(ee::Address text, int32_t c)
    {
        return sce::libc::strchr(text, c);
    }
    READABLE_FUNCTION(stringFind, Verified);

    // stringToUpper: strupr(text): text in upper case, in place. Returns text.
    uint32_t stringToUpper(ee::Address text)
    {
        return sce::libc::strupr(text);
    }
    READABLE_FUNCTION(stringToUpper, Verified);

    // stringToLower: strlwr(text): text in lower case, in place. Returns text.
    uint32_t stringToLower(ee::Address text)
    {
        return sce::libc::strlwr(text);
    }
    READABLE_FUNCTION(stringToLower, Verified);

    // stringCompareNoCase: strcasecmp(a, b): the engine's name comparison ignores
    // case.
    int32_t stringCompareNoCase(ee::Address a, ee::Address b)
    {
        return sce::libc::strcasecmp(a, b);
    }
    READABLE_FUNCTION(stringCompareNoCase, Verified);

    // stringCopyN: strncpy(dst, src, bytes); returns dst.
    uint32_t stringCopyN(ee::Address dst, ee::Address src, uint32_t bytes)
    {
        return sce::libc::strncpy(dst, src, bytes);
    }
    READABLE_FUNCTION(stringCopyN, Verified);

    // appendString: strcat(dst, src); returns dst.
    uint32_t appendString(ee::Address dst, ee::Address src)
    {
        return sce::libc::strcat(dst, src);
    }
    READABLE_FUNCTION(appendString, Verified);

    // compareStrings: strcmp(a, b).
    int32_t compareStrings(ee::Address a, ee::Address b)
    {
        return sce::libc::strcmp(a, b);
    }
    READABLE_FUNCTION(compareStrings, Verified);

    // stringLength: strlen(text).
    int32_t stringLength(ee::Address text)
    {
        return static_cast<int32_t>(sce::libc::strlen(text));
    }
    READABLE_FUNCTION(stringLength, Verified);

    // stringToInt: strtol(text, (the end, on the original's stack), base),
    // as a 32-bit value.
    uint64_t stringToInt(ee::Address text, uint32_t base)
    {
        ee::Local<uint8_t[0x20]> ptr;
        const uint64_t value = static_cast<uint64_t>(sce::libc::strtol(text, ptr, static_cast<int32_t>(base)));
        return static_cast<uint64_t>(static_cast<int64_t>(value << 32) >> 32);
    }
    READABLE_FUNCTION(stringToInt, Verified);

    // compareStringsN: strncmp(a, b, bytes).
    int32_t compareStringsN(ee::Address a, ee::Address b, uint32_t bytes)
    {
        return sce::libc::strncmp(a, b, bytes);
    }
    READABLE_FUNCTION(compareStringsN, Verified);

    // compareStringsNoCaseN (no caller in this build): strncasecmp(a, b, bytes).
    int32_t compareStringsNoCaseN(ee::Address a, ee::Address b, uint32_t bytes)
    {
        return sce::libc::strncasecmp(a, b, bytes);
    }
    READABLE_FUNCTION(compareStringsNoCaseN, Verified);

    // stringFindLast (no caller in this build): strrchr(text, character).
    ee::Ptr<const char> stringFindLast(ee::Address text, int32_t character)
    {
        return sce::libc::strrchr(ee::Ptr<const char>(text), character);
    }
    READABLE_FUNCTION(stringFindLast, Verified);

    namespace
    {
        bool inSet(char character, ee::Ptr<char> set, int32_t setLength)
        {
            for (int32_t j = 0; j < setLength; ++j)
                if (character == set[j])
                    return true;
            return false;
        }
    }

    // stringFindAny (no caller in this build): the first character of text
    // that is one of set's (strpbrk), or 0.
    uint32_t stringFindAny(ee::Address text, ee::Address set)
    {
        const int32_t length = stringLength(text);
        const int32_t setLength = stringLength(set);
        const ee::Ptr<char> characters(text), wanted(set);
        for (int32_t i = 0; i < length; ++i)
            if (inSet(characters[i], wanted, setLength))
                return text + i;
        return 0;
    }
    READABLE_FUNCTION(stringFindAny, Verified);

    // stringSpan (no caller in this build): how many of text's first
    // characters are in set (strspn).
    int32_t stringSpan(ee::Address text, ee::Address set)
    {
        const int32_t length = stringLength(text);
        const int32_t setLength = stringLength(set);
        const ee::Ptr<char> characters(text), wanted(set);
        int32_t i = 0;
        while (i < length && inSet(characters[i], wanted, setLength))
            ++i;
        return i;
    }
    READABLE_FUNCTION(stringSpan, Verified);

    // stringSpanNotIn (no caller in this build): how many of text's first
    // characters are not in set (strcspn; its terminator counted in the
    // walk).
    int32_t stringSpanNotIn(ee::Address text, ee::Address set)
    {
        const int32_t length = stringLength(text);
        const int32_t setLength = stringLength(set);
        const ee::Ptr<char> characters(text), unwanted(set);
        // The terminator counts as a character of the text; with no match
        // the answer is one past it.
        int32_t i = 0;
        while (i <= length && !inSet(characters[i], unwanted, setLength))
            ++i;
        return i;
    }
    READABLE_FUNCTION(stringSpanNotIn, Verified);

    namespace
    {
        constexpr uint32_t kNamesHigh = ee::high(sym::g_textVisible);
        constexpr uint32_t kTextRgba2 = sym::g_textRgba2;
        constexpr uint32_t kTextRgba3 = sym::g_textRgba3;
    }

    namespace
    {
        // splitNumberedName's: the first part keeps at most this many
        // characters, and its number starts at a digit 1 to 8.
        constexpr uint32_t kFirstPartMost = 14;
        bool startsNumber(char c) { return static_cast<uint8_t>(c - '1') < 8; }
    }

    // splitNumberedName (was parseBodyName; nothing calls it in AI): a name
    // "first_second" with a number after the second part (it starts at the
    // first digit 1 to 8): the first part's hash (at most 14 characters), the
    // second's, and the number (base 10), which it also returns; -1 when the
    // name has no '_'. Kept bugs: past 14 characters the second part starts
    // at the 16th, not after the '_'; the second part is not bounded by the
    // 16-byte buffer; and its end mark goes one byte late, so the hash takes
    // the byte after it as it was (0 only when the part is empty).
    uint64_t splitNumberedName(uint32_t name, ee::Ptr<uint32_t> firstHash, ee::Ptr<uint32_t> secondHash, ee::Ptr<uint32_t> number)
    {
        ee::Local<char[0x10]> part;
        const ee::Ptr<const char> text(name);
        int32_t underscore = -1;
        for (uint32_t i = 0; i < static_cast<uint32_t>(stringLength(name)); i = (i + 1) & 0xFFFFu)
        {
            if (text[i] == '_')
            {
                underscore = static_cast<int32_t>(i);
                break;
            }
        }
        if (underscore == -1)
            return ~uint64_t{0};
        const uint32_t firstLength = underscore <= static_cast<int32_t>(kFirstPartMost) ? static_cast<uint32_t>(underscore) : kFirstPartMost;
        memoryCopy(part.address(), name, firstLength);
        part[firstLength] = '\0';
        *firstHash = nameHash(&part[0]);
        const uint32_t second = firstLength + 1;
        part[0] = '\0';
        uint32_t length = 0;
        while (!startsNumber(text[second + length]))
        {
            part[length] = text[second + length];
            length = (length + 1) & 0xFFFFu;
        }
        part[length + 1] = '\0';
        *secondHash = nameHash(&part[0]);
        const uint64_t value = stringToInt(name + second + length, 10);
        *number = static_cast<uint32_t>(value);
        return value;
    }
    READABLE_FUNCTION(splitNumberedName, Draft);
}
