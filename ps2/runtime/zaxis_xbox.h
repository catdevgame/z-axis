#pragma once

// Z-Axis ZGE3 level files of the Xbox release made into the PS2 release's
// (Aggressive Inline): both use the same chunk formats ("FORM", 4-byte tag,
// u32 little-endian size), and differ in two places.
//
// Textures (.TXR): a FORM of "TXRC" entries, each a "TXRH" header (u32 id,
// u16 width, u16 height, 12 bytes 0, the name at +0x14; 52 bytes, the same
// on both) and the picture. Xbox: "TXPR", a u32 count and an XPR0 resource
// (magic, total size, header size; then the D3D texture header: common,
// data offset, lock, format, size; the data at the header size; every
// level texture is DXT3, mipmapped). PS2: "TBMP" (opaque) or "TZMP" (with
// alpha), a Windows BMP with 8 bits per pixel and a 256-colour BGRA palette
// (alpha 0-255 in a TZMP, 0 in a TBMP), rows bottom-up, the chunk padded
// to 4 bytes; optional smaller mipmaps follow as more TBMP chunks.
//
// Strips (.GEO): a "TGPT" (an object's geometry: "TGPH" header with the
// group count at +8 and the submesh count at +0x0E, vertices, faces, ...) has a "TGFM" chunk of groups
// {u16 submesh << 8, u16 count, count u16 vertex indices, padded to 4}.
// An index with 0x8000 draws no triangle (a strip start), else it ends the
// triangle (previous two, it); 0x4000 flips that triangle's winding. The
// PS2 renderer takes at most 60 entries a group (its VU1 buffer); the Xbox
// release has one group per submesh, a long D3D strip (winding by
// position, restarts by repeated indices) of up to 1766 entries.
// convertGeometry re-cuts them as the PS2 exporter did (the triangles of
// all eight levels both discs have come out the same).
//
// Materials (.GEO): every "TGEO" object has a "TMTB" chunk of 160-byte
// "MATI" materials (flags, the material's name hash, colours, then at +0x2C
// a layer count and at +0x30 the layers, 28 bytes each: header, two words of
// render state, the texture id at +0x10, two more words). The layers' render
// state words are the platforms' own; convertMaterials rewrites the Xbox
// ones as the PS2 exporter wrote them (found by comparing the eight levels
// both discs have: 99.7% of the header bytes and 88% of whole layers
// identical, the rest in clamp regions that depend on the PS2's smaller
// textures).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zx::zaxis::xbox
{
    struct Picture
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> rgba; // top row first, R in the low byte
    };

    // The first mip level of a TXPR chunk's texture (DXT1/3/5; swizzled or
    // linear A8R8G8B8/X8R8G8B8). False with a reason otherwise.
    bool decodeTexture(const uint8_t *txpr, size_t size, Picture &out, std::string *error = nullptr);

    // At most 256 colours (alpha included if withAlpha), and each pixel's
    // index into them: median cut, then refined by k-means.
    struct Palettized
    {
        std::vector<uint32_t> palette; // RGBA, R low, 256 entries
        std::vector<uint8_t> index;    // width * height, top row first
    };
    void palettize(const Picture &picture, bool withAlpha, Palettized &out);

    // A Windows BMP, 8 bits per pixel (palette alpha as given), bottom-up.
    std::vector<uint8_t> writeBmp8(int width, int height, const Palettized &image, bool keepAlpha);

    // Halved (2x2 box filter, alpha-weighted colour) until neither side is
    // over maxSide.
    void shrink(Picture &picture, int maxSide);

    struct TextureReport
    {
        uint32_t id = 0;
        std::string name;
        int sourceWidth = 0; // the Xbox texture's
        int sourceHeight = 0;
        int width = 0; // written
        int height = 0;
        bool alpha = false;
        // Its alpha cuts the picture out (at least 3% of the texels under
        // 64), rather than a mask for an effect: the shells' blades on the
        // Zoo's grass, gloss on its coral and water.
        bool cutout = false;
        std::string error; // the texture is left out if set
    };

    // The Xbox picture of a converted texture at its own size, for a
    // renderer to draw in place of the converted one (smaller for the PS2
    // renderer's texture cache, and 8-bit): keyed as runtime/texture_replace.h
    // keys a texture, by the converted texture's size and the FNV-1a 64 of
    // its indices (top row first).
    struct HostTexture
    {
        std::string name;
        int width = 0, height = 0; // the converted (PS2) texture's
        uint64_t indexHash = 0;
        int pictureWidth = 0, pictureHeight = 0;
        int mips = 1;
        bool dxt3 = false;          // data: DXT3 (BC2) levels, mip 0 first; else RGBA8 of mip 0 (R low)
        std::vector<uint8_t> data;
    };

    // An Xbox texture library made into the PS2's: every texture at its own
    // size, or at most maxSide pixels a side (0: no limit). Worker threads:
    // 0 = the machine's. hostTextures (optional): the Xbox picture of every
    // texture (DXT3 as it is; the converted one is smaller or palettized).
    // texelBudget (bytes, 0: none): the converted textures' texels (8 bits
    // each) come to at most that, the largest made one level smaller first
    // (budgetSides): the game's GS texture cache holds a frame's textures
    // and does not check its end (runtime/gs_texture_audit.h).
    bool convertTextureLibrary(const std::vector<uint8_t> &xboxTxr, std::vector<uint8_t> &ps2Txr,
                               std::vector<TextureReport> *report = nullptr, std::string *error = nullptr,
                               unsigned threads = 0, int maxSide = 0, std::vector<HostTexture> *hostTextures = nullptr,
                               size_t texelBudget = 0);

    // Each texture's largest side (from maxSide down, halving, none below
    // 16) so that the textures' texels, each halved as shrink() halves it
    // (sizes: width, height before), come to at most budget bytes: the
    // largest texture one level smaller first, ties by order
    // (tools/modding/dm2_xbox.py budget_sides, for one 8-bit level).
    std::vector<int> budgetSides(const std::vector<std::pair<int, int>> &sizes, int maxSide, size_t budget,
                                 size_t *texels = nullptr);

    // One MATI layer (28 bytes) in place, Xbox to PS2. For layers with no
    // render state (the Zoo's): textureAlpha, its texture's alpha cuts it
    // out (the Xbox blends by the texture); grass, the base layer of a
    // grass material (drawn opaque, the alpha is for the shells).
    void convertLayer(uint8_t *layer, bool textureAlpha = false, bool grass = false);
    // An Xbox TGFM group's strip as PS2 groups of at most maxEntries.
    std::vector<std::vector<uint16_t>> restrip(const std::vector<uint16_t> &xbox, size_t maxEntries = 60u);

    struct GeometryReport
    {
        long materials = 0;  // MATI converted
        long objects = 0;    // TGPT converted
        long groupsIn = 0;   // Xbox TGFM groups
        long groupsOut = 0;  // PS2 TGFM groups
    };
    // alphaTextures: ids of the textures whose alpha cuts them out
    // (TextureReport::cutout).
    // A whole GEO file: materials and strips. False (and the file left
    // alone) if it is not a GEO or its chunks do not add up.
    bool convertGeometry(std::vector<uint8_t> &geo, GeometryReport *report = nullptr,
                         const std::unordered_set<uint32_t> *alphaTextures = nullptr, size_t maxGroupEntries = 60u);

    // Models (.ZIT: NPCs, props; "FMDL" > "MODL" > textures "TXRC" and
    // objects "TGEO", the same chunks as in the level files): the Xbox
    // release's made into the PS2's, as the level files are (textures as
    // 8-bit BMPs, at most maxSide a side if given; strips cut into groups;
    // materials). The PS2 exporter laid the vertices of skinned models out in
    // its own strip order; the PS2 loader takes either (the vertex chunks
    // are the same formats).
    struct ModelReport
    {
        std::vector<TextureReport> textures;
        long texturesLeftOut = 0;
        GeometryReport geometry;
    };
    bool convertModel(std::vector<uint8_t> &zit, ModelReport *report = nullptr, std::string *error = nullptr,
                      int maxSide = 0, std::vector<HostTexture> *hostTextures = nullptr, size_t maxGroupEntries = 60u);

    // Only the MATI layers in place; the number converted, or -1 if the
    // file is not a GEO.
    long convertMaterials(std::vector<uint8_t> &geo, const std::unordered_set<uint32_t> *alphaTextures = nullptr);
}
