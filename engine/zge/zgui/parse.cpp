// zgui's number parsers (readable round gamehand4): the Sint32 and ZFloat
// variable types' +0x40 methods (variable_types.cpp) hand their text to
// these, and the UI scripts' values are read with them.
//
// Both skip spaces, then '{' (AI only), then take one '-' and one '+' (a '+' after
// '-' makes it positive again), then the digits; the float a '.' and the
// fraction's digits. The value is written as it grows (the original's
// stores) and once more with its sign.
//

#include "zge/zgui/widget.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "sym/zge/zgui/parse.cpp.h"

namespace zge::zgui
{
    namespace
    {
        bool isDigit(uint8_t c) { return static_cast<uint8_t>(c - '0') < 10; }
        // The digit's value as the original takes it (the byte sign-extended, less '0').
        int32_t digitOf(uint8_t c) { return static_cast<int32_t>(static_cast<int8_t>(c)) - '0'; }

        // The spaces and then the '{'s at `text`, and the sign: the first
        // character after them.
        ee::Ptr<const char> skipToNumber(ee::Ptr<const char> text, int32_t &sign, bool &negative)
        {
            negative = false;
            while (*text == ' ')
                ++text;
            while (!game::readable::kIsDm2 && *text == '{') // (DM2 takes no '{')
                ++text;
            if (*text == '-')
            {
                ++text;
                sign = -1;
                negative = true;
            }
            if (*text == '+')
            {
                ++text;
                sign = 1;
                negative = false;
            }
            return text;
        }
    }

    // parseFloat: the number at `text` into *value (0 first); returns
    // the text after it, 0 without a value pointer or text.
    ee::Ptr<const char> parseFloat(ee::Ptr<const char> text, ee::Ptr<ee::Float> value)
    {
        if (!value)
            return {};
        *value = 0.0f;
        if (!text)
            return {};
        const ee::Float start = *value; // (read back: 0)
        int32_t sign = 1;
        bool negative;
        ee::Ptr<const char> cursor = skipToNumber(text, sign, negative);
        const ee::Float signValue = negative ? -1.0f : 1.0f;

        // The whole part: value * 10 + digit (as floats).
        ee::Float whole = start;
        if (isDigit(*cursor))
        {
            for (;;)
            {
                const uint8_t c = *cursor;
                ++cursor;
                whole = whole * 10.0f + ee::toFloat(digitOf(c));
                *value = whole;
                if (!isDigit(*cursor))
                    break;
            }
        }

        // The fraction: each digit over 10, 100 ... (divided, nearest).
        if (*cursor != '.')
        {
            *value = whole * signValue;
            return cursor;
        }
        ++cursor;
        ee::Float place = 10.0f;
        if (isDigit(*cursor))
        {
            for (;;)
            {
                const uint8_t c = *cursor;
                ++cursor;
                whole = whole + ee::toFloat(digitOf(c)) / place;
                *value = whole;
                place = place * 10.0f;
                if (!isDigit(*cursor))
                    break;
            }
        }
        *value = whole * signValue;
        return cursor;
    }
    READABLE_FUNCTION(parseFloat, Verified);

    // parseInt: the integer at `text` into *value (0 first); returns
    // the text after it, 0 without a value pointer or text.
    ee::Ptr<const char> parseInt(ee::Ptr<const char> text, ee::Ptr<int32_t> value)
    {
        if (!value)
            return {};
        *value = 0;
        if (!text)
            return {};
        int32_t sign = 1;
        bool negative;
        ee::Ptr<const char> cursor = skipToNumber(text, sign, negative);

        uint32_t number = 0; // (wraps as the original's 32-bit mult and addu)
        if (isDigit(*cursor))
        {
            for (;;)
            {
                const uint8_t c = *cursor;
                number = number * 10u + static_cast<uint32_t>(digitOf(c));
                *value = static_cast<int32_t>(number);
                ++cursor;
                if (!isDigit(*cursor))
                    break;
            }
        }
        *value = static_cast<int32_t>(number * static_cast<uint32_t>(sign));
        return cursor;
    }
    READABLE_FUNCTION(parseInt, Verified);
}
