#include "runtime/ps2_texture_dds.h"

// bcdec (runtime/cmake/zx_bcdec.cmake), its implementation here alone.
#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace zx::texture_dds
{
    namespace
    {
        using texture_replace::Format;
        using texture_replace::Picture;

        // The DDS file's layout (Microsoft's DDS.h, as PCSX2 copies it).
        constexpr uint32_t kMagic = 0x20534444u; // "DDS "
        constexpr size_t kHeaderBytes = 124;     // DDS_HEADER
        constexpr size_t kDx10HeaderBytes = 20;  // DDS_HEADER_DXT10
        constexpr uint32_t kMaxSize = 32768;     // DDS_MAX_TEXTURE_SIZE

        // DDS_HEADER's flags.
        constexpr uint32_t kFlagPitch = 0x8u;          // DDSD_PITCH
        constexpr uint32_t kFlagMipmapCount = 0x20000u; // DDSD_MIPMAPCOUNT
        constexpr uint32_t kFlagLinearSize = 0x80000u; // DDSD_LINEARSIZE
        constexpr uint32_t kFlagDepth = 0x800000u;     // DDSD_DEPTH: a volume
        // DDS_PIXELFORMAT's flags.
        constexpr uint32_t kPixelFourCC = 0x4u;  // DDPF_FOURCC
        constexpr uint32_t kPixelRgb = 0x40u;    // DDPF_RGB
        constexpr uint32_t kPixelRgba = 0x41u;   // DDPF_RGB | DDPF_ALPHAPIXELS
        constexpr uint32_t kDimension2D = 3;     // DDS_DIMENSION_TEXTURE2D

        // DXGI formats a DX10 header names.
        constexpr uint32_t kDxgiBc1 = 71, kDxgiBc2 = 74, kDxgiBc3 = 77, kDxgiBc7 = 98;

        constexpr uint32_t fourCC(char a, char b, char c, char d)
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
        }

        uint32_t readU32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v; // DDS is little-endian, as every host here
        }

        struct PixelFormat
        {
            uint32_t size, flags, fourCC, bitCount, redMask, greenMask, blueMask, alphaMask;
            bool operator==(const PixelFormat &o) const = default;
        };

        // The uncompressed formats PCSX2 reads, and how each becomes R G B A.
        enum class Layout : uint8_t { A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8, R8G8B8 };
        struct Uncompressed
        {
            PixelFormat format;
            Layout layout;
        };
        constexpr Uncompressed kUncompressed[] = {
            {{32, kPixelRgba, 0, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u}, Layout::A8R8G8B8},
            {{32, kPixelRgb, 0, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00000000u}, Layout::X8R8G8B8},
            {{32, kPixelRgb, 0, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0x00000000u}, Layout::X8B8G8R8},
            {{32, kPixelRgb, 0, 24, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00000000u}, Layout::R8G8B8},
            {{32, kPixelRgba, 0, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u}, Layout::A8B8G8R8},
        };

        // One row of an uncompressed level as R G B A (PCSX2's
        // ConvertTexture_* functions).
        void convertRow(Layout layout, const uint8_t *src, uint8_t *dst, uint32_t width)
        {
            for (uint32_t x = 0; x < width; ++x, dst += 4)
            {
                switch (layout)
                {
                case Layout::A8R8G8B8: // B G R A in memory
                    dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = src[3];
                    src += 4;
                    break;
                case Layout::X8R8G8B8: // B G R X
                    dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = 0xFF;
                    src += 4;
                    break;
                case Layout::A8B8G8R8: // R G B A already
                    std::memcpy(dst, src, 4);
                    src += 4;
                    break;
                case Layout::X8B8G8R8: // R G B X
                    dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = 0x80;
                    src += 4;
                    break;
                case Layout::R8G8B8: // B G R
                    dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = 0xFF;
                    src += 3;
                    break;
                }
            }
        }

        uint32_t blockCount(uint32_t extent, uint32_t blockSize)
        {
            return std::max<uint32_t>((extent + blockSize - 1u) / blockSize, 1u);
        }

        bool fail(std::string *error, const char *why)
        {
            if (error)
                *error = why;
            return false;
        }

        // A 4 x 4 block as 16 R G B A texels.
        void decodeBlock(Format format, const uint8_t *block, uint8_t out[64])
        {
            switch (format)
            {
            case Format::BC1: bcdec_bc1(block, out, 16); break;
            case Format::BC2: bcdec_bc2(block, out, 16); break;
            case Format::BC3: bcdec_bc3(block, out, 16); break;
            case Format::BC7: bcdec_bc7(block, out, 16); break;
            default: std::memset(out, 0, 64); break;
            }
        }
    }

    bool isDds(const std::vector<uint8_t> &bytes) { return bytes.size() >= 4 && readU32(bytes.data()) == kMagic; }

    bool readDds(const std::vector<uint8_t> &bytes, Picture &out, std::string *error)
    {
        if (!isDds(bytes))
            return fail(error, "not a DDS file");
        if (bytes.size() < 4 + kHeaderBytes)
            return fail(error, "cut short in its header");
        const uint8_t *h = bytes.data() + 4;
        const uint32_t headerSize = readU32(h), flags = readU32(h + 4);
        const uint32_t height = readU32(h + 8), width = readU32(h + 12), pitchOrLinearSize = readU32(h + 16);
        const uint32_t mipMapCount = readU32(h + 24);
        const PixelFormat pf{readU32(h + 72), readU32(h + 76), readU32(h + 80), readU32(h + 84),
                             readU32(h + 88), readU32(h + 92), readU32(h + 96), readU32(h + 100)};
        // (DDSD_CAPS and the other flags DDS_HEADER_FLAGS_TEXTURE names
        // are not asked for: some tools leave them out, compressonator.)
        if (headerSize < kHeaderBytes)
            return fail(error, "its header's size is too small");
        if (width == 0u || width >= kMaxSize || height == 0u || height >= kMaxSize)
            return fail(error, "its size is 0 or 32768 or more");
        if (flags & kFlagDepth)
            return fail(error, "a volume picture");
        if (width > 4096u || height > 4096u)
            return fail(error, "larger than 4096 x 4096");

        // Levels in the file, level 0 included.
        uint32_t levels = 1;
        if (flags & kFlagMipmapCount)
            levels = mipMapCount != 0u ? mipMapCount : static_cast<uint32_t>(std::bit_width(std::max(width, height)));

        size_t dataStart = 4 + kHeaderBytes;
        Format format = Format::RGBA8;
        uint32_t blockSize = 1, bytesPerBlock = 4;
        const Uncompressed *uncompressed = nullptr;
        if (pf.flags & kPixelFourCC)
        {
            uint32_t dxgi = 0;
            if (pf.fourCC == fourCC('D', 'X', '1', '0'))
            {
                if (bytes.size() < dataStart + kDx10HeaderBytes)
                    return fail(error, "cut short in its DX10 header");
                const uint8_t *d = bytes.data() + dataStart;
                dxgi = readU32(d);
                if (readU32(d + 4) != kDimension2D || readU32(d + 12) != 1u)
                    return fail(error, "not one 2D picture (an array or another dimension)");
                dataStart += kDx10HeaderBytes;
            }
            if (pf.fourCC == fourCC('D', 'X', 'T', '1') || dxgi == kDxgiBc1)
                format = Format::BC1, bytesPerBlock = 8;
            else if (pf.fourCC == fourCC('D', 'X', 'T', '2') || pf.fourCC == fourCC('D', 'X', 'T', '3') || dxgi == kDxgiBc2)
                format = Format::BC2, bytesPerBlock = 16;
            else if (pf.fourCC == fourCC('D', 'X', 'T', '4') || pf.fourCC == fourCC('D', 'X', 'T', '5') || dxgi == kDxgiBc3)
                format = Format::BC3, bytesPerBlock = 16;
            else if (dxgi == kDxgiBc7)
                format = Format::BC7, bytesPerBlock = 16;
            else
                return fail(error, "a format other than BC1, BC2, BC3, BC7 or 24 and 32-bit RGB");
            blockSize = 4;
        }
        else
        {
            for (const Uncompressed &u : kUncompressed)
                if (u.format == pf)
                    uncompressed = &u;
            if (!uncompressed)
                return fail(error, "a format other than BC1, BC2, BC3, BC7 or 24 and 32-bit RGB");
            bytesPerBlock = pf.bitCount / 8u;
        }
        if (blockSize > 1u && (width % blockSize != 0u || height % blockSize != 0u))
            return fail(error, "a block format whose size is not a multiple of 4");
        if (dataStart >= bytes.size())
            return fail(error, "no picture after its header");

        // Level 0's row pitch: the header's with both PITCH and LINEARSIZE
        // set (as PCSX2 reads them), else the rows packed.
        uint32_t pitch0 = blockCount(width, blockSize) * bytesPerBlock;
        if ((flags & kFlagPitch) && (flags & kFlagLinearSize))
        {
            if (pitchOrLinearSize < bytesPerBlock)
                return fail(error, "its pitch is smaller than a texel");
            pitch0 = pitchOrLinearSize;
        }

        out = Picture{};
        out.width = width;
        out.height = height;
        out.format = format;
        const uint32_t fullChain = static_cast<uint32_t>(std::bit_width(std::max(width, height)));
        levels = std::min(levels, fullChain);
        size_t at = dataStart;
        uint32_t read = 0;
        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t lw = std::max<uint32_t>(width >> level, 1u), lh = std::max<uint32_t>(height >> level, 1u);
            const uint32_t rows = blockCount(lh, blockSize);
            const uint32_t pitch = level == 0u ? pitch0 : blockCount(lw, blockSize) * bytesPerBlock;
            const size_t size = static_cast<size_t>(pitch) * rows;
            if (at + size > bytes.size())
                break; // cut short: the levels before it
            const uint8_t *src = bytes.data() + at;
            const size_t packedRow = static_cast<size_t>(blockCount(lw, blockSize)) * (uncompressed ? 4u : bytesPerBlock);
            const size_t offset = out.data.size();
            out.data.resize(offset + packedRow * rows);
            uint8_t *dst = out.data.data() + offset;
            for (uint32_t r = 0; r < rows; ++r)
            {
                if (uncompressed)
                    convertRow(uncompressed->layout, src + static_cast<size_t>(r) * pitch, dst + r * packedRow, lw);
                else
                    std::memcpy(dst + r * packedRow, src + static_cast<size_t>(r) * pitch, packedRow);
            }
            at += size;
            ++read;
        }
        if (read == 0u)
            return fail(error, "cut short in its first level");
        out.mips = read;
        return true;
    }

    std::vector<uint8_t> rgbaLevel(const Picture &p, uint32_t level)
    {
        const uint32_t lw = p.levelWidth(level), lh = p.levelHeight(level);
        std::vector<uint8_t> out(static_cast<size_t>(lw) * lh * 4u, 0);
        if (level >= p.mips || p.levelOffset(level + 1u) > p.data.size())
            return out;
        const uint8_t *src = p.data.data() + p.levelOffset(level);
        if (p.format == Format::RGBA8)
        {
            std::memcpy(out.data(), src, out.size());
            return out;
        }
        if (!texture_replace::isBlockFormat(p.format))
            return out;
        const size_t blockBytes = p.format == Format::BC1 ? 8u : 16u;
        const uint32_t bw = blockCount(lw, 4u), bh = blockCount(lh, 4u);
        uint8_t texels[64];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx)
            {
                decodeBlock(p.format, src + (static_cast<size_t>(by) * bw + bx) * blockBytes, texels);
                // A level smaller than the block keeps the block's top left.
                for (uint32_t y = 0; y < 4u && by * 4u + y < lh; ++y)
                    for (uint32_t x = 0; x < 4u && bx * 4u + x < lw; ++x)
                        std::memcpy(out.data() + ((static_cast<size_t>(by) * 4u + y) * lw + bx * 4u + x) * 4u,
                                    texels + (y * 4u + x) * 4u, 4u);
            }
        return out;
    }

    texture_replace::PicturePtr asRgba8(const texture_replace::PicturePtr &picture)
    {
        if (!picture || picture->format == Format::RGBA8)
            return picture;
        auto p = std::make_shared<Picture>();
        p->width = picture->width;
        p->height = picture->height;
        p->mips = picture->mips;
        p->format = Format::RGBA8;
        p->name = picture->name;
        p->data.reserve(p->levelOffset(p->mips));
        for (uint32_t level = 0; level < p->mips; ++level)
        {
            const std::vector<uint8_t> texels = rgbaLevel(*picture, level);
            p->data.insert(p->data.end(), texels.begin(), texels.end());
        }
        return p;
    }
}
