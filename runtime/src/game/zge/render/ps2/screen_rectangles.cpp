// Screen rectangles for the shadow pass, as the PS2 draws them (zge/render/shadow.h; the shadow
// block screenTexturedRectangle-beginShadowPass): one GS sprite as a VU1 unpack in the shadow
// packet, a GIF tag with its registers and the corners in GS screen
// coordinates. The pass clears the shadow buffer with the flat one and lays
// the buffer over the frame with the textured one (beginShadowPass,
// applyShadowBuffer).

#include "zge/render/shadow.h"
#include "zge/render/ps2/shadow_pass.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "sce/packet_calls.h"
#include "zge/render/display.h"
#include "zge/render/fixed_point.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"
#include "zge/math/vec.h"

#include <cstdint>
#include <cstring>
#include "sym/zge/render/ps2/screen_rectangles.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kCloseUnpack = sym::vif1PacketCloseUnpack; // sceVif1PkCloseUpkCode (called by address: a game override hooks it)
        constexpr uint32_t kAddDword = sym::vif1PacketAddDword2;    // the second copy of the library's doubleword add


        // PRIM: a sprite; with a texture, its UVs as texel coordinates;
        // the caller's blend sets ABE, its context CTXT (context 2).
        constexpr uint64_t kPrimSprite = gs::kSprite;
        constexpr uint64_t kPrimTexturedSprite = gs::kSprite | gs::kTextured | gs::kTexelCoords;
        // The register lists: RGBAQ, XYZ2, XYZ2 for the flat sprite; RGBAQ,
        // ST, UV, XYZ2, UV, XYZ2 for the textured one.
        constexpr uint64_t kRegsFlat = gs::registerList({gs::kRgbaq, gs::kXyz2, gs::kXyz2});
        constexpr uint64_t kRegsTextured = gs::registerList({gs::kRgbaq, gs::kSt, gs::kUv, gs::kXyz2, gs::kUv, gs::kXyz2});
        constexpr uint64_t kFlatRegisters = 3, kTexturedRegisters = 6;

        // A 32-bit value sign-extended to a doubleword, as the originals
        // widen one.
        uint64_t widened(uint32_t value) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))); }
        // The PRIM word as the originals build it (a 32-bit value
        // sign-extended).
        uint64_t primWord(uint64_t prim, uint32_t blend, uint32_t context)
        {
            const uint64_t word = (widened(blend) << gs::kPrimBlendShift) | (widened(context) << gs::kPrimContextShift) | prim;
            return widened(static_cast<uint32_t>(word));
        }
        // The sprite's GIF tag: once through the list of `registers`, PRIM
        // set from `prim`, EOP from the caller.
        uint64_t spriteTag(uint64_t prim, uint64_t registers, uint32_t endOfPacket)
        {
            return gs::GifTag{.loops = 1, .endOfPacket = widened(endOfPacket), .setsPrim = 1, .prim = prim, .registers = registers}.bits();
        }

        // The caller's ninth argument: past the eighth, the calling
        // convention puts an argument on the caller's stack, in the word
        // just above this function's frame.
        uint32_t ninthArgument() { return *ee::at<uint32_t>(ee::t_ee.frame->fnSp); }

        // The quadword at an address (rounded down to its 16 bytes), or a
        // 16-byte record, as the packet takes it.
        template <typename T>
        ee::Qword asQuadword(const T &value)
        {
            static_assert(sizeof(T) == sizeof(ee::Qword));
            ee::Qword quadword;
            std::memcpy(&quadword, &value, sizeof quadword);
            return quadword;
        }
        ee::Qword quadwordAt(uint32_t address) { return asQuadword(quadwordOf(ee::Ptr<uint32_t>(address))); }

        // The second copy of the library's doubleword add, at its own symbol.
        void addDword(ee::Ptr<sce::Vif1Packet> packet, uint64_t value) { sce::vif1PacketAddDword2(packet, value); }

        // A GS vertex in the frame: X, Y (12.4 screen coordinates), Z.
        struct ScreenVertex
        {
            int32_t x, y;
            uint64_t z;
        };
        static_assert(sizeof(ScreenVertex) == 16);

        // ST with Q; the quadword's last word goes out as the frame has it
        // (the sprite's code never writes it).
        struct ScreenSt
        {
            int32_t s, t;
            ee::Float q;
            uint32_t last;
        };
        // UV (12.4 texel coordinates); the upper doubleword as the frame has it.
        struct ScreenUv
        {
            int32_t u, v;
            uint64_t upper;
        };

        // screenTexturedRectangle's frame, as the original lays it out.
        struct TexturedSpriteFrame
        {
            ScreenVertex corners[2]; // +0x00, +0x10
            ScreenSt st;             // +0x20
            ScreenUv uv;             // +0x30
        };
        static_assert(offsetof(TexturedSpriteFrame, uv) == 0x30);
    }

    // screenRectangle: an untextured sprite from (x, y) `width` by `height`
    // pixels in `colour` (a quadword: RGBAQ), at the GS depth of the near
    // clip; `blend` sets ABE, the caller's ninth argument (the GS context)
    // CTXT (ZGE2's takes none: context 1), `endOfPacket` the GIF tag's EOP.
    // Returns the VU1 address after the unpack (also kept in
    // shadow::g_vuAddress).
    uint32_t screenRectangle(ee::Ptr<sce::Vif1Packet> packet, int32_t x, int32_t y, int32_t width, int32_t height,
                             uint32_t blend, uint32_t colour, uint32_t endOfPacket)
    {
        const uint32_t context = game::readable::kIsDm2 ? 0u : ninthArgument(); // (ZGE2's has eight arguments: no GS context)
        ee::Local<ScreenVertex> local;
        ScreenVertex &vertex = *local;
        const DisplaySetup &display = *g_display;
        sce::vif1PacketOpenUnpack(packet, (*shadow::g_vuAddress & 0xFFFFu) | vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);
        addDword(packet, spriteTag(primWord(kPrimSprite, blend, context), kFlatRegisters, endOfPacket));
        addDword(packet, kRegsFlat);
        sce::vif1PacketAddQword(packet, quadwordAt(colour));

        // The corners in subpixels.
        vertex.x = display.offsetX + subpixels(x);
        vertex.y = display.offsetY + subpixels(y);
        if constexpr (game::readable::kIsDm2) // ZGE2 converts the depth unsigned, to 64 bits (libgcc's __fixunssfdi)
            vertex.z = sce::libgcc::__fixunssfdi(display.depthAtNear);
        else
            vertex.z = static_cast<uint64_t>(static_cast<int64_t>(ee::toInt(display.depthAtNear)));
        sce::vif1PacketAddQword(packet, asQuadword(vertex));
        vertex.x += subpixels(width);
        vertex.y += subpixels(height);
        sce::vif1PacketAddQword(packet, asQuadword(vertex));

        const ee::Ptr<uint32_t> unpack = packet->vifCode;
        sce::vif1PacketCloseUnpack(packet);
        *shadow::g_vuAddress = *shadow::g_vuAddress + vif::unpackQuadwords(*unpack);
        return *shadow::g_vuAddress;
    }
    READABLE_FUNCTION(screenRectangle, Verified);

    // screenTexturedRectangle: the textured sprite (same arguments): the bound texture
    // (TEX0 set by the caller) one texel a pixel over `width` by `height`
    // from the screen's left edge (x is not read: both calls pass 0) at row
    // y, half a line lower on the even field, Z 1. Returns the VU1 address
    // after the unpack.
    uint32_t screenTexturedRectangle(ee::Ptr<sce::Vif1Packet> packet, int32_t /*x*/, int32_t y, int32_t width, int32_t height,
                                     uint32_t blend, uint32_t colour, uint32_t endOfPacket)
    {
        const uint32_t context = ninthArgument();
        ee::Local<TexturedSpriteFrame> local;
        TexturedSpriteFrame &f = *local;
        const DisplaySetup &display = *g_display;

        // A field frame's lines fall half a line apart: the even one moved
        // down half a line (8 subpixels).
        const int32_t fieldOffset = display.oddField != 0 ? 0 : 8;
        sce::vif1PacketOpenUnpack(packet, (*shadow::g_vuAddress & 0xFFFFu) | vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);

        const int32_t width16 = subpixels(width), height16 = subpixels(height);
        const int32_t top = display.offsetY + subpixels(y) + fieldOffset;
        f.corners[1].x = display.offsetX + width16;
        f.corners[1].y = top + height16;
        f.corners[1].z = 1;
        f.corners[0].x = display.offsetX;
        f.corners[0].y = top;
        f.corners[0].z = 1;
        addDword(packet, spriteTag(primWord(kPrimTexturedSprite, blend, context), kTexturedRegisters, endOfPacket));
        addDword(packet, kRegsTextured);
        sce::vif1PacketAddQword(packet, quadwordAt(colour));

        // ST (0, 0) with Q 1.
        f.st.s = 0;
        f.st.q = ee::Float(1.0f);
        f.st.t = 0;
        sce::vif1PacketAddQword(packet, asQuadword(f.st));
        f.uv.u = 0;
        f.uv.v = 0;
        sce::vif1PacketAddQword(packet, asQuadword(f.uv));
        sce::vif1PacketAddQword(packet, asQuadword(f.corners[0]));
        f.uv.u += width16;
        f.uv.v += height16;
        sce::vif1PacketAddQword(packet, asQuadword(f.uv));
        sce::vif1PacketAddQword(packet, asQuadword(f.corners[1]));

        const ee::Ptr<uint32_t> unpack = packet->vifCode;
        sce::vif1PacketCloseUnpack(packet);
        *shadow::g_vuAddress = *shadow::g_vuAddress + vif::unpackQuadwords(*unpack);
        return *shadow::g_vuAddress;
    }
    READABLE_FUNCTION(screenTexturedRectangle, Verified);
}
