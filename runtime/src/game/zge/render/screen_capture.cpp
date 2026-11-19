// Screen captures and raw images (the initScreenCapture-initScreenOverlay part of the frame
// block; debug tools no code of the game's calls but their set-up flag and
// the view hook): the frame buffers read back from the GS and written to the
// host as raw RGB files, and a raw image file shown on the screen.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/render/gs_registers.h"
#include "zge/text/cstring.h"
#include "zge/memory/regions.h"

#include <cstdint>
#include "sym/zge/render/screen_capture.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_screenCaptureReady> g_screenCaptureReady;
        // The number the next capture's file names get.
        inline ee::Global<int32_t, sym::g_screenCaptureNumber> g_screenCaptureNumber;

        constexpr uint32_t kResetDisplayMode = sym::resetDisplayMode;    // (width, height, 0, 0, 0, 0, 1)
        constexpr uint32_t kSceGsSyncV = sym::sceGsSyncV;          // sceGsSyncV(0)
        constexpr uint32_t kSceGsSyncPath = sym::sceGsSyncPath;       // sceGsSyncPath(0, 0)
        constexpr uint32_t kSceGsSetDefLoadImage = sym::sceGsSetDefLoadImage; // (image, dbp, dbw, dpsm, x, y, w, h)
        constexpr uint32_t kSceGsSetDefStoreImage = sym::sceGsSetDefStoreImage; // (image, sbp, sbw, spsm, x, y, w, h)
        constexpr uint32_t kSceGsExecLoadImage = sym::sceGsExecLoadImage;  // (image, source)
        constexpr uint32_t kSceGsExecStoreImage = sym::sceGsExecStoreImage; // (image, destination)
        constexpr uint32_t kFlushCache = sym::flushCache;          // FlushCache(0)
        constexpr uint32_t kSceOpen = sym::sceOpen;
        constexpr uint32_t kSceClose = sym::sceClose;
        constexpr uint32_t kSceWrite = sym::sceWrite;

        constexpr uint32_t kOpenForWriting = 0x602; // SCE_WRONLY | SCE_CREAT | SCE_TRUNC
        // The two captures' names ("host:image0%d.raw", "host:image1%d.raw"),
        // the failed open's message ("Failed to open:%s\n").
        constexpr uint32_t kCaptureName0 = sym::captureName0;
        constexpr uint32_t kCaptureName1 = sym::captureName1;
        constexpr uint32_t kFailedToOpen = sym::failedToOpen;
        // libgraph's image transfer records (sceGsStoreImage, sceGsLoadImage).
        constexpr uint32_t kStoreImage = sym::storeImage;
        constexpr uint32_t kLoadImage = sym::loadImage;

        int16_t low16(int32_t value) { return static_cast<int16_t>(value); }
    }

    // initScreenCapture, shutdownScreenCapture (no caller): the capture's set-up flag set
    // and cleared. Always 1.
    uint32_t initScreenCapture()
    {
        *g_screenCaptureReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initScreenCapture, Draft);

    uint32_t shutdownScreenCapture()
    {
        if (*g_screenCaptureReady != 0)
            *g_screenCaptureReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownScreenCapture, Draft);

    // endViewOverlay (beginViewPacket's second view hook): nothing in this
    // build. Always 1.
    uint32_t endViewOverlay() { return 1; }
    READABLE_FUNCTION(endViewOverlay, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t sceOpen(ee::Ptr<uint8_t> name, uint32_t openForWriting) { return ee::game<int32_t>(kSceOpen, name, openForWriting); }
        void sceGsSetDefStoreImage(uint32_t image, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { ee::game(kSceGsSetDefStoreImage, image, sbp, sbw, spsm, x, y, w, h); }
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
        void sceGsExecStoreImage(uint32_t image, uint32_t destination) { ee::game(kSceGsExecStoreImage, image, destination); }
        void sceGsSyncPath(uint32_t value, uint32_t value2) { ee::game(kSceGsSyncPath, value, value2); }
        void sceWrite(uint32_t descriptor, uint32_t data, uint32_t bytes) { ee::game(kSceWrite, descriptor, data, bytes); }
        void sceClose(uint32_t openForWritingSlot) { ee::game(kSceClose, openForWritingSlot); }
    }

    // captureScreenToHost (no caller): two more frames drawn, then both frame
    // buffers read back (24-bit) and written to host:image0N.raw and
    // host:image1N.raw, a row of each buffer in turn (the second's first in
    // image0, the first's in image1: the two fields of an interlaced frame
    // [inferred]); N counted on when `advance`. Always 1.
    uint32_t captureScreenToHost(uint32_t advance)
    {
        ee::Local<uint8_t[0x80]> name;
        ee::Local<int32_t> buffer0Base;
        ee::Local<int32_t> buffer1Base;
        ee::Local<int32_t> framesPending;
        DisplaySetup &display = *g_display;
        *framesPending = display.framesPending;
        const int32_t width = ee::toInt(display.bufferWidth);
        const int32_t height = ee::toInt(display.bufferHeight);
        // Each draw buffer's base (FRAME's FBP, in pages).
        *buffer0Base = static_cast<int32_t>(gs::frameBasePage(display.buffers.draw[0].draw1.frame.value));
        *buffer1Base = static_cast<int32_t>(gs::frameBasePage(display.buffers.draw[1].draw1.frame.value));
        display.framesPending = 0;
        renderCountedFrame();
        renderCountedFrame();

        formatString(name, kCaptureName0, *g_screenCaptureNumber);
        const int32_t image0File = sceOpen(name, kOpenForWriting);
        const int32_t number = *g_screenCaptureNumber;
        formatString(name, kCaptureName1, number);
        const int32_t image1File = sceOpen(name, kOpenForWriting);
        if (advance != 0)
            ++*g_screenCaptureNumber;

        // Both buffers into the heap, 4 bytes a pixel allocated.
        const int32_t bytes = width * height * 4;
        uint32_t row = 0;
        const uint32_t pixels1 = heapAllocate(bytes); // draw buffer 1's
        const uint32_t pixels0 = heapAllocate(bytes); // draw buffer 0's
        const int32_t widthIn64s = low16(gs::bufferWidthUnits(width));
        const int32_t imageWidth = low16(width);
        const int32_t imageHeight = low16(height);
        const auto readBack = [&](int32_t fbp, uint32_t into) {
            sceGsSetDefStoreImage(kStoreImage, gs::blockOfPage(fbp), widthIn64s, gs::kPsmCt24, 0, 0, imageWidth, imageHeight);
            flushCache(0);
            sceGsExecStoreImage(kStoreImage, into);
            sceGsSyncPath(0, 0);
        };
        readBack(*buffer1Base, pixels1);
        readBack(*buffer0Base, pixels0);

        // The rows interleaved, 3 bytes a pixel.
        const int32_t rowBytes = width * 3;
        for (; static_cast<int32_t>(row) < height; ++row)
        {
            const int32_t offset = static_cast<int32_t>(row * static_cast<uint32_t>(width)) * 3;
            const int32_t row1 = static_cast<int32_t>(pixels1) + offset;
            const int32_t row0 = static_cast<int32_t>(pixels0) + offset;
            sceWrite(image0File, row1, rowBytes);
            sceWrite(image0File, row0, rowBytes);
            sceWrite(image1File, row0, rowBytes);
            sceWrite(image1File, row1, rowBytes);
        }
        heapFree(pixels0);
        heapFree(pixels1);
        sceClose(image1File);
        sceClose(image0File);
        display.framesPending = static_cast<int8_t>(*framesPending);
        return 1;
    }
    READABLE_FUNCTION_WAITS(captureScreenToHost, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceGsSetDefLoadImage(uint32_t image, uint32_t dbp, uint32_t dbw, uint32_t dpsm, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { ee::game(kSceGsSetDefLoadImage, image, dbp, dbw, dpsm, x, y, w, h); }
        void sceGsExecLoadImage(uint32_t image, uint32_t source) { ee::game(kSceGsExecLoadImage, image, source); }
        void sceGsSyncV(uint32_t value) { ee::game(kSceGsSyncV, value); }
    }

    // showRawImage (no caller): a raw image file (`bitsPerPixel` 32, else
    // 24) shown full screen: read whole, the display set to its size, sent
    // to the GS a 64th of its rows at a time, the next vsync waited for.
    // 1, or 0 when the file does not open (a message on the debug output).
    uint32_t showRawImage(ee::Address path, uint32_t width, uint32_t height, uint32_t bitsPerPixel)
    {
        const uint32_t bytesPerPixel = bitsPerPixel >> 3;
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
        {
            const uint32_t message = formatScratch(kFailedToOpen, path);
            debugWrite(message);
            return 0;
        }
        const uint32_t psm = bitsPerPixel != 32u ? gs::kPsmCt24 : gs::kPsmCt32;
        uint32_t strip = 0;
        int32_t bytes = static_cast<int32_t>(width * height * bytesPerPixel);
        const uint32_t pixels = heapAllocate(bytes);
        // In strips of 64 rows (the height rounded towards zero).
        const int32_t strips = static_cast<int32_t>(height) / 64;
        fileRead(file, pixels, bytes, 0);
        uint32_t source = pixels;
        closeFile(file, 0);
        resetDisplayMode(width, height, 0, 0, 0, 0, 1);
        if (strips > 0)
        {
            const int32_t widthIn64s = static_cast<int32_t>(gs::bufferWidthUnits(width));
            const uint32_t imageFormat = psm & 0xFFFFu;
            const int32_t imageWidth = low16(static_cast<int32_t>(width));
            for (;;)
            {
                const int32_t rows = static_cast<int32_t>(height / static_cast<uint32_t>(strips));
                const int32_t perStrip = static_cast<int32_t>(width * height) / strips;
                const int32_t blocks = perStrip / static_cast<int32_t>(gs::kWordsPerBlock); // GS memory a strip takes, in blocks
                const int32_t address = static_cast<int32_t>(strip * static_cast<uint32_t>(blocks));
                ++strip;
                sceGsSetDefLoadImage(kLoadImage, low16(address), low16(widthIn64s), imageFormat, 0, 0, imageWidth, low16(rows));
                flushCache(0);
                sceGsExecLoadImage(kLoadImage, source);
                bytes = static_cast<int32_t>(width * static_cast<uint32_t>(rows));
                source += static_cast<uint32_t>(bytes) * bytesPerPixel;
                sceGsSyncPath(0, 0);
                if (static_cast<int32_t>(strip) >= strips)
                    break;
            }
        }
        heapFree(pixels);
        sceGsSyncPath(0, 0);
        sceGsSyncV(0);
        return 1;
    }

    namespace
    {
        // What ZGE2's start-up picture reads of its file: a 512 by 256 picture of 32-bit pixels.
        constexpr uint32_t kScreenImageBytes = 0x80000u; // not an address
    }

    // showScreenSizedRawImage: ZGE2's showRawImage takes the file alone: a
    // raw 32-bit picture the size of the frame buffer, read (its first
    // 512 KB) and sent whole to both frame buffers. 1 always; a file that
    // does not open leaves a message on the debug output (and the block
    // allocated for it).
    uint32_t showScreenSizedRawImage(ee::Address path)
    {
        const DisplaySetup &display = *g_display;
        const uint32_t frameBases[2] = {static_cast<uint32_t>(gs::frameBasePage(display.buffers.draw[0].draw1.frame.value)),
                                        static_cast<uint32_t>(gs::frameBasePage(display.buffers.draw[1].draw1.frame.value))};
        const int32_t height = ee::toInt(display.bufferHeight);
        const int32_t width = ee::toInt(display.bufferWidth);
        const uint32_t pixels = heapAllocate(kScreenImageBytes);
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        if (!file)
        {
            debugWrite(formatScratch(kFailedToOpen, path));
            return 1;
        }
        fileRead(file, pixels, kScreenImageBytes, 0);
        const int32_t widthIn64s = low16(gs::bufferWidthUnits(width));
        for (const uint32_t frameBase : frameBases)
        {
            sceGsSetDefLoadImage(kLoadImage, gs::blockOfPage(frameBase), widthIn64s, gs::kPsmCt32, 0, 0, low16(width), low16(height));
            flushCache(0);
            sceGsExecLoadImage(kLoadImage, pixels);
            sceGsSyncPath(0, 0);
        }
        heapFree(pixels);
        closeFile(file, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(showRawImage, Verified, dm2, showScreenSizedRawImage);
}
