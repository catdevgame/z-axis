#pragma once

// The renderer's state (0x1B0 bytes at g_renderer, cleared by initRenderer,
// renderer.cpp, ps2/frame_packets.cpp): the two frame packet buffers DMA sends one of while
// the other is built, the view's mesh packet the class renderers append
// to, the texture requests' queues, the statistics, and the lights' VU0
// matrices. Other files keep their own globals over some of these words
// (render/ps2/texture_upload.cpp, render/ps2/class_draws.cpp); the offsets are
// the same.

#include "ee/ee.h"
#include "ee/readable.h"
#include "sce/packet_records.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/renderer.h.h"

namespace zge
{
#if !PS2X_GAME_DM2 && !PS2X_GAME_BMX
    // AI's renderer, 0x1B0 bytes.
    struct Renderer
    {
        uint32_t ready;                  // +0x000 set by initRenderer, cleared by shutDownRenderer
        uint32_t frameBuffers[2];        // +0x004 the frame packets' buffers (0x20000 bytes each)
        uint32_t frame;                  // +0x00C which one is being built (flipMeshPacket)
        uint32_t frameBuffer;            // +0x010 its address (0: none yet)
        uint8_t _014[0x0C];
        sce::Vif1Packet meshPacket;      // +0x020 the view's packet (written in frameBuffer, uncached)
        uint8_t _038[0x08];
        sce::Vif1Packet clearPacket;     // +0x040 writes an empty chain into both buffers (clearFramePackets)
        uint8_t _058[0x08];
        uint32_t framePrologue;          // +0x060 what addFramePrologue sends each view first [inferred]
        ee::Ptr<uint32_t> requestQueues[2]; // +0x064 the texture requests' queues, a frame each
        ee::Ptr<uint32_t> requestQueueEnd; // +0x06C where the next request goes
        ee::Ptr<uint32_t> requestQueueRead; // +0x070 the next queued request (the VIF1 interrupt's)
        uint32_t uploadBuffers[2];       // +0x074 the GIF packets' buffers
        uint32_t uploadBuffer;           // +0x07C which one this upload uses
        uint32_t marker;                 // +0x080
        uint32_t vu1StateOwner;          // +0x084 the class renderer whose VU1 state went out last
        uint32_t markerNeeded;           // +0x088
        uint32_t _08c;
        uint32_t profileTimer;           // +0x090 the frame's profile timer (zprofile.c)
        uint32_t frameDmaTime;           // +0x094 its time when the frame packet's DMA ended (onFramePacketSent)
        uint32_t materialsSeen;          // +0x098 statistics: materials looked at
        uint32_t interrupts;             // +0x09C interrupts handled
        uint32_t clutBytes;              // +0x0A0 CLUT bytes uploaded
        uint32_t texelBytes;             // +0x0A4 texel bytes uploaded
        uint32_t leastQueueWords;        // +0x0A8 the fewest request queue words a frame used (0x300 at the start)
        uint32_t leastPacketBytes;       // +0x0AC the fewest bytes a frame left free in its buffer (0x20000 at the start)
        Matrix normalLightMatrix;        // +0x0B0 sceVu0NormalLightMatrix of the default lights
        Matrix lightColourMatrix;        // +0x0F0 sceVu0LightColorMatrix of them
        Vec4 globalLightDirections[4];   // +0x130 the scene's lights (zge/scene/light.h)
        Vec4 globalLightColours[4];      // +0x170
    };
#elif PS2X_GAME_DM2
    // DM2's renderer, 0x130 bytes: no clear packet, no statistics, of the scene lights only their colours. AI's words it
    // does not have stand past the end, for the functions with no DM2 counterpart.
    struct Renderer
    {
        uint32_t ready;                  // DM2 +0x000
        // DM2 +0x004 1 from initRenderer: DM2's queuePartDraw queues nothing at 0.
        uint32_t drawingOn;
        uint32_t frameBuffers[2];        // DM2 +0x008
        uint32_t frame;                  // DM2 +0x010
        uint32_t frameBuffer;            // DM2 +0x014
        uint8_t _018[0x08];
        sce::Vif1Packet meshPacket;      // DM2 +0x020
        uint8_t _038[0x08];
        ee::Ptr<uint32_t> requestQueues[2]; // DM2 +0x040
        ee::Ptr<uint32_t> requestQueueEnd; // DM2 +0x048
        ee::Ptr<uint32_t> requestQueueRead; // DM2 +0x04C
        uint32_t uploadBuffers[2];       // DM2 +0x050
        uint32_t uploadBuffer;           // DM2 +0x058
        uint32_t marker;                 // DM2 +0x05C
        uint32_t vu1StateOwner;          // DM2 +0x060
        uint32_t markerNeeded;           // DM2 +0x064
        uint8_t _068[0x08];
        Matrix normalLightMatrix;        // DM2 +0x070 (initRenderer; DM2's drawSkinned takes its lights from here)
        Matrix lightColourMatrix;        // DM2 +0x0B0
        Vec4 globalLightColours[4];      // DM2 +0x0F0 the scene lights' colours (initRenderer's third matrix)
        sce::Vif1Packet clearPacket;
        uint32_t framePrologue, profileTimer, frameDmaTime, materialsSeen, interrupts, clutBytes, texelBytes;
        uint32_t leastQueueWords, leastPacketBytes;
        Vec4 globalLightDirections[4];
    };
#else
    // BMX XXX's renderer, 0x230 bytes (initRenderer clears them): the frame being built is the display's
    // frameParity (beginFramePackets), so no word for it here, and the packets come 0x10 sooner; a word more
    // after the request queues' read word; two pairs of default light matrices before the scene's lights
    // (updateNodeLights). AI's words not found in it stand past the end, for the functions with no BMX counterpart.
    struct Renderer
    {
        uint32_t ready;                  // +0x000
        uint32_t frameBuffers[2];        // +0x004
        uint32_t frameBuffer;            // +0x00C frameBuffers[frame]
        sce::Vif1Packet meshPacket;      // +0x010
        uint8_t _028[0x08];
        sce::Vif1Packet clearPacket;     // +0x030
        uint8_t _048[0x08];
        uint32_t framePrologue;          // +0x050
        ee::Ptr<uint32_t> requestQueues[2]; // +0x054
        ee::Ptr<uint32_t> requestQueueEnd; // +0x05C
        ee::Ptr<uint32_t> requestQueueRead; // +0x060
        ee::Ptr<uint32_t> frameRequestQueue; // +0x064 where the frame's request queue starts (beginFramePackets; finishFrame reads it)
        uint32_t uploadBuffers[2];       // +0x068
        uint32_t uploadBuffer;           // +0x070
        uint32_t marker;                 // +0x074
        uint32_t vu1StateOwner;          // +0x078
        uint32_t markerNeeded;           // +0x07C
        uint32_t _080;
        uint32_t profileTimer;           // +0x084
        uint32_t frameDmaTime;           // +0x088
        uint32_t materialsSeen;          // +0x08C
        uint32_t interrupts;             // +0x090
        uint32_t clutBytes;              // +0x094 (vif1Interrupt)
        uint32_t texelBytes;             // +0x098
        uint32_t leastQueueWords;        // +0x09C
        uint32_t leastPacketBytes;       // +0x0A0
        uint8_t _0a4[0x0C];
        // +0x0B0, +0x0F0 ZGE4's second pair of default light matrices: updateNodeLights starts the light block of a
        // node of the first draw bucket (kNodeSecondBucket clear) from them (initRenderer's own light data).
        Matrix firstBucketNormalLightMatrix;
        Matrix firstBucketLightColourMatrix;
        Matrix normalLightMatrix;        // +0x130 the default lights' (AI's data), for the second bucket's nodes
        Matrix lightColourMatrix;        // +0x170
        Vec4 globalLightDirections[4];   // +0x1B0 (g_globalLights: its data pair)
        Vec4 globalLightColours[4];      // +0x1F0
        uint32_t frame;
    };
#endif
    constexpr uint32_t kRendererBytes = game::readable::perGame(0x1B0u, 0x130u, 0x230u);
    // Each game's offsets, as its machine code uses them; a word of AI's a game does not have stands past its
    // renderer's end.
    static_assert(offsetof(Renderer, meshPacket) == game::readable::perGame(0x20u, 0x20u, 0x10u) &&
                  offsetof(Renderer, requestQueues) == game::readable::perGame(0x64u, 0x40u, 0x54u) &&
                  offsetof(Renderer, uploadBuffers) == game::readable::perGame(0x74u, 0x50u, 0x68u) &&
                  offsetof(Renderer, vu1StateOwner) == game::readable::perGame(0x84u, 0x60u, 0x78u));
    static_assert(game::readable::perGame(offsetof(Renderer, clearPacket) == 0x40, offsetof(Renderer, clearPacket) >= kRendererBytes,
                                          offsetof(Renderer, clearPacket) == 0x30) &&
                  game::readable::perGame(offsetof(Renderer, profileTimer) == 0x90, offsetof(Renderer, profileTimer) >= kRendererBytes,
                                          offsetof(Renderer, profileTimer) == 0x84) &&
                  game::readable::perGame(offsetof(Renderer, leastQueueWords) == 0xA8, offsetof(Renderer, leastQueueWords) >= kRendererBytes,
                                          offsetof(Renderer, leastQueueWords) == 0x9C) &&
                  game::readable::perGame(offsetof(Renderer, leastPacketBytes) == 0xAC, offsetof(Renderer, leastPacketBytes) >= kRendererBytes,
                                          offsetof(Renderer, leastPacketBytes) == 0xA0));
    static_assert(game::readable::perGame(offsetof(Renderer, normalLightMatrix) == 0xB0, offsetof(Renderer, normalLightMatrix) == 0x70,
                                          offsetof(Renderer, normalLightMatrix) == 0x130) &&
                  game::readable::perGame(offsetof(Renderer, globalLightDirections) == 0x130, offsetof(Renderer, globalLightDirections) >= kRendererBytes,
                                          offsetof(Renderer, globalLightDirections) == 0x1B0) &&
                  game::readable::perGame(offsetof(Renderer, globalLightColours) == 0x170, offsetof(Renderer, globalLightColours) == 0xF0,
                                          offsetof(Renderer, globalLightColours) == 0x1F0) &&
                  game::readable::perGame(offsetof(Renderer, frame) == 0x0C, offsetof(Renderer, frame) == 0x10,
                                          offsetof(Renderer, frame) >= kRendererBytes));
#if PS2X_GAME_BMX
    static_assert(offsetof(Renderer, firstBucketNormalLightMatrix) == 0xB0 && offsetof(Renderer, normalLightMatrix) == 0x130 &&
                  offsetof(Renderer, lightColourMatrix) == 0x170);
#endif
    static_assert(game::readable::perGame(sizeof(Renderer) == 0x1B0, sizeof(Renderer) >= kRendererBytes, sizeof(Renderer) >= kRendererBytes));

    // The MARKs around the texture requests in the view's packets: the VIF1 interrupt (vif1Interrupt,
    // ps2/texture_upload.cpp) reads VIF1_MARK to tell whose block stopped VIF1. Each block ends with kMarkNoTextures.
    enum TextureMark : uint32_t
    {
        kMarkNoTextures = 0,        // past a block: nothing to upload
        kMarkNodeTextures = 1,      // a node's request (ps2/texture_request.cpp; ZGE2's class renderers): the next queued
        kMarkPrimitiveTextures = 2, // the primitive API's materials of the frame (ps2/primitive_packets.cpp)
    };
    // A node request's marker (Renderer::marker: the code that takes the interrupt bit) has the first level to
    // upload in its immediate's bits 0-3: kMarkerFar, from level 1 (the far ones), or 0, every level.
    constexpr uint32_t kMarkerFirstLevel = 0xFu;
    constexpr uint32_t kMarkerFar = 1u;

    // ZGE2's frame counts: DM2's 0x34-byte record after the frame's state (startEngine clears it), where ZGE3 keeps
    // its statistics in the renderer. No code of the release build counts into them: a camera's view clears the
    // view's two groups of three as it begins (beginViewDraws), finishFrame the frame's two once the frame is sent,
    // and summedFrameCounts adds the groups up for playFrame's debug line (which shows only the frame rate and
    // vramFreeBytes).
    struct FrameCounts
    {
        uint32_t totals[3];  // +0x00 view[0][i] + view[1][i] (summedFrameCounts)
        uint32_t view[2][3]; // +0x0C
        uint32_t frame[2];   // +0x24
        int32_t vramFreeBytes; // +0x2C the texture cache's free bytes (advanceTextureCache; the debug line's "VramFree", in KB)
        uint32_t unused;     // +0x30 (only startEngine's clear writes it)
    };
    static_assert(sizeof(FrameCounts) == 0x34 && offsetof(FrameCounts, frame) == 0x24 && offsetof(FrameCounts, vramFreeBytes) == 0x2C);
    inline ee::Global<FrameCounts, sym::dm2::g_frameCounts> g_frameCounts;

    // The frame's request queue, from its start (finishFrame): ZGE4 keeps where it starts as it begins the frame
    // (beginFramePackets), ZGE2 and ZGE3 take the queue of the frame being built.
    inline ee::Ptr<uint32_t> frameRequestQueue(const Renderer &renderer)
    {
#if PS2X_GAME_BMX
        return renderer.frameRequestQueue;
#else
        return renderer.requestQueues[renderer.frame];
#endif
    }
    inline void startFrameRequestQueue([[maybe_unused]] Renderer &renderer, [[maybe_unused]] ee::Ptr<uint32_t> queue)
    {
#if PS2X_GAME_BMX
        renderer.frameRequestQueue = queue;
#endif
    }

    // The default light matrices a node's own light block starts from (updateNodeLights): ZGE4 keeps a second pair
    // for the nodes of the first draw bucket; ZGE2 and ZGE3 start every node from the one pair.
    inline Matrix &firstBucketNormalLights(Renderer &renderer)
    {
#if PS2X_GAME_BMX
        return renderer.firstBucketNormalLightMatrix;
#else
        return renderer.normalLightMatrix;
#endif
    }
    inline Matrix &firstBucketLightColours(Renderer &renderer)
    {
#if PS2X_GAME_BMX
        return renderer.firstBucketLightColourMatrix;
#else
        return renderer.lightColourMatrix;
#endif
    }

    inline ee::Global<Renderer, sym::g_renderer> g_renderer;
    constexpr uint32_t kRendererAddress = sym::g_renderer;

    // The class builders' packet (ps2/class_packets.cpp) and the material
    // chains' (buildMaterialChain), right after the renderer's state.
    constexpr uint32_t kChainPacket = sym::chainPacket;

    // ps2/frame_packets.cpp: meshPacket, the view's mesh packet (always
    // kMeshPacket); setVu1StateOwner, the class renderer whose VU1 state went
    // out last set (its result: the renderer's address).
    uint32_t meshPacket();
    int32_t setVu1StateOwner(uint32_t owner);
}

namespace zge
{
    // (renderer.cpp)
    uint32_t initRenderer();
    uint32_t shutDownRenderer();
    void finishFrame();
    void beginViewPacket();
    int32_t endViewDraws(); // ZGE2 only: a camera's view ended (its renderFrame)
    // (ps2/frame_packets.cpp)
    int32_t sendFramePacket(uint32_t other);
    uint32_t beginFramePackets();
    void closeFramePacket();
    uint32_t clearFramePackets();
}
