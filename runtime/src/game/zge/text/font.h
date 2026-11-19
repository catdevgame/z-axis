#pragma once

// The engine's bitmap fonts and their text draw (zge/text/font.cpp; the
// glyph lookup is zge/utilities.cpp's fontGlyph). A font has pages
// (textures) and a glyph per character code; the draw lays a text out in
// lines and turns each glyph into a textured quad for the 2D primitive API
// (zge/render/ps2/primitive_packets.cpp), or into records for the caller.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zge
{
    // One of a font's textures.
    struct FontPage
    {
        uint16_t width;       // +0x00 texels
        uint16_t height;      // +0x02
        char name[0x20];      // +0x04 the texture's name (its hash finds it)
        uint32_t texture;     // +0x24 the texture, looked up on first use (0 before)
    };
    static_assert(sizeof(FontPage) == 0x28 && offsetof(FontPage, texture) == 0x24);

    // A character's place in its page and its size.
    struct Glyph
    {
        uint8_t code;         // +0x00 the character
        uint8_t page;         // +0x01 FontPage index
        int16_t top;          // +0x02 drawn this far below the line's top
        uint16_t u;           // +0x04 texel column of its left edge
        uint16_t v;           // +0x06 texel row of its top edge
        uint16_t width;       // +0x08
        uint16_t height;      // +0x0A
    };
    static_assert(sizeof(Glyph) == 0x0C);

    struct Font
    {
        uint8_t height;               // +0x00 a line's height (and a monospaced glyph's advance)
        uint8_t headerByte;           // +0x01 the file's second byte: nothing in the font code reads it
        uint16_t glyphCount;          // +0x02
        uint16_t pageCount;           // +0x04
        uint16_t _06;
        ee::Ptr<FontPage> pages;      // +0x08
        ee::Ptr<Glyph> glyphRecords;  // +0x0C the glyphs themselves
        ee::Ptr<Glyph> glyphs[256];   // +0x10 by code, null where the font has none
    };
    static_assert(offsetof(Font, pages) == 0x08 && offsetof(Font, glyphs) == 0x10);

    // How a text is laid out and drawn (the caller's; zgui's text widgets
    // fill one from the widget). Escapes in the text (#RGBA{...},
    // #RGB{...}, #SCALE{...}) change colour and size as it goes.
    // ZGE2's style has 16 bytes more after the position (DM2: scale +0x20,
    // colour +0x30, align +0x34, lineSpacing +0x38, letterSpacing +0x3C in
    // its textEscape and measureText): the text's orientation. No ZGE2 text
    // code reads its fields after letterSpacing (its copies are 0x50 bytes).
    struct TextStyle
    {
        Vec4 position;          // +0x00 the text's anchor on the screen
#if PS2X_GAME_DM2
        // DM2 +0x10: ZGE2's style carries the text's orientation (DM2's
        // drawGuiTreeView copies its widget's quaternion here before
        // drawText): measureText turns the scale by it, drawTextQuads the
        // quads.
        Quaternion orientation;
#endif
        ee::Float scaleX;       // +0x10
        ee::Float scaleY;       // +0x14
        ee::Float scaleExtra[2]; // +0x18 the third and fourth values of a SCALE{} escape
        uint8_t colour[4];      // +0x20 RGBA
#if !PS2X_GAME_BMX
        // +0x24 (BMX +0x2C) bits 0-3 horizontal (0 the anchor is the left,
        // 1 the middle, 2 the right), bits 4-7 vertical (0 top, 1 middle, 2
        // bottom).
        uint32_t align;
#endif
        ee::Float lineSpacing;  // +0x28 (BMX +0x24) lines are font height * this apart
        int32_t letterSpacing;  // +0x2C (BMX +0x28) added to each advance (texels)
#if PS2X_GAME_BMX
        uint32_t align;         // BMX +0x2C
        // BMX +0x30: ZGE4 keeps monospaced in bit 4 of this word (BMX XXX's
        // measureText and drawTextQuads read that bit alone). Its low four
        // bits are ZGE3's cleared byte: the text widgets clear them.
        union
        {
            uint32_t reserved;
            struct
            {
                uint32_t reservedBits : 4;
                uint32_t monospaced : 1;
                uint32_t : 27;
            };
        };
#else
        union
        {
            uint32_t _30;
            uint32_t reserved; // +0x30 callers clear it; no drawing code reads it
        };
        uint32_t monospaced;    // +0x34 non-zero: every glyph advances the font's height
#endif
        ee::Float angle;        // +0x38 (BMX +0x34) the text's rotation (times g_angleUnitsPerUnit: an Angle)
        ee::Float width;        // +0x3C (BMX +0x38) measured (drawText)
        ee::Float height;       // +0x40 (BMX +0x3C)
#if !PS2X_GAME_BMX
        uint32_t _44[3];        // (ZGE4's style ends at 0x40)
#endif

        // scaleX, scaleY and the word after them, as a vector.
        const Vec3 *scale() const { return reinterpret_cast<const Vec3 *>(&scaleX); }
    };
    // What zgui's text widgets clear in a style before drawing with it:
    // ZGE3's word at +0x30, ZGE4's low four bits of it (bit 4 is its
    // monospaced). Always inlined: AI's callers compile as with the store.
    [[gnu::always_inline]] inline void clearStyleReserved(TextStyle &style)
    {
#if PS2X_GAME_BMX
        style.reservedBits = 0;
#else
        style.reserved = 0;
#endif
    }

    // The same where ZGE3's caller clears only the word's low byte.
    [[gnu::always_inline]] inline void clearStyleReservedByte(TextStyle &style)
    {
#if PS2X_GAME_BMX
        style.reservedBits = 0;
#else
        std::memset(&style.reserved, 0, 1);
#endif
    }

    // Where ZGE2's style keeps the text's orientation (no other version's
    // style has one: none there).
    inline Quaternion *styleOrientation([[maybe_unused]] TextStyle &style)
    {
#if PS2X_GAME_DM2
        return &style.orientation;
#else
        return nullptr;
#endif
    }
#if !PS2X_GAME_DM2 && !PS2X_GAME_BMX
    static_assert(sizeof(TextStyle) == 0x50);
    static_assert(offsetof(TextStyle, monospaced) == 0x34);
#endif
#if !PS2X_GAME_DM2
    static_assert(offsetof(TextStyle, angle) == game::readable::perGame(0x38u, 0x38u, 0x34u) &&
                  offsetof(TextStyle, width) == game::readable::perGame(0x3Cu, 0x3Cu, 0x38u) &&
                  offsetof(TextStyle, height) == game::readable::perGame(0x40u, 0x40u, 0x3Cu));
#endif
#if PS2X_GAME_BMX
    static_assert(sizeof(TextStyle) == 0x40 && offsetof(TextStyle, reserved) == 0x30);
#endif
    static_assert(offsetof(TextStyle, scaleX) == game::readable::perGame(0x10u, 0x20u, 0x10u) &&
                  offsetof(TextStyle, colour) == game::readable::perGame(0x20u, 0x30u, 0x20u) &&
                  offsetof(TextStyle, align) == game::readable::perGame(0x24u, 0x34u, 0x2Cu) &&
                  offsetof(TextStyle, lineSpacing) == game::readable::perGame(0x28u, 0x38u, 0x24u) &&
                  offsetof(TextStyle, letterSpacing) == game::readable::perGame(0x2Cu, 0x3Cu, 0x28u));

    // A quad as drawTextQuads hands it to the caller (0x80 bytes; a texture
    // of -1 ends the list). ZGE4's is 0x70 bytes, the corners first (BMX
    // XXX's drawTextQuads).
    struct TextQuadRecord
    {
#if PS2X_GAME_BMX
        Vec4 corners[4];      // BMX +0x00
        uint32_t texture;     // BMX +0x40
#else
        uint32_t texture;     // +0x00
        uint32_t _04[3];
        Vec4 corners[4];      // +0x10 x, y, 0 and the vertex's w
#endif
        ee::Float u[4];       // +0x50 (BMX +0x44)
        ee::Float v[4];       // +0x60 (BMX +0x54)
        uint8_t colour[4];    // +0x70 (BMX +0x64)
#if PS2X_GAME_BMX
        uint32_t _68[2];
#else
        uint32_t _74[3];
#endif
    };
    static_assert(sizeof(TextQuadRecord) == game::readable::perGame(0x80u, 0x80u, 0x70u) &&
                  offsetof(TextQuadRecord, texture) == game::readable::perGame(0x00u, 0x00u, 0x40u) &&
                  offsetof(TextQuadRecord, corners) == game::readable::perGame(0x10u, 0x10u, 0x00u) &&
                  offsetof(TextQuadRecord, u) == game::readable::perGame(0x50u, 0x50u, 0x44u) &&
                  offsetof(TextQuadRecord, colour) == game::readable::perGame(0x70u, 0x70u, 0x64u));

    enum TextAlign : uint32_t
    {
        kAlignLeft = 0x0,
        kAlignCentre = 0x1,
        kAlignRight = 0x2,
        kAlignHorizontal = 0x0F,
        kAlignTop = 0x00,
        kAlignMiddle = 0x10,
        kAlignBottom = 0x20,
        kAlignVertical = 0xF0,
    };

    // The rectangle a text covers (measureText).
    struct TextBounds
    {
        ee::Float left, top, right, bottom;
    };

    // measureText: the text's bounds and each line's width into lineWidths
    // (if given); the number of glyphs (font.cpp).
    int32_t measureText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text, ee::Ptr<TextBounds> bounds,
                        ee::Ptr<int32_t> lineWidths);

    // drawText: the text drawn in the style (font.cpp).
    int32_t drawText(ee::Ptr<Font> font, ee::Ptr<TextStyle> style, ee::Ptr<const int8_t> text);

    ee::Ptr<Glyph> fontGlyph(ee::Ptr<Font> font, uint8_t code); // (zge/utilities.cpp)
    // initFonts, shutdownFonts (font_file.cpp): the fonts' ready flag set and cleared. Both 1.
    uint32_t initFonts();
    uint32_t shutdownFonts();
    // ZGE4's readFontHeader (font_file.cpp): the font made from a font file's data, in one heap block.
    ee::Ptr<Font> newFontFromData(ee::Address src);
}
