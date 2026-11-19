#pragma once

// Aggressive Inline's TUT/Estate (only on the GameCube disc): an earlier
// export of the Tutorial site, already in the PS2's formats (strip groups of
// at most 60, 8- and 4-bit BMP textures, PS2 render state words, PS2 INS and
// DAT records, scripts version 2.70). build() makes the files the PS2 game
// loads from it at build time (ps2xGcConvert, run by tools/make-app.sh and
// tools/make-windows.ps1 on the folder tools/gc_disc.py extracted); the game
// override (aggressive_inline_estate.cpp) only serves them. Nothing here
// runs in the game.
//
// What changes: one texture of the library is a 24-bit BMP (BB4L, 256x256;
// no PS2 level has one), made 8-bit with ps2_zaxis_xbox's palettize (median
// cut, k-means; the same tie-breaks on every platform); every other file and
// every other texture byte for byte. The files are named as the game opens
// them below ASSETS/LEVELS/TUT (TUT.ZIT names tut.TXR, tut.GEO, tut.INS,
// tut.DAT): TUT.ZIT, TUT.GEO, TUT.INS, TUT.TXR, TUT.DAT, TUT.BIN (career),
// TUTT.BIN (free skate), MOTIONS.ZAL (its doors and trigger). The exporter's
// log (tut.TXT) and outputf are not loaded by the game and not written.
//
// VISIBILITY.TXT: the free skate script's missing show and hide calls
// (VisibilityRule below).
//
// TEXTURES.TXT lists the library's textures as the GS will hold them, one
// "<width> <height> <index hash hex> <name>" line each (level 0, the key of
// runtime/ps2_texture_replace.h: FNV-1a 64 of the indices, a byte a texel,
// rows top down): the game override keeps the Xbox texture pack off them.
//
// ICONS.UFNT: Estate's level-select icon ('n' unselected, 'N' selected) on a
// one-page font of the level font's format, made from a picture of the
// level drawn from its own TUT.GEO, TUT.INS and TUT.TXR (renderPicture),
// in AI's oval badge (ps2xGcConvert --picture
// writes the picture itself, 256 x 256, as a BMP). STILL.ULIB: the level
// select's still behind the entry (in place of the Editor Level's), the same
// view at 512 x 256 shown 4:3, as a one-asset UI library.

#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ps2x::zaxis::estate
{
    // One file of the Estate folder by its name on the GameCube disc, any
    // case ("tut.GEO", "TUT.bin", "motions.zal"). False if it is not there.
    using Reader = std::function<bool(const std::string &name, std::vector<uint8_t> &out)>;

    struct TextureKey
    {
        uint32_t width = 0, height = 0;
        uint64_t indexHash = 0;
        std::string name;
    };

    struct TextureReport
    {
        size_t textures = 0;    // TXRC entries
        size_t pictures = 0;    // TBMP / TZMP chunks (mip levels included)
        size_t converted = 0;   // 24- or 32-bit pictures made 8-bit
        std::vector<std::string> convertedNames;
    };

    // A texture library (FORM of TXRC entries) with every 24- or 32-bit
    // TBMP/TZMP made 8-bit (a TZMP keeps its alpha in the palette), the rest
    // byte for byte. False (and txr unchanged) if it is not a library or a
    // picture cannot be read.
    bool palettizeLibrary(std::vector<uint8_t> &txr, TextureReport *report = nullptr, std::string *error = nullptr);

    // Level 0 of every texture of a library (8- and 4-bit pictures only).
    std::vector<TextureKey> textureKeys(const std::vector<uint8_t> &txr);

    // "<w> <h> <hash hex> <name>\n" lines, and back.
    std::string writeTextureKeys(const std::vector<TextureKey> &keys);
    std::vector<TextureKey> readTextureKeys(const std::string &text);

    // A show or hide the free skate script lacks: when `object` enters
    // `state`, setObjectVisible(target, visible, keep), as the career
    // script's same object does in that state's "enter". Estate's TUTT.BIN
    // (free skate) never shows or hides lessons 19, 23, 24 and 25's props
    // (TUT.BIN, the career script, does), so they stayed on where other
    // lessons' props are (lessons 19 and 24's quarter pipes in the same two
    // places, drawn over each other).
    struct VisibilityRule
    {
        uint32_t object = 0, state = 0, target = 0; // name hashes
        bool visible = false, keep = true;
    };
    // The calls in `from`'s enter handlers whose target `into` never shows
    // or hides, for the objects and states `into` has.
    std::vector<VisibilityRule> borrowedVisibility(const std::vector<uint8_t> &from, const std::vector<uint8_t> &into);
    // "<object> <state> <target> <visible> <keep>" lines (hashes in hex), and back.
    std::string writeVisibilityRules(const std::vector<VisibilityRule> &rules);
    std::vector<VisibilityRule> readVisibilityRules(const std::string &text);

    struct Made
    {
        std::map<std::string, std::vector<uint8_t>> files; // name in the output folder -> contents
    };

    // A picture of the level as it starts (the lesson props, *_L<n>, and the
    // panorama left out): every instance's parts textured with level 0 of
    // their materials' textures (alpha-tested) and their prelit colours,
    // looked at from the south-east 50 degrees down, orthographic, framed
    // on the grounds (Terrain_Ground*); 3x3 samples a pixel.
    struct Picture
    {
        int width = 0, height = 0;
        std::vector<uint32_t> rgba; // top row first, R in the low byte
    };
    struct PictureReport
    {
        size_t instances = 0, triangles = 0, drawnTriangles = 0, texturedTriangles = 0, textures = 0;
    };
    bool renderPicture(const std::vector<uint8_t> &geo, const std::vector<uint8_t> &ins, const std::vector<uint8_t> &txr,
                       int size, Picture &out, PictureReport *report = nullptr, std::string *error = nullptr);
    // The same width x height, shown at shownAspect (width over height on
    // screen: a still of 512 x 256 is shown 4:3).
    bool renderPicture(const std::vector<uint8_t> &geo, const std::vector<uint8_t> &ins, const std::vector<uint8_t> &txr,
                       int width, int height, float shownAspect, Picture &out, PictureReport *report = nullptr,
                       std::string *error = nullptr);
    // The level font's format (a UFNT: HFNT, HMTL, TXRC with a 256 x 64 8-bit
    // TZMP page named pageName): the picture as an unselected and a
    // selected icon, glyphs `unselected` and `selected` (ps2_zaxis_ui.h
    // appendFontPages adds them to the level font).
    std::vector<uint8_t> iconFont(const Picture &picture, char unselected, char selected, const std::string &pageName);
    // A UI library (ULIB) of one asset as the FrontEnd library's level-select
    // still (still_bkg_01) is made: the picture as an 8-bit TBMP (256
    // colours, ps2_zaxis_xbox palettize) named `name` (its HAST file name and its TXRH
    // name); ps2_zaxis_ui.h addAssets puts it into the FrontEnd library.
    std::vector<uint8_t> stillLibrary(const Picture &picture, const std::string &name);
    constexpr const char *kStillName = "estate_still";
    // A 24-bit BMP of the picture.
    std::vector<uint8_t> pictureBmp(const Picture &picture);
    constexpr char kIconUnselected = 'n', kIconSelected = 'N';
    constexpr const char *kIconPage = "fnt_lvl_icons_estate.zmp";

    // The files the game loads (names above, upper case), TEXTURES.TXT,
    // VISIBILITY.TXT, ICONS.UFNT and STILL.ULIB (both left out, with a line
    // in the log, if the level's picture cannot be drawn).
    bool build(const Reader &gc, Made &made, std::ostream &log, std::string *error = nullptr);
}
