// The debug text: a line font in static tables (a stroke list a character, four
// bytes a stroke: from x, y to x, y on a grid of 128, y up) and the small
// interface the engine's debug output prints through. Each stroke of a string
// becomes a line of two vertices, handed to the primitive API.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/scene/camera.h"
#include "zge/text/cstring.h"
#include "zge/render/primitives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/debug/debug_text.cpp.h"

namespace zge
{
    namespace
    {
        struct DebugFont
        {
            Vec4 glyphSize;            // +0x00 a stroke unit across the screen (x) and down it (y)
            uint32_t colour;           // +0x10 RGBA, the vertices'
            ee::Float inverseNearClip; // +0x14 the vertices' z
            uint32_t vertices;         // +0x18 two a stroke
            uint32_t strokeCapacity;   // +0x1C
        };
        static_assert(sizeof(DebugFont) == 0x20);

        struct LineVertex
        {
            ee::Float x, y, z, w;   // +0x00
            uint32_t u, v;          // +0x10
            uint32_t colour;        // +0x18
            uint32_t _1c;           // +0x1C left as it is
        };
        static_assert(sizeof(LineVertex) == 0x20);
        constexpr uint32_t kBytesAStroke = 2 * sizeof(LineVertex);

        constexpr uint32_t kGlyphStrokes = sym::glyphStrokes;      // (uint32[256]) a character's stroke list
        constexpr uint32_t kGlyphStrokeCounts = sym::glyphStrokeCounts; // (int32[256])
        constexpr uint32_t kGlyphTablesBytes = 0x800, kCharacters = 256;
        constexpr uint32_t kDigitStrokes = sym::digitStrokes, kLetterStrokes = sym::letterStrokes, kAStrokeList = 0x40;
        constexpr uint32_t kGreaterStrokes = sym::greaterStrokes, kLessStrokes = sym::lessStrokes, kMinusStrokes = sym::minusStrokes,
                           kColonStrokes = sym::colonStrokes, kPointStrokes = sym::pointStrokes;
        constexpr uint32_t kDrawDebugFontText = sym::drawDebugFontText;
        constexpr uint32_t kPrimitiveLines = 0x02000000u;
        constexpr int32_t kCoordinateFlags = 0xC000;         // a print's coordinate: bit 14 no margin
        constexpr int32_t kNoMargin = 0x4000;
        constexpr int32_t kGlyphTop = 0x80;                  // the strokes' y runs down from here

        inline ee::Global<ee::Ptr<DebugFont>, sym::g_debugFont> g_debugFont;
        inline ee::Global<uint32_t, sym::g_debugFontCamera> g_debugFontCamera;

        ee::Ptr<int32_t> glyphStrokeCounts() { return ee::Ptr<int32_t>(kGlyphStrokeCounts); }
        ee::Ptr<uint32_t> glyphStrokes() { return ee::Ptr<uint32_t>(kGlyphStrokes); }

        int32_t strokeCountOf(int8_t character)
        {
            // (A signed index: past 0x7F the original reads before the table.)
            return glyphStrokeCounts()[static_cast<uint32_t>(static_cast<int32_t>(character))];
        }

        ee::Float withMargin(int32_t coordinate, float margin)
        {
            ee::Float position = ee::toFloat(coordinate & ~kCoordinateFlags);
            if ((coordinate & kNoMargin) == 0)
                position = position + ee::Float(margin);
            return position;
        }
        // ZGE2's: the flag bits stay in the coordinate, and the position with
        // its margin is a 16-bit integer again (truncated, then the short)
        // before it is a float.
        ee::Float withMarginAsShort(int32_t coordinate, float margin)
        {
            ee::Float position = ee::toFloat(coordinate);
            if ((coordinate & kNoMargin) == 0)
                position = position + ee::Float(margin);
            return ee::toFloat(static_cast<int16_t>(ee::toInt(position)));
        }
    }

    void drawDebugFontText(ee::Ptr<DebugFont> font, uint32_t text, ee::Float screenX, ee::Float screenY);

    // debugTextPrint: text at a screen position, in 16 bits each: inside the
    // title-safe margin unless bit 14 is set (ZGE2 keeps the flag bits in the
    // position, and the position with its margin a 16-bit integer). Always 0.
    uint32_t debugTextPrint(uint32_t x, uint32_t y, uint32_t text)
    {
        ee::Float screenX;
        ee::Float screenY;
        if constexpr (game::readable::kIsDm2)
        {
            screenX = withMarginAsShort(static_cast<int16_t>(x), 32.0f);
            screenY = withMarginAsShort(static_cast<int16_t>(y), 16.0f);
        }
        else
        {
            screenX = withMargin(static_cast<int16_t>(x), 32.0f);
            screenY = withMargin(static_cast<int16_t>(y), 16.0f);
        }
        drawDebugFontText(*g_debugFont, text, screenX, screenY);
        return 0;
    }
    READABLE_FUNCTION_WAITS(debugTextPrint, Verified);

    // debugTextCellSize: a character's cell. Returns its width.
    int32_t debugTextCellSize(ee::Ptr<int32_t> width, ee::Ptr<int32_t> height)
    {
        constexpr int32_t kCellWidth = 8, kCellHeight = 12;
        *width = kCellWidth;
        *height = kCellHeight;
        return kCellWidth;
    }
    READABLE_FUNCTION(debugTextCellSize, Verified);

    // debugTextUnusedMethodA, debugTextUnusedMethodB: the interface's unused methods.
    void debugTextUnusedMethodA() {}
    READABLE_FUNCTION(debugTextUnusedMethodA, Draft);

    void debugTextUnusedMethodB() {}
    READABLE_FUNCTION(debugTextUnusedMethodB, Draft);

    // setDebugGlyph: a character's strokes, a list that ends with a zero word.
    void setDebugGlyph(uint32_t character, uint32_t strokes)
    {
        int32_t &count = glyphStrokeCounts()[character];
        count = 0;
        glyphStrokes()[character] = strokes;
        // A stroke is four bytes, at any alignment.
        ee::Ptr<uint8_t> stroke(strokes);
        while (stroke[0] != 0 || stroke[1] != 0 || stroke[2] != 0 || stroke[3] != 0)
        {
            stroke += 4;
            ++count;
        }
    }
    READABLE_FUNCTION(setDebugGlyph, Verified);

    // initDebugFontGlyphs: the font's tables (the counts cleared twice): digits,
    // letters of both cases and five signs. Always 1.
    uint32_t initDebugFontGlyphs()
    {
        memoryFill(kGlyphStrokes, 0, kGlyphTablesBytes);
        for (uint32_t character = 0; character < kCharacters; ++character)
            glyphStrokeCounts()[character] = 0;
        const auto setGlyph = [](uint32_t character, uint32_t strokes)
        {
            setDebugGlyph(character, strokes);
        };
        int32_t digit = 0;
        for (;;)
        {
            const uint32_t strokes = kDigitStrokes + digit * kAStrokeList;
            setGlyph('0' + digit, strokes);
            if (++digit < 10)
                continue;
            break;
        }
        int32_t letter = 0;
        for (;;)
        {
            const uint32_t strokes = kLetterStrokes + letter * kAStrokeList;
            setGlyph('a' + letter, strokes);
            setGlyph('A' + letter, strokes);
            if (++letter < 26)
                continue;
            break;
        }
        setGlyph('>', kGreaterStrokes);
        setGlyph('<', kLessStrokes);
        setGlyph('-', kMinusStrokes);
        setGlyph(':', kColonStrokes);
        setGlyph('.', kPointStrokes);
        return 1;
    }
    READABLE_FUNCTION(initDebugFontGlyphs, Verified);

    // freeDebugFontGlyphs: the glyph tables are static.
    void freeDebugFontGlyphs() {}
    READABLE_FUNCTION(freeDebugFontGlyphs, Draft);

    // newDebugFont: a font of a colour and a size in screen units for the
    // grid's 192. Returns it, 0 if it could not be cleared.
    int32_t newDebugFont(ee::Ptr<uint32_t> colour, ee::Float width, ee::Float height)
    {
        ee::Local<Vec4> size; // its z and w stay what the stack held, and the scaling reads them
        const uint32_t memory = heapAllocate(sizeof(DebugFont));
        const uint32_t cleared = memoryFill(memory, 0, sizeof(DebugFont));
        if (cleared == 0)
            return 0;
        size->x = width;
        size->y = height;
        const ee::Ptr<DebugFont> font(cleared);
        font->glyphSize = *size * ee::Float(0.0052083335f); // 1 / 192
        std::memcpy(&font->colour, colour.get(), sizeof(uint32_t)); // it may be unaligned
        font->vertices = 0;
        font->strokeCapacity = 0;
        return static_cast<int32_t>(cleared);
    }
    READABLE_FUNCTION_WAITS(newDebugFont, Verified);

    // freeDebugFont
    void freeDebugFont(ee::Ptr<DebugFont> font)
    {
        const uint32_t vertices = font->vertices;
        if (vertices != 0)
            heapFree(vertices);
    }
    READABLE_FUNCTION_WAITS(freeDebugFont, Draft);

    // drawDebugFontText: the text from (screenX, screenY): every stroke of every
    // character a line, in vertices the font keeps and grows.
    void drawDebugFontText(ee::Ptr<DebugFont> font, uint32_t text, ee::Float screenX, ee::Float screenY)
    {
        ee::Local<ee::Float> penX;
        ee::Local<ee::Float> penY;
        ee::Local<uint32_t> vertices2;
        ee::Local<uint32_t> strokesSlot;
        ee::Local<uint32_t> verticesSlot;
        *penX = screenX;
        *penY = screenY;
        uint32_t strokes = 0;
        const uint32_t camera = *g_debugFontCamera;
        font->inverseNearClip = ee::Float(1.0f) / cameraNearClip(ee::Ptr<Camera>(camera));

        const ee::Ptr<int8_t> characters(text);
        for (uint32_t i = 0; characters[i] != 0; ++i)
            strokes += strokeCountOf(characters[i]);
        if (strokes == 0)
            return;

        if (font->strokeCapacity < strokes)
        {
            const uint32_t vertices = font->vertices;
            const uint32_t bytes = strokes * kBytesAStroke;
            uint32_t grown;
            if (vertices != 0)
                grown = reallocateIn(vertices, bytes, 0);
            else
                grown = allocateIn(bytes, 0, 1);
            if (grown != 0)
            {
                const ee::Ptr<LineVertex> vertex(grown);
                for (uint32_t i = 0; i < strokes * 2; ++i)
                {
                    LineVertex &line = vertex[i];
                    line.x = ee::Float(0.0f);
                    line.y = ee::Float(0.0f);
                    line.w = ee::Float(1.0f);
                    line.z = font->inverseNearClip;
                    line.u = 0;
                    line.v = 0;
                    std::memcpy(&line.colour, &font->colour, sizeof(uint32_t));
                }
                font->vertices = grown;
                font->strokeCapacity = strokes;
            }
            if (font->strokeCapacity < strokes)
                return;
        }

        ee::Ptr<LineVertex> vertex(font->vertices);
        for (uint32_t i = 0; characters[i] != 0; ++i)
        {
            const uint8_t character = static_cast<uint8_t>(characters[i]);
            int32_t left = strokeCountOf(characters[i]);
            ee::Ptr<uint8_t> stroke(glyphStrokes()[character]);
            if (left != 0)
            {
                for (;;)
                {
                    --left;
                    const uint8_t *points = stroke.get();
                    const auto x = [&](uint32_t point) { return *penX + ee::toFloat(static_cast<int32_t>(points[point])) * font->glyphSize.x; };
                    const auto y = [&](uint32_t point) { return *penY + ee::toFloat(kGlyphTop - static_cast<int32_t>(points[point])) * font->glyphSize.y; };
                    LineVertex *line = &*vertex;
                    line[0].x = x(0);
                    line[0].y = y(1);
                    line[1].x = x(2);
                    line[1].y = y(3);
                    stroke += 4;
                    vertex = vertex + 2;
                    if (left == 0)
                        break;
                }
            }
            *penX = *penX + font->glyphSize.x * ee::Float(192.0f);
        }

        *vertices2 = 0;
        *strokesSlot = strokes;
        *verticesSlot = font->vertices;
        drawTexturedPrimitives(kPrimitiveLines, vertices2, 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawDebugFontText, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_debugTextReady> g_debugTextReady;
        // Where the debug text starts: the screen's top left corner in GS
        // coordinates (12.4 fixed point) inside the 32 by 16 pixel margin
        // debugTextPrint keeps. Set by debugTextInit; nothing reads them in AI.
        inline ee::Global<int32_t, sym::g_debugTextOriginX> g_debugTextOriginX;
        inline ee::Global<int32_t, sym::g_debugTextOriginY> g_debugTextOriginY;
        constexpr int32_t kMarginX = 32 << 4, kMarginY = 16 << 4;
        inline ee::Global<int32_t, sym::g_debugFont> g_debugFontWord; // g_debugFontWord ((anonymous)::g_debugFontWord)
        // The debug font's colour: white, opaque.
        inline ee::Global<uint32_t, sym::g_debugTextColour> g_debugTextColour;
        constexpr ee::Float kDebugGlyphWidth = ee::Float(8.0f), kDebugGlyphHeight = ee::Float(12.0f);
    }

    // debugTextInit: the debug text ready: its origin, the font's glyph
    // tables, and a white font of 8 by 12 screen units. 1.
    uint32_t debugTextInit()
    {
        ee::Local<uint32_t> colour;
        *g_debugTextReady = 1;
        *g_debugTextOriginY = g_display->offsetY + kMarginY;
        *g_debugTextOriginX = g_display->offsetX + kMarginX;
        *colour = *g_debugTextColour;
        zge::initDebugFontGlyphs();
        *g_debugFontWord = zge::newDebugFont(ee::Ptr<uint32_t>(colour), kDebugGlyphWidth, kDebugGlyphHeight);
        return 1;
    }
    READABLE_FUNCTION_WAITS(debugTextInit, Verified);

    uint32_t debugTextShutdown()
    {
        if (*g_debugTextReady != 0)
        {
            freeDebugFont(*g_debugFont);
            zge::freeDebugFontGlyphs();
            *g_debugTextReady = 0;
            return 1;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(debugTextShutdown, Draft);
}
