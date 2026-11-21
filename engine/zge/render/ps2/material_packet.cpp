// A material's packet as VU1 and the GS take it (zge/render/material.h lays
// the layers out; zge/render/material_kinds.cpp says what a kind needs): the
// effect routines (the words VU1 runs before its strips, Material +0xE0 on)
// as they are written, and the size that goes into the material's DMA tag,
// UNPACK and GIF tag; its layers' first GS state; a layer's texture
// registers. zge/render/ps2/material_state.cpp has the state setter. BMX
// XXX (PS2) has the same functions.

#include "zge/render/material.h"
#include "zge/render/ps2/material_packet.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"
#include "zge/render/texture.h"
#include "zge/text/cstring.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/ps2/material_packet.cpp.h"

namespace zge
{
    namespace
    {
        // A routine's first word: one quadword each, with what it takes in the
        // quadword's other words, kRoutineMatrix followed by a matrix. The
        // word is the routine's place in the VU1 microprogram (its byte
        // address there / 8, a relocation in the original): BMX XXX's
        // microprogram has them at other places.
        enum : uint32_t
        {
            kRoutineEnd = game::readable::perGame(1236u, 1236u, 1343u),
            kRoutineValue = game::readable::perGame(1246u, 1246u, 1353u),
            kRoutineMatrix = game::readable::perGame(1409u, 1409u, 1450u),    // AI 0x581, material kind 9: the UV transform
            kRoutineSphereMap = game::readable::perGame(1426u, 1426u, 1467u), // AI 0x592, kind 10
            kRoutineEnvironmentMap = game::readable::perGame(1455u, 1455u, 1496u), // AI 0x5AF, kinds 6 and 7
        };
        constexpr uint32_t kGifTagRegisters = 8;     // the GS block's quadwords before the routines

        void addRoutine(ee::Ptr<PacketWriter> writer, uint32_t routine, uint32_t word1, uint32_t word2, uint32_t word3)
        {
            *writer->cursor = Routine{{routine, word1, word2, word3}};
        }

        // Past the routine (or its rows) just written: the cursor, in quadwords.
        ee::Ptr<Routine> advance(ee::Ptr<PacketWriter> writer, uint32_t quadwords = 1)
        {
            writer->cursor = writer->cursor + quadwords;
            return writer->cursor;
        }
        int32_t routineQuadwords(const PacketWriter &writer) { return bytesFrom(writer.start, writer.cursor) >> 4; }
    }

    // setMaterialPacketSize: the routines' quadwords after the GS block: the packet's
    // size in the DMA tag, the UNPACK and qwordCount, and the routines' count's
    // low four bits at bit 32 of the GIF tag's second doubleword. Returns
    // that doubleword.
    uint64_t setMaterialPacketSize(ee::Ptr<Material> material, uint32_t quadwords)
    {
        const uint32_t size = quadwords + kGifTagRegisters;
        MaterialLayer &first = materialLayer(material, 0);
        first.unpack = (first.unpack & ~vif::kNumBits) | (size << 16 & vif::kNumBits);
        setLayerQwordCount(material, 0, size);
        first.dmaQwords = static_cast<uint16_t>(size);
        const uint64_t tag = withField(first.gifTag[1], kTagRoutineQuadwords, quadwords);
        first.gifTag[1] = tag;
        return tag;
    }
    READABLE_FUNCTION(setMaterialPacketSize, Verified);

    // beginMaterialPacket: a writer at the start of the material's routines.
    ee::Ptr<Routine> beginMaterialPacket(ee::Ptr<PacketWriter> writer, ee::Ptr<Material> material)
    {
        const ee::Ptr<Routine> routines = routinesOf(material);
        writer->material = material;
        writer->cursor = routines;
        writer->start = routines;
        return routines;
    }
    READABLE_FUNCTION(beginMaterialPacket, Verified);

    // finishMaterialPacket: the end routine, and the packet's size set (the original's
    // tail call).
    int32_t finishMaterialPacket(ee::Ptr<PacketWriter> writer)
    {
        addRoutine(writer, kRoutineEnd, 0, 0, 0);
        advance(writer);
        return game::tail(setMaterialPacketSize, writer->material, routineQuadwords(*writer));
    }
    READABLE_FUNCTION(finishMaterialPacket, Verified);

    // addMaterialPacketMatrix: a matrix routine and the matrix's four rows. Returns the
    // last row's first doubleword.
    uint64_t addMaterialPacketMatrix(ee::Ptr<PacketWriter> writer, ee::Ptr<Vec4> matrix)
    {
        addRoutine(writer, kRoutineMatrix, 0, 0, 0);
        advance(writer);
        copyRows(rowsAt(writer->cursor), matrix);
        advance(writer, 4);
        const Vec4 &lastRow = quadwordOf(matrix + 3);
        return lastRow.x.bits() | static_cast<uint64_t>(lastRow.y.bits()) << 32;
    }
    READABLE_FUNCTION(addMaterialPacketMatrix, Verified);

    // addMaterialPacketValue: a value routine. Returns the cursor.
    ee::Ptr<Routine> addMaterialPacketValue(ee::Ptr<PacketWriter> writer, uint32_t word1, uint32_t word2, ee::Float value)
    {
        addRoutine(writer, kRoutineValue, word1, word2, value.bits());
        return advance(writer);
    }
    READABLE_FUNCTION(addMaterialPacketValue, Verified);

    // addSphereMapRoutine, addEnvironmentMapRoutine: routines that take nothing in their
    // quadword. Return the cursor.
    ee::Ptr<Routine> addSphereMapRoutine(ee::Ptr<PacketWriter> writer)
    {
        addRoutine(writer, kRoutineSphereMap, 0, 0, 0);
        return advance(writer);
    }
    READABLE_FUNCTION(addSphereMapRoutine, Draft);

    ee::Ptr<Routine> addEnvironmentMapRoutine(ee::Ptr<PacketWriter> writer)
    {
        addRoutine(writer, kRoutineEnvironmentMap, 0, 0, 0);
        return advance(writer);
    }
    READABLE_FUNCTION(addEnvironmentMapRoutine, Draft);

    // initMaterialLayers: both layers cleared, and set to what a material starts
    // with: a GIF tag of seven A+D registers with PRIM's Gouraud bit, the registers' numbers,
    // and an opaque-to-alpha blend with the depth test GREATER. The material's
    // type gets bit 0 (setMaterialState clears it). Returns the word that holds it.
    uint64_t initMaterialLayers(ee::Ptr<Material> material)
    {
        constexpr uint32_t kDmaRet = 0x60, kRegisterQuadwords = 8;
        // The block's VIF codes: STCYCL 4, 4 and an UNPACK of its 8 quadwords to VU1 0 (from TOPS).
        constexpr uint32_t kCycle = vif::stcycl(4, 4), kUnpackRegisters = vif::unpack(vif::kUnpackV4_32, kRegisterQuadwords, vif::kUnpackFromTops | 0u);
        // The tag's prim is a line's type with the Gouraud bit; the strips drawn with the material take its
        // attributes, bits 3-10 (bindTexture, copyPrimToProgramBits). (ZGE2's tag names no VU1 program: it has
        // no material routines.)
        constexpr uint64_t kGifTag =
            gs::GifTag{.loops = kRegisterQuadwords - 1, .endOfPacket = 1, .setsPrim = 1, .prim = gs::kLine | gs::kGouraud, .registers = 1}
                .bits() |
            kTagProgram.placed(game::readable::perGame(0xDull, 0ull, 0xDull));
        constexpr uint64_t kTex1K = uint64_t{0xFE0} << gs::Tex1::kK.shift;
        constexpr uint64_t kTex0Tcc = 1ull << gs::Tex0::kTcc.shift;
        constexpr uint64_t kAlphaSource =
            gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendFrame, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();
        constexpr uint64_t kTestDepthGreater = gs::Test{.zte = 1, .ztst = gs::kDepthGreater}.bits();
        for (uint32_t layer = 0; layer < kMaterialLayerCount; ++layer)
        {
            memoryFill(materialLayerAt(material, layer), 0, kMaterialLayerClearedBytes);
            MaterialLayer &block = materialLayer(material, layer);
            block.dmaQwords = kRegisterQuadwords;
            block.dmaId = kDmaRet;
            block.stcycl = kCycle;
            block.unpack = kUnpackRegisters;
            block.gifTag[0] = kGifTag;
            block.gifTag[1] = gs::kAddressData;
            setLayerQwordCount(material, layer, kRegisterQuadwords);
            block.clamp.gsRegister = gs::kClamp1;
            block.tex1.data = kTex1K;
            block.tex1.gsRegister = gs::kTex1_1;
            block.alpha.data = kAlphaSource;
            block.alpha.gsRegister = gs::kAlpha1;
            block.test.data = kTestDepthGreater;
            block.test.gsRegister = gs::kTest1;
            block.miptbp1.gsRegister = gs::kMipTbp1_1;
            block.texflush.gsRegister = gs::kTexFlush;
            block.tex0.data = kTex0Tcc;
            block.tex0.gsRegister = gs::kTex0_1;
        }
        return markMaterialStateChanged(*material);
    }
    READABLE_FUNCTION(initMaterialLayers, Verified);

    namespace
    {
        // Mip level `level`'s TBW: the texture's width halved for each level, in the GS's units of 64 pixels (at
        // least 1).
        uint32_t levelBufferWidth(uint32_t baseWidth, uint32_t level)
        {
            const uint32_t width = gs::bufferWidthUnits(baseWidth >> level);
            return width != 0 ? width : 1;
        }

        // The layer's TEX0, TEX1's level count (MXL) and MIPTBP1 from the texture's levels and CLUT, as
        // setLayerTextureRegisters writes them (ZGE2's own body below).
        void setTextureRegisters(MaterialLayer &block, const GsTexture &gs)
        {
            uint64_t tex0 = block.tex0.data;
            tex0 = withField(tex0, gs::Tex0::kTbp0, gs::blockOf(gs.levels[0].gsAddress));
            tex0 = withField(tex0, gs::Tex0::kTbw, levelBufferWidth(gs.baseWidth, 0));
            tex0 = withField(tex0, gs::Tex0::kPsm, gs.psm);
            tex0 = withField(tex0, gs::Tex0::kTw, gs.log2Width);
            tex0 = withField(tex0, gs::Tex0::kTh, gs.log2Height);
            tex0 = withField(tex0, gs::Tex0::kCbp, gs::blockOf(gs.clutGsAddress));
            tex0 = withField(tex0, gs::Tex0::kCpsm, gs.clutPsm);
            tex0 = withField(tex0, gs::Tex0::kCsm, gs::kClutCsm1);
            tex0 = withField(tex0, gs::Tex0::kCsa, 0);
            tex0 = withField(tex0, gs::Tex0::kCld, gs::kClutLoadAlways);
            block.tex0.data = tex0;

            block.tex1.data = withField(block.tex1.data, gs::Tex1::kMxl, gs.levelCount - 1u);

            uint64_t miptbp1 = block.miptbp1.data;
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp1, gs::blockOf(gs.levels[1].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw1, levelBufferWidth(gs.baseWidth, 1));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp2, gs::blockOf(gs.levels[2].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw2, levelBufferWidth(gs.baseWidth, 2));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp3, gs::blockOf(gs.levels[3].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw3, levelBufferWidth(gs.baseWidth, 3));
            block.miptbp1.data = miptbp1;
        }
    }

    // setLayerTextureRegisters: a layer's registers for its texture: TME in the tag's PRIM
    // (off with no texture), and TEX0, TEX1's level count and MIPTBP1 from the
    // texture's levels and CLUT; a texture whose flags say so is cut out
    // (kStateCutout); the record at the material's +8, if any, told. Last,
    // copyPrimToProgramBits.
    uint64_t setLayerTextureRegisters(ee::Ptr<Material> material, ee::Ptr<Texture> texture, uint32_t layer)
    {
        MaterialLayer &block = materialLayer(material, layer);
        uint64_t tag = block.gifTag[0];
        const uint64_t prim = fieldOf(tag, kTagPrim);
        if (!texture)
            block.gifTag[0] = withField(tag, kTagPrim, prim & ~uint64_t{kPrimTextured});
        else
        {
            block.gifTag[0] = withField(tag, kTagPrim, prim | kPrimTextured);
            const GsTexture &gs = texture->gs;

            uint64_t tex0 = block.tex0.data;
            tex0 = withField(tex0, gs::Tex0::kTbp0, gs::blockOf(gs.levels[0].gsAddress));
            tex0 = withField(tex0, gs::Tex0::kTbw, levelBufferWidth(gs.baseWidth, 0));
            tex0 = withField(tex0, gs::Tex0::kPsm, gs.psm);
            tex0 = withField(tex0, gs::Tex0::kTw, gs.log2Width);
            tex0 = withField(tex0, gs::Tex0::kTh, gs.log2Height);
            tex0 = withField(tex0, gs::Tex0::kCbp, gs::blockOf(gs.clutGsAddress));
            tex0 = withField(tex0, gs::Tex0::kCpsm, gs.clutPsm);
            tex0 = withField(tex0, gs::Tex0::kCsm, gs::kClutCsm1);
            tex0 = withField(tex0, gs::Tex0::kCsa, 0);
            tex0 = withField(tex0, gs::Tex0::kCld, gs::kClutLoadAlways);
            block.tex0.data = tex0;

            block.tex1.data = withField(block.tex1.data, gs::Tex1::kMxl, gs.levelCount - 1u);

            uint64_t miptbp1 = block.miptbp1.data;
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp1, gs::blockOf(gs.levels[1].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw1, levelBufferWidth(gs.baseWidth, 1));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp2, gs::blockOf(gs.levels[2].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw2, levelBufferWidth(gs.baseWidth, 2));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbp3, gs::blockOf(gs.levels[3].gsAddress));
            miptbp1 = withField(miptbp1, gs::MipTbp1::kTbw3, levelBufferWidth(gs.baseWidth, 3));
            block.miptbp1.data = miptbp1;

            const int32_t flags = textureFlagsByte(texture);
            if ((flags & 1) != 0)
                setMaterialState(material, 0, kStateCutout, 1);
            if (const ee::Ptr<MaterialTable> owner = material->owner)
                owner->textureEpoch = 0;
        }
        return copyPrimToProgramBits(block);
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void setMaterialState(ee::Ptr<Material> material, uint32_t layer, uint32_t state, uint32_t value)
        {
            ee::tailCall(sym::setMaterialState, material, layer, state, value);
        }
        }
    }

    // setLayerTextureRegistersOnly: ZGE2's setLayerTextureRegisters: the
    // same registers and the cut-out state, and nothing else: no texture
    // epoch to clear and no program bits (ZGE2's layers have no routines).
    void setLayerTextureRegistersOnly(ee::Ptr<Material> material, ee::Ptr<Texture> texture, uint32_t layer)
    {
        MaterialLayer &block = materialLayer(material, layer);
        const uint64_t tag = block.gifTag[0];
        const uint64_t prim = fieldOf(tag, kTagPrim);
        if (!texture)
        {
            block.gifTag[0] = withField(tag, kTagPrim, prim & ~uint64_t{kPrimTextured});
            return;
        }
        block.gifTag[0] = withField(tag, kTagPrim, prim | kPrimTextured);
        setTextureRegisters(block, texture->gs);
        if ((textureFlagsByte(texture) & 1) != 0)
            tail::setMaterialState(material, 0, kStateCutout, 1);
    }
    READABLE_FUNCTION_OWN(setLayerTextureRegisters, Verified, dm2, setLayerTextureRegistersOnly);

    // materialEffectOf (no counterpart in the game: the routines as the VU1 programs run them, read back for a
    // renderer that draws without them). The list is the first layer's routines, as many as the packet's size says
    // (setMaterialPacketSize), ending at kRoutineEnd; the value routine is the grass shells' and ends the effect.
    bool materialEffectOf(ee::Ptr<Material> material, MaterialEffect &out)
    {
        out = MaterialEffect{};
        const uint32_t quadwords = layerQwordCount(material, 0);
        if (quadwords < kGifTagRegisters)
            return false;
        const uint32_t routineQuadwords = quadwords - kGifTagRegisters;
        const ee::Ptr<Routine> routines = routinesOf(material);
        for (uint32_t at = 0; at < routineQuadwords; ++at)
        {
            const Routine &routine = routines[at];
            switch (routine.words[0])
            {
            case kRoutineEnd:
                return true;
            case kRoutineEnvironmentMap:
                continue;
            case kRoutineValue:
                out.grass = true;
                out.shells = static_cast<int32_t>(routine.words[1]);
                out.alphaStep = static_cast<int32_t>(routine.words[2]);
                out.shellStep = ee::Float::fromBits(routine.words[3]).raw();
                return true;
            case kRoutineMatrix:
            case kRoutineSphereMap:
            {
                if (out.uvTransformCount == MaterialEffect::kMaxUvTransforms)
                    return false;
                MaterialEffect::UvTransform &uv = out.uvTransforms[out.uvTransformCount++];
                uv.sphereMap = routine.words[0] == kRoutineSphereMap;
                if (!uv.sphereMap)
                {
                    if (at + 4 >= routineQuadwords)
                        return false;
                    const ee::Ptr<Vec4> rows = rowsAt(routines + (at + 1));
                    for (uint32_t row = 0; row < 4; ++row)
                    {
                        const Vec4 &r = rows[row];
                        uv.rows[row][0] = r.x.raw();
                        uv.rows[row][1] = r.y.raw();
                        uv.rows[row][2] = r.z.raw();
                        uv.rows[row][3] = r.w.raw();
                    }
                    at += 4;
                }
                continue;
            }
            default:
                return false;
            }
        }
        return true;
    }

}
