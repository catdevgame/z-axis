// A texture's levels (zge/render/texture.h lays the records out): the
// platform's texture hooks, a picture taken as the next level while the GS
// mode or ZGE4's setting keeps that many, a texture's GS side freed, the
// GS side's start and end and ZGE4's settings, and the movie frame's tiles
// copied into its level. A level's and a CLUT's texels and upload chains as
// the GS takes them are ps2/gs_texture.cpp.

#include "texture.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/texture_levels.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_textureHeadersReady> g_textureHeadersReady;
        inline ee::Global<uint32_t, sym::g_gsTexturesReady> g_gsTexturesReady;
        // The GS mode sceGsResetGraph was given (3: the one that keeps only
        // two mip levels).
        inline ee::Global<uint32_t, sym::g_gsVideoMode> g_gsVideoMode;
        constexpr uint32_t kGsModeFewLevels = 3;
        // ZGE4's two texture settings, which the game sets (BMX XXX lowers them for some levels in multiplayer):
        // the last level a texture may have (ZGE3's fixed four levels, two in mode 3), and whether a texture
        // without alpha drops its first level while that is 64 texels wide or more (it keeps its smaller ones:
        // ps2/gs_texture.cpp's buildTextureUploadPacket).
        inline ee::Global<uint32_t, sym::bmx::g_lastTextureLevel> g_lastTextureLevel;
        inline ee::Global<uint32_t, sym::bmx::g_dropLargeTextureLevels> g_dropLargeTextureLevels;
        constexpr uint32_t kLastTextureLevel = 3, kLastTextureLevelFewLevels = 1;

        // A texture's GS side, through the same pointer: a texture reached through the uncached mirror keeps it, as
        // the original's address sums do.
        ee::Ptr<GsTexture> gsSideOf(ee::Ptr<Texture> texture) { return (texture.cast<uint8_t>() + offsetof(Texture, gs)).cast<GsTexture>(); }
    }

    // ---- the platform's texture hooks -------------------------------------

    // initTextureHeaders, shutDownTextureHeaders (the texture header loader's set-up). 1.
    uint32_t initTextureHeaders()
    {
        *g_textureHeadersReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initTextureHeaders, Verified);

    uint32_t shutDownTextureHeaders()
    {
        if (*g_textureHeadersReady == 0)
            return 1;
        *g_textureHeadersReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownTextureHeaders, Verified);

    // onTextureCreated: a texture made (newLoadedTexture): nothing to do on the PS2.
    void onTextureCreated()
    {
    }
    READABLE_FUNCTION(onTextureCreated, Verified);

    // freeTextureGsSide: a texture freed (releaseLoadedTexture): its GS side freed.
    void freeTextureGsSide(ee::Ptr<Texture> texture)
    {
        freeGsTexture(gsSideOf(texture));
    }
    READABLE_FUNCTION_WAITS(freeTextureGsSide, Verified);

    // textureStubZeroA, textureStubZeroB, textureStubZeroC: platform queries the PS2
    // answers 0 to (their wrappers textureStubCallA, textureStubCallB, textureStubCheckC have no caller).
    uint32_t textureStubZeroA()
    {
        return 0;
    }
    READABLE_FUNCTION(textureStubZeroA, Verified);

    uint32_t textureStubZeroB()
    {
        return 0;
    }
    READABLE_FUNCTION(textureStubZeroB, Verified);

    uint32_t textureStubZeroC()
    {
        return 0;
    }
    READABLE_FUNCTION(textureStubZeroC, Verified);

    // setGsTextureImage: a picture made the texture's next level (its texture
    // data handed on); whether it was.
    int32_t setGsTextureImage(ee::Ptr<Texture> texture, ee::Ptr<TextureImage> image)
    {
        const ee::Ptr<GsTexture> gs = gsSideOf(texture);
        const ee::Ptr<char> imageData = textureName(texture); // buildGsTextureUpload does not read it
        return buildGsTextureUpload(gs, image, imageData);
    }
    READABLE_FUNCTION_WAITS(setGsTextureImage, Verified);

    // textureOfGsTexture: the texture a GsTexture belongs to.
    uint32_t textureOfGsTexture(uint32_t gs)
    {
        return gs - static_cast<uint32_t>(offsetof(Texture, gs));
    }
    READABLE_FUNCTION(textureOfGsTexture, Verified);

    // log2Floor: the highest set bit of value (31 for a negative one,
    // 0 for 0 or 1).
    int32_t log2Floor(uint32_t value)
    {
        int32_t bit = 31;
        if (static_cast<int32_t>(value) >= 0)
        {
            uint32_t mask = 0x40000000u;
            for (;;)
            {
                --bit;
                if ((value & mask) != 0)
                    break;
                mask >>= 1;
                if (bit <= 0)
                    break;
            }
        }
        return bit;
    }
    READABLE_FUNCTION(log2Floor, Verified);

    // buildGsTextureUpload: a picture added as the texture's next level: the first
    // with its CLUT, later ones while the GS mode keeps that many (two in
    // mode 3, four otherwise) and the size halves; whether it was.
    int32_t buildGsTextureUpload(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image, ee::Address imageData)
    {
        GsTexture &gs = *texture;
        if (gs.levelCount == 0)
        {
            buildTextureUploadPacket(texture, image);
            buildClutUpload(texture, image);
        }
        else
        {
            const uint32_t levels = gs.levelCount;
            bool room = static_cast<int32_t>(levels) < 4;
            if constexpr (game::readable::kIsBmx) // (ZGE4: up to a setting, g_lastTextureLevel)
                room = levels <= *g_lastTextureLevel;
            else if constexpr (!game::readable::kIsDm2) // (ZGE2 has no video mode word: room for four levels always)
                if (*g_gsVideoMode == kGsModeFewLevels)
                    room = static_cast<int32_t>(levels) < 2;
            bool added = false;
            if (room)
                added = buildTextureUploadPacket(texture, image) != 0;
            if (!added)
                return 0;
        }
        gs.levelCount = static_cast<uint8_t>(gs.levelCount + 1);
        return 1;
    }
    READABLE_FUNCTION_WAITS(buildGsTextureUpload, Verified);

    // freeGsTexture: a texture's GS side freed: out of the cache, its levels'
    // texels and its CLUT back to their pools (the CLUT's: the original's
    // tail call).
    void freeGsTexture(ee::Ptr<GsTexture> texture)
    {
        evictGsTexture(texture, 1);
        GsTexture &gs = *texture;
        ee::Ptr<TextureLevel> level = levelOf(texture, 0);
        if (gs.levelCount != 0)
        {
            for (uint32_t index = 0;;)
            {
                if (level->texels)
                    freeIn(level->texels, kTexelPool);
                ++index;
                ++level;
                if (static_cast<int32_t>(index) >= static_cast<int32_t>(gs.levelCount))
                    break;
            }
        }
        if (gs.clut)
        {
            game::tail(freeIn, gs.clut, kClutPool); // the original's tail call
            return;
        }
    }
    READABLE_FUNCTION_WAITS(freeGsTexture, Verified);

    // initGsTextures, shutDownGsTextures (the renderer's start and shutdown). 1.
    uint32_t initGsTextures()
    {
        *g_gsTexturesReady = 1;
        if constexpr (game::readable::kIsBmx) // ZGE4's texture settings at their start
        {
            *g_dropLargeTextureLevels = 0;
            *g_lastTextureLevel = *g_gsVideoMode == kGsModeFewLevels ? kLastTextureLevelFewLevels : kLastTextureLevel;
        }
        return 1;
    }
    READABLE_FUNCTION(initGsTextures, Verified);

    // setTextureSetting (ZGE4 only): the game's change to one of the two
    // texture settings (another number sets nothing). 1.
    inline uint32_t setTextureSetting(uint32_t setting, uint32_t value)
    {
        if (setting == kTextureSettingDropLargeLevel)
            *g_dropLargeTextureLevels = value;
        else if (setting == kTextureSettingLastLevel)
            *g_lastTextureLevel = value;
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, setTextureSetting);

    uint32_t shutDownGsTextures()
    {
        if (*g_gsTexturesReady == 0)
            return 1;
        *g_gsTexturesReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownGsTextures, Verified);

    // copyTextureTiles: a 32-bit picture in 16 x 16 tiles (a column of tiles,
    // top to bottom, then the next column) copied into level 0's texels,
    // row by row; marked to upload.
    void copyTextureTiles(ee::Ptr<GsTexture> texture, uint32_t tiles)
    {
        constexpr uint32_t kTileLineBytes = 16 * 4, kTileBytes = 16 * kTileLineBytes;
        GsTexture &gs = *texture;
        const uint32_t tilesAcross = static_cast<uint32_t>(gs.baseWidth) >> 4;
        const uint32_t tilesDown = static_cast<uint32_t>(gs.baseHeight) >> 4;
        const uint32_t tilesAcrossSlot = tilesAcross;
        uint32_t source = tiles;
        if (tilesAcross != 0)
        {
            for (uint32_t column = 0;;)
            {
                if (tilesDown != 0)
                {
                    const uint32_t left = column << 4;
                    for (uint32_t row = 0;;)
                    {
                        const uint32_t nextRow = row + 1;
                        const uint32_t nextTile = source + kTileBytes;
                        const uint32_t tileOffset = (row * (static_cast<uint32_t>(gs.baseWidth) << 4) + left) << 2;
                        for (uint32_t line = 0;;)
                        {
                            const uint32_t lineOffset = line * gs.baseWidth;
                            const uint32_t from = (line << 6) + source;
                            ++line;
                            memoryCopy(gs.levels[0].texels + (tileOffset + (lineOffset << 2)), from, kTileLineBytes);
                            if (line >= 16)
                                break;
                        }
                        row = nextRow;
                        source = nextTile;
                        if (static_cast<int32_t>(row) >= static_cast<int32_t>(tilesDown))
                            break;
                    }
                }
                ++column;
                if (static_cast<int32_t>(column) >= static_cast<int32_t>(tilesAcrossSlot))
                    break;
            }
        }
        if constexpr (!game::readable::kIsDm2) // (ZGE2 does not mark level 0 to upload)
            gs.pendingUploads |= 1u;
    }
    READABLE_FUNCTION(copyTextureTiles, Verified);
}
