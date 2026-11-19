#pragma once

// The text rules the level files' names follow. Names are bytes, Latin-1
// (as the tooling reads and writes them), and compared without case the way
// the games' name hash ignores it; numbers in material tags are written as
// the tooling's Python writes them (%g, %d).

#include <cstdint>
#include <string>
#include <string_view>

namespace ps2x::app::levels
{
    // Whether a Latin-1 character is a letter or a digit (Python's
    // str.isalnum for that character).
    bool isAlnumLatin1(uint8_t c);
    // The text upper-cased as Python's str.upper does it (the key the
    // converters tell names apart by).
    std::string upperKey(std::string_view text);
    // The text lower-cased as Python's str.lower does it for Latin-1 (a
    // tag's key), and with the whitespace at its ends cut (str.strip).
    std::string lowerKey(std::string_view text);
    std::string trimmed(std::string_view text);
    // %g of the value (six significant digits).
    std::string formatG(double value);
    // The value rounded to `digits` decimals, correctly (Python's round).
    double roundTo(double value, int digits);
}
