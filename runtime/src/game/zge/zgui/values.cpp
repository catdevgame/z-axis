// zgui's script values (script.h): an operation on two variables of a type.

#include "zge/zgui/script.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "sym/zge/zgui/values.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // The two texts a String operation works on (0x2000 each).
        constexpr ee::Ptr<char> kDestinationText{sym::sharedText};
        constexpr ee::Ptr<char> kSourceText{sym::secondText};

        // .sdata: closer than this is equal (each type's own, all 1e-5).
        inline ee::Global<ee::Float, sym::g_floatEqual> g_floatEqual;
        inline ee::Global<ee::Float, sym::g_vectorEqual> g_vectorEqual;
        inline ee::Global<ee::Float, sym::g_vectorNotEqual> g_vectorNotEqual;
        // A colour's starting value (opaque white).
        inline ee::Global<Color, sym::g_white> g_white;

        // The value read out of a variable by its type, when the type can.
        template <typename Method, typename Out>
        void readValue(ee::Ptr<Node> variable, Method VariableType::*slot, Out out)
        {
            const Method read = variable->variableType.get()->*slot;
            if (read)
                read(variable, out);
        }
        // The result written back into the variable by its type, when the
        // type can. An operation that writes answers 0.
        template <typename Method, typename Value>
        int32_t writeValue(ee::Ptr<Node> variable, Method VariableType::*slot, Value value)
        {
            const Method write = variable->variableType.get()->*slot;
            if (write)
                write(variable, value);
            return 0;
        }

        // The R5900's DIV and DIVU (LO). A zero divisor stops the original
        // at a BREAK; that is not modelled (no script divides by zero).
        int32_t divide(int32_t a, int32_t b)
        {
            if (b == 0)
                return a < 0 ? 1 : -1;
            if (b == -1 && a == INT32_MIN)
                return INT32_MIN;
            return a / b;
        }
        uint8_t divideUnsigned(uint8_t a, uint8_t b) { return static_cast<uint8_t>(b ? a / b : 0xFFu); }

        bool near(ee::Float a, ee::Float b, ee::Float tolerance) { return ee::abs(a - b) < tolerance; }

        // ZGE4's ROUND of a float: to the nearest whole number, a half away
        // from zero (0.5 added or taken, then the fraction cut).
        [[maybe_unused]] ee::Float rounded(ee::Float x)
        {
            const ee::Float half(0.5f);
            return ee::toFloat(ee::toInt(x < ee::Float(0.0f) ? x - half : x + half));
        }

        // Each operation below changes the destination's value and writes it
        // back, or compares the two and answers 1 or 0. An operation the
        // type has no case for writes the value back unchanged.

        int32_t operateIntegers(ee::Ptr<Node> destination, ee::Ptr<Node> source, Operation operation,
                                ee::Ptr<int32_t> value, ee::Ptr<int32_t> operand)
        {
            *operand = 0;
            *value = 0;
            readValue(destination, &VariableType::intValue, value);
            readValue(source, &VariableType::intValue, operand);
            int32_t &a = *value;
            const int32_t b = *operand;
            if constexpr (game::readable::kIsBmx)
                if (operation == Operation::Round) // (a whole number: the source as it is)
                {
                    a = b;
                    return writeValue(destination, &VariableType::setInt, value);
                }
            switch (operation)
            {
            case Operation::Set: a = b; break;
            case Operation::Add: a = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); break;
            case Operation::Subtract: a = static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); break;
            case Operation::Multiply: a = static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); break;
            case Operation::Divide: a = divide(a, b); break;
            case Operation::Equal: return a == b;
            case Operation::LessOrEqual: return a <= b;
            case Operation::Less: return a < b;
            case Operation::GreaterOrEqual: return a >= b;
            case Operation::Greater: return a > b;
            case Operation::NotEqual: return a != b;
            }
            return writeValue(destination, &VariableType::setInt, value);
        }

        int32_t operateFloats(ee::Ptr<Node> destination, ee::Ptr<Node> source, Operation operation,
                              ee::Ptr<ee::Float> value, ee::Ptr<ee::Float> operand)
        {
            *value = 0.0f;
            *operand = 0.0f;
            readValue(destination, &VariableType::floatValue, value);
            readValue(source, &VariableType::floatValue, operand);
            ee::Float &a = *value;
            const ee::Float b = *operand;
            if constexpr (game::readable::kIsBmx)
                if (operation == Operation::Round)
                {
                    a = rounded(b);
                    return writeValue(destination, &VariableType::setFloat, value);
                }
            switch (operation)
            {
            case Operation::Set: a = b; break;
            case Operation::Add: a = a + b; break;
            case Operation::Subtract: a = a - b; break;
            case Operation::Multiply: a = a * b; break;
            case Operation::Divide: a = a / b; break;
            case Operation::Equal: return near(b, a, *g_floatEqual);
            case Operation::LessOrEqual: return a <= b;
            case Operation::Less: return a < b;
            case Operation::GreaterOrEqual: return b <= a;
            case Operation::Greater: return b < a;
            // Not equal is exact: no tolerance, unlike equal.
            case Operation::NotEqual: return !(a == b);
            }
            return writeValue(destination, &VariableType::setFloat, value);
        }

        // Colours work channel by channel, each kept to its byte. Equal
        // compares the four channels, not a palette colour's name; an order
        // holds when it holds in every channel.
        int32_t operateColors(ee::Ptr<Node> destination, ee::Ptr<Node> source, Operation operation,
                              ee::Ptr<Color> value, ee::Ptr<Color> operand)
        {
            const Color white = *g_white;
            *value = white;
            *operand = white;
            readValue(destination, &VariableType::colorValue, value);
            readValue(source, &VariableType::colorValue, operand);
            Color &a = *value;
            const Color &b = *operand;
            const auto everyChannel = [&a, &b](auto holds) {
                return holds(a.r, b.r) && holds(a.g, b.g) && holds(a.b, b.b) && holds(a.a, b.a);
            };
            if constexpr (game::readable::kIsBmx)
                if (operation == Operation::Round) // (whole already: the source's channels, not its palette name)
                {
                    a.r = b.r, a.g = b.g, a.b = b.b, a.a = b.a;
                    return writeValue(destination, &VariableType::setColor, value);
                }
            switch (operation)
            {
            case Operation::Set:
                a.r = b.r, a.g = b.g, a.b = b.b, a.a = b.a;
                a.paletteName = b.paletteName;
                break;
            case Operation::Add:
                a.r = static_cast<uint8_t>(a.r + b.r), a.g = static_cast<uint8_t>(a.g + b.g);
                a.b = static_cast<uint8_t>(a.b + b.b), a.a = static_cast<uint8_t>(a.a + b.a);
                break;
            case Operation::Subtract:
                a.r = static_cast<uint8_t>(a.r - b.r), a.g = static_cast<uint8_t>(a.g - b.g);
                a.b = static_cast<uint8_t>(a.b - b.b), a.a = static_cast<uint8_t>(a.a - b.a);
                break;
            case Operation::Multiply:
                a.r = static_cast<uint8_t>(a.r * b.r), a.g = static_cast<uint8_t>(a.g * b.g);
                a.b = static_cast<uint8_t>(a.b * b.b), a.a = static_cast<uint8_t>(a.a * b.a);
                break;
            case Operation::Divide:
                a.r = divideUnsigned(a.r, b.r), a.g = divideUnsigned(a.g, b.g);
                a.b = divideUnsigned(a.b, b.b), a.a = divideUnsigned(a.a, b.a);
                break;
            case Operation::Equal: return everyChannel([](uint8_t x, uint8_t y) { return x == y; });
            case Operation::LessOrEqual: return everyChannel([](uint8_t x, uint8_t y) { return x <= y; });
            case Operation::Less: return everyChannel([](uint8_t x, uint8_t y) { return x < y; });
            case Operation::GreaterOrEqual: return everyChannel([](uint8_t x, uint8_t y) { return x >= y; });
            case Operation::Greater: return everyChannel([](uint8_t x, uint8_t y) { return x > y; });
            case Operation::NotEqual: return !everyChannel([](uint8_t x, uint8_t y) { return x == y; });
            }
            return writeValue(destination, &VariableType::setColor, value);
        }

        namespace
        {
            constexpr uint32_t kScaleVector = sym::vec3ScaleAxes;
            void memset(ee::Address memory, int32_t byte, uint32_t bytes) { ee::game(zge::kMemset, memory, byte, bytes); }
            void vec3ScaleAxes(ee::Ptr<zge::Vec4> dst, ee::Ptr<zge::Vec4> src, ee::Float sx, ee::Float sy, ee::Float sz) { ee::game(kScaleVector, dst, src, sx, sy, sz); }
        }

        // Vectors compare x, y and z (w is left out); multiply and divide
        // scale x, y and z only.
        int32_t operateVectors(ee::Ptr<Node> destination, ee::Ptr<Node> source, Operation operation,
                               ee::Ptr<zge::Vec4> value, ee::Ptr<zge::Vec4> operand)
        {
            memset(operand, 0, sizeof(zge::Vec4));
            operand->w = 1.0f;
            memset(value, 0, sizeof(zge::Vec4));
            value->w = 1.0f;
            readValue(destination, &VariableType::vectorValue, value);
            readValue(source, &VariableType::vectorValue, operand);
            zge::Vec4 &a = *value;
            const zge::Vec4 b = *operand;
            if constexpr (game::readable::kIsBmx)
                if (operation == Operation::Round) // (all four lanes)
                {
                    a.x = rounded(b.x);
                    a.y = rounded(b.y);
                    a.z = rounded(b.z);
                    a.w = rounded(b.w);
                    return writeValue(destination, &VariableType::setVector, value);
                }
            switch (operation)
            {
            case Operation::Set: a = b; break;
            case Operation::Add: a = a + b; break;
            case Operation::Subtract: a = a - b; break;
            case Operation::Multiply: vec3ScaleAxes(value, value, b.x, b.y, b.z); break;
            case Operation::Divide:
            {
                const ee::Float one = 1.0f;
                vec3ScaleAxes(value, value, one / b.x, one / b.y, one / b.z);
                break;
            }
            case Operation::Equal:
            {
                const ee::Float tolerance = *g_vectorEqual;
                return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
            }
            case Operation::LessOrEqual: return a.x <= b.x && a.y <= b.y && a.z <= b.z;
            case Operation::Less: return a.x < b.x && a.y < b.y && a.z < b.z;
            case Operation::GreaterOrEqual: return b.x <= a.x && b.y <= a.y && b.z <= a.z;
            case Operation::Greater: return b.x < a.x && b.y < a.y && b.z < a.z;
            case Operation::NotEqual:
            {
                const ee::Float tolerance = *g_vectorNotEqual;
                return !(near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance));
            }
            }
            return writeValue(destination, &VariableType::setVector, value);
        }

        // Texts: set copies, add appends, equal and not equal ignore case.
        // Not equal answers the comparison itself (any value but 0 is true).
        // The other operations write the text back unchanged.
        int32_t operateTexts(ee::Ptr<Node> destination, ee::Ptr<Node> source, Operation operation)
        {
            *kDestinationText = 0;
            *kSourceText = 0;
            if constexpr (game::readable::kIsBmx) // (ZGE4 reads the source first)
            {
                readValue(source, &VariableType::toText, kSourceText);
                readValue(destination, &VariableType::toText, kDestinationText);
            }
            else
            {
                readValue(destination, &VariableType::toText, kDestinationText);
                readValue(source, &VariableType::toText, kSourceText);
            }
            switch (operation)
            {
            case Operation::Set: zge::stringCopy(kDestinationText, kSourceText); break;
            case Operation::Add: zge::appendString(kDestinationText, kSourceText); break;
            case Operation::Equal: return zge::stringCompareNoCase(kSourceText, kDestinationText) == 0;
            case Operation::NotEqual: return zge::stringCompareNoCase(kSourceText, kDestinationText);
            default: break;
            }
            return writeValue(destination, &VariableType::setText, kDestinationText);
        }
    }

    // operateVariables: a script's operation on two variables of a type: the
    // destination's value changed by the source's and written back (0), or
    // the two compared (1 when it holds). 0 for a type with no operations.
    int32_t operateVariables(ee::Ptr<Node> destination, ee::Ptr<Node> source, uint32_t type, Operation operation)
    {
        // Each type's two values, where the original keeps them.
        ee::Local<Color> sourceColor;
        ee::Local<Color> destinationColor;
        ee::Local<zge::Vec4> sourceVector;
        ee::Local<zge::Vec4> destinationVector;
        ee::Local<int32_t> destinationInteger;
        ee::Local<int32_t> sourceInteger;
        ee::Local<ee::Float> destinationFloat;
        ee::Local<ee::Float> sourceFloat;
        if (type == *g_boolType || type == *g_sint32Type || type == *g_uint8Type)
            return operateIntegers(destination, source, operation, destinationInteger, sourceInteger);
        if (type == *g_floatType)
            return operateFloats(destination, source, operation, destinationFloat, sourceFloat);
        if (type == *g_colorType)
            return operateColors(destination, source, operation, destinationColor, sourceColor);
        if (type == *g_vectorType)
            return operateVectors(destination, source, operation, destinationVector, sourceVector);
        if (type == *g_stringType || type == *g_eventType || type == *g_assetType)
            return operateTexts(destination, source, operation);
        return 0;
    }
    READABLE_FUNCTION(operateVariables, Verified);
}
