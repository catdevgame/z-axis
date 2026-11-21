// The UI scripts' variable types' methods: the VariableType tables (.data
// sint32Type "Sint32", alignmentType "Alignment" with Sint32's methods,
// zFloatType "ZFloat", zRgbaPalType "ZRGBA_PAL", the vector type after it,
// and String, Event and ZAssetID) point at these. A Variable node's value is
// where its type keeps it (Node::valueAs). Called through the tables by
// setVariable, copyVariable, variableText and operateVariables (variables.cpp,
// values.cpp), which read no result but toText's.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/heaps.h"
#include "zge/text/cstring.h"
#include "sce/libgcc.h"

#include <cstring>
#include "sym/zge/zgui/variable_types.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // The palette's named colours (a list; the UI loads them).
        struct PaletteColour
        {
            ee::Ptr<PaletteColour> next;     // +0x00 (the palette is a zge::List)
            ee::Ptr<PaletteColour> previous; // +0x04
            uint8_t rgba[4];                 // +0x08
            uint32_t padding;                // +0x0C never written: the 64-bit name's alignment
            uint64_t name;                   // +0x10 up to eight characters, packed
        };
        static_assert(offsetof(PaletteColour, rgba) == 0x08 && offsetof(PaletteColour, name) == 0x10);
        inline ee::Global<ee::Ptr<PaletteColour>, sym::g_palette> g_palette;

        template <typename T>
        T &valueOf(ee::Ptr<Node> variable)
        {
            return *variable->valueAs<T>();
        }

        bool isDigit(char c) { return static_cast<uint8_t>(c - '0') < 10; }

        // A palette colour's name as findPaletteColour takes it: the first
        // eight characters of `text`, each sign-extended into the word (a
        // character above 0x7F fills the bytes above it), into `name` as they
        // are read.
        void packPaletteName(uint64_t &name, ee::Ptr<const char> text)
        {
            if (*text == 0)
                return;
            for (uint32_t i = 0;;)
            {
                const int64_t character = static_cast<int8_t>(text[i]);
                name = name | (static_cast<uint64_t>(character) << (i * 8u));
                ++i;
                if (i >= 8 || text[i] == 0)
                    break;
            }
        }
    }

    // findPaletteColour: the palette colour called `name` copied to `out`;
    // 0 if there is none.
    int32_t findPaletteColour(uint64_t name, ee::Ptr<uint8_t> out)
    {
        for (ee::Ptr<PaletteColour> colour = *g_palette; colour; colour = colour->next)
        {
            if (colour->name == name)
            {
                std::memcpy(out.get(), colour->rgba, 4);
                return 1;
            }
            if (!colour->next)
                break;
        }
        return 0;
    }
    // Tested: the recorded calls (9) all met an empty palette (2 of 6 blocks).
    READABLE_FUNCTION(findPaletteColour, Verified);

    namespace
    {
        constexpr uint32_t kDecimalFormat = sym::decimalFormat2; // "%d"
        // A Bool set from a number is false within this of 0.
        inline ee::Global<ee::Float, sym::g_boolZeroTolerance> g_boolZeroTolerance; // 1e-5

        // A number type's toText: the integer as "%d" into out. 1.
        uint32_t formatDecimal(ee::Ptr<char> out, int32_t value)
        {
            zge::formatString(out, kDecimalFormat, static_cast<uint64_t>(static_cast<int64_t>(value)));
            return 1;
        }

        constexpr uint32_t kFloatFormat = sym::g_zfloatTextFormat;               // "%f"
        constexpr uint32_t kPaletteNameFormat = sym::g_paletteNameFormat;         // "%s"
        constexpr uint32_t kPaletteChannelsFormat = sym::g_paletteChannelsFormat; // "%d %d %d %d"
        constexpr uint32_t kVectorFormat = sym::g_vectorTextFormat;               // "%f %f %f"
        constexpr uint32_t kQuaternionFormat = sym::g_quaternionTextFormat;       // "%f %f %f %f"

        // A colour channel (0 .. 255) as a 0 .. 1 component.
        constexpr ee::Float kChannelToUnit = ee::Float(0.003921569f); // 1 / 255
        // A number as a colour channel: anything above 255 is 255; a negative
        // number is not clamped and keeps its low byte.
        uint8_t channelOf(int32_t value) { return static_cast<uint8_t>(value > 255 ? 255 : value); }

        // A 0 .. 1 component as a colour channel: times 255, at most 255,
        // truncated (a negative one is not clamped and keeps its low byte).
        uint8_t channelOfUnit(ee::Float value)
        {
            return static_cast<uint8_t>(ee::toInt(ee::min(value * ee::Float(255.0f), ee::Float(255.0f))));
        }

        // The palette colour called `name` into `rgba` (a local of the
        // caller's): white when the palette has no such name.
        void namedColour(uint64_t name, ee::Ptr<uint8_t> rgba)
        {
            for (int i = 0; i < 4; ++i)
                rgba[i] = 255;
            findPaletteColour(name, rgba);
        }

        void memset(ee::Address bytes, int32_t value, uint32_t count) { ee::game(sym::memset, bytes, value, count); }
    }

    // Uint8 (the type table at uint8Type): a byte.
    // uint8FloatValue: floatValue.
    void uint8FloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        *out = ee::toFloat(static_cast<int32_t>(valueOf<uint8_t>(variable)));
    }
    READABLE_FUNCTION(uint8FloatValue, Verified);

    // uint8IntValue: intValue.
    void uint8IntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        *out = valueOf<uint8_t>(variable);
    }
    READABLE_FUNCTION(uint8IntValue, Verified);

    // uint8ToText: toText.
    uint32_t uint8ToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        return formatDecimal(out, valueOf<uint8_t>(variable));
    }
    READABLE_FUNCTION(uint8ToText, Verified);

    // uint8ColorValue: colorValue: a grey of the byte, the byte as its alpha.
    void uint8ColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        const uint8_t value = valueOf<uint8_t>(variable);
        out->r = value;
        out->g = value;
        out->b = value;
        out->paletteName = 0;
        out->a = value;
    }
    READABLE_FUNCTION(uint8ColorValue, Verified);

    // uint8VectorValue: vectorValue: the byte in all four components.
    void uint8VectorValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            out[i] = ee::toFloat(static_cast<int32_t>(valueOf<uint8_t>(variable)));
    }
    READABLE_FUNCTION(uint8VectorValue, Verified);

    // uint8SetFloat: setFloat, clamped to 0 .. 255 (truncated).
    void uint8SetFloat(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        const ee::Float f = *value;
        if (ee::Float(255.0f) < f)
        {
            valueOf<uint8_t>(variable) = 255;
            return;
        }
        if (f < ee::Float(0.0f))
        {
            valueOf<uint8_t>(variable) = 0;
            return;
        }
        valueOf<uint8_t>(variable) = static_cast<uint8_t>(ee::toInt(f));
    }
    // Tested: the cov-zguiset route's two calls.
    READABLE_FUNCTION(uint8SetFloat, Verified);

    // uint8SetInt: setInt (the low byte).
    void uint8SetInt(ee::Ptr<Node> variable, ee::Ptr<const uint8_t> value)
    {
        valueOf<uint8_t>(variable) = *value;
    }
    READABLE_FUNCTION(uint8SetInt, Verified);

    // uint8Parse: setText: the number at text, 255 for any above (or
    // negative: the test is unsigned).
    void uint8Parse(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        ee::Local<int32_t> number;
        parseInt(text, number);
        if (static_cast<uint32_t>(*number) >= 0x100u)
            *number = 255;
        valueOf<uint8_t>(variable) = static_cast<uint8_t>(*number);
    }
    READABLE_FUNCTION(uint8Parse, Verified);

    // uint8Copy: copyOut.
    void uint8Copy(ee::Ptr<Node> variable, ee::Ptr<uint8_t> out)
    {
        *out = valueOf<uint8_t>(variable);
    }
    READABLE_FUNCTION(uint8Copy, Verified);

    // uint8Set: set.
    void uint8Set(ee::Ptr<Node> variable, ee::Ptr<const uint8_t> value)
    {
        valueOf<uint8_t>(variable) = *value;
    }
    READABLE_FUNCTION(uint8Set, Verified);

    // Bool (the type table at boolType): a word, 0 or 1. ZGE2 keeps the word
    // as it is given (set, copyOut, setText and toText move it unchanged);
    // ZGE3 makes it 0 or 1.
    constexpr bool kBoolKeepsWord = game::readable::kIsDm2;
    // boolFloatValue: floatValue.
    void boolFloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        ee::Float value = ee::Float(0.0f);
        if (valueOf<int32_t>(variable))
            value = ee::Float(1.0f);
        *out = value;
    }
    READABLE_FUNCTION(boolFloatValue, Verified);

    // boolCopy: copyOut and intValue.
    void boolCopy(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        if constexpr (kBoolKeepsWord)
            *out = valueOf<int32_t>(variable);
        else
            *out = valueOf<int32_t>(variable) != 0;
    }
    READABLE_FUNCTION(boolCopy, Verified);

    // boolToText: toText: "0" or "1".
    uint32_t boolToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        if constexpr (kBoolKeepsWord)
            return formatDecimal(out, valueOf<int32_t>(variable));
        else
            return formatDecimal(out, valueOf<int32_t>(variable) != 0);
    }
    READABLE_FUNCTION(boolToText, Verified);

    // boolSetFloat: setFloat: false within g_boolZeroTolerance of 0.
    void boolSetFloat(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        const bool zero = ee::abs(*value) < *g_boolZeroTolerance;
        if (zero)
        {
            valueOf<int32_t>(variable) = 0;
            return;
        }
        valueOf<int32_t>(variable) = 1;
    }
    // Tested: the cov-zguiset route's two calls.
    READABLE_FUNCTION(boolSetFloat, Verified);

    // boolSet: set and setInt.
    void boolSet(ee::Ptr<Node> variable, ee::Ptr<const int32_t> value)
    {
        if constexpr (kBoolKeepsWord)
        {
            valueOf<int32_t>(variable) = *value;
            return;
        }
        if (*value)
        {
            valueOf<int32_t>(variable) = 1;
            return;
        }
        valueOf<int32_t>(variable) = 0;
    }
    READABLE_FUNCTION(boolSet, Verified);

    // boolParse: setText: true for a non-zero number at text.
    void boolParse(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        ee::Local<int32_t> number;
        parseInt(text, number);
        if constexpr (kBoolKeepsWord)
            valueOf<int32_t>(variable) = *number;
        else if (*number)
            valueOf<int32_t>(variable) = 1;
        else
            valueOf<int32_t>(variable) = 0;
    }
    READABLE_FUNCTION(boolParse, Verified);

    // Void (the type table at voidType): a word with no text.
    // voidSet: set.
    void voidSet(ee::Ptr<Node> variable, ee::Ptr<const int32_t> value)
    {
        valueOf<int32_t>(variable) = *value;
    }
    READABLE_FUNCTION(voidSet, Verified);

    // voidCopyOut: copyOut.
    void voidCopyOut(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        *out = valueOf<int32_t>(variable);
    }
    READABLE_FUNCTION(voidCopyOut, Verified);

    // Sint32 (and Alignment).
    // sint32FloatValue: floatValue.
    void sint32FloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        *out = ee::toFloat(valueOf<int32_t>(variable));
    }
    READABLE_FUNCTION(sint32FloatValue, Verified);

    // sint32Copy: copyOut and intValue.
    void sint32Copy(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        *out = valueOf<int32_t>(variable);
    }
    READABLE_FUNCTION(sint32Copy, Verified);

    // sint32Set: set and setInt.
    void sint32Set(ee::Ptr<Node> variable, ee::Ptr<const int32_t> value)
    {
        valueOf<int32_t>(variable) = *value;
    }
    READABLE_FUNCTION(sint32Set, Verified);

    // formatIntAttribute: Sint32's toText: "%d".
    uint32_t formatIntAttribute(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        return formatDecimal(out, valueOf<int32_t>(variable));
    }
    READABLE_FUNCTION(formatIntAttribute, Verified);

    // sint32ColorValue: colorValue: a grey of the number, the number as its
    // alpha (each channel as channelOf makes it).
    void sint32ColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        out->r = channelOf(valueOf<int32_t>(variable));
        out->g = channelOf(valueOf<int32_t>(variable));
        out->b = channelOf(valueOf<int32_t>(variable));
        out->paletteName = 0;
        out->a = channelOf(valueOf<int32_t>(variable));
    }
    READABLE_FUNCTION(sint32ColorValue, Verified);

    // sint32VectorValue: vectorValue: the number in all four components.
    void sint32VectorValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            out[i] = ee::toFloat(valueOf<int32_t>(variable));
    }
    READABLE_FUNCTION(sint32VectorValue, Verified);

    // sint32SetFloat: setFloat (truncated).
    void sint32SetFloat(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        valueOf<int32_t>(variable) = ee::toInt(*value);
    }
    READABLE_FUNCTION(sint32SetFloat, Verified);

    // sint32Text: setText: the number at `text` parsed into the value; the
    // text after it.
    ee::Ptr<const char> sint32Text(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        return game::tail(parseInt, text, variable->valueAs<int32_t>());
    }
    READABLE_FUNCTION(sint32Text, Verified);

    // ZFloat.
    // zfloatCopy: copyOut and floatValue.
    void zfloatCopy(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        *out = valueOf<ee::Float>(variable);
    }
    READABLE_FUNCTION(zfloatCopy, Verified);

    // zfloatIntValue: intValue (truncated).
    void zfloatIntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        *out = ee::toInt(valueOf<ee::Float>(variable));
    }
    READABLE_FUNCTION(zfloatIntValue, Verified);

    // zfloatSet: set and setFloat.
    void zfloatSet(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        valueOf<ee::Float>(variable) = *value;
    }
    READABLE_FUNCTION(zfloatSet, Verified);

    // zfloatSetInt: setInt.
    void zfloatSetInt(ee::Ptr<Node> variable, ee::Ptr<const int32_t> value)
    {
        valueOf<ee::Float>(variable) = ee::toFloat(*value);
    }
    // Draft: no route sets a ZFloat from an integer (never recorded).
    READABLE_FUNCTION(zfloatSetInt, Verified);

    // zfloatText: setText: the number at `text` parsed into the value, as
    // Sint32's.
    ee::Ptr<const char> zfloatText(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        return game::tail(parseFloat, text, variable->valueAs<ee::Float>());
    }
    READABLE_FUNCTION(zfloatText, Verified);

    // zfloatToText: toText: a whole number as "%d", any other as "%f".
    uint32_t zfloatToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Float value = valueOf<ee::Float>(variable);
        const int32_t whole = ee::toInt(value);
        if (ee::toFloat(whole) == value)
            return formatDecimal(out, whole);
        zge::formatString(out, kFloatFormat, sce::libgcc::fptodp(value));
        return 1;
    }
    READABLE_FUNCTION(zfloatToText, Verified);

    // zfloatColorValue: colorValue: a grey of the number (at most 255,
    // truncated: it is not scaled as a 0 .. 1 component), the number as its
    // alpha.
    void zfloatColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        const ee::Float value = valueOf<ee::Float>(variable);
        out->paletteName = 0;
        const uint8_t channel = static_cast<uint8_t>(ee::toInt(ee::min(value, ee::Float(255.0f))));
        out->a = channel;
        out->r = channel;
        out->g = channel;
        out->b = channel;
    }
    READABLE_FUNCTION(zfloatColorValue, Verified);

    // zfloatVectorValue: vectorValue: the number in all four components.
    void zfloatVectorValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            out[i] = valueOf<ee::Float>(variable);
    }
    READABLE_FUNCTION(zfloatVectorValue, Verified);

    // ZRGBA_PAL.
    // paletteCopy: copyOut: a palette name's colour looked up (the name
    // kept), else the colour.
    void paletteCopy(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        const Color &value = valueOf<Color>(variable);
        if (const uint64_t name = value.paletteName)
        {
            findPaletteColour(name, &out->r);
            out->paletteName = valueOf<Color>(variable).paletteName;
        }
        else
        {
            std::memcpy(&out->r, &value.r, 4);
            out->paletteName = 0;
        }
    }
    READABLE_FUNCTION_OWN(paletteCopy, Verified, dm2, rgbaCopy);

    // rgbaCopy: ZGE2's paletteCopy, its ZRGBA's copyOut: the four channels.
    void rgbaCopy(ee::Ptr<Node> variable, ee::Ptr<Rgba> out)
    {
        std::memcpy(out.get(), variable->valueAs<Rgba>().get(), sizeof(Rgba));
    }

    // paletteParse: setText: a colour from `text`: four numbers (red, green,
    // blue, alpha, a byte each) when it starts with a digit after its spaces,
    // else a palette colour's name (its first eight characters, the spaces
    // included) looked up into the value. Returns the alpha, or the lookup's
    // result.
    int32_t paletteParse(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        ee::Local<int32_t[4]> channels;
        ee::Ptr<const char> p = text;
        while (*p == ' ')
            ++p;
        if (isDigit(*p))
        {
            for (uint32_t i = 0; i < 4; ++i)
                p = parseInt(p, &channels[i]);
            Color &colour = valueOf<Color>(variable);
            colour.r = static_cast<uint8_t>(channels[0]);
            colour.g = static_cast<uint8_t>(channels[1]);
            colour.b = static_cast<uint8_t>(channels[2]);
            colour.a = static_cast<uint8_t>(channels[3]);
            colour.paletteName = 0;
            return static_cast<uint8_t>(channels[3]);
        }
        valueOf<Color>(variable).paletteName = 0;
        packPaletteName(valueOf<Color>(variable).paletteName, text);
        return findPaletteColour(valueOf<Color>(variable).paletteName, &valueOf<Color>(variable).r);
    }

    // parseRgba: ZGE2's paletteParse, its ZRGBA's setText: four numbers
    // (red, green, blue, alpha, a byte each) and nothing else (ZGE2's colours
    // have no palette names). Returns the alpha.
    int32_t parseRgba(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        ee::Local<int32_t[4]> channels;
        ee::Ptr<const char> p = text;
        for (uint32_t i = 0; i < 4; ++i)
            p = parseInt(p, &channels[i]);
        Rgba &colour = *variable->valueAs<Rgba>();
        colour.r = static_cast<uint8_t>(channels[0]);
        colour.g = static_cast<uint8_t>(channels[1]);
        colour.b = static_cast<uint8_t>(channels[2]);
        colour.a = static_cast<uint8_t>(channels[3]);
        return static_cast<uint8_t>(channels[3]);
    }
    READABLE_FUNCTION_OWN(paletteParse, Verified, dm2, parseRgba);

    // paletteSet: set: by name (the colour looked up into the value), or
    // the colour.
    void paletteSet(ee::Ptr<Node> variable, ee::Ptr<const Color> in)
    {
        Color &value = valueOf<Color>(variable);
        if (const uint64_t name = in->paletteName)
        {
            value.paletteName = name;
            game::tail(findPaletteColour, in->paletteName, variable->valueAs<uint8_t>());
            return;
        }
        std::memcpy(&value.r, &in->r, 4);
        valueOf<Color>(variable).paletteName = 0;
    }
    READABLE_FUNCTION_OWN(paletteSet, Verified, dm2, rgbaSet);

    // rgbaSet: ZGE2's paletteSet, its ZRGBA's set: the four channels.
    void rgbaSet(ee::Ptr<Node> variable, ee::Ptr<const Rgba> in)
    {
        std::memcpy(variable->valueAs<Rgba>().get(), in.get(), sizeof(Rgba));
    }

    // paletteFloatValue: floatValue: 255 over the channels' average (the
    // palette colour's for a named one). The other number types' colours
    // scale the other way, so this looks like a slip for average / 255:
    // white gives 1, black divides by 0 (the largest float, the EE has no
    // infinity). Kept as the game does it.
    void paletteFloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        ee::Local<uint8_t[4]> named;
        const Color &value = valueOf<Color>(variable);
        int32_t redAlpha, greenBlue;
        if (value.paletteName)
        {
            namedColour(value.paletteName, named);
            redAlpha = named[0] + named[3];
            greenBlue = named[2] + named[1];
        }
        else
        {
            redAlpha = value.r + value.a;
            greenBlue = value.b + value.g;
        }
        *out = ee::Float(255.0f) / (ee::toFloat(redAlpha + greenBlue) * ee::Float(0.25f));
    }
    READABLE_FUNCTION(paletteFloatValue, Verified);

    // paletteIntValue: intValue: the channels' average (the palette colour's
    // for a named one).
    void paletteIntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        ee::Local<uint8_t[4]> named;
        const Color &value = valueOf<Color>(variable);
        int32_t sum;
        if (value.paletteName)
        {
            namedColour(value.paletteName, named);
            sum = named[0] + named[3] + (named[2] + named[1]);
        }
        else
        {
            sum = value.r + value.a + (value.b + value.g);
        }
        *out = sum / 4;
    }
    READABLE_FUNCTION(paletteIntValue, Verified);

    // paletteToText: toText: a palette colour's name, else "r g b a".
    uint32_t paletteToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        ee::Local<char[9]> nameText; // the eight packed characters and an end
        const Color &value = valueOf<Color>(variable);
        if (const uint64_t name = value.paletteName)
        {
            memset(nameText, 0, 9);
            for (uint32_t i = 0; i < 8; ++i)
                nameText[i] = static_cast<char>(name >> (i * 8));
            zge::formatString(out, kPaletteNameFormat, nameText);
            return 1;
        }
        zge::formatString(out, kPaletteChannelsFormat, value.r, value.g, value.b, value.a);
        return 1;
    }
    READABLE_FUNCTION_OWN(paletteToText, Verified, dm2, rgbaToText);

    // rgbaToText: ZGE2's paletteToText, its ZRGBA's toText: the four channels
    // as numbers; 0 for opaque white (what a colour starts as), else 1.
    int32_t rgbaToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const Rgba colour = *variable->valueAs<Rgba>();
        zge::formatString(out, kPaletteChannelsFormat, colour.r, colour.g, colour.b, colour.a);
        if (colour.r != 0xFF || colour.g != colour.r || colour.b != colour.g)
            return 1;
        return colour.a != 0xFF;
    }

    // paletteVectorValue: vectorValue: the channels as 0 .. 1 components
    // (the colour as kept: a named one's last lookup).
    void paletteVectorValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        const Color &value = valueOf<Color>(variable);
        out[0] = ee::toFloat(static_cast<int32_t>(value.r)) * kChannelToUnit;
        out[1] = ee::toFloat(static_cast<int32_t>(value.g)) * kChannelToUnit;
        out[2] = ee::toFloat(static_cast<int32_t>(value.b)) * kChannelToUnit;
        out[3] = ee::toFloat(static_cast<int32_t>(value.a)) * kChannelToUnit;
    }
    READABLE_FUNCTION(paletteVectorValue, Verified);

    // paletteSetFloat: setFloat: a grey of the 0 .. 1 number, the number as
    // its alpha; no longer named.
    void paletteSetFloat(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        Color &colour = valueOf<Color>(variable);
        colour.r = channelOfUnit(*value);
        colour.g = channelOfUnit(*value);
        colour.b = channelOfUnit(*value);
        colour.a = channelOfUnit(*value);
        colour.paletteName = 0;
    }
    READABLE_FUNCTION(paletteSetFloat, Verified);

    // paletteSetInt: setInt: a grey of the number, the number as its alpha;
    // no longer named.
    void paletteSetInt(ee::Ptr<Node> variable, ee::Ptr<const int32_t> value)
    {
        Color &colour = valueOf<Color>(variable);
        colour.r = channelOf(*value);
        colour.g = channelOf(*value);
        colour.b = channelOf(*value);
        colour.a = channelOf(*value);
        colour.paletteName = 0;
    }
    READABLE_FUNCTION(paletteSetInt, Verified);

    // paletteSetVector: setVector: the four 0 .. 1 components as red, green,
    // blue and alpha; no longer named.
    void paletteSetVector(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        Color &colour = valueOf<Color>(variable);
        colour.r = channelOfUnit(value[0]);
        colour.g = channelOfUnit(value[1]);
        colour.b = channelOfUnit(value[2]);
        colour.a = channelOfUnit(value[3]);
        colour.paletteName = 0;
    }
    READABLE_FUNCTION(paletteSetVector, Verified);

    // The vector type. vectorCopy: copyOut.
    void vectorCopy(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            out[i] = variable->valueAs<ee::Float>()[i];
    }
    READABLE_FUNCTION(vectorCopy, Verified);

    // vectorParse: the vector type's setText: x, y and z from `text` into
    // the value; the text after them.
    ee::Ptr<const char> vectorParse(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        const ee::Ptr<ee::Float> vector = variable->valueAs<ee::Float>();
        text = parseFloat(text, vector + 0);
        text = parseFloat(text, vector + 1);
        return game::tail(parseFloat, text, vector + 2);
    }
    READABLE_FUNCTION(vectorParse, Verified);

    // vectorToText: toText: "x y z"; 0 for a vector of zeros (each within
    // kNearZero), else 1.
    uint32_t vectorToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Ptr<ee::Float> vector = variable->valueAs<ee::Float>();
        const uint64_t x = sce::libgcc::fptodp(vector[0]);
        const uint64_t y = sce::libgcc::fptodp(vector[1]);
        const uint64_t z = sce::libgcc::fptodp(vector[2]);
        zge::formatString(out, kVectorFormat, x, y, z);
        const bool zero = nearZero(vector[0]) && nearZero(vector[1]) && nearZero(vector[2]);
        return zero ? 0 : 1;
    }
    READABLE_FUNCTION(vectorToText, Verified);

    // vectorColorValue: colorValue: x, y, z and w as 0 .. 1 components of
    // red, green, blue and alpha.
    void vectorColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        const ee::Ptr<ee::Float> vector = variable->valueAs<ee::Float>();
        out->r = channelOfUnit(vector[0]);
        out->g = channelOfUnit(vector[1]);
        out->b = channelOfUnit(vector[2]);
        const ee::Float alpha = vector[3];
        out->paletteName = 0;
        out->a = channelOfUnit(alpha);
    }
    READABLE_FUNCTION(vectorColorValue, Verified);

    // vectorSetColor: setColor: red, green, blue and alpha (a palette
    // colour's for a named one) as 0 .. 1 components.
    void vectorSetColor(ee::Ptr<Node> variable, ee::Ptr<const Color> in)
    {
        ee::Local<uint8_t[4]> named;
        const ee::Ptr<ee::Float> vector = variable->valueAs<ee::Float>();
        int32_t alpha;
        if (const uint64_t name = in->paletteName)
        {
            namedColour(name, named);
            vector[0] = ee::toFloat(static_cast<int32_t>(named[0])) * kChannelToUnit;
            vector[1] = ee::toFloat(static_cast<int32_t>(named[1])) * kChannelToUnit;
            vector[2] = ee::toFloat(static_cast<int32_t>(named[2])) * kChannelToUnit;
            alpha = named[3];
        }
        else
        {
            vector[0] = ee::toFloat(static_cast<int32_t>(in->r)) * kChannelToUnit;
            vector[1] = ee::toFloat(static_cast<int32_t>(in->g)) * kChannelToUnit;
            vector[2] = ee::toFloat(static_cast<int32_t>(in->b)) * kChannelToUnit;
            alpha = in->a;
        }
        vector[3] = ee::toFloat(alpha) * kChannelToUnit;
    }
    READABLE_FUNCTION(vectorSetColor, Verified);

    // vectorSet: set and setVector.
    void vectorSet(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            variable->valueAs<ee::Float>()[i] = value[i];
    }
    READABLE_FUNCTION(vectorSet, Verified);

    // ZQuaternion: four floats, x, y, z and w.
    // zquaternionCopyOut: copyOut.
    void zquaternionCopyOut(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            out[i] = variable->valueAs<ee::Float>()[i];
    }
    READABLE_FUNCTION(zquaternionCopyOut, Verified);

    // zquaternionSet: set.
    void zquaternionSet(ee::Ptr<Node> variable, ee::Ptr<const ee::Float> value)
    {
        for (int i = 0; i < 4; ++i) // (unrolled in the original: no loop)
            variable->valueAs<ee::Float>()[i] = value[i];
    }
    READABLE_FUNCTION(zquaternionSet, Verified);

    // zquaternionSetText: setText: x, y, z and w from `text` into the value;
    // the text after them.
    ee::Ptr<const char> zquaternionSetText(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        text = parseFloat(text, variable->valueAs<ee::Float>() + 0);
        text = parseFloat(text, variable->valueAs<ee::Float>() + 1);
        text = parseFloat(text, variable->valueAs<ee::Float>() + 2);
        return game::tail(parseFloat, text, variable->valueAs<ee::Float>() + 3);
    }
    READABLE_FUNCTION(zquaternionSetText, Verified);

    // zquaternionToText: toText: "x y z w"; 0 for the identity (0, 0, 0, 1),
    // each within kNearZero, else 1.
    uint32_t zquaternionToText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Ptr<ee::Float> quaternion = variable->valueAs<ee::Float>();
        const uint64_t x = sce::libgcc::fptodp(quaternion[0]);
        const uint64_t y = sce::libgcc::fptodp(quaternion[1]);
        const uint64_t z = sce::libgcc::fptodp(quaternion[2]);
        const uint64_t w = sce::libgcc::fptodp(quaternion[3]);
        zge::formatString(out, kQuaternionFormat, x, y, z, w);
        const bool identity = nearZero(quaternion[0]) && nearZero(quaternion[1]) && nearZero(quaternion[2]) &&
                              nearZero(quaternion[3] - ee::Float(1.0f));
        return identity ? 0 : 1;
    }
    READABLE_FUNCTION(zquaternionToText, Verified);

    namespace
    {
        // The children's names and types.
        constexpr ee::Ptr<const char> kTextRed{sym::g_textRed};
        constexpr ee::Ptr<const char> kTextGreen{sym::g_textGreen};
        constexpr ee::Ptr<const char> kTextBlue{sym::g_textBlue};
        constexpr ee::Ptr<const char> kTextAlpha{sym::g_textAlpha};
        constexpr ee::Ptr<const char> kTextUint8{sym::g_textUint8};
        constexpr ee::Ptr<const char> kTextComponentX{sym::g_textComponentX}; // "x"
        constexpr ee::Ptr<const char> kTextComponentY{sym::g_textComponentY}; // "y"
        constexpr ee::Ptr<const char> kTextComponentZ{sym::g_textComponentZ}; // "z"
        constexpr ee::Ptr<const char> kTextZFloat4{sym::g_textZFloat4};
    }

    // paletteExpand: ZRGBA_PAL's expand: its four bytes as the properties
    // red, green, blue and alpha (Uint8s) under the variable.
    uint32_t paletteExpand(ee::Ptr<Node> node)
    {
        defineTypedProperty(node, kTextRed, kTextUint8, node->value);
        defineTypedProperty(node, kTextGreen, kTextUint8, node->value + 1);
        defineTypedProperty(node, kTextBlue, kTextUint8, node->value + 2);
        return game::tail(zge::zgui::defineTypedProperty, node, kTextAlpha, kTextUint8, node->value + 3);
    }
    READABLE_FUNCTION(paletteExpand, Verified);

    // vectorExpand: the vector's expand: x, y and z as ZFloat properties.
    uint32_t vectorExpand(ee::Ptr<Node> node)
    {
        defineTypedProperty(node, kTextComponentX, kTextZFloat4, node->value);
        defineTypedProperty(node, kTextComponentY, kTextZFloat4, node->value + 4);
        return game::tail(zge::zgui::defineTypedProperty, node, kTextComponentZ, kTextZFloat4, node->value + 8);
    }
    READABLE_FUNCTION(vectorExpand, Verified);

    namespace
    {
        constexpr char kBufferEnd = 1; // the byte after a String value's room
        constexpr bool kEscapesBackslash = !game::readable::kIsDm2; // ZGE2 has no \\ escape: it keeps both
    }

    // stringSetText: String's setText: `text` into the variable's value (a
    // holder of the text's buffer, its room ended by a 0x01 byte; a buffer
    // too small is given back and a new one of the text's length taken), its
    // escapes turned: \" into " (kept as \" between quotes), \\ into \ (ZGE2 keeps both), \n
    // into CR LF (not between quotes), CR LF kept, a lone LF into CR; each "
    // (not escaped) starts or ends a quote. No text (or an empty one) empties
    // the value. The holder is read again at every byte written, as the
    // original reads it.
    void stringSetText(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        const ee::Ptr<ee::Ptr<char>> holder = variable->valueAs<ee::Ptr<char>>();
        const auto put = [holder](uint32_t at, char c) { (*holder)[at] = c; };

        // The room the buffer has: up to its end byte.
        const ee::Ptr<char> start = *holder;
        uint32_t room = 0;
        if (start)
        {
            while (start[room] != kBufferEnd)
                ++room;
        }
        if (!text || *text == 0)
        {
            if (start)
                *start = 0;
            return;
        }

        const uint32_t length = static_cast<uint32_t>(zge::stringLength(text));
        bool take = true;
        if (start)
        {
            if (!(room < length + 1))
                take = false;
            else
                zguiRelease(*g_heap, *holder);
        }
        if (take)
        {
            *holder = ee::Ptr<char>(zguiAllocate(*g_heap, length + 2));
            (*holder)[length + 1] = kBufferEnd;
        }
        if (*text == 0)
            return;

        bool quoted = false;
        uint32_t in = 0;
        uint32_t out = 0;
        for (;;)
        {
            const char c = text[in];
            bool done = false; // written: on to the next byte
            if (c == '\\')
            {
                const char next = text[in + 1];
                if (next == '"')
                {
                    if (quoted)
                    {
                        // \" between quotes: both kept.
                        put(out, c);
                        out += 1;
                        put(out, text[in + 1]);
                    }
                    else
                        put(out, '"');
                    in += 2;
                    done = true;
                }
                else if (next == '\\' && kEscapesBackslash)
                {
                    in += 2;
                    put(out, '\\');
                    done = true;
                }
            }
            if (!done)
            {
                const ee::Ptr<const char> here = text + in;
                if (!quoted && here[0] == '\\' && here[1] == 'n')
                {
                    in += 2;
                    put(out, '\r');
                    out += 1;
                    put(out, '\n');
                }
                else if (here[0] == '\r' && here[1] == '\n')
                {
                    in += 2;
                    put(out, '\r');
                    out += 1;
                    put(out, '\n');
                }
                else if (here[0] == '\n')
                {
                    in += 1;
                    put(out, '\r');
                }
                else
                {
                    if (here[0] == '"')
                        quoted = !quoted;
                    in += 1;
                    put(out, here[0]);
                }
            }
            out += 1;
            put(out, 0);
            if (text[in] == 0)
                break;
        }
    }
    READABLE_FUNCTION(stringSetText, Verified);

    // String (the type table at stringType), Event (eventType: a script's
    // text, its set, copyOut, destroy and toText String's) and ZAssetID
    // (zAssetIdType: an asset's name; setting or freeing it tells the widget).
    // The value is a holder of the text's buffer (stringSetText).
    namespace
    {
        ee::Ptr<ee::Ptr<char>> holderOf(ee::Ptr<Node> variable) { return variable->valueAs<ee::Ptr<char>>(); }
        ee::Ptr<const char> textOf(ee::Ptr<Node> variable) { return *variable->valueAs<ee::Ptr<const char>>(); }
        // What a ZAssetID's widget is sent when its asset changes (it loads it again).
        constexpr uint32_t kRefreshMessage = static_cast<uint32_t>(Message::Refresh);
    }

    // stringCopyOut: copyOut and toText: the text into out ("" without one);
    // 1 when it is not empty.
    int32_t stringCopyOut(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Ptr<const char> text = textOf(variable);
        if (text)
            zge::stringCopy(out, text);
        else
            *out = 0;
        return *out != 0;
    }
    READABLE_FUNCTION_OWN(stringCopyOut, Verified, dm2, copyStringOut);

    // copyStringOut: ZGE2's stringCopyOut, String's copyOut: the text into
    // out, "" without one or for an empty one (no result: ZGE2's toText is
    // stringToQuotedText).
    void copyStringOut(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Ptr<const char> text = textOf(variable);
        if (text && *text)
        {
            game::tail(zge::stringCopy, out, text);
            return;
        }
        *out = 0;
    }

    // stringFloatValue: floatValue: the number the text starts with.
    void stringFloatValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        if (const ee::Ptr<const char> text = textOf(variable))
            game::tail(parseFloat, text, out);
    }
    READABLE_FUNCTION(stringFloatValue, Verified);

    // stringIntValue: intValue: the number the text starts with.
    void stringIntValue(ee::Ptr<Node> variable, ee::Ptr<int32_t> out)
    {
        if (const ee::Ptr<const char> text = textOf(variable))
            game::tail(parseInt, text, out);
    }
    READABLE_FUNCTION(stringIntValue, Verified);

    // stringVectorValue: vectorValue: x, y and z read from the text (w 0).
    void stringVectorValue(ee::Ptr<Node> variable, ee::Ptr<ee::Float> out)
    {
        const ee::Ptr<const char> text = textOf(variable);
        for (int i = 0; i < 4; ++i)
            out[i] = ee::Float(0.0f);
        if (text)
        {
            const ee::Ptr<const char> afterX = parseFloat(text, out);
            const ee::Ptr<const char> afterY = parseFloat(afterX, out + 1);
            game::tail(parseFloat, afterY, out + 2);
        }
    }
    READABLE_FUNCTION(stringVectorValue, Verified);

    // stringColorValue: colorValue: as ZRGBA_PAL parses it (four numbers after
    // spaces, else a palette name of eight characters); white without text.
    void stringColorValue(ee::Ptr<Node> variable, ee::Ptr<Color> out)
    {
        ee::Local<int32_t[4]> channels;
        ee::Ptr<const char> text = textOf(variable);
        out->r = 255;
        out->g = 255;
        out->b = 255;
        out->a = 255;
        out->paletteName = 0;
        if (!text)
            return;
        while (*text == ' ')
            ++text;
        if (isDigit(*text))
        {
            ee::Ptr<const char> at = text;
            for (uint32_t i = 0; i < 4; ++i) // (unrolled in the original)
                at = parseInt(at, &channels[i]);
            out->r = static_cast<uint8_t>(channels[0]);
            out->g = static_cast<uint8_t>(channels[1]);
            out->b = static_cast<uint8_t>(channels[2]);
            out->a = static_cast<uint8_t>(channels[3]);
            out->paletteName = 0;
            return;
        }
        out->paletteName = 0;
        packPaletteName(out->paletteName, text);
        findPaletteColour(out->paletteName, &out->r);
    }
    READABLE_FUNCTION(stringColorValue, Verified);

    // stringDestroy: destroy: the text's buffer given back.
    void stringDestroy(ee::Ptr<Node> variable)
    {
        if (const ee::Ptr<const char> buffer = textOf(variable))
        {
            zguiRelease(*g_heap, buffer);
            *holderOf(variable) = ee::Ptr<char>();
        }
    }
    READABLE_FUNCTION(stringDestroy, Verified);

    // assetIdSet: ZAssetID's set and setText: the name set as String's,
    // then its widget told to load it again.
    void assetIdSet(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        stringSetText(variable, text);
        if (const ee::Ptr<Node> widget = variable->parent)
            game::tail(zge::zgui::sendMessage, widget, kRefreshMessage, 0u);
    }
    // The widget's refresh can load the asset.
    READABLE_FUNCTION_WAITS(assetIdSet, Verified);

    // assetIdDestroy: ZAssetID's destroy: the name's buffer given back, its
    // widget told.
    void assetIdDestroy(ee::Ptr<Node> variable)
    {
        if (const ee::Ptr<const char> buffer = textOf(variable))
        {
            zge::heapGive(ee::Ptr<zge::Heap>(*g_heap), buffer);
            *holderOf(variable) = ee::Ptr<char>();
            if (const ee::Ptr<Node> widget = variable->parent)
                game::tail(zge::zgui::sendMessage, widget, kRefreshMessage, 0u);
        }
    }
    READABLE_FUNCTION_WAITS(assetIdDestroy, Verified);
}

// ZGE2's type methods ZGE3 has no counterpart of. Its String has a setText and
// a toText for a script's source form (the text between quotes) beside the
// set that takes the text as it is; Void takes no text and gives none; the
// types that own nothing have destroys that do nothing.
namespace zge::zgui
{
    // stringSetQuotedText: String's setText: `text` without the quote it may
    // start with, up to the next quote (not escaped) or its end, into the
    // variable's value: \n becomes CR LF until an escaped quote has been
    // passed (each \" becomes " and turns that on or off). The value's buffer
    // as stringSetText keeps it (its room ended by a 0x01 byte, a new one
    // taken when the text does not fit); an empty text empties the value.
    inline void stringSetQuotedText(ee::Ptr<Node> variable, ee::Ptr<const char> text)
    {
        ee::Local<char[0x400]> plain;
        uint32_t out = 0;
        bool pastEscapedQuote = false;
        uint32_t in = text[0] == '"' ? 1 : 0;
        while (text[in] != 0)
        {
            const ee::Ptr<const char> here = text + in;
            if (!pastEscapedQuote && here[0] == '\\' && here[1] == 'n')
            {
                plain[out++] = '\r';
                plain[out++] = '\n';
                in += 2;
                continue;
            }
            if (here[0] == '\\' && here[1] == '"')
            {
                plain[out++] = '"';
                in += 2;
                pastEscapedQuote = !pastEscapedQuote;
                continue;
            }
            if (here[0] == '"')
                break;
            plain[out++] = here[0];
            in += 1;
        }
        plain[out] = 0;

        const ee::Ptr<ee::Ptr<char>> holder = holderOf(variable);
        const ee::Ptr<char> start = *holder;
        if (out == 0)
        {
            if (start)
                *start = 0;
            return;
        }
        const uint32_t needed = static_cast<uint32_t>(zge::stringLength(plain)) + 1;
        if (start)
        {
            uint32_t room = 0;
            while (start[room] != kBufferEnd)
                ++room;
            if (!(room < needed))
            {
                zge::stringCopy(start, plain);
                return;
            }
            zguiRelease(*g_heap, *holderOf(variable));
        }
        const ee::Ptr<char> buffer(zguiAllocate(*g_heap, needed + 1));
        *holderOf(variable) = buffer;
        buffer[needed] = kBufferEnd;
        zge::stringCopy(buffer, plain);
    }
    READABLE_FUNCTION_ONLY(dm2, stringSetQuotedText);

    // stringToQuotedText: String's toText: the text between quotes, each " in
    // it as \" and each CR with the byte after it as \n; 1, or 0 for no text
    // or an empty one (the two quotes are written all the same).
    inline int32_t stringToQuotedText(ee::Ptr<Node> variable, ee::Ptr<char> out)
    {
        const ee::Ptr<const char> text = textOf(variable);
        uint32_t written = 1;
        out[0] = '"';
        if (text && text[0] != 0)
        {
            for (uint32_t in = 0; text[in] != 0;)
            {
                const char c = text[in];
                if (c == '"')
                {
                    out[written++] = '\\';
                    out[written++] = '"';
                    in += 1;
                }
                else if (c == '\r')
                {
                    out[written++] = '\\';
                    out[written++] = 'n';
                    in += 2;
                }
                else
                {
                    out[written++] = c;
                    in += 1;
                }
            }
        }
        out[written] = '"';
        out[written + 1] = 0;
        return text && text[0] != 0;
    }
    READABLE_FUNCTION_ONLY(dm2, stringToQuotedText);

    // Void's setText (nothing) and toText (no text: 0).
    inline void voidSetText(ee::Ptr<Node> /*variable*/, ee::Ptr<const char> /*text*/) {}
    READABLE_FUNCTION_ONLY(dm2, voidSetText);
    inline int32_t voidToText(ee::Ptr<Node> /*variable*/, ee::Ptr<char> /*out*/) { return 0; }
    READABLE_FUNCTION_ONLY(dm2, voidToText);

    // The destroys of the types whose values own nothing.
    inline void boolDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, boolDestroy);
    inline void voidDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, voidDestroy);
    inline void sint32Destroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, sint32Destroy);
    inline void zfloatDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, zfloatDestroy);
    inline void rgbaDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, rgbaDestroy);
    inline void vectorDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, vectorDestroy);
    inline void zquaternionDestroy(ee::Ptr<Node> /*variable*/) {}
    READABLE_FUNCTION_ONLY(dm2, zquaternionDestroy);
}
