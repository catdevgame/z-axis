#pragma once

// Z-Axis ZGE3 engine data (Aggressive Inline and other Z-Axis games of that
// time): the .ZAL archive and the UIBuilder UI scripts it holds, for game
// overrides that change the game's menus (see SETTINGS.md).
//
// ZAL layout (little endian): "ZDIR" <size>, then "DIDF" 8 <count> 0,
// "GRID" <4*count> name hashes, "DLEN" <4*count> (flags << 28 | length),
// "OFST" <4*count> offsets from the start of the file; the entries follow,
// each at a 16-byte aligned offset, zero padded.
//
// A UIBuilder script is text (CRLF lines, tab indented) holding a tree of
//     OBJECT "<type>" "<name>"
//     {
//         <Property>  <value>
//         <Handler>   "<script>"  or  { <script lines> }
//         OBJECT ... (children)
//         ACTION "<type>" "<name>" { ... }
//     }

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zx::zaxis
{
    struct ArchiveEntry
    {
        uint32_t nameHash = 0;
        uint32_t flags = 0; // high 4 bits of the DLEN word
        std::vector<uint8_t> data;
    };

    bool readArchive(const std::vector<uint8_t> &file, std::vector<ArchiveEntry> &entries, std::string *error = nullptr);
    std::vector<uint8_t> writeArchive(const std::vector<ArchiveEntry> &entries);

    // The main object's name of a UIBuilder script entry ("FrontEnd" for
    // OBJECT "guiScreenSpace" "FrontEnd"), or "" if the entry is not one.
    std::string scriptName(const ArchiveEntry &entry);

    // A span of script text.
    struct Span
    {
        size_t begin = std::string::npos;
        size_t end = std::string::npos;
        bool found() const { return begin != std::string::npos; }
    };

    // The object block at a path of object names below `within` (default:
    // the whole text), e.g. "Background/Options/Menu": from the start of its
    // OBJECT line to just after its closing brace line. Each step is the
    // first direct child (OBJECT or ACTION, any type) with that name; the
    // first step is a top-level object (or a child of `within`).
    Span findObject(const std::string &text, std::string_view path, Span within = {});
    // Where children can be added: the start of the line with the object's
    // closing brace.
    size_t objectEnd(const std::string &text, Span object);
    // The tab depth of the line holding position pos.
    int indentAt(const std::string &text, size_t pos);
    // Re-indents a block written with no leading tabs (lines separated by
    // '\n') to `depth` tabs and CRLF line ends, as UIBuilder writes them.
    std::string indentBlock(std::string_view block, int depth);

    // Replaces the first occurrence of `from` inside the span; false if
    // there is none. The span's end moves with the change.
    bool replaceIn(std::string &text, Span &span, std::string_view from, std::string_view to);

    // Every OBJECT block of a script, in text order, with its path as
    // findObject takes it ("InGameUI/PauseMenus/Frame/Tutorial/Menu") and
    // its type ("guiMenu"). Of siblings with the same name only the first
    // is listed (the one findObject finds).
    struct ObjectInfo
    {
        std::string path;
        std::string type;
        Span span;
    };
    std::vector<ObjectInfo> listObjects(const std::string &text);
    // The value of one of the object's own properties ("Scale" ->
    // "1.000000 1.000000 1.000000"), not its children's: the span of the
    // text after the tab, to the line end.
    Span findProperty(const std::string &text, Span object, std::string_view name);

    // Text widgets (guiLabel, guiMenu, guiTextBox) of `text` whose
    // counterpart in `model` (another release's script: same path, same
    // type) has a smaller width scale (Scale's x) take that width; the
    // height and depth scales stay. Returns the paths changed. For another
    // release's translations, whose scripts narrow the widgets their
    // longer texts would overflow, laid out alike otherwise.
    std::vector<std::string> narrowLike(std::string &text, const std::string &model);

    // ---- texture libraries (*.zif archive entries) --------------------------
    //
    // A library is a chunk tree (4-byte tag, u32 size, data): "ULIB" holds
    // "HLIB" and one "UAST" per asset; a UAST holds "HAST" (the artist's
    // source path, NUL terminated) and either "UMTL" (a material: "HMTL",
    // "TXRC" with "TXRH" (u32 hash, u16 width, u16 height, ..., name at
    // +0x14) and the picture as "TZMP" (a Windows BMP, 4 or 8 bits with a
    // BGRA palette, alpha 0-255) or "TTGA" (a 32-bit TGA)) or "UFNT" (a
    // font: "HFNT", then the atlas as "HMTL" and "TXRC").
    //
    // HFNT: u8 line height, u8, u16 glyph count, u16 pages, u16 atlas
    // width, u16 atlas height, the atlas name at +0x0A, then
    // from +0x2E one 12-byte glyph per character: u16 character, u16 y
    // offset, u16 x, u16 y, u16 width, u16 height (atlas pixels).

    struct LibraryTexture
    {
        std::string source; // HAST path ("...\\xbox_btn_A.tga")
        std::string name;   // TXRH name ("select_button")
        int width = 0;
        int height = 0;
        std::vector<uint32_t> rgba; // top row first, R in the low byte, alpha 0-255
    };

    struct FontGlyph
    {
        uint16_t character = 0;
        uint16_t offset = 0; // from the top of the line
        uint16_t x = 0;
        uint16_t y = 0;
        uint16_t width = 0;
        uint16_t height = 0;
    };

    struct LibraryFont
    {
        std::string source; // HAST path ("...\\fnt_14pt.zfn")
        int lineHeight = 0;
        std::vector<FontGlyph> glyphs;
        LibraryTexture atlas;
        const FontGlyph *glyph(char c) const;
    };

    // Every picture of a library entry (materials and font atlases), and
    // every font. False if the entry is not a ULIB.
    bool readLibrary(const std::vector<uint8_t> &entry, std::vector<LibraryTexture> *textures,
                     std::vector<LibraryFont> *fonts = nullptr);
    // The file name at the end of a HAST source path, lower case.
    std::string sourceFileName(std::string_view source);

    // Puts fonts of another library into this one: each font asset (a UAST
    // holding a UFNT) of `library` whose HAST file name ("fnt_14pt.zfn")
    // `donor` also has as a font is replaced by the donor's asset, chunk and
    // all (another release's font with more characters, say); the library's
    // other assets and its HLIB are kept. Returns the file names replaced;
    // `library` is unchanged when that is empty.
    std::vector<std::string> replaceFonts(std::vector<uint8_t> &library, const std::vector<uint8_t> &donor);

    // Puts textures of another library into this one: each asset (UAST) of
    // `donor` whose material's texture (TXRH) name is in `names` and that
    // `library` lacks is added after the library's own (another release's
    // extra picture, say). Returns the names added; `library` is unchanged
    // when that is empty.
    std::vector<std::string> addAssets(std::vector<uint8_t> &library, const std::vector<uint8_t> &donor,
                                       const std::vector<std::string> &names);

    // Adds pages and glyphs to a font of the library: `font` is the font
    // asset's HAST file name ("fnt_lvl_icons.zfn"), `pages` a UFNT chunk of
    // its own (HFNT, then an HMTL and a TXRC per page, as the game reads
    // them: HFNT FUN_0017E808 / FUN_0015FF18). Multi-page HFNT: u8 line
    // height, u8, u16 glyphs, u16 pages, a 40-byte record per page {u16
    // width, u16 height, name[32], u32}, then 12-byte glyphs {u8 character,
    // u8 page, u16, u16 x, u16 y, u16 width, u16 height}. The added glyphs'
    // pages are renumbered after the font's own; the font's line height is
    // kept. False (and `library` unchanged) if the font is not there, either
    // HFNT is not that layout, or a character is in both.
    bool appendFontPages(std::vector<uint8_t> &library, std::string_view font, const std::vector<uint8_t> &pages,
                         std::string *error = nullptr);
}
