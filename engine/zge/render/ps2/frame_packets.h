#pragma once

// The frame packets' PS2 form (ps2/frame_packets.cpp): the double buffer's
// fix-ups, the mesh packet's buffers, and the PS2 steps the renderer
// (zge/render/renderer.cpp) calls.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstdint>

namespace zge
{
    // The bytes of each of the two frame packet buffers.
    constexpr uint32_t kFrameBufferBytes = 0x00020000u;

    // A fix-up of the frame's packet (ps2/frame_packets.cpp), written when the
    // packet is sent, for the draw environment the display then uses.
    // ZGE4 adds a kind, first, and numbers the others after it.
    enum : uint32_t
    {
        // ZGE4 only: FRAME_1's FBP (bits 0-8) set to the drawing
        // environment's: the frame being drawn (the shadow passes' FRAME).
        kFixupDrawFrame = 0,
        // TEX0's TBP0 (bits 0-13) set from the other environment's
        // FRAME_1 FBP x 32: the frame drawn last, as a texture.
        kFixupLastFrameTexture = game::readable::perGame(0u, 0u, 1u),
        // The doubleword set to the drawing environment's XYOFFSET_1
        // [inferred].
        kFixupDrawOffset = game::readable::perGame(1u, 1u, 2u),
    };
    // A fix-up (12 bytes): its kind, the doubleword to write and a value (both uncached).
    struct FrameFixup
    {
        uint32_t kind; // kFixup*
        ee::Ptr<uint64_t> word;
        uint32_t value;
    };
    static_assert(sizeof(FrameFixup) == 12);
    ee::Ptr<FrameFixup> addFrameFixup(uint32_t kind, ee::Address address, uint32_t value);
    // flipFrameFixups (ps2/frame_packets.cpp): the list built becomes the one to apply.
    ee::Ptr<ee::Ptr<FrameFixup>> flipFrameFixups();

    // (ps2/frame_packets.cpp)
    void flipMeshPacket();
    void newFrameFixups();
    void freeFrameFixups();
    void resetFrameFixups();
    void applyFrameFixups();
}

namespace zge::ps2
{
    // The renderer's PS2 steps (ps2/frame_packets.cpp): VU1's microprograms
    // sent (initRenderer); ZGE2's mesh packet ended with NEXT to the screen
    // primitives' packets, or END (its finishFrame); the view's draws made to
    // wait for the frame prologue (beginViewPacket).
    void loadMicroprograms();
    void closeMeshPacket(uint32_t screenPrimitives);
    void waitForFramePrologue();
}
