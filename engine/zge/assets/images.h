#pragma once

// Images read from memory (zge/assets/images.cpp): the record a texture is
// made from, and the file formats' readers (BMP, TGA), found by asking each
// in turn whether the file is its own.

#include "ee/address_compare.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // An image (0x24 bytes, from the readers' pool).
    enum ImageFlags : uint32_t
    {
        kImageHasAlpha = 0x1u,    // some texels transparent: a palette entry was the colour key (255, 0, 255), or (ZGE4)
                                  // it came from RGBA pixels (newTextureFromPixels); its texture keeps it (flags bit 0)
        kImageMainMemory = 0x2u,  // its data comes from allocateMemory, else the heap (newImageRecord sets it)
    };
    struct Image
    {
        uint32_t flags;         // +0x00 ImageFlags
        int32_t width;          // +0x04
        int32_t height;         // +0x08
        int32_t rowBytes;       // +0x0C
        uint32_t pixels;        // +0x10
        int32_t pixelBytes;     // +0x14
        uint8_t bits;           // +0x18 bits a pixel
        uint8_t alignment19;    // +0x19 paletteCount's alignment
        uint16_t paletteCount;  // +0x1A
        uint32_t palette;       // +0x1C R, G, B, A
        int32_t paletteBytes;   // +0x20
    };
    static_assert(offsetof(Image, palette) == 0x1C && sizeof(Image) == 0x24);

    // A pixel or palette entry: red, green, blue, alpha.
    struct Rgba8
    {
        uint8_t red, green, blue, alpha;
    };
    static_assert(sizeof(Rgba8) == 4);

    // The readers by kind (1, 3: BMP; 2: TGA): the kinds findImageReader asks (ZGE2 the first three, ZGE4 a
    // fifth), and what it answers when none knows the file.
    constexpr uint32_t kImageReaderKinds = game::readable::perGame(4u, 3u, 5u);
    constexpr uint32_t kNoImageReader = game::readable::perGame(0u, 4u, 0u);

    // A box of the median cut (quantizeImage, images.cpp): a set of pixels, the box it was split from, and the
    // sums its palette colour is averaged from. ZGE4's (0x2C bytes) also keeps the two halves split from it and
    // where it was split, so that each pixel finds its palette entry by walking down the boxes
    // (texelsFromColourBoxes), and the entry its leaf became.
    struct ColourBox
    {
        ee::Ptr<ColourBox> parent;    // +0x00 0 for the first box
#if ZX_GAME_BMX
        // BMX +0x04: the halves split from it, the pixels at or below the split's middle first; one merged back
        // into it is 0 again.
        ee::Ptr<ColourBox> halves[2];
#endif
        uint32_t pixelCount;          // +0x04 (BMX +0x0C) set when it is not split
        uint32_t redSum;              // +0x08 (BMX +0x10) the same
        uint32_t greenSum;            // +0x0C (BMX +0x14)
        uint32_t blueSum;             // +0x10 (BMX +0x18)
        uint32_t alphaSum;            // +0x14 (BMX +0x1C)
        uint32_t error;               // +0x18 (BMX +0x20) its pixels' squared distances from their mean
#if ZX_GAME_BMX
        uint16_t children;            // BMX +0x24 the boxes split from it
        int16_t paletteEntry;         // BMX +0x26 the palette entry its pixels take (-1 in the blank box)
        uint8_t splitChannel;         // BMX +0x28 the channel it was split on: red, green, blue, alpha
        uint8_t splitMiddle;          // BMX +0x29 that channel's value at the split
        uint8_t _2a[2];
#else
        uint8_t children;             // +0x1C the boxes split from it
        uint8_t _1d[3];
#endif
    };
    static_assert(sizeof(ColourBox) == game::readable::perGame(0x20u, 0x20u, 0x2Cu) &&
                  offsetof(ColourBox, error) == game::readable::perGame(0x18u, 0x18u, 0x20u) &&
                  offsetof(ColourBox, children) == game::readable::perGame(0x1Cu, 0x1Cu, 0x24u));

    // ZGE4's links between the boxes and their palette entries (ColourBox's BMX XXX fields; nothing in ZGE2 or ZGE3
    // keeps them, and their steps run in BMX XXX alone): halfOf the half `half` (0 or 1) of a box, 0 when there is
    // none; setHalf a box made its parent's half; unlinkHalf a half merged back into its parent no longer one of its
    // halves; keepSplit where a box was split, and splitChannelOf / splitMiddleOf read it back; setPaletteEntry and
    // paletteEntryOf the palette entry a leaf's pixels take.
#if ZX_GAME_BMX
    inline ee::Ptr<ColourBox> halfOf(ee::Ptr<ColourBox> box, uint32_t half) { return box->halves[half]; }
    inline void setHalf(ee::Ptr<ColourBox> parent, uint32_t half, ee::Ptr<ColourBox> box) { parent->halves[half] = box; }
    inline void unlinkHalf(ee::Ptr<ColourBox> parent, ee::Ptr<ColourBox> half)
    {
        if (ee::same(parent->halves[0], half))
            parent->halves[0] = ee::Ptr<ColourBox>();
        else
            parent->halves[1] = ee::Ptr<ColourBox>();
    }
    inline void keepSplit(ee::Ptr<ColourBox> box, uint8_t channel, uint8_t middle)
    {
        box->splitChannel = channel;
        box->splitMiddle = middle;
    }
    inline uint32_t splitChannelOf(ee::Ptr<ColourBox> box) { return box->splitChannel; }
    inline uint8_t splitMiddleOf(ee::Ptr<ColourBox> box) { return box->splitMiddle; }
    inline void setPaletteEntry(ee::Ptr<ColourBox> box, uint32_t entry) { box->paletteEntry = static_cast<int16_t>(entry); }
    inline int32_t paletteEntryOf(ee::Ptr<ColourBox> box) { return box->paletteEntry; }
#else
    inline ee::Ptr<ColourBox> halfOf(ee::Ptr<ColourBox>, uint32_t) { return ee::Ptr<ColourBox>(); }
    inline void setHalf(ee::Ptr<ColourBox>, uint32_t, ee::Ptr<ColourBox>) {}
    inline void unlinkHalf(ee::Ptr<ColourBox>, ee::Ptr<ColourBox>) {}
    inline void keepSplit(ee::Ptr<ColourBox>, uint8_t, uint8_t) {}
    inline uint32_t splitChannelOf(ee::Ptr<ColourBox>) { return 0; }
    inline uint8_t splitMiddleOf(ee::Ptr<ColourBox>) { return 0; }
    inline void setPaletteEntry(ee::Ptr<ColourBox>, uint32_t) {}
    inline int32_t paletteEntryOf(ee::Ptr<ColourBox>) { return 0; }
#endif

    struct ChunkReader;

    uint32_t allocateImageData(uint32_t image, uint32_t bytes);
    uint32_t allocateImageDataFromMainRegion(uint32_t bytes); // ZGE2's allocateImageData (images.cpp)
    uint32_t freeImageData(uint32_t image);
    uint32_t initImages();
    uint32_t shutdownImageReaders();
    uint32_t newImageRecord();
    uint32_t freeImageRecord(uint32_t image);
    void setImageReader(uint32_t reader, uint32_t kind);
    int32_t findImageReader(uint32_t memory);
    int32_t readImage(uint32_t image, uint32_t memory, uint32_t kind);
    // setImageMipmaps (ZGE4 only, images.cpp): whether readImageChunk makes a TZMP texture's smaller levels; the
    // setting it replaces.
    uint32_t setImageMipmaps(uint32_t on);
    uint32_t readImageChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture, uint32_t kind
                            READABLE_PARAM_ONLY(bmx, uint32_t mipmaps));
    void setImagePaletteEntry(uint32_t image, uint32_t index, ee::Address colour);
    void imagePaletteEntry(ee::Ptr<Image> image, uint32_t index, ee::Address colour);
    // expandPalettedImage: the image as 32-bit RGBA pixels at out (width x height, packed).
    void expandPalettedImage(ee::Ptr<Image> image, ee::Ptr<Rgba8> out);
    uint32_t readBmpImage(uint32_t image, uint32_t memory, uint32_t options); // BMP options: keep the palette's alpha, skip the palette
    int32_t readBmpImageKeepingAlpha(uint32_t image, uint32_t memory);
    int32_t isBmpImage(uint32_t memory);
    int32_t readTbmpChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture);
    int32_t readTzmpChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture);
    void registerBitmapChunkHandlers();
    void shutdownBmpReader();
    uint32_t readTgaImage(uint32_t image, uint32_t memory);
    uint32_t isTgaImage(uint32_t memory);
    int32_t readTtgaChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture);
    uint32_t registerTextureChunkHandlers();
    uint32_t shutdownTgaReader();
}
