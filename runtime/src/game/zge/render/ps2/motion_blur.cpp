// The motion blur's frame prologue as the PS2 draws it (screen_effects.cpp sets a camera's blur): drawBlur sends
// the last frame over a blurred camera's view as a textured sprite (addMotionBlurSprite), with the GS state around
// it, in a kick of VU1's GS state program.

#include "zge/memory/regions.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/render/display.h"
#include "zge/render/ps2/frame_packets.h"
#include "zge/render/draw.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"
#include "zge/render/material.h"
#include "zge/render/packet.h"
#include "zge/render/renderer.h"
#include "zge/render/fx_manager.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/ps2/motion_blur.cpp.h"

namespace zge
{
    namespace
    {
        // The frame prologue's on flag (screen_effects.cpp's initFramePrologue sets it).
        inline ee::Global<uint32_t, sym::g_framePrologueOn> g_framePrologueOn;
    }

    namespace
    {
        // A GS vertex as the sprite's quadwords hold it: X, Y (12.4 screen
        // coordinates), Z.
        struct SpriteCorner
        {
            int32_t x, y;
            uint64_t z;
        };
        // A UV (12.4 texel coordinates); the quadword's upper half goes out
        // as the frame holds it (the sprite's code never writes it).
        struct SpriteUv
        {
            int32_t u, v;
            uint64_t upper;
        };
        // addMotionBlurSprite's frame, as the original lays it out: what
        // goes into the packet by the quadword, the VU1 address it was
        // given and the zoom's insets.
        struct BlurSpriteFrame
        {
            int32_t colour[4];         // +0x00 RGBA at half, a word each
            SpriteCorner corners[2];   // +0x10, +0x20
            SpriteUv uv;               // +0x30
            uint32_t vuAddress;        // +0x40
            int32_t insetU, insetV;    // +0x44
        };
        static_assert(offsetof(BlurSpriteFrame, uv) == 0x30 && offsetof(BlurSpriteFrame, insetV) == 0x48);

        // Sixteen bytes of the frame as the quadword the packet takes.
        template <typename T>
        ee::Qword asQuadword(const T &value)
        {
            static_assert(sizeof(T) == sizeof(ee::Qword));
            ee::Qword quadword;
            std::memcpy(&quadword, &value, sizeof quadword);
            return quadword;
        }
        // The second copy of the library's doubleword add, at its own symbol.
        void addDword(ee::Ptr<sce::Vif1Packet> packet, uint64_t value) { sce::vif1PacketAddDword2(packet, value); }
    }

    // addMotionBlurSprite (the frame prologue, per blurred camera): a textured
    // sprite over the camera's viewport (rows halved for field rendering),
    // its texels the last frame's from the same rectangle drawn in by
    // (1 - scale) * size sixteenths of a texel each side (the zoom), in the
    // blur colour at half (blend: ABE; context: CTXT; endOfPacket: EOP), as
    // an unpack to VU1 at vuAddress. Returns the VU1 address after it.
    uint32_t addMotionBlurSprite(ee::Ptr<sce::Vif1Packet> packet, ee::Ptr<Camera> camera, uint32_t blend, uint32_t endOfPacket,
                                 uint32_t context, uint32_t vuAddress)
    {
        ee::Local<BlurSpriteFrame> local;
        BlurSpriteFrame &f = *local;
        const DisplaySetup &display = *g_display;
        const uint32_t field = display.flags & kDisplayField;
        const int32_t width = ee::toInt(camera->viewport[2]);
        const int32_t height = ee::toInt(camera->viewport[3]) >> field;
        f.vuAddress = vuAddress;
        const int32_t width16 = subpixels(width);
        // PRIM: a textured sprite with texel coordinates, the caller's blend (ABE) and context (CTXT); a 32-bit
        // value sign-extended.
        const uint64_t primWord = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(
            (static_cast<uint32_t>(blend) << gs::kPrimBlendShift) | (static_cast<uint32_t>(context) << gs::kPrimContextShift) |
            gs::kSprite | gs::kTextured | gs::kTexelCoords)));
        const int32_t x = ee::toInt(camera->viewport[0]);
        f.insetU = ee::toInt(ee::toFloat(width) - camera->motionBlurScale[0] * ee::toFloat(width));
        f.insetV = ee::toInt(ee::toFloat(height) - camera->motionBlurScale[1] * ee::toFloat(height));
        const int32_t y16 = subpixels(ee::toInt(camera->viewport[1]) >> field);
        sce::vif1PacketOpenUnpack(packet, (vuAddress | vif::kUnpackFromTops) & 0xFFFFu, vif::kUnpackV4_32, 4, 4);

        // The GIF tag: once through five registers (RGBAQ, UV, XYZ2, UV,
        // XYZ2), EOP from the caller, PRIM set from the sprite's.
        const int32_t x16 = subpixels(x);
        const int32_t top = display.offsetY + y16;
        const int32_t left = display.offsetX + x16;
        const uint64_t tag = gs::GifTag{.loops = 1,
                                        .endOfPacket = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(endOfPacket))),
                                        .setsPrim = 1,
                                        .prim = primWord,
                                        .registers = 5}
                                 .bits();
        f.corners[1].x = left + width16;
        f.corners[1].y = top + subpixels(height);
        f.corners[1].z = 1;
        f.corners[0].x = left;
        f.corners[0].y = top;
        f.corners[0].z = 1;
        addDword(packet, tag);
        addDword(packet, gs::registerList({gs::kRgbaq, gs::kUv, gs::kXyz2, gs::kUv, gs::kXyz2}));

        // The colour at half, a word per channel.
        const uint8_t *colour = camera->motionBlurColour;
        f.colour[0] = colour[0] >> 1;
        f.colour[1] = colour[1] >> 1;
        f.colour[2] = colour[2] >> 1;
        f.colour[3] = colour[3] >> 1;
        sce::vif1PacketAddQword(packet, asQuadword(f.colour));
        f.uv.u = x16 + f.insetU;
        f.uv.v = y16 + f.insetV;
        sce::vif1PacketAddQword(packet, asQuadword(f.uv));
        sce::vif1PacketAddQword(packet, asQuadword(f.corners[0]));
        // The far corner's UV one row lower than the rectangle's.
        f.uv.u = x16 - f.insetU + width16;
        f.uv.v = y16 - f.insetV + subpixels(height + 1);
        sce::vif1PacketAddQword(packet, asQuadword(f.uv));
        sce::vif1PacketAddQword(packet, asQuadword(f.corners[1]));

        const ee::Ptr<uint32_t> unpack = packet->vifCode;
        sce::vif1PacketCloseUnpack(packet);
        return f.vuAddress + vif::unpackQuadwords(*unpack);
    }
    READABLE_FUNCTION(addMotionBlurSprite, Verified);

    namespace
    {
        // The prologue's two GIF tags (PACKED, one A+D, NLOOP filled in when
        // closed), the second with EOP.
        inline ee::Global<ee::Qword, sym::g_blurGifTag> g_blurStateTag;
        inline ee::Global<ee::Qword, sym::g_blurEndGifTag> g_blurSpriteTag;
        // The state the sprite is drawn with (A+D data).
        inline ee::Global<uint64_t, sym::g_blurClamp1> g_blurClamp1;     // 0
        inline ee::Global<uint64_t, sym::g_blurTex0> g_blurTex0;       // 0
        inline ee::Global<uint64_t, sym::g_blurTex1> g_blurTex1;       // 0
        inline ee::Global<uint64_t, sym::g_blurAlpha1> g_blurAlpha1;     // 0x44: (Cs - Cd) x As + Cd
        inline ee::Global<uint64_t, sym::g_blurDepthTest> g_blurDepthTest;  // 0x70000: TEST_1 ZTST greater
        constexpr uint64_t kTestAlwaysDepth = gs::Test{.zte = 1, .ztst = gs::kDepthAlways}.bits();
        constexpr uint64_t kZbufNoWrites = gs::Zbuf::kZmsk.mask();
        constexpr uint64_t kOffsetYFraction = gs::Field{gs::XyOffset::kOfy.shift, kSubpixelBits}.mask(); // XYOFFSET_1's OFY, its sixteenths
        // Frames since the level started (0 on its loading screen: no last frame yet).
        inline ee::Global<uint32_t, sym::g_levelFrame> g_framesInLevel;
        // The draw environment as beginCameraFrame keeps it in the scratchpad.
        inline ee::Global<uint64_t, PS2_SCRATCHPAD_BASE> g_scratchpadZbuf; // (ZGE4's place for it)
        inline ee::Global<uint64_t, PS2_SCRATCHPAD_BASE + sizeof(uint64_t)> g_sceneZbuf;
        inline ee::Global<uint64_t, PS2_SCRATCHPAD_BASE + 2 * sizeof(uint64_t)> g_sceneXyOffset;
        inline ee::Global<uint64_t, PS2_SCRATCHPAD_BASE + 3 * sizeof(uint64_t)> g_lastFrameRegister; // the other environment's FRAME_1

        // VU1's GS state program [inferred] (ZGE4's microprograms have it two words on)
        constexpr uint32_t kCallGsStateProgram = vif::mscal(game::readable::perGame(0x24u, 0x24u, 0x26u));

        // TEX0_1 for the last frame as a 1024 x 512 texture: FRAME_1's
        // FBP x 32 (a frame fix-up writes the base again when the packet is
        // sent), its width and its format. The original fills an unset
        // doubleword field by field, and the fields cover all 64 bits, so
        // nothing of it is left.
        uint64_t lastFrameTexture(uint64_t frameRegister)
        {
            const uint64_t base = gs::frameBasePage(frameRegister); // FBP
            const uint64_t width = gs::fieldOf(frameRegister, gs::Frame::kFbw);
            const uint64_t format = gs::fieldOf(frameRegister, gs::Frame::kPsm);
            gs::Tex0 tex0;
            tex0.tbp0 = static_cast<uint32_t>(gs::blockOfPage(base));
            tex0.tbw = static_cast<uint32_t>(width);
            tex0.psm = static_cast<uint32_t>(format);
            tex0.tw = 10; // 1024
            tex0.th = 9;  // 512
            return tex0.bits();
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void addCode(ee::Ptr<sce::Vif1Packet> packet, uint32_t code) { sce::vif1PacketAddCode(packet, code); }
    }

    // drawBlur (with the prologue on, a blurred camera, in a level): the
    // last frame over the view: the depth tested but not written, the
    // offset on a whole row, TEX0_1 and TEX1_1 from the last frame, then
    // addMotionBlurSprite and the state put back, in a kick of VU1's GS
    // state program. TEX1_1 is all zero: the original's bit-field writes over
    // a doubleword nothing on the frame's path sets. (The original returns
    // what its last step left, the sprite tag's first doubleword among
    // them; its one caller, beginViewPacket, does not read it.) A game
    // override wraps it (the motion blur setting, the camera's switch and
    // alpha for that call only).
    void drawBlur(ee::Ptr<Camera> camera, ee::Ptr<sce::Vif1Packet> packet)
    {
        const ee::Qword stateTag = *g_blurStateTag;
        const ee::Qword spriteTag = *g_blurSpriteTag;
        if (*g_framePrologueOn == 0)
            return;
        if (camera->motionBlurOn == 0 || *g_framesInLevel == 0)
            return;
        // ZGE4 reads the depth buffer's register from the scratchpad's first word, and the frame's and the
        // offset's from the first draw environment.
        uint64_t zbuf;
        uint64_t frameRegister;
        uint64_t offset;
        if constexpr (game::readable::kIsBmx)
        {
            zbuf = *g_scratchpadZbuf;
            frameRegister = drawEnvironment0()->frame;
            offset = drawEnvironment0()->xyOffset & ~kOffsetYFraction;
        }
        else
        {
            frameRegister = *g_lastFrameRegister;
            zbuf = *g_sceneZbuf;
            offset = *g_sceneXyOffset & ~kOffsetYFraction;
        }
        auto add = [&](uint32_t gsRegister, uint64_t value)
        {
            addGsRegister2(packet, gsRegister, value);
        };

        sce::vif1PacketAlign(packet, 2, 0);
        sce::vif1PacketCnt(packet, 0);
        addCode(packet, vif::flushA());
        addCode(packet, vif::base(0));
        addCode(packet, vif::offset(0));
        sce::vif1PacketAlign(packet, 2, 2);
        sce::vif1PacketOpenUnpack(packet, vif::kUnpackFromTops | 0u, vif::kUnpackV4_32, 4, 4);
        sce::vif1PacketOpenGifTag(packet, stateTag);
        add(gs::kClamp1, *g_blurClamp1);
        add(gs::kTex0_1, *g_blurTex0);
        add(gs::kTex1_1, *g_blurTex1);
        add(gs::kAlpha1, *g_blurAlpha1);
        add(gs::kTest1, *g_blurDepthTest);
        add(gs::kZbuf1, zbuf | kZbufNoWrites);
        add(gs::kTest1, kTestAlwaysDepth);
        add(gs::kXyOffset1, offset);

        const uint64_t texture = lastFrameTexture(frameRegister);
        addFrameFixup(kFixupLastFrameTexture, packet->current, 0);
        add(gs::kTex0_1, texture);
        add(gs::kTex1_1, 0);
        sce::vif1PacketCloseGifTag(packet);

        // The sprite goes in after the state's quadwords.
        const ee::Ptr<uint32_t> unpack = packet->vifCode;
        sce::vif1PacketCloseUnpack(packet);
        const uint32_t afterSprite = addMotionBlurSprite(packet, camera, 1, 0, 0, vif::unpackQuadwords(*unpack));
        sce::vif1PacketAlign(packet, 2, 2);
        sce::vif1PacketOpenUnpack(packet, (afterSprite & 0xFFFFu) | vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);
        sce::vif1PacketOpenGifTag(packet, spriteTag);
        // (ZGE4 puts back the offset as it sent it, its y fraction off.)
        if constexpr (game::readable::kIsBmx)
        {
            add(gs::kZbuf1, *g_scratchpadZbuf);
            addFrameFixup(kFixupDrawOffset, packet->current, 0);
            add(gs::kXyOffset1, offset);
        }
        else
        {
            add(gs::kZbuf1, *g_sceneZbuf);
            addFrameFixup(kFixupDrawOffset, packet->current, 0);
            add(gs::kXyOffset1, *g_sceneXyOffset);
        }
        sce::vif1PacketCloseGifTag(packet);
        sce::vif1PacketCloseUnpack(packet);
        addCode(packet, kCallGsStateProgram);
        addCode(packet, vif::flushA());
        sce::vif1PacketCnt(packet, 0);
    }
    READABLE_FUNCTION(drawBlur, Verified);
}
