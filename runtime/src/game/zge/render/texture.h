#pragma once

// Textures of the ZGE3 engine and the GS texture cache.
//
// A texture object (0x560 bytes, newLoadedTexture) is loaded from a TXR file's
// TXRC chunk: the TXRH header names it (zge/assets/txr.h), the TBMP / TZMP
// picture fills its levels. Texels and CLUT stay in EE RAM for the whole
// level; GS memory is only a cache: when a model is about to be drawn, the
// VIF1 interrupt handler vif1Interrupt gives each level it uploads a GS
// address (placeTextureLevel), and the CLUT one (placeTextureClut), patches
// the upload chains' BITBLTBUF and copies the new TEX0 / MIPTBP1 into the
// materials' GS blocks. The cache is flushed whole (flushTextureCache), never
// evicted piecemeal.
//
// GS memory in a level, in words (blocks of 64):
//   0 .. poolBottom      frame buffers, Z, shadow counter (resetDisplayMode)
//   poolBottom .. low    texels, allocated upwards
//   high .. the top      CLUTs, 0x100 words each, allocated downwards
// When low passes high the overflow flag is set and nothing else happens.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/container/array.h"
#include "zge/render/gs_registers.h"
#include "sym/zge/render/texture.h.h"

namespace sce
{
    struct Vif1Packet;
}

namespace zge
{
    struct Node;          // zge/scene/node.h
    struct MaterialTable; // zge/render/material.h
    struct Material;      // zge/render/material.h

    // One mip level of a texture (0xA0 bytes).
    struct TextureLevel
    {
        ee::Ptr<uint8_t> texels;     // +0x00 in EE RAM (T8 / T4 indices, or colours)
        uint32_t uploadQwords;       // +0x04 size of the upload (the handler's statistics)
        uint32_t gsAddress;          // +0x08 GS word address; 0: not in GS memory
        uint16_t width;              // +0x0C
        uint16_t height;             // +0x0E
        uint16_t bufferWidth;        // +0x10 TBW, in 64-pixel units
        uint16_t _12;          // +0x12
        uint32_t gsWords;            // +0x14 GS memory it takes
        uint8_t unknown18[8];        // +0x18
        // +0x20 the upload: a DMA chain (tag, GIF tag, BITBLTBUF, TRXPOS,
        // TRXREG, TRXDIR, then the texels by REF) the handler CALLs.
        uint8_t uploadHead[0x20];    // +0x20 DMA tag and GIF tag
        uint64_t uploadBitbltbuf;    // +0x40 BITBLTBUF: DBP (bits 32-45) is patched to gsAddress
        uint8_t uploadRest[0x58];    // +0x48
    };
    static_assert(sizeof(TextureLevel) == 0xA0);
    static_assert(offsetof(TextureLevel, gsAddress) == 0x08 && offsetof(TextureLevel, bufferWidth) == 0x10 &&
                  offsetof(TextureLevel, gsWords) == 0x14 && offsetof(TextureLevel, uploadBitbltbuf) == 0x40);

    constexpr int kTextureMaxLevels = 7;

    // Bits of GsTexture::pendingUploads.
    enum : uint32_t
    {
        kTextureUploadClut = 0x100u, // the CLUT still has to be uploaded; bits 0-6: levels
    };

    // The part of a texture object the GS cache works on (texture + 0x30;
    // vif1Interrupt hands this address around). Offsets are from here; the
    // texture object's are 0x30 more.
    struct GsTexture
    {
        uint32_t cacheFlags;              // +0x000 bit 0: on the cache's resident list
        uint32_t pendingUploads;          // +0x004 levels (bit k) and the CLUT (0x100) to upload
        uint8_t _008[8];            // +0x008
        TextureLevel levels[kTextureMaxLevels]; // +0x010
        uint8_t levelCount;               // +0x470 the levels set up
        uint8_t psm;                      // +0x471 gs::Psm of the texels
        uint8_t log2Width;                // +0x472 TEX0.TW
        uint8_t log2Height;               // +0x473 TEX0.TH
        uint16_t baseWidth;               // +0x474 level 0's width (a multiple of the texels a word holds)
        uint16_t baseHeight;              // +0x476 level 0's height (a multiple of 8)
        ee::Ptr<uint8_t> clut;            // +0x478 in EE RAM: CSM1-swizzled, RGB pre-scaled, alpha halved
        uint32_t clutQwords;              // +0x47C
        uint32_t clutGsAddress;           // +0x480 GS word address; 0: not in GS memory
        uint8_t clutLevels;               // +0x484 1 once the CLUT is set up (nothing reads it)
        uint8_t clutPsm;                  // +0x485 TEX0.CPSM
        uint16_t clutEntries;             // +0x486 16 or 256
        uint8_t clutWidth;                // +0x488 its upload's width in pixels (8 or 16)
        uint8_t _489[7];
        uint8_t clutUploadHead[0x20];     // +0x490 the CLUT's upload chain, as a level's
        uint64_t clutUploadBitbltbuf;     // +0x4B0
        uint8_t clutUploadRest[0x58];     // +0x4B8
        uint8_t clutHeight;               // +0x510 its upload's height in pixels (2 or 16)
        uint8_t _511[7];
        uint64_t tex0;                    // +0x518 TEX0 for the material GS blocks (+0xD0)
        uint64_t miptbp1;                 // +0x520 MIPTBP1 (material +0xB0)
        uint8_t _528[8];            // +0x528
    };
    static_assert(sizeof(GsTexture) == 0x530);
    static_assert(offsetof(GsTexture, levels) == 0x10 && offsetof(GsTexture, levelCount) == 0x470 &&
                  offsetof(GsTexture, psm) == 0x471 && offsetof(GsTexture, clut) == 0x478 &&
                  offsetof(GsTexture, clutGsAddress) == 0x480 && offsetof(GsTexture, clutPsm) == 0x485 &&
                  offsetof(GsTexture, clutUploadBitbltbuf) == 0x4B0 && offsetof(GsTexture, tex0) == 0x518 &&
                  offsetof(GsTexture, miptbp1) == 0x520 && offsetof(GsTexture, baseHeight) == 0x476 &&
                  offsetof(GsTexture, clutEntries) == 0x486 && offsetof(GsTexture, clutWidth) == 0x488 &&
                  offsetof(GsTexture, clutHeight) == 0x510);

    // A texture object (newLoadedTexture: 0x560 bytes, type 5; ZGE2 numbers its object types one lower).
    constexpr uint8_t kLoadedTextureType = game::readable::perGame(5, 4, 5);
    struct Texture
    {
        uint8_t type;                 // +0x00 5 (every engine object starts with its type)
        uint8_t flags;                // +0x01 bit 0: an image with alpha (setTextureImage); textureFlagsByte
        uint16_t refCount;            // +0x02
        uint32_t id;                  // +0x04 TXRH id
        uint16_t reserved;            // +0x08 the TXRH header's reserved word at +0x0A (0 in every PS2 file; nothing reads it)
        uint16_t width;               // +0x0A
        uint16_t height;              // +0x0C
        uint8_t bitsPerTexel;         // +0x0E the first image's (setTextureImage)
        uint8_t imagesSet;            // +0x0F the levels setTextureImage has given it
        char name[32];                // +0x10
        GsTexture gs;                 // +0x30
    };
    static_assert(sizeof(Texture) == 0x560);
    static_assert(offsetof(Texture, gs) == 0x30 && offsetof(Texture, gs) + offsetof(GsTexture, tex0) == 0x548 &&
                  offsetof(Texture, gs) + offsetof(GsTexture, levelCount) == 0x4A0);

    // A texture's GS part, at the texture's own EE address plus its offset:
    // a texture held through the uncached mirror (the movie's) stays there.
    inline ee::Ptr<GsTexture> gsTextureOf(ee::Ptr<Texture> texture)
    {
        return ee::Ptr<GsTexture>(texture.address + static_cast<uint32_t>(offsetof(Texture, gs)));
    }

    // A picture handed to a texture as its next level (setTextureImage,
    // setTextureImage; the TBMP / TZMP readers make them).
    struct TextureImage
    {
        uint32_t flags;          // +0x00 bit 0: setTextureImage's follow-up (textureFlagsByte)
        uint32_t width;          // +0x04 in texels
        uint32_t height;         // +0x08
        uint32_t _0c;
        ee::Ptr<uint8_t> texels; // +0x10
        uint32_t _14;
        uint8_t bitsPerTexel;    // +0x18 4, 8, 16, 24 or 32
        uint8_t _19;
        uint16_t clutEntries;    // +0x1A a 4- or 8-bit picture's palette: 16 or 256 RGBA entries
        ee::Ptr<uint8_t> clut;   // +0x1C
    };
    static_assert(offsetof(TextureImage, texels) == 0x10 && offsetof(TextureImage, bitsPerTexel) == 0x18 &&
                  offsetof(TextureImage, clut) == 0x1C && sizeof(TextureImage) == 0x20);

    // The GS texture cache's state (g_textureCache).
    struct TextureCache
    {
        uint32_t ready;          // +0x00 1 from initTextureCache to shutDownTextureCache
        uint32_t poolTop;        // +0x04 where high starts (the top of GS memory)
        uint32_t poolBottom;     // +0x08 where low starts (above the frame and Z buffers)
        uint32_t flushCountdown; // +0x0C frames until a requested flush (onCameraCut and advanceTextureCache set it)
        uint32_t lastPsm;        // +0x10 the format of the last level placed
        uint32_t low;            // +0x14 next free word for texels (grows up)
        uint32_t high;           // +0x18 lowest word taken by CLUTs (grows down)
        ee::Ptr<PointerArray> residentList; // +0x1C the PointerArray of the GsTextures given GS memory (pointerArrayAdd appends)
        uint32_t freeBytes;      // +0x20 (high - low) * 4; a flush is requested under 384 KB
        uint32_t overflow;       // +0x24 set when low passes high
    };
    static_assert(sizeof(TextureCache) == 0x28 && offsetof(TextureCache, low) == 0x14 &&
                  offsetof(TextureCache, residentList) == 0x1C && offsetof(TextureCache, overflow) == 0x24);
    inline ee::Global<TextureCache, sym::g_textureCache> g_textureCache;

    // A CLUT's GS memory: 256 32-bit entries (4 blocks).
    constexpr uint32_t kClutWords = 0x100u;

    // placeTextureLevel: gives a level GS memory at the low cursor (page-aligned
    // when the format differs from the last level's), puts the texture on
    // the resident list and rebuilds its TEX0 and MIPTBP1.
    // The texture comes as the handler has it, through the uncached mirror
    // of its address: kept as an ee::Ptr so the list gets that address.
    void placeTextureLevel(ee::Ptr<GsTexture> texture, ee::Ptr<TextureLevel> level);
    // placeTextureClut: gives the CLUT GS memory below the high cursor and
    // rebuilds TEX0.
    void placeTextureClut(ee::Ptr<GsTexture> texture);
    // resetTextureCache: sets the pool up for a level: [poolBottom, the top of GS memory).
    void resetTextureCache(uint32_t poolBottom);
    // texture_levels.cpp: onTextureCreated what a new texture asks of the GS side;
    // freeTextureGsSide a texture's GS memory given back.
    void onTextureCreated();
    void freeTextureGsSide(ee::Ptr<Texture> texture);
    // flushTexturesOnAreaExit: a flush requested when the current camera has left `area`.
    void flushTexturesOnAreaExit(uint32_t area);
}

namespace zge
{
    // The memory pools a texture's levels' texels and its CLUT come from (allocateIn).
    constexpr uint32_t kTexelPool = 3, kClutPool = 4;
    // A texture's level, through the texture's own pointer: one reached through the uncached mirror keeps it, as
    // the original's address sums do.
    inline ee::Ptr<TextureLevel> levelOf(ee::Ptr<GsTexture> texture, uint32_t level)
    {
        return (texture.cast<uint8_t>() + offsetof(GsTexture, levels)).cast<TextureLevel>() + level;
    }

    // (texture_levels.cpp)
    void copyTextureTiles(ee::Ptr<GsTexture> texture, uint32_t tiles);
    int32_t log2Floor(uint32_t value);
    void freeGsTexture(ee::Ptr<GsTexture> texture);
    int32_t buildGsTextureUpload(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image, ee::Address imageData);
    uint32_t initTextureHeaders();
    uint32_t shutDownTextureHeaders();
    uint32_t textureStubZeroA();
    uint32_t textureStubZeroB();
    uint32_t textureStubZeroC();
    int32_t setGsTextureImage(ee::Ptr<Texture> texture, ee::Ptr<TextureImage> image);
    uint32_t textureOfGsTexture(uint32_t gs);
    uint32_t initGsTextures();
    uint32_t shutDownGsTextures();
    // (ZGE4 only) One of the two texture settings set: kTextureSettingDropLargeLevel (0 or 1) or
    // kTextureSettingLastLevel. 1.
    constexpr uint32_t kTextureSettingDropLargeLevel = 0, kTextureSettingLastLevel = 1;
    uint32_t setTextureSetting(uint32_t setting, uint32_t value);
}

namespace zge
{
    // (ps2/gs_texture.cpp) A picture as a level and a CLUT the GS takes, each with its upload chain; the movie
    // frame's level.
    void scaleClutColours(ee::Address dst, ee::Address src, uint32_t entries, uint32_t format);
    void buildClutUpload(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image);
    int32_t buildTextureUploadPacket(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image);
    uint32_t setGsTextureFromImage(ee::Ptr<GsTexture> texture, uint32_t tiles, uint32_t width, uint32_t height);
}

namespace zge
{
    // (loaded_textures.cpp)
    ee::Ptr<Texture> newLoadedTexture();
    int32_t releaseLoadedTexture(ee::Ptr<Texture> texture);
    uint32_t findLoadedTextureByHash(uint32_t hash);
    uint32_t nameTexture(ee::Ptr<Texture> texture, uint32_t width, uint32_t height, uint32_t bitsPerTexel, uint32_t id, uint32_t reserved, ee::Address name);
    ee::Ptr<Texture> findLoadedTexture(uint32_t id, ee::Ptr<const char> name);
    ee::Ptr<Texture> newProceduralTexture(ee::Ptr<TextureImage> image);
    ee::Ptr<char> textureName(ee::Ptr<Texture> texture);
    int32_t retainTexture(ee::Ptr<Texture> texture);
    uint32_t textureFlagsByte(ee::Ptr<Texture> texture);
    uint32_t setTextureLoadedCallback(uint32_t setTextureLoadedCallbackResult);
    uint32_t textureHash(ee::Ptr<Texture> texture);
    void textureHook();
    uint32_t touchLoadedTextures();
    uint32_t setTextureImage(ee::Ptr<Texture> texture, ee::Ptr<TextureImage> textureImage);
    uint32_t levelWidth(ee::Ptr<Texture> texture);
    uint32_t levelHeight(ee::Ptr<Texture> texture);
}

namespace zge
{
    // (texture.cpp)
    uint32_t initTextureCache();
    uint32_t shutDownTextureCache();
    void requestTextureCacheFlush();
    void requestTextureCacheFlushThunk(); // (ZGE4 only) a jump to it
    void advanceTextureCache();
    void resetTextureCacheAndFlush(uint32_t poolBottom);
    void evictGsTexture(ee::Ptr<GsTexture> texture, uint32_t unlist);
    void gsLevelSize(uint32_t psm, ee::Ptr<uint32_t> width, ee::Ptr<uint32_t> height);
}

namespace zge
{
    // (ps2/texture_request.cpp)
    void requestTextures(sce::Vif1Packet *packet, ee::Ptr<Node> node, ee::Ptr<MaterialTable> materials);
}

namespace zge
{
    // (ps2/texture_upload.cpp)
    void initUploadBuffers();
    void freeUploadBuffers();
}

namespace zge
{
    // (pictures.cpp)
    uint32_t pictureLevel(ee::Ptr<Material> material, uint32_t index);
    uint32_t refreshAllPictures();
    uint32_t findTextureByHash(uint32_t nameHash, uint32_t textureSearchArgument);
    int32_t setMaterialPicture(ee::Ptr<Material> material, ee::Address texture, uint32_t index);
}
