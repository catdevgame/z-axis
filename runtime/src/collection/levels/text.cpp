// Text rules of the level files' names (collection/levels/text.h).
#include "collection/levels/text.h"

#include <cstdio>
#include <cstdlib>

namespace ps2x::app::levels
{
    namespace
    {
        // Python's whitespace among the Latin-1 characters.
        bool isSpaceLatin1(uint8_t c)
        {
            return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0;
        }
    }

    bool isAlnumLatin1(uint8_t c)
    {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            return true;
        // letters: ª µ º, À-Ö, Ø-ö, ø-ÿ; digits and numerics: ² ³ ¹ ¼ ½ ¾
        return c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || c >= 0xF8 ||
               c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE);
    }

    std::string upperKey(std::string_view text)
    {
        std::string out;
        for (const char ch : text)
        {
            const uint8_t c = static_cast<uint8_t>(ch);
            if (c >= 'a' && c <= 'z')
                out.push_back(static_cast<char>(c - 32));
            else if (c == 0xDF) // ß
                out += "SS";
            else if (c == 0xFF) // ÿ: U+0178, outside Latin-1
                out += "\x01\x78";
            else if (c == 0xB5) // µ: U+039C
                out += "\x03\x9c";
            else if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
                out.push_back(static_cast<char>(c - 32));
            else
                out.push_back(ch);
        }
        return out;
    }

    std::string lowerKey(std::string_view text)
    {
        std::string out;
        for (const char ch : text)
        {
            const uint8_t c = static_cast<uint8_t>(ch);
            if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
                out.push_back(static_cast<char>(c + 32));
            else
                out.push_back(ch);
        }
        return out;
    }

    std::string trimmed(std::string_view text)
    {
        size_t start = 0, end = text.size();
        while (start < end && isSpaceLatin1(static_cast<uint8_t>(text[start])))
            ++start;
        while (end > start && isSpaceLatin1(static_cast<uint8_t>(text[end - 1])))
            --end;
        return std::string(text.substr(start, end - start));
    }

    std::string formatG(double value)
    {
        char text[64];
        std::snprintf(text, sizeof text, "%g", value);
        return text;
    }

    double roundTo(double value, int digits)
    {
        char text[512];
        std::snprintf(text, sizeof text, "%.*f", digits, value);
        return std::strtod(text, nullptr);
    }
}
