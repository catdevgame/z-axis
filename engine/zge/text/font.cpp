// The text draw (zge/text/font.h): a text measured line by line, then each
// glyph made a textured quad (four vertices in screen space, the style's
// colour) and drawn through the 2D primitive API in batches of one texture,
// up to 64 quads; or, with an output, each quad written as a record for the
// caller. BMX XXX (PS2) has these functions nearly word for word; the
// GameCube and Xbox builds of all three games draw the same layout.

#include "font.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/memory/regions.h"
#include "zge/render/primitives.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/text/font.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kTextEscape = sym::textEscape;     // (text after '#', style) -> characters used (0: not an escape)
        constexpr uint32_t kMatrixRotateZ = sym::matrixRotateZ;  // (out, m, angle): m turned about z [inferred]
        constexpr uint32_t kFontGlyph = sym::fontGlyph;      // fontGlyph (hooked by a game override: called by address)
        constexpr uint32_t kDrawTextQuads = sym::drawTextQuads;  // drawTextQuads (the two text draws' way in; by address, as the original)

        // The 2D packet's flags for the text's quads (kPrimitiveScreen | 1 << 27 | 4).
        constexpr uint32_t kTextQuadFlags = 0x0A000004u;
        constexpr int32_t kQuadsPerBatch = 64;

        // Turns the style's angle into an Angle [inferred: its value].
        inline ee::Global<ee::Float, sym::g_angleScale> g_angleScale;
        // The width of each line of the text being drawn (texels), ended by
        // -1: measureText fills it for drawTextQuads.
        inline ee::Global<int32_t, sym::g_lineWidths> g_lineWidths;
        int32_t lineWidth(int32_t line) { return ee::Ptr<int32_t>(g_lineWidths.get())[static_cast<uint32_t>(line)]; }

        // A quad's vertex as the 2D primitive API reads it.
        struct TextVertex
        {
            ee::Float x, y;
            uint32_t z;           // +0x08 0
            uint32_t _0c;         // never written (the frame's previous contents)
            ee::Float u, v;       // +0x10 0 to 1 across the page
            uint8_t colour[4];    // +0x18
            uint32_t _1c;
        };
        static_assert(sizeof(TextVertex) == 0x20);

        // A quad for the primitive API: its texture and its four vertices
        // (the corners in the order 3, 2, 1, 0: bottom left first).
        struct TextQuad
        {
            uint32_t texture;
            ee::Ptr<TextVertex> corners[4];
        };
        static_assert(sizeof(TextQuad) == 0x14);


        // A non-negative int's conversion as the compiler does an unsigned
        // one: halved (keeping the low bit) and doubled when the top bit is
        // set. `negative` is called on that path .
        template <typename F>
        ee::Float unsignedToFloat(int32_t value, F negative)
        {
            if (value >= 0)
                return ee::toFloat(value);
            negative();
            const ee::Float half = ee::toFloat(static_cast<int32_t>((static_cast<uint32_t>(value) & 1u) | static_cast<uint32_t>(value) >> 1));
            return half + half;
        }

        // The width a space takes in a proportional font: 3/4 of the glyph
        // of 'a' (or 'A', or '0'); 0 if the font has none of them.
        ee::Ptr<Glyph> spaceModel(ee::Ptr<Font> font)
        {
            if (const ee::Ptr<Glyph> a = font->glyphs['a'])
                return a;
            if (const ee::Ptr<Glyph> capitalA = font->glyphs['A'])
                return capitalA;
            return font->glyphs['0'];
        }
    }

    ee::Ptr<const int8_t> parseTextFloat(ee::Ptr<const int8_t> start, ee::Ptr<ee::Float> value);
    ee::Ptr<const int8_t> parseTextInteger(ee::Ptr<const int8_t> start, ee::Ptr<int32_t> value);

    namespace
    {
        // Each of the font's pages given its texture (looked up by its name's
        // hash) the first time: loadFontTextures, written into ZGE2's
        // drawTextQuads.
        [[gnu::always_inline]] inline void loadPageTextures(ee::Ptr<Font> font)
        {
            int32_t count = font->pageCount;
            if (count == 0)
                return;
            for (int32_t page = 0;;)
            {
                if (font->pages[page].texture == 0)
                {
                    const uint32_t hash = nameHash(font->pages[page].name);
                    font->pages[page].texture = findTextureByHash(hash, 0);
                    count = font->pageCount;
                }
                ++page;
                if (page >= count)
                    break;
            }
        }

        // ZGE2's glyph lookup, written into its text code (ZGE3 made it
        // fontGlyph): a letter the font lacks is looked up in upper case,
        // then in lower case. Null if none.
        ee::Ptr<Glyph> glyphInAnyCase(ee::Ptr<Font> font, uint8_t code)
        {
            if (const ee::Ptr<Glyph> glyph = font->glyphs[code])
                return glyph;
            // The one-letter text the case changes work on.
            ee::Local<uint8_t[2]> letter;
            ee::game(sym::memset, letter, 0u, 2u);
            letter[0] = code;
            stringToUpper(letter);
            if (const ee::Ptr<Glyph> glyph = font->glyphs[letter[0]])
                return glyph;
            stringToLower(letter);
            return font->glyphs[letter[0]];
        }
    }

    // loadFontTextures: each of the font's pages given its texture (looked up by
    // its name's hash) the first time.
    void loadFontTextures(ee::Ptr<Font> font)
    {
        loadPageTextures(font);
    }
    READABLE_FUNCTION_WAITS(loadFontTextures, Verified);

    namespace
    {
        // Where an escape ends from `index`: the first '}' or the text's end.
        int32_t escapeEnd(ee::Ptr<const int8_t> text, int32_t index)
        {
            while (text[index] != '}' && text[index] != 0)
                ++index;
            return index;
        }

        bool startsWith(ee::Ptr<const int8_t> text, const char *word)
        {
            for (; *word; ++word, ++text)
                if (*text != *word)
                    return false;
            return true;
        }
    }

    // textEscape: an escape after a '#' read into the style: RGBA{r,g,b,a}
    // and RGB{r,g,b} (the alpha kept) set its colour, SCALE{x,y,c,d} the two
    // scales and the two words after them. Each value is read by
    // parseTextInteger or parseTextFloat from where the last stopped, but the
    // closing brace is looked for from the first value on, so a value's own
    // text is walked twice. The characters used, the '#' and '}' counted
    // (also when the text ended first); 0 if this is no escape. ZGE4 takes a
    // third word: 0 reads the escape and leaves the style as it is (BMX XXX's
    // drawWrappedText, measuring a word ahead).
    int32_t textEscape(ee::Ptr<const int8_t> text, ee::Ptr<TextStyle> style READABLE_PARAM_ONLY(bmx, uint32_t apply = 1))
    {
        ee::Local<uint8_t[4]> colour;   // the new colour
        ee::Local<ee::Float[4]> scales; // SCALE's four values
        ee::Local<int32_t> rgbaValue;   // the number being read
        ee::Local<int32_t> rgbValue;
        ee::Local<ee::Float> scaleValue;

        // `count` numbers from `from`, one after the other, each one's low
        // byte a channel of the new colour.
        const auto readChannels = [&](ee::Ptr<const int8_t> from, ee::Local<int32_t> &value, int count) {
            ee::Ptr<const int8_t> next = from;
            for (int channel = 0; channel < count; ++channel)
            {
                next = parseTextInteger(next, value);
                colour[channel] = static_cast<uint8_t>(*value);
            }
        };
        const auto storeColour = [&](int32_t valuesStart) {
            if (apply != 0)
                for (int channel = 0; channel < 4; ++channel)
                    style->colour[channel] = colour[channel];
            return escapeEnd(text, valuesStart) + 2;
        };

        if (startsWith(text, "RGBA{"))
        {
            readChannels(text + 5, rgbaValue, 4);
            return storeColour(5);
        }
        if (startsWith(text, "RGB{"))
        {
            readChannels(text + 4, rgbValue, 3);
            colour[3] = style->colour[3];
            return storeColour(4);
        }
        if (startsWith(text, "SCALE{"))
        {
            ee::Ptr<const int8_t> next = text + 6;
            for (uint32_t index = 0; index < 4; ++index)
            {
                next = parseTextFloat(next, scaleValue);
                scales[index] = *scaleValue;
            }
            const int32_t end = escapeEnd(text, 6);
            if (apply != 0)
            {
                style->scaleExtra[0] = scales[2];
                style->scaleExtra[1] = scales[3];
                style->scaleX = scales[0];
                style->scaleY = scales[1];
            }
            return end + 2;
        }
        return 0;
    }
    READABLE_FUNCTION(textEscape, Verified);

    // measureText: the text's bounds (in the style's units, from its
    // anchor and alignment) and each line's width in texels into lineWidths
    // (if given; -1 after the last). Escapes change a copy of the style as
    // they go. Lines end at '\r' (a '\n' after it is skipped as a
    // character); characters without a glyph take no room (a space: the
    // font's space width). Returns the number of glyphs.
    int32_t measureText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text, ee::Ptr<TextBounds> bounds,
                        ee::Ptr<int32_t> lineWidths)
    {
        ee::Local<Matrix> scale;
        ee::Local<TextStyle> local;
        // A copy of the style: escapes change it.
        *local = *style;
        int32_t glyphs = 0;
        if (text)
        {
            *bounds = TextBounds{};
            int32_t index = 0;
            int32_t width = 0;     // this line's, in texels
            int32_t line = 0;
            int32_t nextLine = 1;
            // The walk's steps, at the original's block starts .
            enum class Step { Read, Test, LineEnd, Character, Glyph, GlyphAfterEscape, Add, Advance, Next, Done };
            Step step = Step::Read;
            int32_t c = 0;
            int32_t advance = 0;
            while (step != Step::Done)
            {
                switch (step)
                {
                case Step::Read:
                    c = text[index];
                    step = Step::Test;
                    break;
                case Step::Test:
                    step = (c == '\r' || c == 0) ? Step::LineEnd : Step::Character;
                    break;
                case Step::LineEnd:
                {
                    const ee::Float lineSize = unsignedToFloat(width, [] {  });
                    if (bounds->right < lineSize)
                        bounds->right = unsignedToFloat(width, [] {  });
                    if (lineWidths)
                        lineWidths[line] = width;
                    width = 0;
                    line = nextLine;
                    if (text[index] == 0)
                        step = Step::Done;
                    else
                    {
                        ++nextLine;
                        step = Step::Advance;
                    }
                    break;
                }
                case Step::Character:
                    if (c == '\n')
                        step = Step::Advance;
                    else if (c != '#')
                    {
                        c = static_cast<uint8_t>(text[index]);
                        step = Step::Glyph;
                    }
                    else if (text[index + 1] == 0)
                        step = Step::GlyphAfterEscape;
                    else
                    {
                        index += textEscape(text + index + 1, local READABLE_PARAM_ONLY(bmx, 1u));
                        c = text[index];
                        if (c == '\r')
                            step = Step::LineEnd;
                        else if (c == 0)
                            step = Step::Test;
                        else
                            step = Step::GlyphAfterEscape;
                    }
                    break;
                case Step::GlyphAfterEscape:
                    c = static_cast<uint8_t>(text[index]);
                    step = Step::Glyph;
                    break;
                case Step::Glyph:
                {
                    const ee::Ptr<Glyph> glyph(fontGlyph(font, c));
                    if (glyph)
                    {
                        ++glyphs;
                        step = Step::Add;
                        if (local->monospaced)
                            advance = font->height + local->letterSpacing;
                        else
                        {
                            const int32_t spacing = local->letterSpacing;
                            const int32_t glyphWidth = glyph->width;
                            advance = glyphWidth + spacing;
                            if (spacing <= 0)
                            {
                                // A negative spacing over a quarter of the
                                // glyph: the glyph's width and 2 instead.
                                if (ee::toFloat(glyphWidth) * ee::Float(0.25f) < ee::abs(ee::toFloat(spacing)))
                                {
                                    width = width + 2 + glyphWidth;
                                    step = Step::Advance;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (text[index] != ' ')
                        {
                            ++index;
                            step = Step::Next;
                        }
                        else if (local->monospaced)
                        {
                            advance = font->height + local->letterSpacing;
                            step = Step::Add;
                        }
                        else
                        {
                            if (const ee::Ptr<Glyph> model = spaceModel(font))
                            {
                                advance = ee::toInt(ee::toFloat(model->width) * ee::Float(0.75f));
                                step = Step::Add;
                            }
                            else
                            {
                                ++index;
                                step = Step::Next;
                            }
                        }
                    }
                    break;
                }
                case Step::Add:
                    width += advance;
                    step = Step::Advance;
                    break;
                case Step::Advance:
                    ++index;
                    step = Step::Next;
                    break;
                case Step::Next:
                    step = Step::Read;
                    break;
                case Step::Done:
                    break;
                }
            }
            if (lineWidths)
                lineWidths[nextLine] = -1;
            // The style's scale as a matrix's diagonal (x at +0x00, y at +0x14).
            matrixIdentity(&*scale);
            matrixScale(&*scale, &*scale, local->scale());
            const ee::Float scaleX = scale->m[0][0];
            const ee::Float scaleY = scale->m[1][1];
            const uint32_t align = local->align;
            const uint32_t vertical = align & kAlignVertical;
            const ee::Float anchorY = local->position.y;
            // The text's height: lines * the font's height * the spacing * scale.
            auto linesHeight = [&]() {
                return unsignedToFloat(nextLine * static_cast<int32_t>(font->height), [&] {  });
            };
            if (vertical == kAlignMiddle)
            {
                const ee::Float spacing = local->lineSpacing;
                const ee::Float lines = linesHeight();
                bounds->top = anchorY - lines * spacing * scaleY * ee::Float(0.5f);
                const ee::Float lines2 = linesHeight();
                bounds->bottom = anchorY + lines2 * spacing * scaleY * ee::Float(0.5f);
            }
            else if (vertical == kAlignTop)
            {
                bounds->top = anchorY;
                const ee::Float lines = linesHeight();
                bounds->bottom = anchorY + lines * local->lineSpacing * scaleY;
            }
            else if (vertical > kAlignMiddle)
            {
                if (vertical == kAlignBottom)
                {
                    const ee::Float spacing = local->lineSpacing;
                    const ee::Float lines = linesHeight();
                    bounds->bottom = anchorY;
                    bounds->top = anchorY - lines * spacing * scaleY;
                }
            }
            const uint32_t across = align & kAlignHorizontal;
            const ee::Float anchorX = local->position.x;
            if (across == kAlignCentre)
            {
                const ee::Float size = bounds->right;
                const ee::Float scaled = size * scaleX;
                bounds->left = anchorX - ee::toFloat(ee::toInt(scaled) / 2);
                bounds->right = anchorX + ee::toFloat(ee::toInt(scaled) / 2);
            }
            else if (across == kAlignLeft)
            {
                const ee::Float size = bounds->right * scaleX;
                bounds->left = anchorX;
                bounds->right = anchorX + size;
            }
            else if (across == kAlignRight)
            {
                const ee::Float size = bounds->right * scaleX;
                bounds->right = anchorX;
                bounds->left = anchorX - size;
            }
            else
                return glyphs;
        }
        return glyphs;
    }

    // measureOrientedText: ZGE2's measureText. The same walk, with these
    // differences: the bounds are cleared before the text is looked at; a
    // glyph is looked up with its case fallback written in; there is no
    // monospaced style; a negative spacing is weighed against a quarter of
    // the glyph's width in whole texels; a space takes 3/4 of the model
    // glyph converted as unsigned; lineWidths ends with 0, not -1. The
    // style's scale is turned by its orientation (only the scale matrix's
    // diagonal is read), and a centred text's half width is truncated as
    // unsigned.
    int32_t measureOrientedText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text,
                                ee::Ptr<TextBounds> bounds, ee::Ptr<int32_t> lineWidths)
    {
        ee::Local<Matrix> scale;
        ee::Local<TextStyle> local;
        // A copy of the style that escapes change. ZGE2 copies it up to its
        // height (0x50 bytes).
        std::memcpy(&*local, &*style, offsetof(TextStyle, height));
        *bounds = TextBounds{};
        if (!text)
            return 0;
        int32_t glyphs = 0;
        int32_t index = 0;
        int32_t width = 0; // this line's, in texels
        int32_t line = 0;
        int32_t lines = 1;
        for (;;)
        {
            int32_t c = text[index];
            bool lineEnds = c == '\r' || c == 0;
            if (!lineEnds && c == '\n')
            {
                ++index;
                continue;
            }
            if (!lineEnds && c == '#' && text[index + 1] != 0)
            {
                // The character after an escape is drawn as it is (a
                // second escape straight after one is not read).
                index += textEscape(text + index + 1, local READABLE_PARAM_ONLY(bmx, 1u));
                c = text[index];
                lineEnds = c == '\r' || c == 0;
            }
            if (lineEnds)
            {
                if (bounds->right < unsignedToFloat(width, [] {}))
                    bounds->right = unsignedToFloat(width, [] {});
                if (lineWidths)
                    lineWidths[line] = width;
                width = 0;
                line = lines;
                if (text[index] == 0)
                    break;
                ++lines;
                ++index;
                continue;
            }
            if (const ee::Ptr<Glyph> glyph = glyphInAnyCase(font, static_cast<uint8_t>(text[index])))
            {
                ++glyphs;
                const int32_t spacing = local->letterSpacing;
                // A negative spacing over a quarter of the glyph: the
                // glyph's width and 2 instead.
                if (spacing <= 0 && ee::toFloat(glyph->width >> 2) < ee::abs(ee::toFloat(spacing)))
                    width = width + 2 + glyph->width;
                else
                    width += glyph->width + spacing;
            }
            else if (text[index] == ' ')
            {
                if (const ee::Ptr<Glyph> model = spaceModel(font))
                    width += static_cast<int32_t>(sce::libgcc::fptoui(ee::toFloat(model->width) * ee::Float(0.75f)));
            }
            ++index;
        }
        if (lineWidths)
            lineWidths[lines] = 0;

        quaternionToMatrix(&*scale, styleOrientation(*local));
        matrixScale(&*scale, &*scale, local->scale());
        const ee::Float scaleX = scale->m[0][0];
        const ee::Float scaleY = scale->m[1][1];
        const ee::Float anchorX = local->position.x;
        const ee::Float anchorY = local->position.y;
        const ee::Float spacing = local->lineSpacing;
        const uint32_t align = local->align;
        // The text's height before the spacing and scale: lines * the font's height.
        const auto linesHeight = [&] { return unsignedToFloat(lines * static_cast<int32_t>(font->height), [] {}); };
        switch (align & kAlignVertical)
        {
        case kAlignMiddle:
            bounds->top = anchorY - linesHeight() * spacing * scaleY * ee::Float(0.5f);
            bounds->bottom = anchorY + linesHeight() * spacing * scaleY * ee::Float(0.5f);
            break;
        case kAlignTop:
            bounds->top = anchorY;
            bounds->bottom = anchorY + linesHeight() * spacing * scaleY;
            break;
        case kAlignBottom:
            bounds->bottom = anchorY;
            bounds->top = anchorY - linesHeight() * spacing * scaleY;
            break;
        default:
            break;
        }
        const ee::Float size = bounds->right;
        switch (align & kAlignHorizontal)
        {
        case kAlignCentre:
        {
            // Half the scaled width, truncated (worked out for each side).
            const auto half = [&] { return ee::toFloat(static_cast<int32_t>(sce::libgcc::fptoui(size * scaleX) >> 1)); };
            bounds->left = anchorX - half();
            bounds->right = anchorX + half();
            break;
        }
        case kAlignLeft:
            bounds->left = anchorX;
            bounds->right = anchorX + size * scaleX;
            break;
        case kAlignRight:
            bounds->right = anchorX;
            bounds->left = anchorX - size * scaleX;
            break;
        default:
            break;
        }
        return glyphs;
    }
    READABLE_FUNCTION_OWN(measureText, Verified, dm2, measureOrientedText);

    // drawTextQuads: the text drawn in the style: measured (its size into the
    // style, each line's width into g_lineWidths), then laid out from the
    // anchor by the alignment, line by line, each glyph a quad in the
    // style's colour, turned by the style's angle about the anchor. The
    // quads go to the 2D primitive API in batches of one texture, or, given
    // out, into a list allocated for them (*out; ended by a texture of -1).
    void drawTextQuads(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text,
                       ee::Ptr<ee::Ptr<TextQuadRecord>> out)
    {
        ee::Local<TextQuad[kQuadsPerBatch]> quads;
        ee::Local<TextVertex[4 * kQuadsPerBatch]> vertices;
        ee::Local<Matrix> matrix;
        ee::Local<TextBounds> bounds;
        if (!text)
            return;
        if (!style)
            return;
        int32_t index = 0;
        int32_t batch = 0; // quads waiting to be drawn
        ee::Ptr<TextQuadRecord> record;
        loadFontTextures(font);
        const int32_t glyphCount = measureText(font, style, text, bounds, ee::Ptr<int32_t>(g_lineWidths.get()));
        style->width = bounds->right - bounds->left;
        style->height = bounds->bottom - bounds->top;
        if (out)
        {
            record = ee::Ptr<TextQuadRecord>(heapAllocate((glyphCount + 1) * static_cast<int32_t>(sizeof(TextQuadRecord))));
            *out = record;
        }
        matrixIdentity(&*matrix);
        const int32_t angle = static_cast<int16_t>(ee::toInt(style->angle * *g_angleScale));
        matrixRotateZ(matrix, matrix, angle);
        matrix->m[3][0] = style->position.x;
        matrix->m[3][1] = style->position.y;
        matrix->m[3][2] = style->position.z;

        int32_t lines = 0;
        uint32_t align;
        if (lineWidth(0) >= 0)
        {
            align = style->align;
            for (;;)
            {
                ++lines;
                if (lineWidth(lines) < 0)
                    break;
            }
        }
        else
            align = style->align;

        // The first line's top, from the vertical alignment.
        ee::Float lineY = ee::Float(0.0f);
        const uint32_t vertical = align & kAlignVertical;
        auto textHeight = [&]() {
            return unsignedToFloat(static_cast<int32_t>(static_cast<uint32_t>(lines) * font->height), [&] {  });
        };
        if (vertical == kAlignMiddle)
        {
            const ee::Float height = textHeight();
            const ee::Float spaced = height * style->lineSpacing;
            lineY = -(style->scaleY * spaced * ee::Float(0.5f));
        }
        else if (vertical == kAlignTop)
            lineY = ee::Float(0.0f);
        else if (vertical > kAlignMiddle)
        {
            if (vertical == kAlignBottom)
            {
                const ee::Float height = textHeight();
                const ee::Float spacing = style->lineSpacing;
                lineY = -(height * spacing * style->scaleY);
            }
        }

        uint32_t batchTexture = 0;
        bool afterEscape = false;        // an escape was read since the last batch started
        ee::Float penX = ee::Float(0.0f); // (kept from the previous line under an unknown alignment)
        if (lineWidth(0) >= 0)
        {
            for (int32_t line = 0;;)
            {
                switch (align & kAlignHorizontal)
                {
                case kAlignCentre:
                    penX = -(ee::toFloat(lineWidth(line)) * style->scaleX * ee::Float(0.5f));
                    break;
                case kAlignLeft:
                    penX = ee::Float(0.0f);
                    break;
                case kAlignRight:
                    penX = -(ee::toFloat(lineWidth(line)) * style->scaleX);
                    break;
                default:
                    break;
                }
                const int32_t nextLine = line + 1;
                // How the line ended: the text's end (reached in the walk),
                // a '\r' or an empty line.
                if (text[index] != 0)
                {
                    bool lineBreak = false;
                    for (;;)
                    {
                        if (text[index] == '#')
                        {
                            if (text[index + 1] != 0)
                            {
                                const int32_t used = textEscape(text + index + 1, style READABLE_PARAM_ONLY(bmx, 1u));
                                index += used;
                                afterEscape = used != 0;
                                if (text[index] == 0)
                                    break;
                            }
                        }
                        if (text[index] == '\r')
                        {
                            lineBreak = true;
                            break;
                        }
                        const ee::Ptr<Glyph> glyph(fontGlyph(font, static_cast<uint8_t>(text[index])));
                        const int32_t next = index + 1;
                        if (glyph)
                        {
                            const ee::Ptr<FontPage> page = font->pages + glyph->page;
                            const ee::Float pageWidth = ee::toFloat(page->width);
                            const ee::Float pageHeight = ee::toFloat(page->height);
                            if (!out)
                            {
                                if (page->texture != batchTexture || batch == kQuadsPerBatch)
                                {
                                    // A new batch: the waiting quads drawn.
                                    bool started = afterEscape;
                                    if (batch != 0)
                                    {
                                        drawPrimitiveQuads(kTextQuadFlags, quads, batch, matrix);
                                        started = true;
                                    }
                                    afterEscape = false;
                                    if (started)
                                        batch = 0;
                                    batchTexture = font->pages[glyph->page].texture;
                                }
                                TextQuad &quad = quads[batch];
                                const ee::Ptr<TextVertex> first = ee::Ptr<TextVertex>(vertices) + 4 * batch;
                                quad.corners[0] = first + 3;
                                quad.corners[1] = first + 2;
                                quad.corners[2] = first + 1;
                                quad.corners[3] = first;
                                quad.texture = font->pages[glyph->page].texture;
                            }
                            // (After a new batch's reset: 64 waiting quads leave
                            // batch one past the vertices, on the matrix.)
                            const ee::Ptr<TextVertex> corner = ee::Ptr<TextVertex>(vertices) + 4 * batch;
                            // A monospaced glyph is centred in its cell; a
                            // negative spacing over a quarter of the glyph
                            // moves it a texel on.
                            ee::Float monoOffset = ee::Float(0.0f);
                            ee::Float scaleX;
                            uint32_t monospaced = style->monospaced;
                            if (monospaced)
                            {
                                scaleX = style->scaleX;
                                monoOffset = ee::toFloat(static_cast<int32_t>(font->height) - glyph->width) * scaleX * ee::Float(0.5f);
                            }
                            else
                            {
                                const int32_t spacing = style->letterSpacing;
                                scaleX = style->scaleX;
                                if (spacing <= 0 && ee::toFloat(glyph->width) * ee::Float(0.25f) < ee::abs(ee::toFloat(spacing)))
                                    penX = penX + ee::Float(1.0f);
                            }
                            const ee::Float scaleY = style->scaleY;
                            const ee::Float left = penX + ee::Float(-0.5f) + monoOffset;
                            const ee::Float right = penX + ee::toFloat(glyph->width) * scaleX + ee::Float(-0.5f) + monoOffset;
                            const ee::Float top = lineY + ee::toFloat(glyph->top) * scaleY + ee::Float(-0.5f);
                            const ee::Float bottom = lineY + ee::toFloat(glyph->top) * scaleY + ee::toFloat(glyph->height) * scaleY + ee::Float(-0.5f);
                            const ee::Float uLeft = ee::toFloat(glyph->u) / pageWidth;
                            const ee::Float uRight = ee::toFloat(glyph->u + glyph->width) / pageWidth;
                            const ee::Float vTop = ee::toFloat(glyph->v) / pageHeight;
                            const ee::Float vBottom = ee::toFloat(glyph->v + glyph->height) / pageHeight;
                            auto set = [&](int i, ee::Float x, ee::Float y, ee::Float u, ee::Float v) {
                                TextVertex &vertex = corner[i];
                                vertex.x = x;
                                vertex.y = y;
                                vertex.z = 0;
                                vertex.u = u;
                                vertex.v = v;
                                std::memcpy(vertex.colour, style->colour, 4);
                            };
                            set(0, left, top, uLeft, vTop);
                            set(1, right, top, uRight, vTop);
                            set(2, right, bottom, uRight, vBottom);
                            set(3, left, bottom, uLeft, vBottom);
                            if (!out)
                                ++batch;
                            else
                            {
                                record->texture = font->pages[glyph->page].texture;
                                for (int i = 0;;)
                                {
                                    std::memcpy(&record->corners[i], &corner[i], 0x10);
                                    record->corners[i].z = ee::Float(0.0f);
                                    record->u[i] = corner[i].u;
                                    record->v[i] = corner[i].v;
                                    std::memcpy(record->colour, corner[i].colour, 4);
                                    if (++i == 4)
                                        break;
                                }
                                scaleX = style->scaleX;
                                ++record;
                                monospaced = style->monospaced;
                            }
                            // The pen moved on by the glyph's advance.
                            const int32_t spacing = style->letterSpacing;
                            int32_t advance = 0;
                            bool added = false;
                            if (monospaced)
                                advance = font->height;
                            else
                            {
                                advance = glyph->width;
                                if (spacing <= 0)
                                {
                                    const ee::Float glyphWidth = ee::toFloat(advance);
                                    if (glyphWidth * ee::Float(0.25f) < ee::abs(ee::toFloat(spacing)))
                                    {
                                        penX = penX + (glyphWidth * scaleX + ee::Float(1.0f));
                                        added = true;
                                    }
                                    else
                                    {
                                        penX = penX + ee::toFloat(advance + spacing) * scaleX;
                                        added = true;
                                    }
                                }
                            }
                            if (!added)
                                penX = penX + ee::toFloat(advance + spacing) * scaleX;
                        }
                        else
                        {
                            // No glyph: only a space takes room.
                            if (text[index] == ' ')
                            {
                                if (style->monospaced)
                                    penX = penX + ee::toFloat(font->height + style->letterSpacing) * style->scaleX;
                                else
                                {
                                    if (const ee::Ptr<Glyph> model = spaceModel(font))
                                        penX = penX + ee::toFloat(ee::toInt(ee::toFloat(model->width) * style->scaleX * ee::Float(0.75f)));
                                }
                            }
                        }
                        index = next;
                        if (text[index] == 0)
                            break;
                    }
                    if (lineBreak)
                    {
                        // '\r', and a '\n' after it, skipped.
                        ++index;
                        if (text[index] == '\n')
                            ++index;
                    }
                }
                line = nextLine;
                lineY = lineY + ee::toFloat(ee::toInt(ee::toFloat(font->height) * style->scaleY * style->lineSpacing));
                if (lineWidth(line) < 0)
                    break;
                align = style->align;
            }
        }
        if (!out)
        {
            if (batch != 0)
            {
                drawPrimitiveQuads(kTextQuadFlags, quads, batch, matrix);
                return;
            }
        }
        else
            record->texture = 0xFFFFFFFFu;
    }

    namespace
    {
        // ZGE2's flags for the text's quads: the 2D packet, its depth from
        // the first vertex (kPrimitiveScreen | kPrimitiveDepthFromVertex,
        // zge/render/ps2/primitive_packets.cpp; no bit 27).
        constexpr uint32_t kOrientedTextQuadFlags = 0x02000004u;
    }

    // drawOrientedTextQuads: ZGE2's drawTextQuads. The text measured
    // (measureText, ZGE2's measureOrientedText: each line's width into
    // g_lineWidths), then laid out on the screen itself: each line from the
    // anchor by the alignment, the first line's top the measured top; the
    // glyphs looked up in any case (glyphInAnyCase); the quads go
    // through a matrix of the style's orientation (normalized in place) at
    // the anchor's depth. Unlike ZGE3: the style's measured size is not
    // stored and there is no null style check or monospaced style; the
    // lines end at a width of 0 (so an empty line ends the text); a '\r'
    // skips the character after it whatever it is; the advances are not
    // truncated to whole texels.
    void drawOrientedTextQuads(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text,
                               ee::Ptr<ee::Ptr<TextQuadRecord>> out)
    {
        ee::Local<TextQuad[kQuadsPerBatch]> quads;
        ee::Local<TextVertex[4 * kQuadsPerBatch]> vertices;
        ee::Local<Matrix> matrix;
        ee::Local<TextBounds> bounds;
        if (!text)
            return;
        int32_t index = 0;
        int32_t batch = 0; // quads waiting to be drawn
        uint32_t batchTexture = 0;
        ee::Ptr<TextQuadRecord> record;
        loadPageTextures(font);
        const int32_t glyphCount = measureText(font, style, text, bounds, ee::Ptr<int32_t>(g_lineWidths.get()));
        if (out)
        {
            record = ee::Ptr<TextQuadRecord>(heapAllocate((glyphCount + 1) * static_cast<int32_t>(sizeof(TextQuadRecord))));
            *out = record;
        }
        quaternionToMatrix(&*matrix, styleOrientation(*style));
        matrix->m[3][0] = ee::Float(0.0f);
        matrix->m[3][1] = ee::Float(0.0f);
        matrix->m[3][2] = style->position.z;

        ee::Float penX(0.0f); // (kept from the previous line under an unknown alignment)
        ee::Float lineY = bounds->top;
        for (int32_t line = 0; lineWidth(line) != 0;)
        {
            switch (style->align & kAlignHorizontal)
            {
            case kAlignCentre:
                penX = style->position.x - ee::Float(0.5f) * (unsignedToFloat(lineWidth(line), [] {}) * style->scaleX);
                break;
            case kAlignLeft:
                penX = style->position.x;
                break;
            case kAlignRight:
                penX = style->position.x - unsignedToFloat(lineWidth(line), [] {}) * style->scaleX;
                break;
            default:
                break;
            }
            ++line;
            while (text[index] != 0)
            {
                if (text[index] == '#' && text[index + 1] != 0)
                {
                    index += textEscape(text + index + 1, style READABLE_PARAM_ONLY(bmx, 1u));
                    if (text[index] == 0)
                        break;
                }
                if (text[index] == '\r')
                {
                    index += 2;
                    break;
                }
                if (const ee::Ptr<Glyph> glyph = glyphInAnyCase(font, static_cast<uint8_t>(text[index])))
                {
                    const ee::Ptr<FontPage> page = font->pages + glyph->page;
                    const ee::Float pageWidth = ee::toFloat(page->width);
                    const ee::Float pageHeight = ee::toFloat(page->height);
                    if (!out)
                    {
                        if (page->texture != batchTexture || batch == kQuadsPerBatch)
                        {
                            // A new batch: the waiting quads drawn.
                            if (batch != 0)
                            {
                                drawPrimitiveQuads(kOrientedTextQuadFlags, quads, batch, matrix);
                                batch = 0;
                            }
                            batchTexture = page->texture;
                        }
                        TextQuad &quad = quads[batch];
                        const ee::Ptr<TextVertex> first = ee::Ptr<TextVertex>(vertices) + 4 * batch;
                        quad.corners[0] = first + 3;
                        quad.corners[1] = first + 2;
                        quad.corners[2] = first + 1;
                        quad.corners[3] = first;
                        quad.texture = page->texture;
                    }
                    const ee::Ptr<TextVertex> corner = ee::Ptr<TextVertex>(vertices) + 4 * batch;
                    const int32_t spacing = style->letterSpacing;
                    // A negative spacing over a quarter of the glyph (in
                    // whole texels) moves it a texel on.
                    const bool tight = spacing <= 0 && ee::toFloat(glyph->width >> 2) < ee::abs(ee::toFloat(spacing));
                    if (tight)
                        penX = penX + ee::Float(1.0f);
                    const ee::Float scaleX = style->scaleX;
                    const ee::Float scaleY = style->scaleY;
                    const ee::Float left = penX + ee::Float(-0.5f);
                    const ee::Float right = penX + ee::toFloat(glyph->width) * scaleX + ee::Float(-0.5f);
                    const ee::Float top = lineY + ee::toFloat(glyph->top) * scaleY + ee::Float(-0.5f);
                    const ee::Float bottom = lineY + ee::toFloat(glyph->top) * scaleY + ee::toFloat(glyph->height) * scaleY + ee::Float(-0.5f);
                    const ee::Float uLeft = ee::toFloat(glyph->u) / pageWidth;
                    const ee::Float uRight = ee::toFloat(glyph->u + glyph->width) / pageWidth;
                    const ee::Float vTop = ee::toFloat(glyph->v) / pageHeight;
                    const ee::Float vBottom = ee::toFloat(glyph->v + glyph->height) / pageHeight;
                    auto set = [&](int i, ee::Float x, ee::Float y, ee::Float u, ee::Float v) {
                        TextVertex &vertex = corner[i];
                        vertex.x = x;
                        vertex.y = y;
                        vertex.z = 0;
                        vertex.u = u;
                        vertex.v = v;
                        std::memcpy(vertex.colour, style->colour, 4);
                    };
                    set(0, left, top, uLeft, vTop);
                    set(1, right, top, uRight, vTop);
                    set(2, right, bottom, uRight, vBottom);
                    set(3, left, bottom, uLeft, vBottom);
                    if (!out)
                        ++batch;
                    else
                    {
                        record->texture = page->texture;
                        for (int i = 0; i < 4; ++i)
                        {
                            std::memcpy(&record->corners[i], &corner[i], 0x10);
                            record->corners[i].z = ee::Float(0.0f);
                            record->u[i] = corner[i].u;
                            record->v[i] = corner[i].v;
                            std::memcpy(record->colour, corner[i].colour, 4);
                        }
                        ++record;
                    }
                    // The pen moved on by the glyph's advance.
                    if (tight)
                        penX = penX + (ee::toFloat(glyph->width) * scaleX + ee::Float(1.0f));
                    else
                        penX = penX + ee::toFloat(glyph->width + spacing) * scaleX;
                }
                else if (text[index] == ' ')
                {
                    // No glyph: only a space takes room.
                    if (const ee::Ptr<Glyph> model = spaceModel(font))
                        penX = penX + ee::toFloat(model->width) * style->scaleX * ee::Float(0.75f);
                }
                ++index;
            }
            lineY = lineY + ee::toFloat(font->height) * style->scaleY * style->lineSpacing;
        }
        if (!out)
        {
            if (batch != 0)
                drawPrimitiveQuads(kOrientedTextQuadFlags, quads, batch, matrix);
        }
        else
            record->texture = 0xFFFFFFFFu;
    }
    READABLE_FUNCTION_WAITS_OWN(drawTextQuads, Verified, dm2, drawOrientedTextQuads);

    namespace // the game functions below call: ee/plain.h
    {
        using QuadList = ee::Ptr<ee::Ptr<TextQuadRecord>>;
        int32_t drawQuadsOf(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text, QuadList out) { return ee::game<int32_t>(kDrawTextQuads, font, style, text, out); }
        namespace tail
        {
        int32_t drawQuadsOf(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text, QuadList out) { return ee::tailCall<int32_t>(kDrawTextQuads, font, style, text, out); }
        }
    }

    // textQuadList (measureTextAsset; no route reaches it): drawTextQuads
    // into a list instead of the screen: the list (its quads, ended by a
    // texture of -1). It measures nothing itself: the caller does.
    ee::Ptr<TextQuadRecord> textQuadList(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text)
    {
        ee::Local<ee::Ptr<TextQuadRecord>> list;
        drawQuadsOf(font, style, text, list);
        return *list;
    }
    READABLE_FUNCTION_WAITS(textQuadList, Draft);

    // drawText: drawTextQuads with no output: the text drawn. A game
    // override wraps it (the button prompts).
    int32_t drawText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text)
    {
        return tail::drawQuadsOf(font, style, text, ee::Ptr<ee::Ptr<TextQuadRecord>>());
    }
    READABLE_FUNCTION_WAITS(drawText, Verified);

    namespace
    {
        constexpr uint32_t kTextBufferBytes = 0x1FFE;
        // What the width of a space is taken from, by the font's glyphs (three
        // quarters of the first it has).
        constexpr uint8_t kSpaceWidthGlyphs[3] = {'a', 'A', '0'};
    }

    // drawWrappedText: the text word-wrapped to maxWidth and to maxHeight's
    // lines: a copy of it (at most 8190 characters, in the frame) gets a CR
    // for each line break it needs (or a 0 at the first line past the height)
    // and is drawn from line firstLine with drawTextQuads if `draw`. The line
    // count goes to lineCount. Escapes are read on a copy of the style. A word
    // wider than maxWidth stays whole on a line of its own, and the line then
    // counts as full. A game override wraps it (the button prompts).
    void drawWrappedText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const char> text, int64_t firstLine,
                            ee::Ptr<int32_t> lineCount, uint32_t draw, ee::Float maxWidth, ee::Float maxHeight)
    {
        ee::Local<int8_t[0x2000]> buffer;
        ee::Local<TextStyle> styleCopy;
        *styleCopy = *style;
        if (!style || !text || *text == 0)
            return;

        // Scan position in the buffer's characters, and the one after it.
        int32_t position = 0;
        int32_t next = 1;
        ee::Float heightLeft = maxHeight;
        int64_t wordWidth = 0;
        const ee::Float quarter(0.25f);
        const ee::Float threeQuarters(0.75f);
        stringCopyN(buffer, text, kTextBufferBytes);
        buffer[0x1FFF] = 0;
        const ee::Ptr<int8_t> chars = buffer;

        int32_t lineWidth = 0;         // of the words so far on this line
        ee::Ptr<int8_t> lastBreak;     // the last place a line could break at
        ee::Ptr<int8_t> drawFrom = chars;  // the first line drawn
        ee::Ptr<int8_t> lineStart = chars;
        int64_t lineIndex = 0;
        int32_t linesCounted = 1;
        bool beforeFirstLine = 0 < firstLine;
        ee::Ptr<int8_t> cursor = chars;

        // A line is done: the first drawn line noted, the height it takes
        // used. A line that would not fit ends the text at `end`.
        auto lineDone = [&](ee::Ptr<int8_t> end)
        {
            if (lineIndex == firstLine)
                drawFrom = lineStart;
            const ee::Float lineHeight = ee::toFloat(static_cast<int32_t>(font->height)) * style->scaleY * style->lineSpacing;
            heightLeft = heightLeft - ee::toFloat(ee::toInt(lineHeight));
            if (heightLeft < lineHeight)
                *end = 0;
        };
        auto nextLine = [&]
        {
            lineIndex = linesCounted;
            beforeFirstLine = static_cast<int64_t>(linesCounted) < firstLine;
            ++linesCounted;
        };

        for (;;)
        {
            int32_t c = *cursor;
            if (c == '#')
            {
                if (cursor[1] != 0)
                {
                    position += textEscape(chars + next, styleCopy READABLE_PARAM_ONLY(bmx, 1u));
                    next = position + 1;
                    cursor = chars + position;
                    c = *cursor;
                }
            }

            // The line's end, or the text's.
            if (c == '\r' || c == 0)
            {
                if (!beforeFirstLine)
                    lineDone(cursor);
                lineIndex = linesCounted;
                if (cursor[1] == '\n')
                    position = next;
                lineWidth = 0;
                if (chars[position] == 0)
                    break;
                ++position;
                beforeFirstLine = static_cast<int64_t>(linesCounted) < firstLine;
                cursor = chars + position;
                next = position + 1;
                lineStart = cursor;
                ++linesCounted;
                continue;
            }

            // A space or a tab: its width; past the width, a line break.
            if (c == '\t' || c == ' ')
            {
                if (style->monospaced != 0)
                    lineWidth += font->height + style->letterSpacing;
                else
                {
                    ee::Ptr<Glyph> glyph;
                    for (const uint8_t code : kSpaceWidthGlyphs)
                        if (!glyph)
                            glyph = font->glyphs[code];
                    if (glyph)
                    {
                        const int32_t spaceWidth = ee::toInt(ee::toFloat(static_cast<int32_t>(glyph->width)) * threeQuarters);
                        lineWidth += ee::toInt(ee::toFloat(spaceWidth));
                    }
                }
                if (maxWidth < ee::toFloat(lineWidth))
                {
                    *cursor = '\r';
                    if (!beforeFirstLine)
                        lineDone(cursor);
                    lineStart = lastBreak + 1;
                    nextLine();
                    lineWidth = 0;
                    lastBreak = lineStart;
                }
                position = next;
                cursor = chars + next;
                ++next;
                continue;
            }

            // A word: its width, glyph by glyph, up to the next break or
            // escape. A negative spacing (letters pulled together) that
            // would take more than a quarter of a glyph gives 2 texels instead.
            wordWidth = 0;
            if (c != '\n')
            {
                ee::Ptr<int8_t> wordEnd = chars + next;
                bool first = true;
                for (;;)
                {
                    if (!first)
                        ++next;
                    first = false;
                    if (*cursor == '#')
                        break;
                    const uint8_t code = static_cast<uint8_t>(*cursor);
                    const ee::Ptr<Glyph> glyph(fontGlyph(font, code));
                    if (glyph)
                    {
                        const int32_t spacing = styleCopy->letterSpacing;
                        int32_t advance;
                        if (style->monospaced != 0)
                            advance = font->height + spacing;
                        else
                        {
                            const int32_t width = glyph->width;
                            advance = width + spacing;
                            if (!(spacing > 0))
                            {
                                if (ee::toFloat(width) * quarter < ee::abs(ee::toFloat(spacing)))
                                    advance = width + 2;
                            }
                        }
                        wordWidth = static_cast<int64_t>(static_cast<int32_t>(wordWidth) + advance);
                    }
                    cursor = wordEnd;
                    const int32_t following = *cursor;
                    position = next;
                    if (following == '\n' || following == '\t' || following == 0)
                    {
                        ++next;
                        break;
                    }
                    if (following != '\r')
                    {
                        wordEnd = cursor + 1;
                        if (following != ' ')
                            continue;
                    }
                    ++next;
                    break;
                }
            }

            // The word on this line or the next.
            const int32_t wordPixels = static_cast<int32_t>(wordWidth);
            const int32_t withWord = lineWidth + wordPixels;
            if (ee::toFloat(wordPixels) <= maxWidth)
            {
                lineWidth = withWord;
                if (ee::toFloat(withWord) <= maxWidth)
                {
                    lastBreak = cursor;
                    continue;
                }
                *lastBreak = '\r';
                if (!beforeFirstLine)
                    lineDone(lastBreak);
                lineStart = lastBreak + 1;
                nextLine();
                lineWidth = wordPixels;
                lastBreak = cursor;
                continue;
            }
            if (lineWidth != 0)
            {
                *lastBreak = '\r';
                if (!beforeFirstLine)
                    lineDone(cursor); // (here the original ends the text after the word, not at the break)
                nextLine();
                lineStart = lastBreak + 1;
            }
            lineWidth = ee::toInt(maxWidth);
            lastBreak = cursor;
        }

        if (lineCount)
            *lineCount = linesCounted;
        if (draw != 0)
            drawQuadsOf(font, style, ee::Ptr<const int8_t>(drawFrom.get()), QuadList());
    }
    READABLE_FUNCTION_WAITS(drawWrappedText, Verified);
    namespace
    {
        // Spaces, then opening braces, then a sign ("-+1" is +1). `character`
        // is left the first after them; true: negative.
        bool skipToNumber(ee::Ptr<const uint8_t> &text, int32_t &character)
        {
            character = *text;
            auto skip = [&](int32_t repeated) {
                do
                {
                    ++text;
                    character = *text;
                } while (character == repeated);
            };
            if (character == ' ')
                skip(' ');
            if (!game::readable::kIsDm2 && character == '{') // (ZGE2 takes no '{')
                skip('{');
            bool negative = false;
            if (character == '-')
            {
                ++text;
                negative = true;
                character = *text;
            }
            if (character == '+')
            {
                ++text;
                negative = false;
                character = *text;
            }
            return negative;
        }

        bool isDigit(int32_t character) { return static_cast<uint32_t>(character - '0') < 10u; }
    }

    // parseTextFloat: the number at `text` into *value (each digit stored as
    // it is read: a float that stops at the end keeps its sign), the text
    // after it; 0 if either pointer is. The fraction's digits divide by
    // growing powers of ten, in floats.
    ee::Ptr<const int8_t> parseTextFloat(ee::Ptr<const int8_t> start, ee::Ptr<ee::Float> value)
    {
        if (!value)
            return ee::Ptr<const int8_t>();
        *value = ee::Float(0.0f);
        if (!start)
            return ee::Ptr<const int8_t>();
        ee::Ptr<const uint8_t> text(static_cast<const uint8_t *>(static_cast<const void *>(start.get())));
        ee::Float number = *value;
        int32_t character = 0;
        const bool negative = skipToNumber(text, character);
        const ee::Float sign = negative ? ee::Float(-1.0f) : ee::Float(1.0f);
        while (isDigit(character))
        {
            ++text;
            number = number * ee::Float(1e+01f) + ee::toFloat(character - '0');
            *value = number;
            character = *text;
        }
        if (character == '.')
        {
            ++text;
            character = *text;
            const ee::Float ten(1e+01f);
            ee::Float place = ten;
            while (isDigit(character))
            {
                ++text;
                number = number + ee::toFloat(character - '0') / place;
                *value = number;
                character = *text;
                place = place * ten;
            }
        }
        *value = number * sign;
        return ee::Ptr<const int8_t>(static_cast<const int8_t *>(static_cast<const void *>(text.get())));
    }
    READABLE_FUNCTION(parseTextFloat, Verified);

    // parseTextInteger: as parseTextFloat, a whole number.
    ee::Ptr<const int8_t> parseTextInteger(ee::Ptr<const int8_t> start, ee::Ptr<int32_t> value)
    {
        if (!value)
            return ee::Ptr<const int8_t>();
        *value = 0;
        if (!start)
            return ee::Ptr<const int8_t>();
        ee::Ptr<const uint8_t> text(static_cast<const uint8_t *>(static_cast<const void *>(start.get())));
        int32_t character = 0;
        const bool negative = skipToNumber(text, character);
        uint32_t number = 0;
        while (isDigit(character))
        {
            ++text;
            number = number * 10u + static_cast<uint32_t>(character - '0');
            *value = static_cast<int32_t>(number);
            character = *text;
        }
        *value = static_cast<int32_t>(negative ? 0u - number : number);
        return ee::Ptr<const int8_t>(static_cast<const int8_t *>(static_cast<const void *>(text.get())));
    }
    READABLE_FUNCTION(parseTextInteger, Verified);

}
