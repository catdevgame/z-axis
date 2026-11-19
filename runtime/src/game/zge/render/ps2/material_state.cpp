// A material's render state as the GS takes it: every engine setting and
// every GS field a layer has, by number, written into the layer's GS
// registers (zge/render/material_state.cpp loads them from a MATI). The
// settings the first group and the last group name are combinations of the
// same GS fields (TEST, ALPHA, TEX1, CLAMP, TEX0 and PRIM in the layer's GIF
// tag; GS User's Manual 7.2). BMX XXX (PS2) has the same function; ZGE2's
// (DM2) is the own body setMaterialRegisterState.

#include "zge/render/material.h"

#include "zge/sound/streams.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include "sym/zge/render/ps2/material_state.cpp.h"

namespace zge
{
    namespace
    {
        enum BlendMode : uint32_t // kStateBlendMode's values
        {
            kBlendNone = 0,
            kBlendHalf = 1,        // (source - frame) * 0x40 / 0x80 + frame
            kBlendAdd = 2,         // source * alpha + frame
            kBlendSubtract = 3,    // frame - source * alpha
            kBlendAlpha = 4,       // (source - frame) * alpha + frame
            kBlendModeCount = 5,
        };
        // The blend modes' ALPHA words (gs::Alpha: (A - B) * C / 0x80 + D).
        constexpr uint64_t kAlphaHalf =
            gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendFrame, .c = gs::kBlendFix, .d = gs::kBlendFrame, .fix = 0x40}.bits();
        constexpr uint64_t kAlphaAdd = gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendZero, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();
        constexpr uint64_t kAlphaSubtract =
            gs::Alpha{.a = gs::kBlendZero, .b = gs::kBlendSource, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();
        constexpr uint64_t kAlphaByAlpha = gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendFrame, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();
        // The source colour as it is: (source - frame) * FIX + frame with FIX 0x80 (1.0).
        constexpr uint64_t kAlphaSourceColourInputs =
            gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendFrame, .c = gs::kBlendFix, .d = gs::kBlendFrame}.bits();
        constexpr uint64_t kAlphaFixOne = 0x80;

        constexpr uint32_t kCutoutReference = 0x40;       // the cutout's alpha test: alpha above 0x40 drawn
    }

    namespace
    {
        // ALPHA's FIX (bits 32-39 of the register), which the original
        // writes as the byte it is.
        uint8_t &alphaFix(GsAdRegister &alpha) { return reinterpret_cast<uint8_t *>(&alpha.data)[4]; }
    }

    // setMaterialState: state `state` of the layer set to `value`. Bit 0 of the
    // material's type goes (a layer has changed), and every path but one ends
    // in copyPrimToProgramBits, whose result it returns.
    uint64_t setMaterialState(ee::Ptr<Material> material, uint32_t layer, uint32_t state, uint32_t value)
    {
        MaterialLayer &block = materialLayer(material, layer);
        material->type &= ~1u;

        // Another state set the same way (the original calls itself).
        const auto setAlso = [&](uint32_t otherState, uint32_t otherValue)
        {
            setMaterialState(material, layer, otherState, otherValue);
        };

        // The paths' common ends, as the original's labels run into each other.
        const auto finishTag = [&]
        {
            return copyPrimToProgramBits(block);
        };
        const auto finish = [&]
        {
            return finishTag();
        };
        const auto storeTest = [&](uint64_t test)
        {
            block.test.data = test;
            return finish();
        };
        const auto orIntoTest = [&](uint64_t test, uint64_t bits)
        {
            return storeTest(test | bits);
        };
        const auto setTestField = [&](gs::Field field)
        {
            return orIntoTest(withField(block.test.data, field, 0), static_cast<uint64_t>(value & ((1u << field.bits) - 1)) << field.shift);
        };
        const auto prim = [&] { return fieldOf(block.gifTag[0], kTagPrim); };
        const auto setPrim = [&](uint64_t bits)
        {
            block.gifTag[0] = withField(block.gifTag[0], kTagPrim, bits);
            return finish();
        };
        const auto setPrimBit = [&](uint64_t bit)
        {
            if (value != 0)
                return setPrim(prim() | bit);
            return setPrim(prim() & ~bit);
        };
        const auto setTex0Bits = [&](uint64_t tex0, uint64_t bits)
        {
            block.tex0.data = tex0 | bits;
            return finish();
        };
        const auto tex0WithoutTfx = [&] { return withField(block.tex0.data, gs::Tex0::kTfx, 0); };
        const auto setClamp = [&](gs::Field field)
        {
            block.clamp.data = withField(block.clamp.data, field, value);
            return finish();
        };
        const auto setTex0 = [&](gs::Field field)
        {
            block.tex0.data = withField(block.tex0.data, field, value);
            return finish();
        };
        const auto setTex1 = [&](gs::Field field)
        {
            block.tex1.data = withField(block.tex1.data, field, value);
            return finish();
        };
        const auto setAlpha = [&](gs::Field field)
        {
            block.alpha.data = withField(block.alpha.data, field, value);
            return finish();
        };
        const auto setAlphaWord = [&](uint64_t word)
        {
            block.alpha.data = word;
            return finish();
        };
        // The source colour as it is: FIX 0x80 over (source - frame) + frame.
        const auto alphaSourceColour = [&]
        {
            block.alpha.data = withField(block.alpha.data, gs::Alpha::kInputs, kAlphaSourceColourInputs);
            alphaFix(block.alpha) = kAlphaFixOne;
        };
        const auto setFilter = [&](uint64_t mmin)
        {
            block.tex1.data = withField(block.tex1.data | 1ull << gs::Tex1::kMmag.shift, gs::Tex1::kMmin, mmin);
            return finish();
        };
        const auto setWrap = [&](gs::Field field, uint32_t wrap)
        {
            block.clamp.data = withField(block.clamp.data, field, wrap);
            return finish();
        };
        const auto setCullBits = [&](uint32_t bits)
        {
            return withField(block.gifTag[1], kTagCull, bits);
        };

        if (state >= kMaterialStateCount)
            return finish();
        switch (state)
        {
        case kStateBlending:
            setAlso(kStateAbe, value);
            return finishTag();
        case kStateTextured:
            return setPrimBit(kPrimTextured);
        case kStateModulate:
            if (value != 0)
            {
                block.tex0.data = tex0WithoutTfx();
                return finish();
            }
            return setTex0Bits(tex0WithoutTfx(), 1ull << gs::Tex0::kTfx.shift);
        case kStateDepthGreater:
            if (value != 0)
            {
                block.test.data = withField(block.test.data, gs::Test::kZtst, gs::kDepthGreater);
                return finish();
            }
            block.test.data = withField(block.test.data, gs::Test::kZtst, gs::kDepthAlways);
            return finish();
        case kStateDepthWrite:
            if (value != 0)
                return storeTest(withField(block.test.data, gs::Test::kAte, 0));
            setAlso(kStateAbe, 1);
            {
                uint64_t test = withField(block.test.data, gs::Test::kAte, 1);
                test = withField(test, gs::Test::kAtst, 0);
                return storeTest(withField(test, gs::Test::kAfail, gs::kFailFrameOnly));
            }
        case kStateBlendMode:
            setAlso(kStateAbe, 1);
            if (value >= kBlendModeCount)
                return finish();
            switch (value)
            {
            case kBlendNone:
                setAlso(kStateAbe, 0);
                return finishTag();
            case kBlendHalf:
                return setAlphaWord(kAlphaHalf);
            case kBlendAdd:
                return setAlphaWord(kAlphaAdd);
            case kBlendSubtract:
                return setAlphaWord(kAlphaSubtract);
            default:
                return setAlphaWord(kAlphaByAlpha);
            }
        case kStateGouraud:
            return setPrimBit(kPrimGouraud);
        case kStateTwoSided:
            if (value != 0)
            {
                material->flags |= 1ull << kMaterialFlagTwoSided;
                setAlso(kStateCullMode, 1);
                return finishTag();
            }
            material->flags &= ~(1ull << kMaterialFlagTwoSided);
            setAlso(kStateCullMode, 3);
            return finishTag();
        case kStateDestinationAlpha:
            // DATM 1, and DATE as asked.
            return orIntoTest(withField(withField(block.test.data, gs::Test::kDate, 0), gs::Test::kDatm, 1),
                              static_cast<uint64_t>(value != 0 ? 1 : 0) << gs::Test::kDate.shift);
        case kStateFilter:
            if (value == 0)
            {
                block.tex1.data = withField(withField(block.tex1.data, gs::Tex1::kMmag, 0), gs::Tex1::kMmin, 0);
                return finish();
            }
            if (value == 1)
                return setFilter(gs::kMinLinear);
            if (value == 2)
                return setFilter(gs::kMinLinearMipLinear);
            return finishTag();
        case kStateCullMode:
            if (value <= 3)
            {
                uint64_t cull;
                if (value == 2)
                    cull = setCullBits(kTagCullMode2);
                else if (value == 3)
                    cull = setCullBits(kTagCullMode3);
                else
                    cull = withField(block.gifTag[1], kTagCull, 0);
                block.gifTag[1] = cull;
            }
            material->flags = withField(material->flags, kMaterialFlagCullMode, 2, value);
            return finish();
        case kStateRepeatU:
            if (value == 0)
                return setWrap(gs::Clamp::kWms, gs::kWrapRepeat);
            if (value != 1)
                return finishTag();
            return setWrap(gs::Clamp::kWms, gs::kWrapClamp);
        case kStateRepeatV:
            if (value == 0)
                return setWrap(gs::Clamp::kWmt, gs::kWrapRepeat);
            if (value != 1)
                return finishTag();
            return setWrap(gs::Clamp::kWmt, gs::kWrapClamp);
        case kStateKind:
            material->type = static_cast<uint32_t>(withField(material->type, 1, 8, value));
            buildMaterialLayers(material);
            return finishTag();
        case kStateFlag:
            if (value != 0)
            {
                material->flags |= 1ull << kMaterialFlagState;
                return finish();
            }
            material->flags &= ~(1ull << kMaterialFlagState);
            return finish();
        case kStatePrimType:
            return setPrim((prim() & gs::kPrimAttributes) | value);
        case kStateIip:
            return setPrimBit(kPrimGouraud);
        case kStateTme:
            return setPrimBit(kPrimTextured);
        case kStateFge:
            return setPrimBit(kPrimFogged);
        case kStateAbe:
            if (value != 0)
            {
                block.gifTag[0] = withField(block.gifTag[0], kTagPrim, prim() | kPrimBlended);
                if (layer != 0)
                    return finish();
            }
            else
            {
                block.gifTag[0] = withField(block.gifTag[0], kTagPrim, prim() & ~uint64_t{kPrimBlended});
                if (layer != 0)
                    return finish();
            }
            material->flags = withField(material->flags, kMaterialFlagBlended, 1, value != 0 ? 1 : 0);
            markMaterialOwnerDirty(material);
            return finishTag();
        case kStateWms:
            return setClamp(gs::Clamp::kWms);
        case kStateWmt:
            return setClamp(gs::Clamp::kWmt);
        case kStateMinU:
            return setClamp(gs::Clamp::kMinU);
        case kStateMaxU:
            return setClamp(gs::Clamp::kMaxU);
        case kStateMinV:
            return setClamp(gs::Clamp::kMinV);
        case kStateMaxV:
            return setClamp(gs::Clamp::kMaxV);
        case kStateTfx:
            return setTex0(gs::Tex0::kTfx);
        case kStateTcc:
            return setTex0(gs::Tex0::kTcc);
        case kStateLcm:
            block.tex1.data = withField(block.tex1.data, gs::Tex1::kLcm, value);
            return finish();
        case kStateMxl:
            return setTex1(gs::Tex1::kMxl);
        case kStateMmag:
            return setTex1(gs::Tex1::kMmag);
        case kStateMmin:
            return setTex1(gs::Tex1::kMmin);
        case kStateMtba:
            return setTex1(gs::Tex1::kMtba);
        case kStateL:
            return setTex1(gs::Tex1::kL);
        case kStateK:
            return setTex1(gs::Tex1::kK);
        case kStateAlphaA:
            block.alpha.data = withField(block.alpha.data, gs::Alpha::kA, value);
            return finish();
        case kStateAlphaB:
            return setAlpha(gs::Alpha::kB);
        case kStateAlphaC:
            return setAlpha(gs::Alpha::kC);
        case kStateAlphaD:
            return setAlpha(gs::Alpha::kD);
        case kStateAlphaFix:
            alphaFix(block.alpha) = static_cast<uint8_t>(value);
            return finish();
        case kStateAte:
            return storeTest(withField(block.test.data, gs::Test::kAte, value));
        case kStateAtst:
            return setTestField(gs::Test::kAtst);
        case kStateAref:
            return setTestField(gs::Test::kAref);
        case kStateAfail:
            return setTestField(gs::Test::kAfail);
        case kStateDate:
            return setTestField(gs::Test::kDate);
        case kStateDatm:
            return setTestField(gs::Test::kDatm);
        case kStateZte:
            return setTestField(gs::Test::kZte);
        case kStateZtst:
            return setTestField(gs::Test::kZtst);
        case kStateSourceColourGequal:
        case kStateSourceColourGequal2:
            alphaSourceColour();
            setAlso(kStateAbe, 1);
            return storeTest(withField(block.test.data, gs::Test::kZtst, gs::kDepthGequal));
        case kStateSourceColourGreater:
            alphaSourceColour();
            setAlso(kStateAbe, 0);
            return orIntoTest(block.test.data, static_cast<uint64_t>(gs::kDepthGreater) << gs::Test::kZtst.shift);
        case kStateCutout:
            if (value != 0)
            {
                uint64_t test = withField(block.test.data, gs::Test::kAte, 1);
                test = withField(test, gs::Test::kAtst, gs::kAlphaGreater);
                test = withField(test, gs::Test::kAref, kCutoutReference);
                block.test.data = withField(test, gs::Test::kAfail, 0);
                setAlso(kStateAbe, 1);
                block.alpha.data = withField(block.alpha.data, gs::Alpha::kInputs, kAlphaByAlpha);
                return setTex0Bits(block.tex0.data, 1ull << gs::Tex0::kTcc.shift);
            }
            return storeTest(withField(block.test.data, gs::Test::kAte, 0));
        case kStateAlphaReference:
        {
            uint64_t test = block.test.data;
            if (value == 0xFFFFFFFFu)
                return storeTest(withField(test, gs::Test::kAte, 0));
            test = withField(test, gs::Test::kAte, 1);
            test = withField(test, gs::Test::kAtst, gs::kAlphaGreater);
            test = withField(test, gs::Test::kAref, value);
            block.test.data = test;
            // (Frame-buffer-only stays: kStateDepthWrite's.)
            if (fieldOf(test, gs::Test::kAfail) == gs::kFailFrameOnly)
                return finish();
            return storeTest(withField(test, gs::Test::kAfail, 0));
        }
        case kStateSourceColour:
            if (value != 0)
            {
                alphaSourceColour();
                return finish();
            }
            setAlso(kStateBlendMode, 1);
            return finishTag();
        default: // kStateUnused9, kStateUnused15
            return finish();
        }
    }

    namespace
    {
        // ZGE2's GS-field states: each sets one field of one of the layer's registers.
        enum LayerRegister : uint8_t { kRegClamp, kRegTex0, kRegTex1, kRegAlpha, kRegTest };
        struct RegisterField
        {
            uint32_t state;
            LayerRegister reg;
            gs::Field field;
        };
        constexpr RegisterField kRegisterFields[] = {
            {kStateWms, kRegClamp, gs::Clamp::kWms},    {kStateWmt, kRegClamp, gs::Clamp::kWmt},
            {kStateMinU, kRegClamp, gs::Clamp::kMinU},  {kStateMaxU, kRegClamp, gs::Clamp::kMaxU},
            {kStateMinV, kRegClamp, gs::Clamp::kMinV},  {kStateMaxV, kRegClamp, gs::Clamp::kMaxV},
            {kStateTfx, kRegTex0, gs::Tex0::kTfx},      {kStateTcc, kRegTex0, gs::Tex0::kTcc},
            {kStateLcm, kRegTex1, gs::Tex1::kLcm},      {kStateMxl, kRegTex1, gs::Tex1::kMxl},
            {kStateMmag, kRegTex1, gs::Tex1::kMmag},    {kStateMmin, kRegTex1, gs::Tex1::kMmin},
            {kStateMtba, kRegTex1, gs::Tex1::kMtba},    {kStateL, kRegTex1, gs::Tex1::kL},
            {kStateK, kRegTex1, gs::Tex1::kK},          {kStateAlphaA, kRegAlpha, gs::Alpha::kA},
            {kStateAlphaB, kRegAlpha, gs::Alpha::kB},   {kStateAlphaC, kRegAlpha, gs::Alpha::kC},
            {kStateAlphaD, kRegAlpha, gs::Alpha::kD},   {kStateAlphaFix, kRegAlpha, gs::Alpha::kFix},
            {kStateAte, kRegTest, gs::Test::kAte},      {kStateAtst, kRegTest, gs::Test::kAtst},
            {kStateAref, kRegTest, gs::Test::kAref},    {kStateAfail, kRegTest, gs::Test::kAfail},
            {kStateDate, kRegTest, gs::Test::kDate},    {kStateDatm, kRegTest, gs::Test::kDatm},
            {kStateZte, kRegTest, gs::Test::kZte},      {kStateZtst, kRegTest, gs::Test::kZtst},
        };
        GsAdRegister &layerRegister(MaterialLayer &block, LayerRegister reg)
        {
            switch (reg)
            {
            case kRegClamp: return block.clamp;
            case kRegTex0: return block.tex0;
            case kRegTex1: return block.tex1;
            case kRegAlpha: return block.alpha;
            default: return block.test;
            }
        }
    }

    // setMaterialRegisterState: ZGE2's setMaterialState. The same states over the same registers and flags
    // (MaterialState's ZGE2 numbers), with no program bits to copy and nothing returned: ZGE2 has no material
    // routines, so no path ends in copyPrimToProgramBits. Where ZGE3 changed a state: kStateDepthWrite sets nothing;
    // kStateBlendMode's 1 blends by alpha (ZGE3 made 1 the half blend and moved by-alpha to 4); kStateFilter blends
    // the layer and turns its alpha test off after the filter, whatever the value; kStateCullMode 0 sets nothing;
    // kStateAlphaReference always clears AFAIL (ZGE3 keeps kStateDepthWrite's frame-buffer-only).
    void setMaterialRegisterState(ee::Ptr<Material> material, uint32_t layer, uint32_t state, uint32_t value)
    {
        MaterialLayer &block = materialLayer(material, layer);
        const auto prim = [&] { return fieldOf(block.gifTag[0], kTagPrim); };
        const auto setPrim = [&](uint64_t bits)
        {
            block.gifTag[0] = withField(block.gifTag[0], kTagPrim, bits);
        };
        const auto setPrimBit = [&](uint64_t bit) { setPrim(value != 0 ? prim() | bit : prim() & ~bit); };
        const auto setFlag = [&](uint32_t bit)
        {
            if (value != 0)
                material->flags |= 1ull << bit;
            else
                material->flags &= ~(1ull << bit);
        };
        // The source colour as it is: FIX 0x80 over (source - frame) + frame.
        const auto alphaSourceColour = [&]
        {
            block.alpha.data = withField(block.alpha.data, gs::Alpha::kInputs, kAlphaSourceColourInputs);
            alphaFix(block.alpha) = kAlphaFixOne;
        };

        // A state set as another state starts over as that one (the original jumps to its start).
        for (;;)
        {
            materialStateChangedWord(*material) &= ~1u;
            if (state >= kMaterialStateCount)
                return;
            switch (state)
            {
            case kStateBlending:
                state = kStateAbe;
                continue;
            case kStateTextured:
                return setPrimBit(kPrimTextured);
            case kStateModulate:
                block.tex0.data = withField(block.tex0.data, gs::Tex0::kTfx, value != 0 ? 0 : 1);
                return;
            case kStateDepthGreater:
                block.test.data = withField(block.test.data, gs::Test::kZtst, value != 0 ? gs::kDepthGreater : gs::kDepthAlways);
                return;
            case kStateDepthWrite:
                return;
            case kStateBlendMode:
                setMaterialRegisterState(material, layer, kStateAbe, 1);
                switch (value)
                {
                case kBlendNone:
                    state = kStateAbe;
                    continue;
                case 1:
                    block.alpha.data = kAlphaByAlpha;
                    return;
                case kBlendAdd:
                    block.alpha.data = kAlphaAdd;
                    return;
                case kBlendSubtract:
                    block.alpha.data = kAlphaSubtract;
                    return;
                default:
                    return;
                }
            case kStateGouraud:
                return setPrimBit(kPrimGouraud);
            case kStateTwoSided:
                setFlag(kMaterialFlagTwoSided);
                value = value != 0 ? 1 : 3;
                state = kStateCullMode;
                continue;
            case kStateDestinationAlpha:
                // DATM 1, and DATE as asked.
                block.test.data = withField(withField(block.test.data, gs::Test::kDate, 0), gs::Test::kDatm, 1) |
                                  static_cast<uint64_t>(value != 0 ? 1 : 0) << gs::Test::kDate.shift;
                return;
            case kStateFilter:
                if (value == 0)
                    block.tex1.data = withField(withField(block.tex1.data, gs::Tex1::kMmag, 0), gs::Tex1::kMmin, 0);
                else if (value == 1)
                    block.tex1.data = withField(block.tex1.data | 1ull << gs::Tex1::kMmag.shift, gs::Tex1::kMmin, gs::kMinLinear);
                else if (value == 2)
                    block.tex1.data = withField(block.tex1.data | 1ull << gs::Tex1::kMmag.shift, gs::Tex1::kMmin, gs::kMinLinearMipLinear);
                setMaterialRegisterState(material, layer, kStateAbe, 1);
                block.test.data = withField(block.test.data, gs::Test::kAte, 0);
                return;
            case kStateCullMode:
            {
                if (value == 0 || value > 3)
                    return;
                material->flags = withField(material->flags, kMaterialFlagCullMode, 2, value);
                const uint64_t tag = withField(block.gifTag[1], kTagCull, 0);
                block.gifTag[1] = value == 1 ? tag : tag | kTagCull.placed(value == 2 ? kTagCullMode2 : kTagCullMode3);
                return;
            }
            case kStateFlag:
                return setFlag(kMaterialFlagState);
            case kStatePrimType:
                return setPrim((prim() & gs::kPrimAttributes) | value);
            case kStateIip:
                return setPrimBit(kPrimGouraud);
            case kStateTme:
                return setPrimBit(kPrimTextured);
            case kStateFge:
                return setPrimBit(kPrimFogged);
            case kStateAbe:
                setPrimBit(kPrimBlended);
                if (layer != 0)
                    return;
                material->flags = withField(material->flags, kMaterialFlagBlended, 1, value != 0 ? 1 : 0);
                return game::tail(markMaterialOwnerDirty, material);
            case kStateSourceColourGequal:
            case kStateSourceColourGequal2:
                alphaSourceColour();
                setMaterialRegisterState(material, layer, kStateAbe, 1);
                block.test.data = withField(block.test.data, gs::Test::kZtst, gs::kDepthGequal);
                return;
            case kStateSourceColourGreater:
                alphaSourceColour();
                setMaterialRegisterState(material, layer, kStateAbe, 0);
                block.test.data |= static_cast<uint64_t>(gs::kDepthGreater) << gs::Test::kZtst.shift;
                return;
            case kStateCutout:
                if (value != 0)
                {
                    uint64_t test = withField(block.test.data, gs::Test::kAte, 1);
                    test = withField(test, gs::Test::kAtst, gs::kAlphaGreater);
                    test = withField(test, gs::Test::kAref, kCutoutReference);
                    block.test.data = withField(test, gs::Test::kAfail, 0);
                    setMaterialRegisterState(material, layer, kStateAbe, 1);
                    block.alpha.data = withField(block.alpha.data, gs::Alpha::kInputs, kAlphaByAlpha);
                    block.tex0.data |= 1ull << gs::Tex0::kTcc.shift;
                    return;
                }
                block.test.data = withField(block.test.data, gs::Test::kAte, 0);
                return;
            case kStateAlphaReference:
            {
                if (value == 0xFFFFFFFFu)
                {
                    block.test.data = withField(block.test.data, gs::Test::kAte, 0);
                    return;
                }
                uint64_t test = withField(block.test.data, gs::Test::kAte, 1);
                test = withField(test, gs::Test::kAtst, gs::kAlphaGreater);
                test = withField(test, gs::Test::kAref, value);
                block.test.data = withField(test, gs::Test::kAfail, 0);
                return;
            }
            case kStateSourceColour:
                if (value != 0)
                {
                    alphaSourceColour();
                    return;
                }
                state = kStateBlendMode;
                value = 1;
                continue;
            default:
                for (const RegisterField &field : kRegisterFields)
                    if (field.state == state)
                    {
                        GsAdRegister &reg = layerRegister(block, field.reg);
                        reg.data = withField(reg.data, field.field, value);
                        return;
                    }
                return; // kStateUnused9, kStateUnused15
            }
        }
    }
    READABLE_FUNCTION_OWN(setMaterialState, Verified, dm2, setMaterialRegisterState);
}
