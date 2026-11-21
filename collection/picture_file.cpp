#include "collection/picture_file.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace zx::collection
{
    namespace
    {
        constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

        uint32_t bigEndian32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
        }

        // A PNG's size from its header chunk (IHDR, first after the
        // signature: its length, its name, the width and the height).
        bool pngSize(const std::vector<uint8_t> &file, uint32_t &width, uint32_t &height)
        {
            if (file.size() < 24 || std::memcmp(file.data() + 12, "IHDR", 4) != 0)
                return false;
            width = bigEndian32(file.data() + 16);
            height = bigEndian32(file.data() + 20);
            return true;
        }

        // A picture's pixels are made before it is decoded, from the size
        // its header gives: one larger than kMostPixels is not read.
        bool sizeKept(uint64_t width, uint64_t height)
        {
            return width > 0 && height > 0 && width * height <= kMostPixels;
        }

        // A surface's pixels into `out` as RGBA (R in the low byte).
        bool imageOf(SDL_Surface *surface, Image &out)
        {
            SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            if (!rgba)
                return false;
            out.width = rgba->w;
            out.height = rgba->h;
            out.pixels.resize(static_cast<size_t>(rgba->w) * static_cast<size_t>(rgba->h));
            for (int y = 0; y < rgba->h; ++y)
                std::memcpy(out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rgba->w),
                            static_cast<const uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * static_cast<size_t>(rgba->pitch),
                            static_cast<size_t>(rgba->w) * 4u);
            SDL_DestroySurface(rgba);
            return true;
        }

        bool readPng(const std::vector<uint8_t> &file, Image &out)
        {
            uint32_t width = 0, height = 0;
            if (!pngSize(file, width, height) || !sizeKept(width, height))
                return false;
            SDL_IOStream *io = SDL_IOFromConstMem(file.data(), file.size());
            if (!io)
                return false;
            SDL_Surface *surface = SDL_LoadPNG_IO(io, true);
            if (!surface)
                return false;
            const bool ok = imageOf(surface, out);
            SDL_DestroySurface(surface);
            return ok;
        }

        bool readJpeg(const std::vector<uint8_t> &file, Image &out)
        {
            int width = 0, height = 0;
            if (!jpegSize(file, width, height) || !sizeKept(static_cast<uint64_t>(width), static_cast<uint64_t>(height)) ||
                file.size() > static_cast<size_t>(INT32_MAX))
                return false;
            std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
            // (an MJPG "picture" is one JPEG: its pitch is the file's length)
            if (!SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_MJPG, file.data(), static_cast<int>(file.size()),
                                   SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4))
                return false;
            out.width = width;
            out.height = height;
            out.pixels = std::move(pixels);
            return true;
        }
    }

    bool jpegSize(const std::vector<uint8_t> &file, int &width, int &height)
    {
        if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8)
            return false;
        size_t at = 2;
        while (at + 4 <= file.size())
        {
            if (file[at] != 0xFF)
                return false;
            const uint8_t marker = file[at + 1];
            if (marker == 0xFF) // (a fill byte)
            {
                ++at;
                continue;
            }
            // (markers without a segment: TEM, the restarts)
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            {
                at += 2;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) // (the end, or the scan: no frame header before it)
                return false;
            const size_t length = static_cast<size_t>(file[at + 2]) << 8 | file[at + 3];
            if (length < 2 || at + 2 + length > file.size())
                return false;
            // SOF0-SOF15 but DHT (C4), JPG (C8) and DAC (CC): length, precision, height, width.
            const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (frame)
            {
                if (length < 7)
                    return false;
                height = static_cast<int>(file[at + 5]) << 8 | file[at + 6];
                width = static_cast<int>(file[at + 7]) << 8 | file[at + 8];
                return width > 0 && height > 0;
            }
            at += 2 + length;
        }
        return false;
    }

    bool readPictureFile(const std::vector<uint8_t> &file, Image &out)
    {
        if (file.size() >= 2 && file[0] == 'B' && file[1] == 'M')
            return readBmpImage(file, out);
        if (file.size() >= sizeof(kPngSignature) && std::memcmp(file.data(), kPngSignature, sizeof(kPngSignature)) == 0)
            return readPng(file, out);
        if (file.size() >= 2 && file[0] == 0xFF && file[1] == 0xD8)
            return readJpeg(file, out);
        return false;
    }
}
