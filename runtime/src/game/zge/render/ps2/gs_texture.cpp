// The textures' GS side (zge/render/texture.h lays the records out;
// zge/render/texture_levels.cpp adds the levels): a texture's levels and
// CLUT as the GS will take them, each with the upload
// chain the VIF1 handler (ps2/texture_upload.cpp) CALLs: a DMA cnt tag, a GIF tag
// of three A+D registers (BITBLTBUF, TRXREG, TRXDIR: host to local), the
// IMAGE tag and a REF of the texels. The pictures come from the TXR loader
// (setTextureImage) and from the movie player (setGsTextureFromImage, a
// 32-bit frame in 16 x 16 tiles).
//
// Original quirks kept: a CLUT's upload gives BITBLTBUF.DBW its width in
// pixels (8 or 16) where the GS counts 64-pixel units (harmless: the CLUT
// fits in one page); 16-bit CLUT entries get red in bits 10-14 (the 16-bit
// formats' blue); a picture of a depth the table does not list gets format
// 0 and an alignment of 0, and the original divides by it.

#include "zge/render/texture.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/text/cstring.h"
#include "zge/memory/alignment.h"
#include "zge/platform/platform.h"
#include "zge/render/gs_registers.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/ps2/gs_texture.cpp.h"

namespace zge
{
    namespace
    {
        // ZGE4's setting (texture_levels.cpp): whether a texture without alpha drops its first level while that is
        // 64 texels wide or more.
        inline ee::Global<uint32_t, sym::bmx::g_dropLargeTextureLevels> g_dropLargeTextureLevels;
        constexpr uint32_t kDroppedLevelWidth = 64;
        constexpr uint32_t kImageHasAlpha = 1u; // TextureImage::flags
        constexpr uint32_t kTexel32Bytes = 4, kRed = 0, kGreen = 1, kBlue = 2, kAlpha = 3; // a 32-bit texel's bytes
        // The display's colour scale (the display set-up's +0x30 .. +0x38:
        // red, green, blue), applied to the CLUTs as 8.8 fixed point.
        inline ee::Global<ee::Float, sym::g_clutRedScale> g_clutRedScale;
        inline ee::Global<ee::Float, sym::g_clutGreenScale> g_clutGreenScale;
        inline ee::Global<ee::Float, sym::g_clutBlueScale> g_clutBlueScale;

        // The upload's GIF tags: three address and data pairs (PACKED), then the texels (IMAGE, NLOOP their
        // quadwords).
        constexpr uint64_t kUploadGifTag = gs::GifTag{.loops = 3, .registers = 1}.bits();
        constexpr uint64_t kImageTag = gs::GifTag{.format = gs::kImage}.bits();
        constexpr uint32_t kMaxImageQwords = 0x7FFEu;            // the movie frame's upload, at most

        // A level's and a CLUT's upload chains, through the texture's own
        // pointer: a texture reached through the uncached mirror keeps it in
        // all of them, as the original's address sums do.
        ee::Ptr<uint32_t> uploadChainOf(ee::Ptr<TextureLevel> level) { return (level.cast<uint8_t>() + offsetof(TextureLevel, uploadHead)).cast<uint32_t>(); }
        ee::Ptr<uint32_t> clutUploadChainOf(ee::Ptr<GsTexture> texture) { return (texture.cast<uint8_t>() + offsetof(GsTexture, clutUploadHead)).cast<uint32_t>(); }

        // DIVU (the original traps on a zero divisor, BREAK 7: a depth the
        // format table does not list).
        uint32_t dividedBy(uint32_t value, uint32_t divisor) { return divisor != 0 ? value / divisor : 0u; }

        // An upload chain built in `chain` with the GIF packet `packet`: the
        // DMA cnt tag, the GIF tag and the three registers, the IMAGE tag,
        // the REF of `data` (an address, as the DMA tag takes it), the RET.
        // gifPacketTerminate's result.
        uint32_t buildUploadChain(ee::Ptr<sce::GifPacket> packet, ee::Ptr<uint32_t> chain, uint64_t bitbltbuf, uint64_t trxreg,
                                  uint32_t data, uint32_t qwords)
        {
            sce::gifPacketInit(packet, chain);
            sce::gifPacketReset(packet);
            sce::gifPacketCnt(packet, 0, 0, 0);
            const auto addDword = [&](uint64_t value) { sce::gifPacketAddDword(packet, value); };
            addDword(kUploadGifTag);
            addDword(gs::kAddressData);
            addDword(bitbltbuf);
            addDword(gs::kBitBltBuf);
            addDword(trxreg);
            addDword(gs::kTrxReg);
            addDword(gs::kHostToLocal); // TRXDIR
            addDword(gs::kTrxDir);
            addDword(static_cast<uint64_t>(qwords) | kImageTag);
            sce::gifPacketRef(packet, data, qwords, 0, 0, 0);
            sce::gifPacketRet(packet, 0, 0, 0);
            return sce::gifPacketTerminate(packet);
        }

        // BITBLTBUF (DBP, DBW, DPSM) and TRXREG (RRW, RRH) of an upload.
        uint64_t uploadBitbltbuf(uint32_t gsAddress, uint32_t bufferWidth, uint32_t psm)
        {
            return gs::BitBltBuf::kDbp.placed(gs::blockOf(gsAddress)) | gs::BitBltBuf::kDbw.placed(bufferWidth) |
                   gs::BitBltBuf::kDpsm.placed(psm);
        }
        uint64_t uploadTrxreg(uint32_t width, uint32_t height)
        {
            return gs::TrxReg::kRrw.placed(width) | gs::TrxReg::kRrh.placed(height);
        }

        // A level's size as gsLevelSize gives it back (in the frame, after
        // the GIF packet).
        struct GsLevelSize
        {
            uint32_t width, height;
        };

        // A level's GS words and buffer width (TBW, at least 1) from its
        // size; alignment: the texels a word holds.
        constexpr uint32_t kGsBlockWords = gs::kWordsPerBlock; // GS memory is given out in blocks
        void setLevelGsSize(TextureLevel &level, const GsLevelSize &size, uint32_t alignment)
        {
            const uint32_t width = size.width;
            const uint32_t words = dividedBy(width, alignment) * size.height;
            level.gsWords = (words + kGsBlockWords - 1) & ~(kGsBlockWords - 1);
            const uint32_t bufferWidth = gs::bufferWidthUnits(width);
            level.bufferWidth = static_cast<uint16_t>(bufferWidth != 0 ? bufferWidth : 1u);
        }
    }

    // ---- CLUTs ----------------------------------------------------------------

    // scaleClutColours: a palette's entries (RGBA) into a CLUT in GS order with
    // the display's colour scale applied (each channel capped at 255) and
    // alpha halved (the GS's 0x80 is opaque): PSMCT32 (16 entries straight,
    // 256 in CSM1 order: bits 3 and 4 of the index swapped) or 16-bit
    // entries (PSMCT16, PSMCT16S: 5 bits a channel, no alpha).
    void scaleClutColours(ee::Address dst, ee::Address src, uint32_t entries, uint32_t format)
    {
        ee::Local<int32_t> redScaleSlot;
        ee::Local<int32_t> greenScaleSlot;
        ee::Local<int32_t> blueScaleSlot;
        const int32_t redScale = ee::toInt(*g_clutRedScale * ee::Float(256.0f));
        *redScaleSlot = redScale;
        const int32_t greenScale = ee::toInt(*g_clutGreenScale * ee::Float(256.0f));
        *greenScaleSlot = greenScale;
        const int32_t blueScale = ee::toInt(*g_clutBlueScale * ee::Float(256.0f));
        *blueScaleSlot = blueScale;
        const ee::Ptr<uint8_t> in(src);
        const auto scaled = [](uint8_t channel, int32_t scale) {
            const int32_t value = static_cast<int32_t>(static_cast<uint32_t>(channel) * static_cast<uint32_t>(scale)) >> 8;
            return value >= 256 ? 255 : value;
        };
        const auto entry32 = [&](uint32_t from) {
            return static_cast<uint32_t>(scaled(in[from], redScale)) | (static_cast<uint32_t>(in[from + 3]) >> 1) << 24 |
                   (static_cast<uint32_t>(scaled(in[from + 2], blueScale)) << 16 |
                    static_cast<uint32_t>(scaled(in[from + 1], greenScale)) << 8);
        };
        const auto sixteenBit = [&]() {
            if (entries != 0)
            {
                const ee::Ptr<uint16_t> out(dst);
                for (uint32_t i = 0;;)
                {
                    const int32_t red = scaled(in[4 * i], *redScaleSlot);
                    const int32_t green = scaled(in[4 * i + 1], *greenScaleSlot);
                    const int32_t blue = scaled(in[4 * i + 2], *blueScaleSlot);
                    out[i] = static_cast<uint16_t>((red >> 3) << 10 | (green >> 3) << 5 | (blue >> 3));
                    if (++i == entries)
                        break;
                }
            }
        };
        if (format == gs::kPsmCt16)
        {
            sixteenBit();
            return;
        }
        if (format >= 3)
        {
            if (format == gs::kPsmCt16S)
                sixteenBit();
            return;
        }
        if (format != gs::kPsmCt32)
            return;
        const ee::Ptr<uint32_t> out(dst);
        if (entries == 16)
        {
            for (uint32_t i = 0;;)
            {
                out[i] = entry32(4 * i);
                if (++i == 16)
                    break;
            }
            return;
        }
        if (entries == 0)
            return;
        for (uint32_t i = 0;;)
        {
            const uint32_t from = gs::csm1Entry(i);
            out[i] = entry32(4 * from);
            if (++i >= entries)
                break;
        }
    }

    // copyClutColours: ZGE2's scaleClutColours: the palette's entries into the CLUT in the same GS orders, as they
    // are: no colour scale (ZGE2 has no g_clutRedScale), alpha kept whole in the 32-bit entries (ZGE3 halves it).
    void copyClutColours(ee::Address dst, ee::Address src, uint32_t entries, uint32_t format)
    {
        const ee::Ptr<uint8_t> in(src);
        const auto entry32 = [&](uint32_t from) {
            return static_cast<uint32_t>(in[from]) | static_cast<uint32_t>(in[from + 3]) << 24 |
                   (static_cast<uint32_t>(in[from + 2]) << 16 | static_cast<uint32_t>(in[from + 1]) << 8);
        };
        const auto sixteenBit = [&]() {
            if (entries != 0)
            {
                const ee::Ptr<uint16_t> out(dst);
                for (uint32_t i = 0;;)
                {
                    out[i] = static_cast<uint16_t>((in[4 * i] >> 3) << 10 | (in[4 * i + 1] >> 3) << 5 | (in[4 * i + 2] >> 3));
                    if (++i == entries)
                        break;
                }
            }
        };
        if (format == gs::kPsmCt16)
        {
            sixteenBit();
            return;
        }
        if (format >= 3)
        {
            if (format == gs::kPsmCt16S)
                sixteenBit();
            return;
        }
        if (format != gs::kPsmCt32)
            return;
        const ee::Ptr<uint32_t> out(dst);
        if (entries == 16)
        {
            for (uint32_t i = 0;;)
            {
                out[i] = entry32(4 * i);
                if (++i == 16)
                    break;
            }
            return;
        }
        if (entries == 0)
            return;
        for (uint32_t i = 0;;)
        {
            const uint32_t from = gs::csm1Entry(i);
            out[i] = entry32(4 * from);
            if (++i >= entries)
                break;
        }
    }
    READABLE_FUNCTION_OWN(scaleClutColours, Verified, dm2, copyClutColours);

    // buildClutUpload: a 4- or 8-bit picture's CLUT made (its palette scaled
    // into memory from the CLUT pool) and its upload chain built; marked to
    // upload. Nothing for a deeper picture.
    void buildClutUpload(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image)
    {
        ee::Local<sce::GifPacket> packet;
        if (image->bitsPerTexel >= 9)
            return;
        GsTexture &gs = *texture;
        gs.clutGsAddress = 0;
        gs.clutLevels = 1;
        gs.clutPsm = gs::kPsmCt32;
        gs.clutEntries = static_cast<uint16_t>(1u << (image->bitsPerTexel & 0x1Fu));
        const uint32_t bytes = roundUp16(static_cast<uint32_t>(gs.clutEntries) * 4u); // whole quadwords
        const uint32_t clut = allocateIn(bytes, kClutPool, 1);
        const uint16_t clutEntries = gs.clutEntries;
        gs.clut = ee::Ptr<uint8_t>(clut);
        gs.clutQwords = bytes >> 4;
        // The upload's rectangle: 8 x 2 for 16 entries, 16 x 16 for 256.
        if (clutEntries == 16)
        {
            gs.clutWidth = 8;
            gs.clutHeight = 2;
        }
        else
        {
            if (clutEntries == 256)
            {
                gs.clutHeight = 16;
                gs.clutWidth = 16;
            }
        }
        scaleClutColours(gs.clut, image->clut, static_cast<uint32_t>(image->clutEntries), static_cast<uint32_t>(gs.clutPsm));
        gs.pendingUploads |= kTextureUploadClut;
        // (gs.clut is still the allocation: the chain's REF takes it as the number it is.)
        buildUploadChain(packet, clutUploadChainOf(texture), uploadBitbltbuf(gs.clutGsAddress, gs.clutWidth, gs.clutPsm),
                         uploadTrxreg(gs.clutWidth, gs.clutHeight), clut, gs.clutQwords);
    }
    READABLE_FUNCTION_WAITS(buildClutUpload, Verified);

    // ---- levels ---------------------------------------------------------------

    // buildTextureUploadPacket: the picture as the texture's next level: its format from
    // its depth, its size rounded (the width to the texels a word holds, the
    // height to 8; a later level must be the first's halved), its texels
    // copied into memory from the texel pool, its GS size and buffer width,
    // and its upload chain; marked to upload. 0 for a level that does not
    // halve the first.
    int32_t buildTextureUploadPacket(ee::Ptr<GsTexture> texture, ee::Ptr<TextureImage> image)
    {
        ee::Local<sce::GifPacket> packet;
        ee::Local<GsLevelSize> size;
        GsTexture &gs = *texture;
        uint32_t psm = gs::kPsmCt32;
        uint32_t alignment = 0; // texels a 32-bit word holds
        const uint32_t depthCase = static_cast<uint32_t>(image->bitsPerTexel) - 4u;
        if (depthCase < 29)
        {
            // The format by depth (4 .. 32); another depth keeps CT32 with no
            // alignment.
            switch (image->bitsPerTexel)
            {
            case 4: psm = gs::kPsmT4; alignment = 8; break;
            case 8: psm = gs::kPsmT8; alignment = 4; break;
            case 16: psm = gs::kPsmCt16; alignment = 2; break;
            case 24: psm = gs::kPsmCt24; break;
            case 32: psm = gs::kPsmCt32; break;
            default: break;
            }
            if (image->bitsPerTexel == 24 || image->bitsPerTexel == 32)
                alignment = 1;
        }
        gs.psm = static_cast<uint8_t>(psm);
        const uint32_t level = gs.levelCount;
        const uint32_t imageWidth = image->width;
        const uint32_t width = (imageWidth - 1u + alignment) & (0u - alignment);
        const uint32_t height = (image->height + 7u) & ~7u;
        if (level != 0)
        {
            const uint32_t halving = 1u << (level & 0x1Fu);
            const uint32_t expected = static_cast<uint32_t>(static_cast<int32_t>(gs.baseWidth) / static_cast<int32_t>(halving));
            if (imageWidth != expected)
                return 0;
        }
        else
        {
            gs.baseWidth = static_cast<uint16_t>(width);
            gs.baseHeight = static_cast<uint16_t>(height);
            gs.psm = static_cast<uint8_t>(psm);
            gs.log2Width = static_cast<uint8_t>(log2Floor(width & 0xFFFFu));
            gs.log2Height = static_cast<uint8_t>(log2Floor(static_cast<uint32_t>(gs.baseHeight)));
        }
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4: a texture without alpha may drop its first level when its second arrives, which becomes the
            // first (g_dropLargeTextureLevels).
            if ((image->flags & kImageHasAlpha) == 0 && *g_dropLargeTextureLevels != 0 && gs.levelCount == 1 &&
                gs.baseWidth >= kDroppedLevelWidth)
            {
                gs.levelCount = 0;
                freeIn(levelOf(texture, 0)->texels, kTexelPool);
                gs.baseWidth = static_cast<uint16_t>(width);
                gs.baseHeight = static_cast<uint16_t>(height);
                gs.log2Width = static_cast<uint8_t>(log2Floor(width & 0xFFFFu));
                gs.log2Height = static_cast<uint8_t>(log2Floor(static_cast<uint32_t>(gs.baseHeight)));
            }
        }
        const uint32_t wordsWide = dividedBy(width, alignment);
        // (By the texture's own address: it may come in through the uncached mirror.)
        const ee::Ptr<TextureLevel> newLevel = levelOf(texture, gs.levelCount);
        TextureLevel &out = *newLevel;
        const uint32_t bytes = (wordsWide * height) << 2;
        const uint32_t texels = allocateIn(bytes, kTexelPool, 1);
        out.texels = ee::Ptr<uint8_t>(texels);
        out.uploadQwords = bytes >> 4;
        out.gsAddress = 0;
        out.width = static_cast<uint16_t>(width);
        out.height = static_cast<uint16_t>(height);
        size->width = width;
        size->height = height;
        gsLevelSize(static_cast<uint32_t>(gs.psm), &size->width, &size->height);
        setLevelGsSize(out, size, alignment);
        if (game::readable::kIsBmx && image->bitsPerTexel == 32)
        {
            // ZGE4 halves a 32-bit picture's alpha as it copies it (the GS's 0x80 is opaque).
            const ee::Ptr<uint8_t> to = out.texels;
            const ee::Ptr<uint8_t> from = image->texels;
            for (uint32_t texel = 0; texel < bytes; texel += kTexel32Bytes)
            {
                to[texel + kRed] = from[texel + kRed];
                to[texel + kGreen] = from[texel + kGreen];
                to[texel + kBlue] = from[texel + kBlue];
                to[texel + kAlpha] = static_cast<uint8_t>(from[texel + kAlpha] >> 1);
            }
        }
        else
            memoryCopy(out.texels, image->texels, bytes);
        gs.pendingUploads |= 1u << (gs.levelCount & 0x1Fu);
        buildUploadChain(packet, uploadChainOf(newLevel), uploadBitbltbuf(out.gsAddress, out.bufferWidth, gs.psm),
                         uploadTrxreg(out.width, out.height), texels, out.uploadQwords);
        return 1;
    }
    READABLE_FUNCTION_WAITS(buildTextureUploadPacket, Verified);

    // setGsTextureFromImage: a 32-bit picture in 16 x 16 tiles (the movie player's
    // frame) as the texture's level: the first time its level 0 and upload
    // chain are set up (the upload at most 0x7FFE quadwords) and the tiles
    // copied in, later only the tiles copied again; marked to upload. The
    // first time gifPacketTerminate's result, else the pending uploads.
    uint32_t setGsTextureFromImage(ee::Ptr<GsTexture> texture, uint32_t tiles, uint32_t width, uint32_t height)
    {
        ee::Local<sce::GifPacket> packet;
        ee::Local<GsLevelSize> size;
        GsTexture &gs = *texture;
        const uint32_t alignedHeight = (height + 7u) & ~7u;
        const uint32_t alignment = 1; // 32-bit texels
        const ee::Ptr<TextureLevel> level = levelOf(texture, gs.levelCount);
        TextureLevel &out = *level;
        if (gs.levelCount != 0)
        {
            copyTextureTiles(texture, tiles);
            gs.pendingUploads |= 1u;
            return gs.pendingUploads;
        }
        gs.baseWidth = static_cast<uint16_t>(width);
        gs.psm = gs::kPsmCt32;
        gs.baseHeight = static_cast<uint16_t>(alignedHeight);
        gs.log2Width = static_cast<uint8_t>(log2Floor(width & 0xFFFFu));
        gs.log2Height = static_cast<uint8_t>(log2Floor(static_cast<uint32_t>(gs.baseHeight)));
        const uint32_t bytes = ((width / alignment) * alignedHeight) << 2;
        const uint32_t texels = allocateIn(bytes, kTexelPool, 1);
        out.texels = ee::Ptr<uint8_t>(texels);
        const uint32_t qwords = bytes >> 4;
        out.uploadQwords = qwords;
        if (qwords >= kMaxImageQwords + 1u)
            out.uploadQwords = kMaxImageQwords;
        out.gsAddress = 0;
        out.width = static_cast<uint16_t>(width);
        out.height = static_cast<uint16_t>(alignedHeight);
        size->width = width;
        size->height = alignedHeight;
        gsLevelSize(static_cast<uint32_t>(gs.psm), &size->width, &size->height);
        setLevelGsSize(out, size, alignment);
        copyTextureTiles(texture, tiles);
        gs.pendingUploads |= 1u << (gs.levelCount & 0x1Fu);
        gs.levelCount = static_cast<uint8_t>(gs.levelCount + 1);
        return buildUploadChain(packet, uploadChainOf(level), uploadBitbltbuf(out.gsAddress, out.bufferWidth, gs.psm),
                                uploadTrxreg(out.width, out.height), texels, out.uploadQwords);
    }
    READABLE_FUNCTION_WAITS(setGsTextureFromImage, Verified);

}
