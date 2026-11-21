// Images read from memory (zge/assets/images.h; the block allocateImageData-readTtgaChunk
// but the colour quantizer): records from a pool, a table of readers by kind
// (each a test and a read: BMP, and BMP keeping the palette's alpha, TGA) and
// the texture chunks that hold such files (TBMP, TZMP, TTGA).
//
// Original quirks kept: isTgaImage turns down anything that starts "BM", and
// images whose sides are not powers of two; findImageReader answers 0, a kind
// with no reader, when none knows the file.

#include "images.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/render/texture.h"

#include <cstdint>
#include "sym/zge/assets/images.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kAllocateImageData = sym::allocateImageData;
        constexpr uint32_t kFreeImageData = sym::freeImageData;
        constexpr uint32_t kNewImageRecord = sym::newImageRecord;
        constexpr uint32_t kFreeImageRecord = sym::freeImageRecord;
        constexpr uint32_t kSetImageReader = sym::setImageReader;
        constexpr uint32_t kFindImageReader = sym::findImageReader;
        constexpr uint32_t kReadImage = sym::readImage;
        constexpr uint32_t kReadImageChunk = sym::readImageChunk;
        constexpr uint32_t kReadBmpImage = sym::readBmpImage;
        constexpr uint32_t kReadTbmpChunk = sym::readTbmpChunk;
        constexpr uint32_t kReadTzmpChunk = sym::readTzmpChunk;
        constexpr uint32_t kRegisterBitmapChunkHandlers = sym::registerBitmapChunkHandlers;
        constexpr uint32_t kShutdownBmpReader = sym::shutdownBmpReader;
        constexpr uint32_t kCreatePool = sym::createPool;       // (record bytes, records a block, free callback) -> pool

        // The images' state (g_imagesReady): ready, the records' pool, the readers.
        constexpr uint32_t kImagesAddress = sym::g_imagesReady;
        inline ee::Global<uint32_t, kImagesAddress> g_imagesReady;
        inline ee::Global<uint32_t, sym::g_imageRecordPool> g_imageRecordPool;
        // ZGE2's one image record, in place of the pool (newImageRecord hands it out cleared).
        constexpr uint32_t kZge2ImageRecord = sym::dm2::g_imageRecord;
        inline ee::Global<uint32_t, sym::g_bmpReaderReady> g_bmpReaderReady;
        // ZGE4: whether TZMP textures get their smaller levels made (setImageMipmaps), and how many.
        inline ee::Global<uint32_t, sym::bmx::g_imageMipmapsOn> g_imageMipmapsOn;
        constexpr uint32_t kMipmapLevelsMade = 2u;
        constexpr uint32_t kImageMipmaps = 1u; // readImageChunk's mipmaps: make the smaller levels

        constexpr uint32_t kImageRecordsABlock = 0x10u;

        // A format's two functions (the table in data).
        struct ImageReader
        {
            uint32_t isIts;  // (memory) -> not 0 when the file is this format's
            uint32_t read;   // (image, memory, 0)
        };
        constexpr uint32_t kBmpReader = sym::bmpReader;           // isBmpImage, readBmpImage
        constexpr uint32_t kBmpAlphaReader = sym::bmpAlphaReader;      // isBmpImage, readBmpImageKeepingAlpha
        // ZGE2 numbers the kinds from 0 (registerBitmapChunkHandlers, registerTextureChunkHandlers).
        constexpr uint32_t kImageKindBmp = game::readable::perGame(1u, 0u, 1u);
        constexpr uint32_t kImageKindTga = game::readable::perGame(2u, 1u, 2u);
        constexpr uint32_t kImageKindBmpAlpha = game::readable::perGame(3u, 2u, 3u);

        // The formats' readers, by kind (0: none registered).
        ee::Ptr<ImageReader> &imageReader(uint32_t kind) { return ee::Ptr<ee::Ptr<ImageReader>>(sym::imageReaders)[kind]; }
        int32_t callIsItsFormat(uint32_t function, uint32_t memory) { return ee::game<int32_t>(function, memory); }
        int32_t callReadFormat(uint32_t function, uint32_t image, uint32_t memory, uint32_t options) { return ee::game<int32_t>(function, image, memory, options); }

        // readBmpImage's options.
        constexpr uint32_t kBmpKeepPaletteAlpha = 0x1u;
        constexpr uint32_t kBmpSkipPalette = 0x2u;

        constexpr uint16_t kBmpSignature = 0x4D42u;  // "BM"
        constexpr uint32_t kChunkTbmp = 0x504D4254u; // "TBMP"
        constexpr uint32_t kChunkTzmp = 0x504D5A54u; // "TZMP"
        constexpr uint32_t kChunkTtga = 0x41475454u; // "TTGA"

        // A BMP file's header as the original copies it to its frame.
        struct BmpInfo
        {
            int32_t headerBytes;  // +0x10
            int32_t width;        // +0x14
            int32_t height;       // +0x18
            uint16_t planes;      // +0x1C
            int16_t bits;         // +0x1E
            int32_t compression;  // +0x20
            int32_t imageBytes;   // +0x24
            int32_t xResolution;  // +0x28
            int32_t yResolution;  // +0x2C
            int32_t coloursUsed;  // +0x30
            int32_t coloursImportant; // +0x34
        };
        static_assert(sizeof(BmpInfo) == 40 && offsetof(BmpInfo, coloursUsed) == 0x20);
        constexpr uint32_t kBmpFileHeaderBytes = 14u;

        // A palette entry as a BMP file has it (RGBQUAD), and as the image
        // keeps it.
        struct BmpColour
        {
            uint8_t blue, green, red, reserved;
        };
        struct PaletteColour
        {
            uint8_t red, green, blue, alpha;
        };
        static_assert(sizeof(BmpColour) == 4 && sizeof(PaletteColour) == 4);

        // A palette of 2^n entries read from the file: BGRA there, RGBA here;
        // the colour key (255, 0, 255) becomes transparent black.
        void readBmpPalette(Image &image, uint32_t palette, uint32_t &read, int32_t colours, uint32_t options)
        {
            ee::Ptr<PaletteColour> out(palette);
            for (;;)
            {
                const BmpColour &in = *ee::Ptr<BmpColour>(read);
                const uint8_t blue = in.blue;
                const uint8_t green = in.green;
                const uint8_t red = in.red;
                out->blue = blue;
                out->green = green;
                out->red = red;
                if (red == 255 && green == 0 && blue == 255)
                {
                    out->blue = 0;
                    out->green = 0;
                    out->red = 0;
                    out->alpha = 0;
                    image.flags |= kImageHasAlpha;
                }
                else
                    out->alpha = (options & kBmpKeepPaletteAlpha) ? in.reserved : 255;
                if constexpr (game::readable::kIsDm2) // ZGE2 halves each entry's alpha
                    out->alpha = static_cast<uint8_t>(out->alpha >> 1);
                ++out;
                --colours;
                read += sizeof(BmpColour);
                if (colours == 0)
                    break;
            }
        }
    }

    // allocateImageData (not in the sweep): an image's data, from
    // the main region's memory if the record says so, else the heap.
    uint32_t allocateImageData(uint32_t imageAddress, uint32_t bytes)
    {
        if (ee::Ptr<Image>(imageAddress)->flags & kImageMainMemory)
            return allocateMemory(bytes);
        return heapAllocate(bytes);
    }

    // allocateImageDataFromMainRegion: ZGE2's allocateImageData, the bytes alone: always from the main region's
    // memory (ZGE2's image record has no flag for it).
    uint32_t allocateImageDataFromMainRegion(uint32_t bytes)
    {
        return allocateMemory(bytes);
    }
    READABLE_FUNCTION_OWN(allocateImageData, Verified, dm2, allocateImageDataFromMainRegion);

    namespace
    {
        // The readers' image data: ZGE2's allocateImageData is given the bytes alone.
        uint32_t readerImageData(uint32_t imageAddress, uint32_t bytes)
        {
            if constexpr (game::readable::kIsDm2)
                return allocateImageDataFromMainRegion(bytes);
            else
                return allocateImageData(imageAddress, bytes);
        }
    }

    // freeImageData: the pixels and the palette given back, each as it was taken.
    uint32_t freeImageData(uint32_t imageAddress)
    {
        Image &image = *ee::Ptr<Image>(imageAddress);
        auto give = [&](uint32_t &memory)
        {
            // (ZGE2's record has no flag: its data always comes from the main region.)
            if (game::readable::kIsDm2 || (image.flags & kImageMainMemory) != 0)
                freeMemory(memory);
            else
                heapFree(memory);
            memory = 0;
        };
        if (image.pixels != 0)
            give(image.pixels);
        if (image.palette != 0)
            give(image.palette);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeImageData, Verified);


    // initImages: the formats' chunks and readers, the records' pool.
    uint32_t initImages()
    {
        *g_imagesReady = 1;
        registerBitmapChunkHandlers();
        registerTextureChunkHandlers();
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps one static image record: no pool)
            *g_imageRecordPool = createPool(static_cast<uint32_t>(sizeof(Image)), kImageRecordsABlock, kFreeImageRecord);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initImages, Verified);


    // shutdownImageReaders.
    uint32_t shutdownImageReaders()
    {
        if (*g_imagesReady != 0)
        {
            shutdownTgaReader();
            shutdownBmpReader();
            if constexpr (game::readable::kIsDm2) // (ZGE2 has no record pool)
                *g_imagesReady = 0;
            else
            {
                const uint32_t pool = *g_imageRecordPool;
                *g_imagesReady = 0;
                destroyPool(ee::Ptr<Pool>(pool));
                *g_imageRecordPool = 0;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownImageReaders, Verified);

    // newImageRecord: a cleared record that takes its data from the main
    // region; 0 before initImages. ZGE2 hands out its one record, cleared.
    uint32_t newImageRecord()
    {
        if (*g_imagesReady == 0)
            return 0;
        if constexpr (game::readable::kIsDm2)
        {
            memoryFill(kZge2ImageRecord, 0, sizeof(Image));
            return kZge2ImageRecord;
        }
        const uint32_t record = poolAlloc(ee::Ptr<Pool>(*g_imageRecordPool));
        memoryFill(record, 0, sizeof(Image));
        ee::Ptr<Image>(record)->flags |= kImageMainMemory;
        return record;
    }
    READABLE_FUNCTION_WAITS(newImageRecord, Verified);

    // freeImageRecord (also the pool's free callback).
    uint32_t freeImageRecord(uint32_t image)
    {
        freeImageData(image);
        if constexpr (!game::readable::kIsDm2) // (ZGE2's one record stays)
            poolFree(ee::Ptr<Pool>(*g_imageRecordPool), image);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeImageRecord, Verified);

    // setImageReader.
    void setImageReader(uint32_t reader, uint32_t kind)
    {
        imageReader(kind) = ee::Ptr<ImageReader>(reader);
    }
    READABLE_FUNCTION(setImageReader, Verified);

    // findImageReader: the first reader that knows the file; kNoImageReader
    // if none does. (ZGE2 takes every kind it asks to have its reader.)
    int32_t findImageReader(uint32_t memory)
    {
        for (uint32_t kind = 0; kind < kImageReaderKinds; ++kind)
        {
            const ee::Ptr<ImageReader> reader = imageReader(kind);
            if ((game::readable::kIsDm2 || reader) && reader->isIts != 0 && callIsItsFormat(reader->isIts, memory) != 0)
                return static_cast<int32_t>(kind);
        }
        return static_cast<int32_t>(kNoImageReader);
    }
    READABLE_FUNCTION(findImageReader, Verified);

    // readImage: the file read by the kind's reader (0: whichever knows it).
    int32_t readImage(uint32_t image, uint32_t memory, uint32_t kind)
    {
        if (!game::readable::kIsDm2 && kind == 0) // (ZGE2 reads with the kind it is given)
            kind = static_cast<uint32_t>(findImageReader(memory));
        return callReadFormat(imageReader(kind)->read, image, memory, 0);
    }
    READABLE_FUNCTION_WAITS(readImage, Verified); // handed on: none: a reader's read takes the three it is given


    // setImageMipmaps (ZGE4): whether readImageChunk makes the smaller levels of the textures that ask for them
    // (readTerrChunk turns it on for a level's textures); the setting it replaces.
    inline uint32_t setImageMipmaps(uint32_t on)
    {
        const uint32_t previous = *g_imageMipmapsOn;
        *g_imageMipmapsOn = on;
        return previous;
    }
    READABLE_FUNCTION_ONLY(bmx, setImageMipmaps);

    // newHalfSizeImage (ZGE4): a new image half the width and height of a square, paletted one (4 or 8 bits a
    // pixel, even sides): the same palette, the pixels of every other row and column; 0 for any other image.
    // An image of another depth comes back with no pixels. Quirk kept: a 4-bit row is read as `width` bytes,
    // each pair of them one byte out (the upper halves of both), so it reads past the row's width / 2 bytes.
    inline uint32_t newHalfSizeImage(ee::Ptr<Image> image)
    {
        const uint32_t width = static_cast<uint32_t>(image->width);
        const uint32_t height = static_cast<uint32_t>(image->height);
        if (width != height || (height & 1u) != 0 || image->bits > 8)
            return 0;
        const ee::Ptr<Image> half(newImageRecord());
        memoryCopy(half, image, sizeof(Image));
        half->pixels = 0;
        half->pixelBytes = 0;
        half->palette = 0;
        half->width = static_cast<int32_t>(static_cast<uint32_t>(image->width) >> 1);
        half->height = static_cast<int32_t>(static_cast<uint32_t>(image->height) >> 1);
        half->rowBytes = static_cast<int32_t>(static_cast<uint32_t>(image->rowBytes) >> 1);
        half->paletteBytes = image->paletteBytes;
        half->palette = allocateImageData(ee::Address(half), static_cast<uint32_t>(half->paletteBytes));
        memoryCopy(half->palette, image->palette, static_cast<uint32_t>(half->paletteBytes));
        const uint8_t bits = image->bits;
        if (bits != 4 && bits != 8)
            return ee::Address(half);
        uint32_t pixelBytes = static_cast<uint32_t>(half->width) * static_cast<uint32_t>(half->height);
        if (bits == 4)
            pixelBytes >>= 1;
        half->pixelBytes = static_cast<int32_t>(pixelBytes);
        half->pixels = allocateImageData(ee::Address(half), pixelBytes);
        uint32_t out = half->pixels;
        uint32_t in = image->pixels;
        for (uint32_t row = 0; row < height; row += 2)
        {
            ee::Ptr<uint8_t> to(out);
            for (uint32_t column = 0; column < width; column += 2)
            {
                const ee::Ptr<uint8_t> from(in + column);
                if (bits == 4)
                    *to = static_cast<uint8_t>((from[0] >> 4) | (from[1] & 0xF0u));
                else
                    *to = from[0];
                ++to;
            }
            out += static_cast<uint32_t>(half->rowBytes);
            in += static_cast<uint32_t>(image->rowBytes) * 2u;
        }
        return ee::Address(half);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newHalfSizeImage);

    // readImageChunk: a chunk holding an image file made the texture's image. ZGE4: with the mipmaps on and
    // `mipmaps` bit 0 set (TZMP), the image halved twice more (newHalfSizeImage), each half the texture's next level.
    uint32_t readImageChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture, uint32_t kind
                            READABLE_PARAM_ONLY(bmx, uint32_t mipmaps = 0))
    {
        const uint32_t memory = allocateMemory(chunkDataSize(chunkReader));
        readChunkData(chunkReader, memory);
        uint32_t image = newImageRecord();
        readImage(image, memory, kind);
        setTextureImage(ee::Ptr<Texture>(texture), ee::Ptr<TextureImage>(image));
        if constexpr (game::readable::kIsBmx)
        {
            if (*g_imageMipmapsOn != 0 && (mipmaps & 1u) != 0)
            {
                for (uint32_t level = 0; level < kMipmapLevelsMade; ++level)
                {
                    const uint32_t half = newHalfSizeImage(ee::Ptr<Image>(image));
                    if (half == 0)
                        break;
                    setTextureImage(ee::Ptr<Texture>(texture), ee::Ptr<TextureImage>(half));
                    freeImageRecord(image);
                    image = half;
                }
            }
        }
        freeImageRecord(image);
        freeMemory(memory);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readImageChunk, Verified);

    // setImagePaletteEntry.
    void setImagePaletteEntry(uint32_t image, uint32_t index, ee::Address colour)
    {
        const uint32_t entry = ee::Ptr<Image>(image)->palette + index * sizeof(uint32_t);
        if constexpr (game::readable::kIsBmx) // (ZGE4's ends in the copy, as a tail call)
            game::tail(memoryCopy, entry, colour, sizeof(uint32_t));
        else
            memoryCopy(entry, colour, sizeof(uint32_t));
    }
    READABLE_FUNCTION(setImagePaletteEntry, Verified);

    // imagePaletteEntry: the image's palette entry `index` copied to colour.
    void imagePaletteEntry(ee::Ptr<Image> image, uint32_t index, ee::Address colour)
    {
        const uint32_t entry = image->palette + index * sizeof(uint32_t);
        if constexpr (game::readable::kIsBmx) // (ZGE4's ends in the copy, as a tail call)
            game::tail(memoryCopy, colour, entry, sizeof(uint32_t));
        else
            memoryCopy(colour, entry, sizeof(uint32_t));
    }
    READABLE_FUNCTION(imagePaletteEntry, Draft);

    // readBmpImageKeepingAlpha: reader kind 3, BMP keeping the palette's alpha.
    int32_t readBmpImageKeepingAlpha(uint32_t image, uint32_t memory)
    {
        return static_cast<int32_t>(readBmpImage(image, memory, kBmpKeepPaletteAlpha));
    }
    READABLE_FUNCTION_WAITS(readBmpImageKeepingAlpha, Verified);

    // readBmpImage: a BMP: its palette (BGRA in the file) and its rows, bottom
    // up in the file, top down here; 4-bit pixels swapped in their bytes, 24-bit
    // to RGB, rows without the file's padding. Other depths get the
    // room and no pixels. options: BmpOptions.
    uint32_t readBmpImage(uint32_t imageAddress, uint32_t memory, uint32_t options)
    {
        ee::Local<uint8_t[0x10]> fileHeader;
        ee::Local<BmpInfo> info;
        Image &image = *ee::Ptr<Image>(imageAddress);
        if constexpr (game::readable::kIsDm2) // ZGE2 clears the record first
            memoryFill(imageAddress, 0, sizeof(Image));
        memoryCopy(fileHeader, memory, kBmpFileHeaderBytes);
        const uint32_t infoInFile = memory + kBmpFileHeaderBytes;
        memoryCopy(info, infoInFile, sizeof(BmpInfo));
        uint32_t read = infoInFile + info->headerBytes;
        const int32_t height = info->height;
        image.bits = static_cast<uint8_t>(info->bits);
        image.width = info->width;
        image.height = height;

        int32_t colours = info->coloursUsed;
        if (colours == 0)
        {
            if (info->bits == 4)
                colours = 16;
            else if (info->bits == 8)
                colours = 256;
        }
        if (colours != 0)
        {
            const int32_t paletteBytes = colours * static_cast<int32_t>(sizeof(uint32_t));
            if (options & kBmpSkipPalette)
                read += paletteBytes;
            else
            {
                image.paletteBytes = paletteBytes;
                const uint32_t palette = readerImageData(imageAddress, paletteBytes);
                image.paletteCount = static_cast<uint16_t>(colours);
                image.palette = palette;
                if (colours > 0)
                    readBmpPalette(image, palette, read, colours, options);
            }
        }

        const int32_t rowBytes = static_cast<int32_t>(static_cast<uint32_t>(info->width) * static_cast<uint32_t>(info->bits)) / 8;
        const int32_t pixelBytes = static_cast<int32_t>(static_cast<uint32_t>(rowBytes) * static_cast<uint32_t>(height));
        image.pixelBytes = pixelBytes;
        image.pixels = readerImageData(imageAddress, pixelBytes);
        image.rowBytes = rowBytes;
        // Rows are 4-byte aligned in the file, the bottom row first.
        const int32_t paddedRow = (rowBytes + 3) / 4 * 4;
        const auto rowAt = [&](int32_t y) { return ee::Ptr<uint8_t>(image.pixels + static_cast<uint32_t>(y) * static_cast<uint32_t>(rowBytes)); };

        if (info->bits == 8)
        {
            for (int32_t y = height - 1; y >= 0; --y, read += paddedRow)
                memoryCopy(rowAt(y), read, rowBytes);
        }
        else if (info->bits == 4)
        {
            // Two pixels a byte: the file has the left one in the high half.
            for (int32_t y = height - 1; y >= 0; --y, read += paddedRow)
            {
                const ee::Ptr<const uint8_t> in(read);
                const ee::Ptr<uint8_t> out = rowAt(y);
                for (int32_t x = 0; x < rowBytes; ++x)
                    out[x] = static_cast<uint8_t>((in[x] & 0xFu) << 4 | in[x] >> 4);
            }
        }
        else if (info->bits == 24)
        {
            // BGR in the file, RGB here.
            const int32_t padding = paddedRow - rowBytes;
            for (int32_t y = height - 1; y >= 0; --y, read += padding)
            {
                ee::Ptr<uint8_t> out = rowAt(y);
                int32_t left = rowBytes;
                do
                {
                    const ee::Ptr<const uint8_t> in(read);
                    out[0] = in[2];
                    out[1] = in[1];
                    out[2] = in[0];
                    read += 3;
                    out += 3;
                    left -= 3;
                } while (left != 0);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readBmpImage, Verified);

    // isBmpImage.
    int32_t isBmpImage(uint32_t memory)
    {
        return *ee::Ptr<uint16_t>(memory) == kBmpSignature ? 1 : 0;
    }
    READABLE_FUNCTION(isBmpImage, Verified);

    // readTbmpChunk, readTzmpChunk, readTtgaChunk: the chunks by the kind they
    // are read as (the handler's data holds the texture).
    int32_t readTbmpChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture)
    {
        return static_cast<int32_t>(readImageChunk(chunkReader, *ee::Ptr<uint32_t>(texture), kImageKindBmp READABLE_PARAM_ONLY(bmx, 0u)));
    }
    READABLE_FUNCTION_WAITS(readTbmpChunk, Verified);

    int32_t readTzmpChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture)
    {
        return static_cast<int32_t>(readImageChunk(chunkReader, *ee::Ptr<uint32_t>(texture), kImageKindBmpAlpha READABLE_PARAM_ONLY(bmx, kImageMipmaps)));
    }
    READABLE_FUNCTION_WAITS(readTzmpChunk, Verified);

    // registerBitmapChunkHandlers.
    void registerBitmapChunkHandlers()
    {
        *g_bmpReaderReady = 1;
        setImageReader(kBmpReader, kImageKindBmp);
        setImageReader(kBmpAlphaReader, kImageKindBmpAlpha);
        addChunkHandler(kChunkTbmp, kReadTbmpChunk);
        addChunkHandler(kChunkTzmp, kReadTzmpChunk);
    }
    READABLE_FUNCTION_WAITS(registerBitmapChunkHandlers, Verified);

    // shutdownBmpReader.
    void shutdownBmpReader()
    {
        if (*g_bmpReaderReady == 0)
            return;
        *g_bmpReaderReady = 0;
    }
    READABLE_FUNCTION(shutdownBmpReader, Verified);

    // readTgaImage: a TGA without RLE: the 18-byte header copied field by
    // field to the frame; the pixels as they are (top left origin) or the
    // rows reversed (bottom left); other origins get the room and no pixels.
    namespace
    {
        // A TGA file's 18-byte header. Packed in the file; each field is
        // copied out to its place here, one at a time (unpackTgaHeader).
        struct TgaHeader
        {
            uint8_t idLength;         // +0x00 bytes of text between the header and the pixels
            uint8_t colourMapType;    // +0x01
            uint8_t imageType;        // +0x02
            uint8_t gap03[0x0D];
            uint16_t colourMapFirst;  // +0x10
            uint16_t colourMapLength; // +0x12
            uint8_t colourMapBits;    // +0x14
            uint8_t gap15[0x0B];
            uint16_t xOrigin;         // +0x20
            uint16_t yOrigin;         // +0x22
            uint16_t width;           // +0x24
            uint16_t height;          // +0x26
            uint8_t bits;             // +0x28 bits a pixel
            uint8_t descriptor;       // +0x29 kTgaOriginMask: which corner the file's first pixel is
        };
        static_assert(offsetof(TgaHeader, width) == 0x24 && offsetof(TgaHeader, descriptor) == 0x29);
        constexpr uint32_t kTgaFileHeaderBytes = 18u;
        constexpr uint32_t kTgaOriginMask = 0x30u;
        constexpr uint32_t kTgaOriginBottomLeft = 0x00u;
        constexpr uint32_t kTgaOriginTopLeft = 0x20u;

        // The fields in the file's order.
        constexpr struct
        {
            uint8_t offset, bytes;
        } kTgaField[] = {
            {offsetof(TgaHeader, idLength), 1}, {offsetof(TgaHeader, colourMapType), 1}, {offsetof(TgaHeader, imageType), 1},
            {offsetof(TgaHeader, colourMapFirst), 2}, {offsetof(TgaHeader, colourMapLength), 2}, {offsetof(TgaHeader, colourMapBits), 1},
            {offsetof(TgaHeader, xOrigin), 2}, {offsetof(TgaHeader, yOrigin), 2}, {offsetof(TgaHeader, width), 2},
            {offsetof(TgaHeader, height), 2}, {offsetof(TgaHeader, bits), 1}, {offsetof(TgaHeader, descriptor), 1},
        };
        constexpr uint32_t kTgaFields = sizeof(kTgaField) / sizeof(kTgaField[0]);

        void unpackTgaHeader(ee::Ptr<TgaHeader> header, uint32_t file, uint32_t fields)
        {
            for (uint32_t i = 0; i < fields; ++i)
            {
                memoryCopy(header.cast<uint8_t>() + kTgaField[i].offset, file, kTgaField[i].bytes);
                file += kTgaField[i].bytes;
            }
        }
    }

    uint32_t readTgaImage(uint32_t imageAddress, uint32_t memory)
    {
        ee::Local<TgaHeader> header;
        Image &image = *ee::Ptr<Image>(imageAddress);
        if constexpr (game::readable::kIsDm2) // ZGE2 clears the record first
            memoryFill(imageAddress, 0, sizeof(Image));
        unpackTgaHeader(header, memory, kTgaFields);
        const uint32_t pixelsInFile = memory + kTgaFileHeaderBytes + header->idLength;
        const uint32_t width = header->width;
        const uint32_t height = header->height;
        const uint32_t bits = header->bits;
        image.bits = static_cast<uint8_t>(bits);
        image.width = static_cast<int32_t>(width);
        image.height = static_cast<int32_t>(height);
        image.rowBytes = static_cast<int32_t>(width * (bits >> 3));
        image.pixelBytes = static_cast<int32_t>(width * height * (bits >> 3));
        image.pixels = readerImageData(imageAddress, image.pixelBytes);

        const uint32_t origin = header->descriptor & kTgaOriginMask;
        if (origin == kTgaOriginTopLeft)
            memoryCopy(image.pixels, pixelsInFile, image.pixelBytes);
        else if (origin == kTgaOriginBottomLeft)
        {
            const uint32_t rowBytes = static_cast<uint32_t>(image.width) * (image.bits >> 3);
            for (uint32_t y = 0; y < height; ++y)
                memoryCopy(image.pixels + y * rowBytes, (height - y - 1) * rowBytes + pixelsInFile, rowBytes);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTgaImage, Verified);

    // isTgaImage: whether the file looks like a TGA: 16, 24 or 32 bits and
    // both sides powers of two (there is no signature).
    uint32_t isTgaImage(uint32_t memory)
    {
        ee::Local<TgaHeader> header;
        if (*ee::Ptr<uint16_t>(memory) == kBmpSignature)
            return 0;
        // The descriptor is not needed here.
        unpackTgaHeader(header, memory, kTgaFields - 1);
        const uint32_t bits = header->bits;
        if (bits != 16 && bits != 24 && bits != 32)
            return 0;
        const uint32_t width = header->width;
        if ((width & (width - 1u)) != 0)
            return 0;
        const uint32_t height = header->height;
        return (height & (height - 1u)) == 0;
    }
    READABLE_FUNCTION(isTgaImage, Verified);

    int32_t readTtgaChunk(ee::Ptr<ChunkReader> chunkReader, uint32_t texture)
    {
        return static_cast<int32_t>(readImageChunk(chunkReader, *ee::Ptr<uint32_t>(texture), kImageKindTga READABLE_PARAM_ONLY(bmx, 0u)));
    }
    READABLE_FUNCTION_WAITS(readTtgaChunk, Draft);

    namespace
    {
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
        inline ee::Global<int32_t, sym::g_tgaReaderReady> g_tgaReaderReady;
    }

    namespace
    {
        namespace routed
        {
        }
    }


    uint32_t registerTextureChunkHandlers()
    {
        *g_tgaReaderReady = 1;
        setImageReader(sym::g_imageReaderMethods, kImageKindTga);
        addChunkHandler(kChunkTtga, sym::readTtgaChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerTextureChunkHandlers, Verified);

    uint32_t shutdownTgaReader()
    {
        if (*g_tgaReaderReady == 0)
            return 1;
        *g_tgaReaderReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownTgaReader, Verified);


    // (Rgba8, a pixel or palette entry, is in images.h.)

    // (ColourBox, the median cut's box, is in images.h: ZGE4's has more.)

    namespace
    {
        constexpr uint32_t kChannels = 4;
        constexpr uint32_t kSmallBoxError = 0x80; // a box this close to its mean is not split
        constexpr uint32_t kMaxSplitDepth = 10;

        uint8_t channel(const Rgba8 &colour, uint32_t index)
        {
            const uint8_t channels[kChannels] = {colour.red, colour.green, colour.blue, colour.alpha};
            return channels[index];
        }

        bool sameColour(const Rgba8 &a, const Rgba8 &b)
        {
            return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
        }
    }

    ee::Ptr<ColourBox> splitColourBox(ee::Ptr<ColourBox> parent, ee::Ptr<Rgba8> pixels, uint32_t count, uint32_t depth, ee::Ptr<ee::Ptr<ColourBox>> cursor, ee::Ptr<ColourBox> end
                                      READABLE_PARAM_ONLY(bmx, uint8_t half));


    // splitColourBox: the median cut. The next box from *cursor (none when
    // there are no pixels or the boxes run out at `end`: 0) takes the pixels.
    // One close to its mean (error up to 128) or at depth 10 keeps their count
    // and sums; any other is split at the middle of its widest channel (the
    // first of equals: red, green, blue, alpha), the pixels above the middle
    // to the back, and each half made a box under it. Returns the box.
    // ZGE4 also links the box into its parent as the half it is (`half`: 0 the
    // pixels at or below the middle, 1 those above), and keeps where it split.
    // Kept bug: the partition carries a pixel in hand, starting with a copy of
    // the first while the first slot still holds it, and the last one in hand
    // is never put back: the first pixel ends up twice and one pixel is lost.
    ee::Ptr<ColourBox> splitColourBox(ee::Ptr<ColourBox> parent, ee::Ptr<Rgba8> pixels, uint32_t count, uint32_t depth, ee::Ptr<ee::Ptr<ColourBox>> cursor, ee::Ptr<ColourBox> end
                                      READABLE_PARAM_ONLY(bmx, uint8_t half = 0))
    {
        if (count == 0 || same(*cursor, end))
            return ee::Ptr<ColourBox>();
        const ee::Ptr<ColourBox> box = *cursor;
        *cursor = box + 1;
        box->parent = parent;
        if constexpr (game::readable::kIsBmx)
            if (parent)
                setHalf(parent, half, box);

        uint32_t sums[kChannels] = {};
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t c = 0; c < kChannels; ++c)
                sums[c] += channel(pixels[i], c);
        uint8_t mean[kChannels];
        for (uint32_t c = 0; c < kChannels; ++c)
            mean[c] = static_cast<uint8_t>(sums[c] / count);
        for (uint32_t i = 0; i < count; ++i)
        {
            int32_t distance[kChannels];
            for (uint32_t c = 0; c < kChannels; ++c)
                distance[c] = mean[c] - channel(pixels[i], c);
            box->error += static_cast<uint32_t>(distance[0] * distance[0] + distance[1] * distance[1] + distance[2] * distance[2] + distance[3] * distance[3]);
        }

        if (box->error > kSmallBoxError && depth < kMaxSplitDepth)
        {
            uint8_t lowest[kChannels] = {255, 255, 255, 255}, highest[kChannels] = {};
            for (uint32_t i = 0; i < count; ++i)
            {
                for (uint32_t c = 0; c < kChannels; ++c)
                {
                    const uint8_t value = channel(pixels[i], c);
                    if (value < lowest[c])
                        lowest[c] = value;
                    if (highest[c] < value)
                        highest[c] = value;
                }
            }
            uint8_t range[kChannels];
            for (uint32_t c = 0; c < kChannels; ++c)
                range[c] = static_cast<uint8_t>(highest[c] - lowest[c]);
            uint32_t axis = kChannels - 1;
            for (uint32_t c = 0; c < kChannels - 1; ++c)
            {
                if (range[c] >= range[0] && range[c] >= range[1] && range[c] >= range[2] && range[c] >= range[3])
                {
                    axis = c;
                    break;
                }
            }
            const uint8_t middle = static_cast<uint8_t>(lowest[axis] + (range[axis] >> 1));
            if constexpr (game::readable::kIsBmx)
                keepSplit(box, static_cast<uint8_t>(axis), middle);

            Rgba8 inHand = pixels[0];
            uint32_t low = 0, high = count - 1;
            while (low < high)
            {
                if (middle < channel(inHand, axis))
                {
                    const Rgba8 taken = pixels[high];
                    pixels[high] = inHand;
                    inHand = taken;
                    --high;
                }
                else
                {
                    const Rgba8 taken = pixels[low];
                    pixels[low] = inHand;
                    inHand = taken;
                    ++low;
                }
            }

            if (splitColourBox(box, pixels, low, depth + 1, cursor, end READABLE_PARAM_ONLY(bmx, 0)))
                ++box->children;
            if (splitColourBox(box, pixels + low, count - low, depth + 1, cursor, end READABLE_PARAM_ONLY(bmx, 1)))
                ++box->children;
            return box;
        }

        box->pixelCount = count;
        box->redSum = sums[0];
        box->greenSum = sums[1];
        box->blueSum = sums[2];
        box->alphaSum = sums[3];
        return box;
    }
    READABLE_FUNCTION(splitColourBox, Draft);

    // The palette entries the median cut's leaves fill: ZGE4 keeps entry 0
    // for a clear pixel (all four channels 0) and starts at entry 1.
    constexpr uint32_t kFirstLeafEntry = game::readable::perGame(0u, 0u, 1u);

    // paletteFromColourBoxes: the median cut's boxes (the first `boxCount` of
    // them) brought down to at most `colours`, and the palette made of them.
    // The leaves (boxes with a parent and nothing split from them) are listed;
    // then, round by round, every leaf whose error is not above the bar goes
    // back into its parent (its count and sums added in), the parent becoming
    // a leaf in its place once its last child is in, and the bar rises to the
    // smallest error left above it, until no more than `colours` are left
    // (ZGE4: no more than the entries after entry 0).
    // Each leaf's average colour is a palette entry, in list order; the
    // palette's other entries are 0. ZGE4 also takes the merged box out of its
    // parent's halves and gives each leaf the number of its entry.
    void paletteFromColourBoxes(ee::Ptr<ColourBox> boxes, int32_t boxCount, uint32_t colours, ee::Ptr<Rgba8> palette)
    {
        const auto isLeaf = [](const ColourBox &box) { return box.parent && box.children == 0; };
        int32_t leafCount = 0;
        for (int32_t i = 0; i < boxCount; ++i)
            if (isLeaf(boxes[i]))
                ++leafCount;
        const uint32_t listBytes = static_cast<uint32_t>(leafCount) * sizeof(uint32_t);
        const uint32_t list = allocateMemory(listBytes);
        memoryFill(list, 0, listBytes);
        const ee::Ptr<ee::Ptr<ColourBox>> leaves(list);
        int32_t listed = 0;
        for (int32_t i = 0; i < boxCount; ++i)
            if (isLeaf(boxes[i]))
                leaves[listed++] = boxes + i;

        uint32_t bar = 0;
        uint32_t left;
        do
        {
            left = 0;
            uint32_t nextBar = 0xFFFFFFFFu;
            for (int32_t i = 0; i < listed; ++i)
            {
                const ee::Ptr<ColourBox> leaf = leaves[i];
                if (!leaf)
                    continue;
                if (leaf->error > bar)
                {
                    ++left;
                    if (leaf->error < nextBar)
                        nextBar = leaf->error;
                    continue;
                }
                const ee::Ptr<ColourBox> parent = leaf->parent;
                parent->pixelCount += leaf->pixelCount;
                parent->redSum += leaf->redSum;
                parent->greenSum += leaf->greenSum;
                parent->blueSum += leaf->blueSum;
                parent->alphaSum += leaf->alphaSum;
                if constexpr (game::readable::kIsBmx)
                    unlinkHalf(parent, leaf);
                if (--parent->children == 0)
                {
                    leaves[i] = parent; // and looked at again
                    --i;
                }
                else
                    leaves[i] = ee::Ptr<ColourBox>();
            }
            bar = nextBar;
        } while (colours - kFirstLeafEntry < left);

        memoryFill(palette, 0, colours * sizeof(Rgba8));
        if constexpr (game::readable::kIsBmx)
            palette[0] = Rgba8{}; // a clear pixel's
        uint32_t entry = kFirstLeafEntry;
        for (int32_t i = 0; i < listed; ++i)
        {
            const ee::Ptr<ColourBox> leaf = leaves[i];
            if (!leaf)
                continue;
            Rgba8 &colour = palette[entry];
            colour.red = static_cast<uint8_t>(leaf->redSum / leaf->pixelCount);
            colour.green = static_cast<uint8_t>(leaf->greenSum / leaf->pixelCount);
            colour.blue = static_cast<uint8_t>(leaf->blueSum / leaf->pixelCount);
            colour.alpha = static_cast<uint8_t>(leaf->alphaSum / leaf->pixelCount);
            if constexpr (game::readable::kIsBmx)
                setPaletteEntry(leaf, entry);
            ++entry;
        }
        game::tail(freeMemory, list);
    }
    READABLE_FUNCTION_WAITS(paletteFromColourBoxes, Draft);

    // texelsFromColourBoxes (ZGE4 only; quantizeImage's last step): each
    // pixel's palette entry, found by walking down the median cut's boxes from
    // the first: at each split the half the pixel's channel puts it in (above
    // the middle: the second), the other one where that half was merged away,
    // down to a box with nothing split from it, whose entry it takes. A clear
    // pixel (all four channels 0) takes entry 0. A run of equal pixels takes
    // the first one's entry. (`colours` and `palette`, quantizeImage's, are
    // passed and not read.)
    inline void texelsFromColourBoxes(ee::Ptr<const Rgba8> pixels, uint32_t width, uint32_t height, [[maybe_unused]] uint32_t colours,
                                      ee::Ptr<uint8_t> texels, [[maybe_unused]] ee::Ptr<Rgba8> palette, ee::Ptr<ColourBox> boxes)
    {
        const uint32_t count = width * height;
        uint32_t p = 0;
        while (p < count)
        {
            const Rgba8 &pixel = pixels[p];
            int32_t entry = 0;
            if (!sameColour(pixel, Rgba8{}))
            {
                ee::Ptr<ColourBox> box = boxes;
                while (box->children != 0)
                {
                    const uint32_t half = splitMiddleOf(box) < channel(pixel, splitChannelOf(box)) ? 1 : 0;
                    ee::Ptr<ColourBox> next = halfOf(box, half);
                    if (!next)
                        next = halfOf(box, 1 - half);
                    if (!next)
                        break;
                    box = next;
                }
                entry = paletteEntryOf(box);
            }
            uint32_t run = 1;
            while (p + run < count && sameColour(pixels[p + run], pixel))
                ++run;
            for (uint32_t i = 0; i < run; ++i)
                texels[p + i] = static_cast<uint8_t>(entry);
            p += run;
        }
    }
    READABLE_FUNCTION_ONLY(bmx, texelsFromColourBoxes, Draft);

    // The boxes quantizeImage hands the median cut (ZGE4: fewer, each bigger).
    constexpr uint32_t kColourBoxCount = game::readable::perGame(0x1000u, 0x1000u, 0x800u);
    constexpr uint32_t kColourBoxBytes = kColourBoxCount * sizeof(ColourBox);

    // quantizeImage: a true-colour picture (width x height RGBA pixels) as
    // texels of up to `colours` palette entries: the palette by median cut
    // (splitColourBox over a copy of the pixels, then paletteFromColourBoxes),
    // each texel the entry nearest its pixel (the sum of the four channels'
    // squared differences; the first of equals). ZGE4 starts each box as the
    // blank box (no entry yet), takes the copy from the heap, and finds each
    // texel's entry by walking the boxes (texelsFromColourBoxes) before it
    // frees them.
    void quantizeImage(ee::Ptr<const Rgba8> pixels, uint32_t width, uint32_t height, uint32_t colours, ee::Ptr<uint8_t> texels, ee::Ptr<Rgba8> palette)
    {
        ee::Local<ee::Ptr<ColourBox>> boxCursor;
        const uint32_t count = width * height;
        const uint32_t boxes = allocateMemory(kColourBoxBytes);
        if constexpr (game::readable::kIsBmx)
        {
            for (uint32_t box = boxes; box < boxes + kColourBoxBytes; box += sizeof(ColourBox))
                memoryCopy(box, sym::bmx::g_blankColourBox, sizeof(ColourBox));
        }
        else
            memoryFill(boxes, 0, kColourBoxBytes);
        const uint32_t pixelBytes = count * sizeof(Rgba8);
        const uint32_t copy = game::readable::kIsBmx ? heapAllocate(pixelBytes) : allocateMemory(pixelBytes);
        memoryCopy(copy, pixels, pixelBytes);
        *boxCursor = ee::Ptr<ColourBox>(boxes);
        splitColourBox(ee::Ptr<ColourBox>(), ee::Ptr<Rgba8>(copy), count, 1, boxCursor, ee::Ptr<ColourBox>(boxes + kColourBoxBytes) READABLE_PARAM_ONLY(bmx, 0));
        if constexpr (game::readable::kIsBmx)
            heapFree(copy);
        else
            freeMemory(copy);
        paletteFromColourBoxes(ee::Ptr<ColourBox>(boxes), kColourBoxCount, colours, palette);
        if constexpr (game::readable::kIsBmx)
        {
            texelsFromColourBoxes(pixels, width, height, colours, texels, palette, ee::Ptr<ColourBox>(boxes));
            freeMemory(boxes);
            return;
        }
        freeMemory(boxes);
        for (uint32_t p = 0; p < count; ++p)
        {
            const Rgba8 &pixel = pixels[p];
            uint32_t nearest = 0;
            uint32_t nearestDistance = 0xFFFFFFFFu;
            for (uint32_t entry = 0; entry < colours; ++entry)
            {
                const Rgba8 &colour = palette[entry];
                const int32_t red = colour.red - pixel.red, green = colour.green - pixel.green;
                const int32_t blue = colour.blue - pixel.blue, alpha = colour.alpha - pixel.alpha;
                const uint32_t distance = static_cast<uint32_t>(red * red + alpha * alpha + (blue * blue + green * green));
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearest = entry;
                }
            }
            texels[p] = static_cast<uint8_t>(nearest);
        }
    }
    READABLE_FUNCTION_WAITS(quantizeImage, Draft);

    // newTextureFromPixels: a procedural texture of width x height RGBA
    // pixels: kept as 32-bit texels, or with `quantize` as 8-bit ones of a
    // 256-entry palette (quantizeImage). The image record is freed after.
    int32_t newTextureFromPixels(ee::Ptr<const Rgba8> pixels, uint32_t width, uint32_t height, uint32_t quantize)
    {
        const uint32_t imageAddress = newImageRecord();
        Image &image = *ee::Ptr<Image>(imageAddress);
        image.width = static_cast<int32_t>(width);
        image.height = static_cast<int32_t>(height);
        if constexpr (game::readable::kIsBmx) // ZGE4 takes pixels as having alpha
            image.flags |= kImageHasAlpha;
        if (quantize)
        {
            const uint32_t count = width * height;
            image.rowBytes = static_cast<int32_t>(width);
            image.pixelBytes = static_cast<int32_t>(count);
            image.pixels = allocateImageData(imageAddress, count);
            image.bits = 8;
            image.paletteCount = 0x100;
            image.palette = allocateImageData(imageAddress, 0x100 * sizeof(Rgba8));
            quantizeImage(pixels, width, height, 0x100, ee::Ptr<uint8_t>(image.pixels), ee::Ptr<Rgba8>(image.palette));
        }
        else
        {
            image.rowBytes = static_cast<int32_t>(width * sizeof(Rgba8));
            image.pixelBytes = static_cast<int32_t>(width * height * sizeof(Rgba8));
            const uint32_t data = allocateImageData(imageAddress, static_cast<uint32_t>(image.pixelBytes));
            image.bits = 32;
            image.pixels = data;
            memoryCopy(data, pixels, static_cast<uint32_t>(image.pixelBytes));
        }
        const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(imageAddress));
        freeImageRecord(imageAddress);
        return static_cast<int32_t>(ee::Address(texture));
    }
    READABLE_FUNCTION_WAITS(newTextureFromPixels, Draft);

    // expandPalettedImage: the image as 32-bit RGBA pixels at `out` (width x
    // height, packed). 4- and 8-bit pixels go through the palette; 16-bit ones
    // are split 5:6:5 into red, green and blue as they are (not scaled up),
    // alpha 255; 24-bit ones are copied with alpha 255, magenta (255, 0, 255)
    // alpha 0; 32-bit rows are copied. Other depths write nothing.
    // Kept: ZGE3's 4-bit case steps a byte after every pixel and one more after
    // each odd one, so it reads byte 0's low half, byte 1's high half, byte 3's
    // low half and so on, not two pixels a byte (ZGE4 reads two a byte).
    // ZGE4's 8-bit case looks a run of one index up once and copies it along,
    // and copies a run from the longest one so far when that had its index.
    void expandPalettedImage(ee::Ptr<Image> image, ee::Ptr<Rgba8> out)
    {
        const uint32_t width = static_cast<uint32_t>(image->width);
        const uint32_t height = static_cast<uint32_t>(image->height);
        uint32_t row = image->pixels;
        switch (image->bits)
        {
        case 4:
            for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
            {
                ee::Ptr<uint8_t> source(row);
                for (uint32_t x = 0; x < width; ++x)
                {
                    uint32_t index;
                    if (x & 1u)
                    {
                        index = *source >> 4;
                        ++source;
                    }
                    else
                        index = *source & 0xFu;
                    imagePaletteEntry(image, index, out);
                    if constexpr (!game::readable::kIsBmx)
                        ++source;
                    ++out;
                }
            }
            break;
        case 8:
            if constexpr (game::readable::kIsBmx)
            {
                ee::Ptr<Rgba8> longest; // the longest run looked up so far, its index and length
                uint32_t longestIndex = 0, longestRun = 1;
                for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
                {
                    ee::Ptr<uint8_t> source(row);
                    for (uint32_t x = 0; x < width;)
                    {
                        uint32_t run = 1;
                        while (x + run < width && source[run] == source[0])
                            ++run;
                        const bool longer = longestRun < run;
                        if (!longer && longest && *source == longestIndex)
                            memoryCopy(out, longest, run * static_cast<uint32_t>(sizeof(Rgba8)));
                        else
                        {
                            imagePaletteEntry(image, *source, out);
                            for (uint32_t i = 0; i < run; ++i)
                                out[i] = out[0];
                        }
                        if (longer)
                        {
                            longest = out;
                            longestRun = run;
                            longestIndex = *source;
                        }
                        source += run;
                        out += run;
                        x += run;
                    }
                }
                break;
            }
            for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
            {
                ee::Ptr<uint8_t> source(row);
                for (uint32_t x = 0; x < width; ++x, ++source, ++out)
                    imagePaletteEntry(image, *source, out);
            }
            break;
        case 16:
            for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
            {
                ee::Ptr<uint16_t> source(row);
                for (uint32_t x = 0; x < width; ++x, ++source, ++out)
                {
                    const uint16_t pixel = *source;
                    out->alpha = 255;
                    out->red = static_cast<uint8_t>(pixel >> 11);
                    out->green = static_cast<uint8_t>((pixel >> 5) & 0x3Fu);
                    out->blue = static_cast<uint8_t>(pixel & 0x1Fu);
                }
            }
            break;
        case 24:
            for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
            {
                ee::Ptr<uint8_t> source(row);
                for (uint32_t x = 0; x < width; ++x, source += 3, ++out)
                {
                    const uint8_t red = source[0], green = source[1], blue = source[2];
                    out->red = red;
                    out->green = green;
                    out->blue = blue;
                    out->alpha = red == 255 && green == 0 && blue == 255 ? 0 : 255;
                }
            }
            break;
        case 32:
            for (uint32_t y = 0; y < height; ++y, row += image->rowBytes)
            {
                memoryCopy(out, row, width * sizeof(Rgba8));
                out += width;
            }
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(expandPalettedImage, Draft);

}
