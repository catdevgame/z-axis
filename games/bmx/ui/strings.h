#pragma once
// BMX XXX's texts (bmx.c, strings.cpp): the string table of the language the
// saved game names, read from the packed library of them, and the texts the
// game formats itself. BMX XXX's own code.

#include "ee/ee.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/ui/strings.h.h"

namespace game::bmx
{
    // The languages (each a string table of the library: "string.common.eus",
    // "euk", "spa", "ger", "fre").
    constexpr uint32_t kLanguages = 5;

    // The string table the game loaded (0 before the first, and after the UI's end).
    inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_gameStrings> g_gameStrings;

    // A string table of the library ("assets\string.zal") by its name, on
    // the heap; none (0) if the library has no such table.
    ee::Ptr<zge::StringTable> loadStringLibrary(const char *name);
    // A loaded table freed.
    void freeGameStrings(ee::Ptr<zge::StringTable> table);
    // The game's language: kept in the saved game, its table loaded in place
    // of the one in use. 0 for a language there is none of, else 1.
    uint32_t setLanguage(uint32_t language);
    // The language a new saved game starts with (0, American English).
    uint32_t defaultLanguage();

    // A text copied onto one line: its line ends become spaces (carriage
    // returns are left out); with a limit, the copy is cut with "..." at the
    // first space or line end past it. The copy's length.
    uint32_t copyOnOneLine(char *out, const char *text, uint32_t limit);

    // A text copied with line breaks put in: once a line has `width`
    // characters, its next space becomes a line break (a backslash and an
    // n, as the string tables write one). What stands between braces is a
    // command: not counted, never broken. How many breaks went in.
    uint32_t wrapText(char *out, const char *text, uint32_t width);

    // A distance (metres) as feet and inches: 3' 7".
    int32_t formatFeetInches(ee::Address out, ee::Float metres);
}
