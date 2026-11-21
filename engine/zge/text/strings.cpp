#include "strings.h"

#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/text/strings.cpp.h"

namespace zge
{
    uint32_t nameHash(const char *name)
    {
        // The classic ELF (System V) hash, case-insensitive for ASCII letters.
        uint32_t hash = 0;
        for (const char *c = name; *c;)
        {
            uint8_t letter = static_cast<uint8_t>(*c);
            if (letter >= 'a' && letter <= 'z')
                letter = static_cast<uint8_t>((*g_upperCaseLetters)[letter - 'a']);
            hash = (hash << 4) + letter;
            const uint32_t high = hash & 0xF0000000u;
            if (high)
                hash ^= high >> 24;
            hash &= ~high;
            if (!*++c)
                break;
        }
        return hash;
    }
    READABLE_FUNCTION(nameHash, Verified);

    const char *stringTableText(uint32_t key)
    {
        const StringTable *table = *g_stringTable;
        if (!table)
            return g_emptyString.get();
        // Entries are not sorted: a linear search, first match wins.
        for (uint32_t i = 0; i < table->count; ++i)
        {
            const StringTableEntry &entry = table->entries[i];
            if (entry.key == key)
                return entry.offset == 0 ? g_emptyString.get() : table->textAt(entry.offset);
        }
        return g_emptyString.get();
    }
    READABLE_FUNCTION(stringTableText, Verified);

    namespace
    {
        // A string table's text at an entry's offset (from the table's start).
        ee::Ptr<char> tableText(ee::Ptr<StringTable> table, uint32_t offset) { return table.cast<char>() + offset; }
    }

    // loadStringTable: a copy of a string table on the heap, each "\n" in
    // its texts (a backslash and an n) turned into a carriage return and a
    // line feed. Returns the copy.
    ee::Ptr<StringTable> loadStringTable(ee::Address data, uint32_t bytes)
    {
        const ee::Ptr<StringTable> table(heapAllocate(bytes));
        memoryCopy(table, data, bytes);
        for (uint32_t i = 0; i < table->count; ++i)
        {
            const ee::Ptr<char> text = tableText(table, table->entries[i].offset);
            for (uint32_t at = 0; text[at] != '\0';)
            {
                const char c = text[at++];
                if (c == '\\' && text[at] == 'n')
                {
                    text[at - 1] = '\r';
                    text[at] = '\n';
                    ++at;
                }
            }
        }
        return table;
    }
    READABLE_FUNCTION_WAITS(loadStringTable, Verified);

    // freeStringTable (ZGE4 only): a loaded table given back to the heap; if it was the table in use, none is.
    inline void freeStringTable(ee::Ptr<StringTable> table)
    {
        heapFree(table);
        if (*g_stringTable == table)
            *g_stringTable = ee::Ptr<StringTable>();
    }
    READABLE_FUNCTION_ONLY(bmx, freeStringTable);

    namespace // the game functions below call: ee/plain.h
    {
        // zfile by address: the unit tests link this file without it.
        uint32_t loadFile(uint32_t path, ee::Ptr<uint32_t> bytesOut) { return ee::game<uint32_t>(sym::loadFile, path, bytesOut); }
        void freeLoadedFile(uint32_t data) { ee::game(sym::freeLoadedFile, data); }
    }


    // loadStringTableFile (initGame): a string table read from a file
    // (loadStringTable's copy; the file's own buffer is freed).
    ee::Ptr<StringTable> loadStringTableFile(ee::Address path)
    {
        ee::Local<uint32_t> bytes;
        const uint32_t data = loadFile(path, bytes);
        const ee::Ptr<StringTable> table = zge::loadStringTable(data, *bytes);
        freeLoadedFile(data);
        return table;
    }
    READABLE_FUNCTION_WAITS(loadStringTableFile, Verified);

    namespace
    {
        // What ends a $NAME after a letter of it (tested there); '"' and '\\'
        // end it too, tested at the next letter's turn.
        inline ee::Global<const char, sym::g_textDollar> g_dollar; // "$"

        bool endsKey(char c) { return c == ' ' || c == '\0' || c == '\r' || c == '\n' || c == ':'; }

        // ZGE4's key letters: a letter or digit (the C library's ctype table:
        // _U, _L, _N), '_' or '-'.
        constexpr uint8_t kCtypeLetterOrDigit = 0x01 | 0x02 | 0x04;
        bool isKeyLetter(char c)
        {
            const uint8_t type = ee::Ptr<uint8_t>(sym::ctypeTable)[static_cast<uint32_t>(c + 1)];
            return (type & kCtypeLetterOrDigit) != 0 || c == '_' || c == '-';
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Ptr<char> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    void expandStringKeys(char *out, const char *text)
    {
        if (out)
            out[0] = '\0';
        if (!out || !text)
            return;
        ee::Local<char[64]> name;
        while (*text)
        {
            if (*text != '$')
            {
                *out++ = *text++;
                continue;
            }
            ++text;
            name[0] = '\0';
            memset(ee::Ptr<char>(name) + 1, 0, 63);
            char *nameEnd = &name[0];
            while (!endsKey(*text) && *text != '"' && *text != '\\')
            {
                *nameEnd++ = *text++;
                *nameEnd = '\0';
            }
            const char *found = stringTableText(nameHash(name));
            if (*found)
            {
                stringCopy(out, found);
                out += stringLength(found);
            }
            else
            {
                // An unknown key stays as it was written.
                stringCopy(out, g_dollar.get());
                ++out;
                stringCopy(out, &name[0]);
                out += stringLength(&name[0]);
            }
        }
        *out = '\0';
    }

    // expandNestedStringKeys: ZGE4's expandStringKeys. A key is the letters,
    // digits, '_' and '-' after a '$'; a text found for a key that starts
    // with '~' is expanded in turn (its '~' left out) before it goes in; and a
    // text that starts with '~' leaves that first character out.
    void expandNestedStringKeys(char *out, const char *text)
    {
        if (out)
            out[0] = '\0';
        if (!out || !text)
            return;
        ee::Local<char[64]> name;
        ee::Local<char[0x800]> nested;
        uint32_t index = 0;
        while (text[index] != '\0')
        {
            const char c = text[index];
            if (c != '$')
            {
                if (text[0] != '~' || index != 0)
                    *out++ = c;
                ++index;
                continue;
            }
            ++index;
            name[0] = '\0';
            memset(ee::Ptr<char>(name) + 1, 0, 63);
            char *nameEnd = &name[0];
            while (isKeyLetter(text[index]))
            {
                *nameEnd++ = text[index++];
                *nameEnd = '\0';
            }
            const char *found = stringTableText(nameHash(name));
            if (*found == '\0')
            {
                // An unknown key stays as it was written.
                stringCopy(out, g_dollar.get());
                ++out;
                stringCopy(out, &name[0]);
                out += stringLength(&name[0]);
            }
            else if (*found != '~')
            {
                stringCopy(out, found);
                out += stringLength(found);
            }
            else
            {
                expandStringKeys(&nested[0], found + 1);
                stringCopy(out, &nested[0]);
                out += stringLength(&nested[0]);
            }
        }
        *out = '\0';
    }
    READABLE_FUNCTION_OWN(expandStringKeys, Verified, bmx, expandNestedStringKeys);

    // setStringTable, stringTable: the string table in use set, read.
    void setStringTable(ee::Ptr<StringTable> table) { *g_stringTable = table; }
    READABLE_FUNCTION(setStringTable, Verified);

    ee::Ptr<StringTable> stringTable() { return *g_stringTable; }
    READABLE_FUNCTION(stringTable, Verified);

    namespace
    {
        // The numbered string tables ("STRS"; AI loads none and adds none):
        // a table goes in the slot its first id's top four bits give.
        struct StrsTable
        {
            char magic[4];      // +0x00 "STRS"
            uint16_t firstId;   // +0x04
            uint16_t count;     // +0x06
            uint16_t offsets[1]; // +0x08 [count]: each text from the table's start
        };
        static_assert(offsetof(StrsTable, offsets) == 0x08);
        constexpr uint32_t kStrsSlots = 16;
        constexpr uint32_t kSlotShift = 12;
        // The slot whose texts are "KEY=value" (findStringKey); each text
        // starts one byte before its key.
        constexpr uint32_t kKeySlot = 15;
        inline ee::Global<ee::Ptr<StrsTable>[kStrsSlots], sym::g_stringTables> g_strsTables;
        // translateString's result (0x2C8 bytes, unbounded).
        constexpr uint32_t kTranslated = sym::g_translatedString;
        constexpr char kKeyMark = '=';

        uint32_t slotOf(uint32_t id) { return (id & 0xFFFFu) >> kSlotShift; }
        ee::Ptr<char> textAt(ee::Ptr<StrsTable> table, uint32_t offset) { return table.cast<char>() + offset; }
    }

    // initStringTables (initEngine): every numbered table slot emptied, the
    // last first. Returns 1.
    uint32_t initStringTables()
    {
        for (int32_t slot = kStrsSlots - 1; slot >= 0; --slot)
            (*g_strsTables)[slot] = ee::Ptr<StrsTable>();
        return 1;
    }
    READABLE_FUNCTION(initStringTables, Verified);

    // addStringTable (was validStringTable; no caller in this build): the
    // table put in its slot if it starts "STRS": 1, else 0. (The original
    // also counts up to the table's count and keeps nothing: its loop's
    // body was compiled out.)
    int32_t addStringTable(ee::Ptr<StrsTable> table)
    {
        if (compareStringsN(table, sym::g_textStrs, 4) != 0)
            return 0;
        (*g_strsTables)[table->firstId >> kSlotShift] = table;
        return 1;
    }
    READABLE_FUNCTION(sym::addStringTable, addStringTable, Draft);

    // removeStringTable (no caller in this build): the table's slot emptied
    // if the table is in it: 1, else 0.
    int32_t removeStringTable(ee::Ptr<StrsTable> table)
    {
        ee::Ptr<StrsTable> &slot = (*g_strsTables)[table->firstId >> kSlotShift];
        if (slot != table)
            return 0;
        slot = ee::Ptr<StrsTable>();
        return 1;
    }
    READABLE_FUNCTION(removeStringTable, Draft);

    // stringTableEntry (no caller in this build): the text of string id, 0
    // when its slot has no table or the table has no such id.
    ee::Ptr<char> stringTableEntry(uint32_t id)
    {
        id &= 0xFFFFu;
        const ee::Ptr<StrsTable> table = (*g_strsTables)[slotOf(id)];
        if (!table || id < table->firstId)
            return ee::Ptr<char>();
        if (!(static_cast<int32_t>(id) < static_cast<int32_t>(table->firstId + table->count)))
            return ee::Ptr<char>();
        return textAt(table, (ee::Ptr<uint16_t>(table->offsets))[id - table->firstId]);
    }
    READABLE_FUNCTION(stringTableEntry, Draft);

    // findStringKey (translateString's): the value of key in the key table
    // (its "KEY=value" texts, in order), 0 when it has none or there is no
    // such table. Keys longer than the buffer overrun it, as the original.
    ee::Ptr<char> findStringKey(ee::Ptr<char> key)
    {
        ee::Local<char[0x80]> name;
        const ee::Ptr<StrsTable> table = (*g_strsTables)[kKeySlot];
        if (!table)
            return ee::Ptr<char>();
        for (int32_t i = 0; i < table->count; ++i)
        {
            ee::Ptr<char> at = textAt(table, (ee::Ptr<uint16_t>(table->offsets))[i]) + 1;
            uint32_t length = 0;
            while (*at != 0 && *at != kKeyMark)
            {
                name[length++] = *at;
                at += 1;
            }
            name[length] = 0;
            if (*at == kKeyMark)
                at += 1;
            if (compareStrings(name, key) == 0)
                return at;
        }
        return ee::Ptr<char>();
    }
    READABLE_FUNCTION(findStringKey, Draft);

    // translateString (no caller in this build): the text with each "=KEY="
    // (or "=KEY" at the end) replaced by its value (findStringKey), into the
    // translation buffer (returned as it is when given). A key with no value
    // ends the translation there.
    ee::Ptr<char> translateString(ee::Ptr<const char> text)
    {
        ee::Local<char[0x70]> name;
        ee::Ptr<char> out(kTranslated);
        if (text == ee::Ptr<const char>(kTranslated))
            return out;
        ee::Ptr<const char> at = text;
        while (*at != 0)
        {
            if (*at != kKeyMark)
            {
                *out = *at;
                at += 1;
                out += 1;
                continue;
            }
            at += 1;
            uint32_t length = 0;
            while (*at != 0 && *at != kKeyMark)
            {
                name[length++] = *at;
                at += 1;
            }
            if (*at == kKeyMark)
                at += 1;
            name[length] = 0;
            ee::Ptr<char> value = findStringKey(ee::Ptr<char>(name));
            if (!value)
                break;
            for (; *value != 0; value += 1, out += 1)
                *out = *value;
        }
        *out = 0;
        return ee::Ptr<char>(kTranslated);
    }
    READABLE_FUNCTION(translateString, Draft);

    uint32_t shutdownStringTables()
    {
        return 1;
    }
    READABLE_FUNCTION(shutdownStringTables, Verified);

}
