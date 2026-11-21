// The shadow pass as the PS2 draws it (zge/render/shadow.h; shadow.cpp is the
// shadow volumes' game side): the pass's two chains in the shadow packet (the
// shadow buffer cleared, the casters' volumes drawn into it in GS context 2,
// ZGE2's 1; the buffer laid over the frame), each caster's volume as VU1
// takes it (the GS state, the light, the colours, the matrices, the
// silhouette's points, the volume's microprogram), the split screen's disc,
// and the packet's two buffers.

#include "zge/render/shadow.h"
#include "zge/render/ps2/shadow_pass.h"

#include "ee/mirror.h"
#include "ee/dispatch.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "sce/packet_calls.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/render/ps2/frame_packets.h"
#include "zge/render/renderer.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/render/packet.h"
#include "zge/render/part.h"

#include <array>
#include <cstring>
#include "sym/zge/render/ps2/shadow_pass.cpp.h"

namespace zge
{
    namespace
    {
        using ee::Float;

        // VU1 microprograms (MSCAL addresses): the GS state block (each game's
        // microcode has it elsewhere: DM2's applyShadowBuffer calls 0x1C, BMX's
        // applyShadowBuffer and buildShadowVolumePacket 0x26), the volume.
        constexpr uint32_t kProgramGsState = game::readable::perGame(0x24u, 0x1Cu, 0x26u);
        constexpr uint32_t kProgramVolume = game::readable::perGame(0x1Cu, 0x18u, 0x1Cu);
        // VU1's far scale for the volume's points (ZGE2's microcode takes 1).
        constexpr float kVolumeFarScale = game::readable::perGame(1.25f, 1.0f, 1.25f);
        // The VU1 address the volume's points start at (the header block,
        // the light, the colours, the tags and the matrices are below), and
        // how many go in one unpack (64 edges).
        constexpr uint32_t kFirstPointAddress = 0x20u;
        constexpr uint32_t kPointsPerUnpack = 0x80u;

        // The shadow pass draws in context 2 (ALPHA_2, FRAME_2, ZBUF_2); ZGE2
        // in context 1 (ALPHA_1, FRAME_1, ZBUF_1: DM2's applyShadowBuffer and
        // buildShadowVolumePacket).
        constexpr uint32_t kGsShadowAlpha = game::readable::perGame(gs::kAlpha2, gs::kAlpha1, gs::kAlpha2);
        constexpr uint32_t kGsShadowFrame = game::readable::perGame(gs::kFrame2, gs::kFrame1, gs::kFrame2);
        constexpr uint32_t kGsShadowZbuf = game::readable::perGame(gs::kZbuf2, gs::kZbuf1, gs::kZbuf2);
        // ALPHA_2 for the volume: (Cs - 0) * As + Cd, an additive blend.
        constexpr uint64_t kVolumeAlpha =
            gs::Alpha{.a = gs::kBlendSource, .b = gs::kBlendZero, .c = gs::kBlendSourceAlpha, .d = gs::kBlendFrame}.bits();

        // A GIF tag, as the packet takes it from the frame.
        struct alignas(16) GifTag
        {
            ee::Qword words;
        };
        // (In the ELF's data they sit on 8-byte boundaries: a GifTag is the frame's.)
        // PACKED, EOP: A+D with NLOOP filled in when closed; the first block's; one
        // RGBAQ; one triangle strip in context 2, three XYZ2 per vertex.
        inline ee::Global<ee::Qword, shadow::kTagAd> g_tagAd;
        inline ee::Global<ee::Qword, sym::tagAdState> g_tagAdState;
        inline ee::Global<ee::Qword, shadow::kTagRgbaq> g_tagRgbaq;
        inline ee::Global<ee::Qword, shadow::kTagStrip> g_tagStrip;

        const ee::Ptr<sce::Vif1Packet> kShadowPacket(shadow::kPacket);

        namespace // the game functions below call: ee/plain.h
        {
            void addQuadwords(ee::Ptr<sce::Vif1Packet> packet, ee::Address data, int32_t count) { ee::game(sym::vif1PacketAddQwords, packet, data, count); }
            void addGsData(ee::Ptr<sce::Vif1Packet> packet, ee::Address data, uint32_t count) { ee::game(sym::vif1PacketAddGsData, packet, data, count); }
            void addWords(ee::Ptr<sce::Vif1Packet> packet, ee::Address words, uint32_t count) { ee::game(sym::vif1PacketAddWords, packet, words, count); }
            void addWord(ee::Ptr<sce::Vif1Packet> packet, uint32_t word) { ee::game(sym::vif1PacketAddWord, packet, word); }
            void addCode(ee::Ptr<sce::Vif1Packet> packet, uint32_t code) { ee::game(sym::vif1PacketAddCode, packet, code); }
        }

        // Where the packet's next chain starts, as DMA addresses it.
        ee::Ptr<uint32_t> chainStart(ee::Ptr<sce::Vif1Packet> packet) { return ee::physical(packet->current); }

        // An unpack to VU1 at the current address, or relative to TOPS.
        void openUnpack(ee::Ptr<sce::Vif1Packet> packet)
        {
            sce::vif1PacketOpenUnpack(packet, static_cast<uint16_t>(*shadow::g_vuAddress), vif::kUnpackV4_32, 4, 4);
        }
        void openUnpackFromTops(ee::Ptr<sce::Vif1Packet> packet)
        {
            sce::vif1PacketOpenUnpack(packet, static_cast<uint16_t>(*shadow::g_vuAddress) | vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);
        }

        // A block of GS state, on a quadword boundary, under its GIF tag.
        void openGsBlock(ee::Ptr<sce::Vif1Packet> packet, const GifTag &tag)
        {
            sce::vif1PacketAlign(packet, 2, 2);
            openUnpackFromTops(packet);
            sce::vif1PacketOpenGifTag(packet, tag.words);
        }

        // Closes it (the GIF tag, then the unpack) and moves the VU1 address past it.
        void closeGsBlock(ee::Ptr<sce::Vif1Packet> packet)
        {
            sce::vif1PacketCloseGifTag(packet);
            const ee::Ptr<uint8_t> unpack = packet->vifCode.cast<uint8_t>();
            sce::vif1PacketCloseUnpack(packet);
            *shadow::g_vuAddress = *shadow::g_vuAddress + unpack[2]; // the UNPACK's NUM
        }

        // VU1's GS state program, run on what the block left.
        void runGsStateProgram(ee::Ptr<sce::Vif1Packet> packet)
        {
            addCode(packet, vif::mscal(kProgramGsState));
            addCode(packet, vif::flushA());
        }
        // ZGE2's pass ends with the program's call alone (no FLUSHA after it).
        void callGsStateProgram(ee::Ptr<sce::Vif1Packet> packet) { addCode(packet, vif::mscal(kProgramGsState)); }

        // The scene's FRAME_2 and ZBUF_2 as the shadow pass found them. (Read as I/O:
        // the scratchpad is outside the recordings' RAM.)
        uint64_t sceneFrameRegister() { return ee::io::load<uint64_t>(shadow::kSceneFrame); }
        uint64_t sceneZbufRegister() { return ee::io::load<uint64_t>(shadow::kSceneZbuf); }

        // FRAME_2 and ZBUF_2 back to the scene's. ZGE4's FRAME_2 is the volume's,
        // its FBP the frame being drawn (a frame fix-up writes it as the packet
        // goes out). With sceneAsRegisters the scene's values are read as I/O
        // (sceneFrameRegister, sceneZbufRegister).
        [[gnu::always_inline]] inline void addSceneTargets(ee::Ptr<sce::Vif1Packet> packet, bool sceneAsRegisters = false)
        {
            if constexpr (game::readable::kIsBmx)
            {
                addFrameFixup(kFixupDrawFrame, packet->current, 0);
                addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
            }
            else
                addGsRegister(packet, kGsShadowFrame, sceneAsRegisters ? sceneFrameRegister() : *shadow::g_sceneFrame);
            addGsRegister(packet, kGsShadowZbuf, sceneAsRegisters ? sceneZbufRegister() : *shadow::g_sceneZbuf);
        }
    }

    // ---- the shadow pass's start -----------------------------------------------

    namespace
    {
        constexpr uint32_t kColourBlack = sym::colourBlack; // the clear rectangle's colour
        // The values the first block gives both contexts: CLAMP, TEX0, TEX1, ALPHA, TEST.
        inline ee::Global<std::array<uint64_t, 5>, sym::passState> g_passState;
        constexpr uint32_t kPassRegisters[2][5] = {{gs::kClamp1, gs::kTex0_1, gs::kTex1_1, gs::kAlpha1, gs::kTest1},
                                                   {gs::kClamp2, gs::kTex0_2, gs::kTex1_2, gs::kAlpha2, gs::kTest2}};
        // The volume's FRAME: the shadow buffer's (its FBP the page of the display's shadow buffer word),
        // 16-bit (PSMCT16S).
        uint64_t inShadowBuffer(uint64_t frame)
        {
            const uint32_t fbp = gs::pageOf(g_display->shadowBufferWord) & gs::Frame::kFbp.mask();
            return withField(withField(frame, gs::Frame::kFbp, fbp), gs::Frame::kPsm, gs::kPsmCt16S);
        }
        // The volume's ZBUF: depth not written.
        uint64_t depthMasked(uint64_t zbuf) { return zbuf | gs::Zbuf::kZmsk.mask(); }

        // The rectangle over the whole frame buffer (its GS context is the stack word).
        int32_t displayWidth() { return ee::toInt(g_display->bufferWidth); }
        int32_t displayHeight() { return ee::toInt(g_display->bufferHeight); }
    }

    // beginShadowPass: the shadow pass's packet opened (a RET chain; returns its
    // start, physical): the GS state for both contexts, FRAME_2 / ZBUF_2 to
    // the shadow buffer (16-bit, Z masked; kept in g_volumeFrame and
    // g_volumeZbuf), the buffer cleared by a screen rectangle, FRAME_2 /
    // ZBUF_2 back to the scene's, then VU1's GS state program. ZGE4 builds the
    // volume's FRAME_2 from nothing: the FBP and PSM as ZGE3's, the FBW and
    // FBMSK from the display's first draw environment.
    ee::Ptr<uint32_t> beginShadowPass()
    {
        ee::Local<uint32_t> rectangleContext; // the 9th argument
        ee::Local<GifTag> tagState;
        ee::Local<GifTag> tagAd;
        tagState->words = *g_tagAdState;
        tagAd->words = *g_tagAd;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        *shadow::g_vuAddress = 0u;
        *shadow::g_volumeFrame = game::readable::kIsBmx ? uint64_t{0} : sceneFrameRegister();
        *shadow::g_volumeZbuf = sceneZbufRegister();
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);
        addCode(packet, vif::flushA());
        addCode(packet, vif::base(0));
        addCode(packet, vif::offset(0));

        openGsBlock(packet, tagState);
        for (const auto &context : kPassRegisters)
            for (uint32_t i = 0; i < 5; ++i)
                addGsRegister(packet, context[i], (*g_passState)[i]);
        *shadow::g_volumeFrame = inShadowBuffer(*shadow::g_volumeFrame);
        if constexpr (game::readable::kIsBmx)
        {
            // (drawEnvironment0 called for each, as the original does.)
            *shadow::g_volumeFrame = withField(*shadow::g_volumeFrame, gs::Frame::kFbw, fieldOf(drawEnvironment0()->frame, gs::Frame::kFbw));
            const uint32_t frameMask = static_cast<uint32_t>(fieldOf(drawEnvironment0()->frame, gs::Frame::kFbmsk));
            *shadow::g_volumeFrame = withField(*shadow::g_volumeFrame, gs::Frame::kFbmsk, frameMask);
        }
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        *shadow::g_volumeZbuf = depthMasked(*shadow::g_volumeZbuf);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        closeGsBlock(packet);

        // The shadow buffer cleared.
        const int32_t height = displayHeight();
        const int32_t width = displayWidth();
        rectangleContext = 1u;
        *shadow::g_vuAddress = screenRectangle(packet, 0, 0, width, height, 0, kColourBlack, 0);

        openGsBlock(packet, tagAd);
        addSceneTargets(packet);
        closeGsBlock(packet);
        runGsStateProgram(packet);
        return start;
    }

    // beginShadowPassInFlippedBuffer: ZGE2's beginShadowPass: the shadow
    // packet's other buffer taken first (ZGE3's beginFramePackets does that,
    // flipShadowPacket), then the pass's packet as ZGE3's but with no FLUSHA,
    // BASE or OFFSET and no GS state for both contexts: FRAME_1 / ZBUF_1 to
    // the shadow buffer (16-bit, Z masked), the buffer cleared by a screen
    // rectangle the size of the frame buffer, FRAME_1 / ZBUF_1 back to the
    // scene's, VU1's GS state program (with no FLUSHA after it either).
    // Returns where the chain starts.
    ee::Ptr<uint32_t> beginShadowPassInFlippedBuffer()
    {
        ee::Local<GifTag> tagState;
        ee::Local<GifTag> tagAd;
        tagState->words = *g_tagAdState;
        tagAd->words = *g_tagAd;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        const uint32_t buffer = *shadow::g_packetBuffer ^ 1u;
        *shadow::g_packetBuffer = buffer;
        *shadow::g_volumeFrame = sceneFrameRegister();
        *shadow::g_volumeZbuf = sceneZbufRegister();
        *shadow::g_vuAddress = 0u;
        vif1PacketInit(packet, (*shadow::g_packetBuffers)[buffer]);
        vif1PacketReset(packet);
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);

        openGsBlock(packet, tagState);
        *shadow::g_volumeFrame = inShadowBuffer(*shadow::g_volumeFrame);
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        *shadow::g_volumeZbuf = depthMasked(*shadow::g_volumeZbuf);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        closeGsBlock(packet);

        // The shadow buffer cleared.
        *shadow::g_vuAddress = screenRectangle(packet, 0, 0, displayWidth(), displayHeight(), 0, kColourBlack, 0);

        openGsBlock(packet, tagAd);
        addGsRegister(packet, kGsShadowFrame, sceneFrameRegister());
        addGsRegister(packet, kGsShadowZbuf, sceneZbufRegister());
        closeGsBlock(packet);
        callGsStateProgram(packet);
        return start;
    }
    READABLE_FUNCTION_OWN(beginShadowPass, Verified, dm2, beginShadowPassInFlippedBuffer);

    namespace
    {
        // The two rectangles' colours; the second is what buildShadowVolume sets per
        // caster, a node's colour at half strength.
        constexpr uint32_t kRectangleColour0 = sym::rectangleColour0;
        constexpr uint32_t kRectangleColour1 = sym::g_shadowColourRed;
        // The GS register values of the pass.
        // FRAME_2's FBMSK over the shadow buffer: each byte's top bit kept (in PSMCT16S: the top bits of red,
        // green and blue, and alpha).
        constexpr uint32_t kFrameMaskHigh = 0x80808080u;
        constexpr uint64_t kTest2Shadow = gs::Test{.zte = 1, .ztst = gs::kDepthAlways}.bits();
        // The alpha test never passing, a failing pixel still written to the frame (not the depth); depth always.
        constexpr uint64_t kTest1Shadow = gs::Test{.ate = 1, .afail = gs::kFailFrameOnly, .zte = 1, .ztst = gs::kDepthAlways}.bits();
        // ZGE2's TEST_1 in both of its blocks: the Z test on, always passing (no alpha test).
        constexpr uint64_t kTestZAlways = gs::Test{.zte = 1, .ztst = gs::kDepthAlways}.bits();
        // (frame - source) * 1.0 + 0.
        constexpr uint64_t kAlpha1Shadow =
            gs::Alpha{.a = gs::kBlendFrame, .b = gs::kBlendSource, .c = gs::kBlendFix, .d = gs::kBlendZero, .fix = 0x80}.bits();
        // TEX0_1's fields above TBP0 and TBW for the shadow buffer: 16-bit (PSMCT16S), 512 by 256, its alpha used.
        constexpr uint64_t kTex0Shadow = gs::Tex0{.psm = gs::kPsmCt16S, .tw = 9, .th = 8, .tcc = 1}.bits();
        // TEX0_1 for the shadow buffer: TBP0 its block, TBW the width given (in 64s).
        uint64_t shadowBufferTexture(uint64_t bufferWidth)
        {
            return gs::blockOf(g_display->shadowBufferWord) | gs::Tex0::kTbw.placed(bufferWidth) | kTex0Shadow;
        }
        // The FRAME with its FBMSK made kFrameMaskHigh.
        uint64_t withHighBitsMasked(uint64_t frame) { return withField(frame, gs::Frame::kFbmsk, kFrameMaskHigh); }
    }

    // applyShadowBuffer: the shadow pass's second packet (a RET chain; returns its
    // start): FRAME_2 with the colour bits masked, a screen rectangle; then
    // the shadow buffer (the display's +0x5C) as TEX0_1 over a textured
    // screen rectangle; COLCLAMP back; VU1's GS state program.
    ee::Ptr<uint32_t> applyShadowBuffer()
    {
        ee::Local<uint32_t> rectangleContext; // the 9th argument
        ee::Local<GifTag> tagState;
        ee::Local<GifTag> tagAd;
        tagState->words = *g_tagAdState;
        tagAd->words = *g_tagAd;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        *shadow::g_vuAddress = 0u;
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);

        openGsBlock(packet, tagState);
        *shadow::g_volumeFrame = withHighBitsMasked(*shadow::g_volumeFrame);
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        addGsRegister(packet, gs::kTest2, kTest2Shadow);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        closeGsBlock(packet);
        {
            const int32_t width = displayWidth();
            const int32_t height = displayHeight();
            rectangleContext = 1u;
            *shadow::g_vuAddress = screenRectangle(packet, 0, 0, width, height, 0, kRectangleColour0, 0);
        }

        openGsBlock(packet, tagState);
        addGsRegister(packet, gs::kColClamp, gs::kColoursClamped);
        addGsRegister(packet, gs::kTest1, kTest1Shadow);
        // TBW the display's width in 64s.
        addGsRegister(packet, gs::kTex0_1, shadowBufferTexture(gs::bufferWidthUnits(displayWidth())));
        addGsRegister(packet, gs::kAlpha1, kAlpha1Shadow);
        addGsRegister(packet, gs::kTexFlush, 1u);
        closeGsBlock(packet);
        {
            const int32_t height = displayHeight();
            const int32_t width = displayWidth();
            rectangleContext = 0u;
            *shadow::g_vuAddress = screenTexturedRectangle(packet, 0, 0, width, height, 1, kRectangleColour1, 0);
        }

        openGsBlock(packet, tagAd);
        addGsRegister(packet, gs::kColClamp, gs::kColoursClamped);
        closeGsBlock(packet);
        runGsStateProgram(packet);
        return start;
    }

    // applyShadowBufferInContextOne: ZGE2's applyShadowBuffer, all of it in GS
    // context 1: FRAME_1 with the colour bits masked, TEST_1 (Z always) and
    // ZBUF_1 for a screen rectangle; then COLCLAMP, FRAME_1 back to the
    // scene's (kept as the volume's FRAME too), TEST_1 with no alpha test,
    // the shadow buffer as TEX0_1 (its width in 64s through a float) and
    // ALPHA_1 over the textured screen rectangle; then ZBUF_1, FRAME_1 and
    // COLCLAMP back, VU1's GS state program with no FLUSHA after it, and the
    // packet's DMA tag closed. Returns where the chain starts.
    ee::Ptr<uint32_t> applyShadowBufferInContextOne()
    {
        ee::Local<GifTag> tagState;
        ee::Local<GifTag> tagAd;
        tagState->words = *g_tagAdState;
        tagAd->words = *g_tagAd;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        *shadow::g_vuAddress = 0u;
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);

        openGsBlock(packet, tagState);
        *shadow::g_volumeFrame = withHighBitsMasked(*shadow::g_volumeFrame);
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        addGsRegister(packet, gs::kTest1, kTestZAlways);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        closeGsBlock(packet);
        *shadow::g_vuAddress = screenRectangle(packet, 0, 0, displayWidth(), displayHeight(), 0, kRectangleColour0, 0);

        openGsBlock(packet, tagState);
        addGsRegister(packet, gs::kColClamp, gs::kColoursClamped);
        *shadow::g_volumeFrame = sceneFrameRegister();
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        addGsRegister(packet, gs::kTest1, kTestZAlways);
        // TBW the display's width in 64s, through a float.
        addGsRegister(packet, gs::kTex0_1, shadowBufferTexture(sce::libgcc::__fixunssfdi(g_display->bufferWidth * ee::Float(1.0f / 64.0f))));
        addGsRegister(packet, gs::kAlpha1, kAlpha1Shadow);
        addGsRegister(packet, gs::kTexFlush, 1u);
        closeGsBlock(packet);
        *shadow::g_vuAddress = screenTexturedRectangle(packet, 0, 0, displayWidth(), displayHeight(), 1, kRectangleColour1, 0);

        openGsBlock(packet, tagAd);
        addGsRegister(packet, kGsShadowZbuf, sceneZbufRegister());
        addGsRegister(packet, kGsShadowFrame, sceneFrameRegister());
        addGsRegister(packet, gs::kColClamp, gs::kColoursClamped);
        closeGsBlock(packet);
        callGsStateProgram(packet);
        sce::vif1PacketCloseDmaTag(packet);
        return start;
    }
    READABLE_FUNCTION_OWN(applyShadowBuffer, Verified, dm2, applyShadowBufferInContextOne);

    // ---- the volume's packet ------------------------------------------------

    ee::Ptr<uint32_t> buildShadowVolumePacket(ee::Ptr<ZgePart> partAddress, ee::Ptr<Matrix> world, ee::Ptr<Vec4> light)
    {
        // The packet copies the tags, the matrix and the near clip and far scale from
        // these: all of them live in the frame.
        ee::Local<GifTag> tagAd;
        ee::Local<GifTag> tagRgbaq;
        ee::Local<GifTag> tagStrip;
        ee::Local<Matrix> matrix;
        ee::Local<Vec4[2]> points;
        ee::Local<Float> nearClip;
        ee::Local<Float> farScale;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        const ee::Ptr<Camera> camera(*g_currentCamera);
        tagAd->words = *g_tagAd;
        tagRgbaq->words = *g_tagRgbaq;
        tagStrip->words = *g_tagStrip;

        nearClip = cameraNearClip(camera) + *shadow::g_nearOffset;
        farScale = kVolumeFarScale;
        *shadow::g_vuAddress = 0u;
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);

        // The GS state the volume is drawn with (context 2; ZGE2's context 1).
        openGsBlock(packet, tagAd);
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        addGsRegister(packet, gs::kColClamp, uint64_t{0});
        addGsRegister(packet, kGsShadowAlpha, kVolumeAlpha);
        closeGsBlock(packet);
        runGsStateProgram(packet);
        addCode(packet, vif::itop(0u));

        // The light, the colours, the strip's GIF tag and the matrices (model ->
        // camera, the camera's view -> screen) from VU1 address 1.
        *shadow::g_vuAddress = 1u;
        openUnpack(packet);
        addQuadwords(packet, light, 1);
        addGsData(packet, tagRgbaq, 2);
        addQuadwords(packet, shadow::kColour0, 1);
        addGsData(packet, tagRgbaq, 2);
        addQuadwords(packet, shadow::kColour1, 1);
        addGsData(packet, tagStrip, 2);
        const ee::Ptr<Matrix> view = cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), 0);
        matrixMultiply(matrix, view, world);
        addQuadwords(packet, matrix, 4);
        addQuadwords(packet, *shadow::g_screenProjection, 4);
        sce::vif1PacketCloseUnpack(packet);

        // The silhouette (forEachSilhouetteEdge), two points an edge, 64 edges an unpack.
        *shadow::g_vuAddress = kFirstPointAddress;
        bool unpackOpen = false;
        uint32_t pointCount = 0;
        forEachSilhouetteEdge(*partAddress, [&](const Vec4 &first, const Vec4 &second) {
            points[0] = first;
            points[1] = second;
            if (!unpackOpen)
            {
                openUnpack(packet);
                unpackOpen = true;
            }
            pointCount += 2;
            addQuadwords(packet, points, 2);
            if ((pointCount & (kPointsPerUnpack - 1u)) == 0u)
            {
                sce::vif1PacketCloseUnpack(packet);
                unpackOpen = false;
                *shadow::g_vuAddress = *shadow::g_vuAddress + kPointsPerUnpack;
            }
        });
        if (unpackOpen)
            sce::vif1PacketCloseUnpack(packet);

        // VU1's parameters at address 0 (the near clip + 0.1, the point count, the
        // far scale, 0), then the volume's microprogram.
        *shadow::g_vuAddress = 0u;
        openUnpack(packet);
        addWords(packet, nearClip, 1);
        addWord(packet, pointCount);
        addWords(packet, farScale, 1);
        addWord(packet, 0);
        sce::vif1PacketCloseUnpack(packet);
        addCode(packet, vif::mscal(kProgramVolume));
        addCode(packet, vif::flushA());

        // The scene's GS state back (the scratchpad keeps it).
        *shadow::g_vuAddress = 0u;
        openGsBlock(packet, tagAd);
        // (ZGE2's read as I/O, as beginShadowPass reads them: the scratchpad is outside the recordings' RAM)
        addSceneTargets(packet, game::readable::kIsDm2);
        addGsRegister(packet, gs::kColClamp, uint64_t{1});
        closeGsBlock(packet);
        runGsStateProgram(packet);
        return start;
    }
    READABLE_FUNCTION(buildShadowVolumePacket, Verified);

    // ---- the shadow packet's buffers and the pass's ends ----------------------

    namespace
    {
    }

    // allocateShadowBuffers: the shadow packet's two buffers (48 KB each,
    // ZGE2's 32 KB; cleared, the cache written back), kept by their uncached
    // addresses; shadows on (ZGE2 has no switch). Always 1.
    uint32_t allocateShadowBuffers()
    {
        *shadow::g_buffersReady = 1;
        if constexpr (shadow::kHasEnabledFlag)
            *shadow::g_enabled = 1;
        std::array<uint32_t, 2> &buffers = *shadow::g_packetBuffers;
        buffers[0] = allocateIn(shadow::kPacketBufferBytes, kMemoryPackets, 1);
        const uint32_t second = allocateIn(shadow::kPacketBufferBytes, kMemoryPackets, 1);
        const uint32_t first = ee::uncachedMirror(buffers[0]); // (VIF1 reads the packets by DMA)
        buffers[1] = ee::uncachedMirror(second);
        buffers[0] = first;
        return 1;
    }
    READABLE_FUNCTION_WAITS(allocateShadowBuffers, Verified);

    // freeShadowBuffers: the two buffers freed (the second first), once
    // allocated. Always 1.
    uint32_t freeShadowBuffers()
    {
        if (*shadow::g_buffersReady != 0)
        {
            const std::array<uint32_t, 2> &buffers = *shadow::g_packetBuffers;
            freeIn(ee::physical(buffers[1]), kMemoryPackets);
            freeIn(ee::physical(buffers[0]), kMemoryPackets);
            *shadow::g_buffersReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeShadowBuffers, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void vif1PacketInit(ee::Ptr<sce::Vif1Packet> packet, uint32_t buffer) { ee::game(sym::vif1PacketInit, packet, buffer); }
        namespace tail
        {
        void vif1PacketReset(ee::Ptr<sce::Vif1Packet> packet) { ee::tailCall(sym::vif1PacketReset, packet); }
        void vif1PacketCloseDmaTag(ee::Ptr<sce::Vif1Packet> packet) { ee::tailCall(sym::vif1PacketCloseDmaTag, packet); }
        }
    }

    // flipShadowPacket: the frame's start (beginFramePackets): the shadow packet
    // moved to its other buffer and emptied (sceVif1PkInit, then
    // sceVif1PkReset, the original's tail call).
    void flipShadowPacket()
    {
        const uint32_t index = *shadow::g_packetBuffer ^ 1u;
        *shadow::g_packetBuffer = index;
        vif1PacketInit(kShadowPacket, (*shadow::g_packetBuffers)[index]);
        tail::vif1PacketReset(kShadowPacket);
    }
    READABLE_FUNCTION(flipShadowPacket, Verified);

    // closeShadowPacket: the frame's end (closeFramePacket): the shadow packet's
    // open DMA tag closed (the original's tail call).
    void closeShadowPacket()
    {
        tail::vif1PacketCloseDmaTag(kShadowPacket);
    }
    READABLE_FUNCTION(closeShadowPacket, Verified);

    // ---- the disc shadow --------------------------------------------------------

    // buildDiscShadowPacket (from the shadow driver buildShadowVolume in place of
    // buildShadowVolumePacket when three or more scene records are counted:
    // split screen): the same volume packet as buildShadowVolumePacket, its
    // silhouette a fixed disc (8 edges, radius 0.33) instead of the part's
    // edges. `part` is not read. Returns where the chain starts (physical).
    ee::Ptr<uint32_t> buildDiscShadowPacket(ee::Ptr<ZgePart> part, ee::Ptr<Matrix> world, ee::Ptr<Vec4> light)
    {
        (void)part;
        ee::Local<GifTag> tagAd;
        ee::Local<GifTag> tagRgbaq;
        ee::Local<GifTag> tagStrip;
        ee::Local<Matrix> matrix;
        ee::Local<Float> nearClip;
        ee::Local<Float> farScale;
        tagAd->words = *g_tagAd;
        tagRgbaq->words = *g_tagRgbaq;
        tagStrip->words = *g_tagStrip;
        const ee::Ptr<sce::Vif1Packet> packet = kShadowPacket;
        const ee::Ptr<Camera> camera(*g_currentCamera);

        const Float clip = cameraNearClip(camera) + Float(0.1f);
        farScale = kVolumeFarScale;
        nearClip = clip;
        const ee::Ptr<Vec4> outline = discShadowOutline();

        // The GS state the volume is drawn with (context 2), as buildShadowVolumePacket's.
        *shadow::g_vuAddress = 0u;
        sce::vif1PacketAlign(packet, 2, 0);
        const ee::Ptr<uint32_t> start = chainStart(packet);
        sce::vif1PacketRet(packet, 0);
        openGsBlock(packet, tagAd);
        addGsRegister(packet, kGsShadowFrame, *shadow::g_volumeFrame);
        addGsRegister(packet, kGsShadowZbuf, *shadow::g_volumeZbuf);
        addGsRegister(packet, gs::kColClamp, uint64_t{0});
        addGsRegister(packet, kGsShadowAlpha, kVolumeAlpha);
        closeGsBlock(packet);
        runGsStateProgram(packet);
        addCode(packet, vif::itop(0u));

        // The light, the colours, the strip's tag and the matrices (model -> camera,
        // the camera's view -> screen) from VU1 address 1.
        *shadow::g_vuAddress = 1u;
        openUnpack(packet);
        addQuadwords(packet, light, 1);
        addGsData(packet, tagRgbaq, 2);
        addQuadwords(packet, shadow::kColour0, 1);
        addGsData(packet, tagRgbaq, 2);
        addQuadwords(packet, shadow::kColour1, 1);
        addGsData(packet, tagStrip, 2);
        const ee::Ptr<Matrix> view = cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), 0);
        matrixMultiply(matrix, view, world);
        addQuadwords(packet, matrix, 4);
        addQuadwords(packet, *shadow::g_screenProjection, 4);
        sce::vif1PacketCloseUnpack(packet);

        // The disc's edges, points i and i + 1 each, from VU1 address 0x20.
        *shadow::g_vuAddress = kFirstPointAddress;
        openUnpack(packet);
        for (uint32_t edge = 0; edge < kDiscEdges; ++edge)
        {
            addQuadwords(packet, outline + edge, 1);
            addQuadwords(packet, outline + (edge + 1), 1);
        }
        sce::vif1PacketCloseUnpack(packet);

        // VU1's parameters at address 0 (the near clip + 0.1, the point count, 1.25,
        // 0), then the volume's microprogram.
        *shadow::g_vuAddress = 0u;
        openUnpack(packet);
        addWords(packet, nearClip, 1);
        addWord(packet, kDiscEdges * 2u);
        addWords(packet, farScale, 1);
        addWord(packet, 0u);
        sce::vif1PacketCloseUnpack(packet);
        addCode(packet, vif::mscal(kProgramVolume));
        addCode(packet, vif::flushA());

        // The scene's GS state back (the scratchpad keeps it).
        *shadow::g_vuAddress = 0u;
        openGsBlock(packet, tagAd);
        addSceneTargets(packet);
        addGsRegister(packet, gs::kColClamp, uint64_t{1});
        closeGsBlock(packet);
        runGsStateProgram(packet);
        return start;
    }
    READABLE_FUNCTION(buildDiscShadowPacket, Verified);

}
