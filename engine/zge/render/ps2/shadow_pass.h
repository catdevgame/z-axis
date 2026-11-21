#pragma once

// The shadow pass's PS2 form (ps2/shadow_pass.cpp, ps2/screen_rectangles.cpp):
// the shadow packet, its two buffers and the VU1 address its unpacks go to,
// the GS words the volume is drawn with and those it puts back, the GIF tags
// and colours VU1 is given.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <array>
#include <cstdint>
#include "sym/zge/render/ps2/shadow_pass.h.h"

namespace sce
{
    struct Vif1Packet;
}

namespace zge
{
    // The shadow volume's packet and the GS state it is drawn with (ps2/shadow_pass.cpp).
    namespace shadow
    {
        // 1 once the packet's buffers are allocated (allocateShadowBuffers).
        inline ee::Global<uint32_t, sym::g_buffersReady> g_buffersReady;
        // The shadow packet's two 48 KB buffers (ZGE2: 32 KB; uncached
        // addresses), one written while VIF1 reads the other; which one this
        // frame writes.
        constexpr uint32_t kPacketBufferBytes = game::readable::kIsDm2 ? 0x8000u : 0xC000u;
        inline ee::Global<std::array<uint32_t, 2>, sym::g_packetBuffers> g_packetBuffers;
        inline ee::Global<uint32_t, sym::g_packetBuffer> g_packetBuffer;
        // The VU1 address the next unpack goes to (in quadwords).
        inline ee::Global<uint32_t, sym::g_vuAddress> g_vuAddress;
        // The shadow's sceVif1Packet (the chain the driver's caller sends).
        constexpr uint32_t kPacket = sym::packet;
        // FRAME_2 and ZBUF_2 for drawing the volume (context 2) (set up with the
        // shadow buffers); the scratchpad keeps the values they go
        // back to. ZGE4 keeps only the ZBUF there, in its first word (its
        // FRAME_2 goes back as the volume's with the drawn frame's FBP, a frame
        // fix-up): kSceneFrame is ZGE2's and ZGE3's.
        inline ee::Global<uint64_t, sym::g_volumeFrame> g_volumeFrame;
        inline ee::Global<uint64_t, sym::g_volumeZbuf> g_volumeZbuf;
        constexpr uint32_t kSceneFrame = PS2_SCRATCHPAD_BASE;
        // (ZGE2 keeps the scene's ZBUF a quadword in, at +0x10: DM2's beginShadowPass, applyShadowBuffer
        // and buildShadowVolumePacket read it there; ZGE4 in the first word.)
        constexpr uint32_t kSceneZbuf = PS2_SCRATCHPAD_BASE + game::readable::perGame(8u, 0x10u, 0u);
        inline ee::Global<uint64_t, kSceneFrame> g_sceneFrame;
        inline ee::Global<uint64_t, kSceneZbuf> g_sceneZbuf;
        // GIF tags (PACKED, EOP): A+D with NLOOP filled in when closed
        // then one RGBAQ; one triangle strip in context 2 with alpha blending, three
        // XYZ2 per vertex (PRIM 0x244).
        constexpr uint32_t kTagAd = sym::tagAd;
        constexpr uint32_t kTagRgbaq = sym::tagRgbaq;
        constexpr uint32_t kTagStrip = sym::tagStrip;
        // The two RGBAQ values that follow the RGBAQ tags.
        constexpr uint32_t kColour0 = sym::colour0;
        constexpr uint32_t kColour1 = sym::colour1;
        // The current camera's view -> GS screen matrix (the camera's
        // +0x1C0, set by beginCameraFrame).
        inline ee::Global<ee::Ptr<Matrix>, sym::g_screenProjection> g_screenProjection;
        // Added to the camera's near clip for VU1 (0.1).
        inline ee::Global<ee::Float, sym::g_nearOffset> g_nearOffset;
    }

    // ps2/screen_rectangles.cpp: screenRectangle, screenTexturedRectangle (packet, x, y,
    // width, height, blend, colour, endOfPacket; the ninth, on the
    // caller's stack, the GS context): a sprite's unpack; the VU1 address after it.
    uint32_t screenRectangle(ee::Ptr<sce::Vif1Packet> packet, int32_t x, int32_t y, int32_t width, int32_t height,
                             uint32_t blend, uint32_t colour, uint32_t endOfPacket);
    uint32_t screenTexturedRectangle(ee::Ptr<sce::Vif1Packet> packet, int32_t x, int32_t y, int32_t width, int32_t height,
                                     uint32_t blend, uint32_t colour, uint32_t endOfPacket);
}
