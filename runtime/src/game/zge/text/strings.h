#pragma once

// Text of the ZGE3 engine: the string table and the name hash.
//
// The string table is the language's assets\string.eus (.fre, .ger, ...)
// loaded whole (runtime/ps2_zaxis_text.h reads the same files on the
// host). Strings are found by key, the name hash of the string's name
// ("KSTR_RESUMEGAME" for the UI scripts' "$KSTR_RESUMEGAME").

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sym/zge/text/strings.h.h"

namespace zge
{
    struct StringTableEntry
    {
        uint32_t key;    // nameHash of the string's name
        uint32_t offset; // of the text from the table's start; 0: no text
    };
    static_assert(sizeof(StringTableEntry) == 8);

    struct StringTable
    {
        uint32_t size;  // bytes, as in the file
        uint32_t count; // entries
        StringTableEntry entries[1]; // [count], then the NUL-terminated texts (Latin-1)

        const char *textAt(uint32_t offset) const { return reinterpret_cast<const char *>(this) + offset; }
    };
    static_assert(offsetof(StringTable, entries) == 8);

    // The string table in use, or null before one is loaded.
    inline ee::Global<ee::Ptr<StringTable>, sym::g_stringTable> g_stringTable;
    // The "" every failed lookup returns.
    inline ee::Global<const char, sym::g_emptyString> g_emptyString;
    // 'a'..'z' -> 'A'..'Z' (.rodata), the name hash's case folding.
    inline ee::Global<const char[26], sym::g_upperCaseLetters> g_upperCaseLetters;

    // The same hash at compile time, for a name the code knows (nameHashOf("Spawn")): the classic ELF hash with
    // ASCII letters upper-cased, as nameHash computes it.
    constexpr uint32_t nameHashOf(const char *name)
    {
        uint32_t hash = 0;
        for (const char *c = name; *c; ++c)
        {
            uint32_t letter = static_cast<uint8_t>(*c);
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            hash = (hash << 4) + letter;
            const uint32_t high = hash & 0xF0000000u;
            if (high)
                hash ^= high >> 24;
            hash &= ~high;
        }
        return hash;
    }
    // nameHash: ELF hash of the name with ASCII letters upper-cased.
    uint32_t nameHash(const char *name);
    // stringTableText: the text of the string with this key, "" if there is none.
    const char *stringTableText(uint32_t key);
    // setStringTable: the string table in use.
    void setStringTable(ee::Ptr<StringTable> table);
    // loadStringTable: a copy of a string table on the heap (strings.cpp).
    ee::Ptr<StringTable> loadStringTable(ee::Address data, uint32_t bytes);
    // loadStringTableFile: a string table read from a file (loadStringTable's
    // copy; the file's own buffer is freed) (strings.cpp).
    ee::Ptr<StringTable> loadStringTableFile(ee::Address path);
    // initStringTables (initEngine): every numbered table slot emptied; shutdownStringTables: nothing to do. Both 1.
    uint32_t initStringTables();
    uint32_t shutdownStringTables();
    // freeStringTable (ZGE4 only): a loaded table freed; no table is in use if it was this one.
    void freeStringTable(ee::Ptr<StringTable> table);
    // expandStringKeys: copies text to out with each $NAME replaced by the
    // string table's text for NAME ("$KSTR_RESUMEGAME" -> "Resume Game").
    void expandStringKeys(char *out, const char *text);

    // The engine's libc wrappers (each a jal to the library function).
    constexpr uint32_t kStrcpy = sym::stringCopy; // strcpy
    constexpr uint32_t kStrlen = sym::stringLength; // strlen
    constexpr uint32_t kMemset = sym::memset; // libc memset
}
