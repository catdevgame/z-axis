#include "ee/plain.h"
#include "ee/readable.h"
#pragma once

// The engine's wrappers around the C library's memory and string routines
// (zge/text/cstring.cpp): EE addresses in, each hands the newlib routine's
// result back. Engine code calls these, not the library.

#include <cstdint>

namespace zge
{
    uint32_t memoryFill(ee::Address dst, int32_t byte, uint32_t bytes);     // memset
    uint32_t memoryCopy(ee::Address dst, ee::Address src, uint32_t bytes);     // memcpy
    uint32_t stringCopy(ee::Address dst, ee::Address src);                     // strcpy
    uint32_t stringCopyN(ee::Address dst, ee::Address src, uint32_t bytes);    // strncpy
    uint32_t appendString(ee::Address dst, ee::Address src);                   // strcat
    int32_t compareStrings(ee::Address a, ee::Address b);                      // strcmp
    int32_t stringLength(ee::Address text);                                 // strlen
    uint32_t stringFind(ee::Address text, int32_t c);                       // strchr
    uint64_t stringToInt(ee::Address text, uint32_t base);                  // strtol
    uint32_t stringToUpper(ee::Address text);                               // strupr
    uint32_t stringToLower(ee::Address text);                               // strlwr
    int32_t stringCompareNoCase(ee::Address a, ee::Address b);                 // strcasecmp
    int32_t compareStringsN(ee::Address a, ee::Address b, uint32_t bytes);     // strncmp
    int32_t compareStringsNoCaseN(ee::Address a, ee::Address b, uint32_t bytes); // strncasecmp
    ee::Ptr<const char> stringFindLast(ee::Address text, int32_t character); // strrchr
    uint32_t stringFindAny(ee::Address text, ee::Address set);                 // strpbrk
    int32_t stringSpan(ee::Address text, ee::Address set);                     // strspn
    int32_t stringSpanNotIn(ee::Address text, ee::Address set);                // strcspn

    // formatString: sprintf(out, format, ...); formatScratch: sprintf into the
    // next of two 256-byte scratch buffers, the buffer returned (format.cpp).
    // Variadic in the original: a call names the words its format reads
    // (ee::VarArg); the bodies store six (seven) of them for the format.
    int32_t formatString(ee::Address out, ee::Address format, ee::VarArg first = {}, ee::VarArg second = {},
                         ee::VarArg third = {}, ee::VarArg fourth = {}, ee::VarArg fifth = {}, ee::VarArg sixth = {});
    uint32_t formatScratch(ee::Address format, ee::VarArg first = {}, ee::VarArg second = {}, ee::VarArg third = {},
                           ee::VarArg fourth = {}, ee::VarArg fifth = {}, ee::VarArg sixth = {}, ee::VarArg seventh = {});
    // A format that reads more words than the argument registers hold (six after formatString's two, seven after
    // formatScratch's one): the rest are the caller's to store on its stack, where vsprintf reads on. A name of its
    // own, so that formatString names one function (game::tail takes it: ee/readable.h).
    template <typename... More>
    inline int32_t formatStringWithStackWords(ee::Address out, ee::Address format, ee::VarArg first,
                                              ee::VarArg second, ee::VarArg third, ee::VarArg fourth,
                                              ee::VarArg fifth, ee::VarArg sixth, ee::VarArg seventh,
                                              const More &...more)
    {
        ee::storeVariadicStackWords(seventh, more...);
        return formatString(out, format, first, second, third, fourth, fifth, sixth);
    }
    template <typename... More>
    inline uint32_t formatScratchWithStackWords(ee::Address format, ee::VarArg first, ee::VarArg second,
                                                ee::VarArg third, ee::VarArg fourth, ee::VarArg fifth,
                                                ee::VarArg sixth, ee::VarArg seventh, ee::VarArg eighth,
                                                const More &...more)
    {
        ee::storeVariadicStackWords(eighth, more...);
        return formatScratch(format, first, second, third, fourth, fifth, sixth, seventh);
    }
}
