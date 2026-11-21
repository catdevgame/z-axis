// A font's records and its file (zge/text/font.h; the text draw is
// font.cpp): the file is the Font's header, then its pages and glyphs in the
// records' own layout, copied a field at a time. BMX XXX (PS2) the same.

#include "font.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/images.h"
#include "zge/memory/regions.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/text/cstring.h"
#include "zge/assets/file.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "sym/zge/text/font_file.cpp.h"

namespace zge
{
    namespace
    {
        // A font's bytes: the record ends after glyphs[254], so only 255
        // pointers are cleared and code 0xFF's is past its end.
        constexpr uint32_t kFontBytes = 0x40Cu;
        constexpr uint32_t kFontGlyphsCleared = 0x3FCu;

        inline ee::Global<uint32_t, sym::g_fontsReady> g_fontsReady;
    }

    // initFonts.
    uint32_t initFonts()
    {
        memoryFill(ee::Ptr<uint32_t>(g_fontsReady.get()), 0, static_cast<uint32_t>(sizeof(uint32_t)));
        *g_fontsReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initFonts, Verified);

    // shutdownFonts.
    uint32_t shutdownFonts()
    {
        if (*g_fontsReady == 0)
            return 1;
        *g_fontsReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownFonts, Verified);

    // newFontRecord: an empty font.
    ee::Ptr<Font> newFontRecord()
    {
        const ee::Ptr<Font> font(heapAllocate(kFontBytes));
        font->height = 0;
        font->glyphCount = 0;
        font->pages = ee::Ptr<FontPage>();
        font->glyphRecords = ee::Ptr<Glyph>();
        memoryFill(ee::Ptr<ee::Ptr<Glyph>>(font->glyphs), 0, kFontGlyphsCleared);
        return font;
    }
    READABLE_FUNCTION_WAITS(newFontRecord, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t heapFree(ee::Ptr<Font> block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        }
    }

    // freeFont: its pages' materials released, its tables and the font
    // freed. What it returns is heapFree's.
    int32_t freeFont(ee::Ptr<Font> font)
    {
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 releases only the textures that were looked up and frees
            // the font alone: no page or glyph table is freed here.
            for (uint32_t page = 0; page < font->pageCount; ++page)
                if (font->pages[page].texture != 0)
                    releaseMaterial(font->pages[page].texture);
            return tail::heapFree(font);
        }
        if (font->pages)
        {
            for (int32_t page = 0; page < static_cast<int32_t>(font->pageCount); ++page)
                releaseMaterial(font->pages[page].texture); // a texture from kFindTexture, released as a material
            heapFree(font->pages);
            font->pages = ee::Ptr<FontPage>();
        }
        if (font->glyphRecords)
        {
            heapFree(font->glyphRecords);
            font->glyphRecords = ee::Ptr<Glyph>();
        }
        return tail::heapFree(font);
    }
    READABLE_FUNCTION_WAITS(freeFont, Verified);

    namespace
    {
        // A field from a font file, by its own bytes; `next` moves past it.
        template <typename T>
        void readFontField(T &field, ee::Ptr<uint8_t> &next, uint32_t bytes)
        {
            memoryCopy(ee::Ptr<T>(&field), next, bytes);
            next += bytes;
        }

        // The pages and the glyphs after a font file's header, into the font's
        // tables, and each glyph entered in glyphs[] by its code.
        void readFontTables(ee::Ptr<Font> font, ee::Ptr<uint8_t> &next)
        {
            for (uint32_t i = 0; i < font->pageCount; ++i)
            {
                FontPage &page = font->pages[i];
                readFontField(page.width, next, 2);
                readFontField(page.height, next, 2);
                readFontField(page.name, next, sizeof(FontPage::name));
                readFontField(page.texture, next, 4);
            }
            for (uint32_t i = 0; i < font->glyphCount; ++i)
            {
                const ee::Ptr<Glyph> glyph = font->glyphRecords + i;
                readFontField(glyph->code, next, 1);
                readFontField(glyph->page, next, 1);
                readFontField(glyph->top, next, 2);
                readFontField(glyph->u, next, 2);
                readFontField(glyph->v, next, 2);
                readFontField(glyph->width, next, 2);
                readFontField(glyph->height, next, 2);
                font->glyphs[glyph->code] = glyph;
            }
        }

        // A font file's header: the Font's first fields, as ZGE4 reads them
        // before the font is made.
        struct FontHeader
        {
            uint8_t height;
            uint8_t headerByte;
            uint16_t glyphCount;
            uint16_t pageCount;
        };
    }

    // readFontHeader: the header at `src` into the font, with the tables for
    // its pages and glyphs, and each glyph entered in glyphs[] by its code.
    uint32_t readFontHeader(ee::Ptr<Font> font, ee::Address src)
    {
        ee::Ptr<uint8_t> next(src);
        readFontField(font->height, next, 1);
        readFontField(font->headerByte, next, 1);
        readFontField(font->glyphCount, next, 2);
        readFontField(font->pageCount, next, 2);
        font->pages = ee::Ptr<FontPage>(heapAllocate(font->pageCount * sizeof(FontPage)));
        font->glyphRecords = ee::Ptr<Glyph>(heapAllocate(font->glyphCount * sizeof(Glyph)));
        readFontTables(font, next);
        return 1;
    }

    // newFontFromData: ZGE4's readFontHeader: the font made from the file's
    // data in one heap block, which the header (read first) sizes: the
    // record (every glyph pointer cleared), then its page and glyph tables.
    // Returns the font.
    ee::Ptr<Font> newFontFromData(ee::Address src)
    {
        ee::Local<FontHeader> header;
        ee::Ptr<uint8_t> next(src);
        readFontField(header->height, next, 1);
        readFontField(header->headerByte, next, 1);
        readFontField(header->glyphCount, next, 2);
        readFontField(header->pageCount, next, 2);
        const uint32_t pagesBytes = header->pageCount * sizeof(FontPage);
        const ee::Ptr<Font> font(heapAllocate(sizeof(Font) + pagesBytes + header->glyphCount * sizeof(Glyph)));
        font->height = header->height;
        font->headerByte = header->headerByte;
        font->glyphCount = header->glyphCount;
        font->pageCount = header->pageCount;
        font->pages = ee::Ptr<FontPage>(font.address + sizeof(Font));
        font->glyphRecords = ee::Ptr<Glyph>(font.address + sizeof(Font) + pagesBytes);
        memoryFill(ee::Ptr<ee::Ptr<Glyph>>(font->glyphs), 0, sizeof(Font::glyphs));
        readFontTables(font, next);
        return font;
    }
    READABLE_FUNCTION_WAITS_OWN(readFontHeader, Verified, bmx, newFontFromData);

    namespace
    {
        // A page's image file: the font's directory and the page's name.
        constexpr uint32_t kPagePathFormat = sym::g_fontPagePathFormat; // "%s%s"
        constexpr uint32_t kBmpExtension = sym::g_textBmp2;             // "BMP"
        constexpr uint32_t kTgaExtension = sym::g_textTga2;             // "TGA"
        constexpr uint32_t kZmpExtension = sym::g_textZmp2;             // "ZMP"
        // readImage's kinds (assets/images.cpp): 0 lets the readers say.
        constexpr uint32_t kImageKindAny = 0, kImageKindBmp = 1, kImageKindTga = 2, kImageKindBmpAlpha = 3;

        namespace routed
        {
        }

        // (assets/file.cpp, render/materials.cpp)

        // The reader for a page's file, by its extension.
        uint32_t imageKind(ee::Ptr<char> extension)
        {
            if (stringCompareNoCase(extension, kBmpExtension) == 0)
                return kImageKindBmp;
            if (stringCompareNoCase(extension, kTgaExtension) == 0)
                return kImageKindTga;
            return stringCompareNoCase(extension, kZmpExtension) == 0 ? kImageKindBmpAlpha : kImageKindAny;
        }
    }

    // loadFontFile (nothing calls it in AI): a font read from its file into
    // `font`, then each page's image (the page's name beside the font file:
    // BMP, TGA or ZMP) read into a new material's texture, which the page
    // keeps in its texture word (a page whose file is not there keeps the
    // word the font file gave).
    // Returns readFontHeader's result, 0 without the font file.
    int32_t loadFontFile(ee::Ptr<Font> font, ee::Ptr<const char> path)
    {
        ee::Local<char[0x100]> directory;
        ee::Local<char[0x100]> pagePath;
        ee::Local<char[0x10]> extension;
        ee::Local<uint32_t> bytes; // loadFile's size out, not read

        uint32_t data = loadFile(path, bytes);
        if (data == 0)
            return 0;
        const int32_t read = static_cast<int32_t>(zge::readFontHeader(font, data));
        freeLoadedFile(data);
        if (read == 0)
            return read;

        pathDirectory(path, directory);
        for (uint32_t i = 0; i < font->pageCount; ++i)
        {
            formatString(pagePath, kPagePathFormat, directory, ee::Ptr<char>(font->pages[i].name)); // directory, then the page's name
            pathExtension(ee::Ptr<char>(font->pages[i].name), extension);
            data = loadFile(pagePath, bytes);
            if (data == 0)
                continue;
            const uint32_t kind = imageKind(extension);
            const uint32_t image = newImageRecord();
            readImage(image, data, kind);
            freeLoadedFile(data);
            font->pages[i].texture = ee::Address(newMaterial()); // a material here, not a texture
            const ee::Ptr<Texture> texture = zge::newProceduralTexture(ee::Ptr<TextureImage>(image));
            addMaterialTexture(ee::Ptr<Material>(font->pages[i].texture), texture);
            freeImageRecord(image);
        }
        return read;
    }
    READABLE_FUNCTION_WAITS(loadFontFile, Draft);
}
