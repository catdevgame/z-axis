#pragma once
// The primitive API (primitives.cpp; its PS2 form, ps2/primitive_packets.cpp): the effects,
// particles, fog planes, zgui and the text hand in batches of vertices, as
// triangle strips, fans, lines or quads, drawn in the 2D packet or the world
// one by their flags.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/packet_records.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/primitives.h.h"

namespace zge
{
    struct Matrix;   // zge/math/types.h
    struct Material; // zge/render/material.h

    // A vertex as the callers hand it in.
    struct alignas(8) PrimitiveVertex
    {
        ee::Float x, y;      // +0x00 world space, or the 2D packet's screen position
        ee::Float z;         // +0x08
        uint32_t _0c;
        ee::Float u, v;      // +0x10 texture coordinates (the 2D packet copies the eight bytes as they are)
        uint8_t rgba[4];     // +0x18 0x80 is full intensity
    };
    static_assert(sizeof(PrimitiveVertex) == 0x20 && offsetof(PrimitiveVertex, u) == 0x10 && offsetof(PrimitiveVertex, rgba) == 0x18);

    // A batch: strips that share a texture (only the first strip's is bound).
    struct PrimitiveStrip
    {
        uint32_t texture;                        // +0x00 0: the slot's default (setPrimitiveTexture)
        ee::Ptr<ee::Ptr<PrimitiveVertex>> vertices; // +0x04
        int32_t count;                           // +0x08
    };
    static_assert(sizeof(PrimitiveStrip) == 0x0C);

    // One line of a batch.
    struct PrimitiveLine
    {
        uint32_t texture;                 // +0x00 only the first line's is bound
        ee::Ptr<PrimitiveVertex> first;   // +0x04
        ee::Ptr<PrimitiveVertex> second;  // +0x08
    };
    static_assert(sizeof(PrimitiveLine) == 0x0C);

    // A batch of lists of lines: a list's vertices in a row, two a line.
    struct PrimitiveLineList
    {
        uint32_t texture;                  // +0x00 only the first list's is bound
        int32_t lines;                     // +0x04
        ee::Ptr<PrimitiveVertex> vertices; // +0x08
    };
    static_assert(sizeof(PrimitiveLineList) == 0x0C);

    // A triangle as drawPrimitiveStrips' callers hand it in.
    struct PrimitiveTriangle
    {
        uint32_t texture;                     // +0x00 0: the slot's default
        ee::Ptr<PrimitiveVertex> vertices[3]; // +0x04
    };
    static_assert(sizeof(PrimitiveTriangle) == 0x10);

    // The flags (the primitive API's first argument).
    enum PrimitiveFlags : uint32_t
    {
        kPrimitiveSecondSlot = 1u << 1,      // the default texture is the slot's second
        kPrimitiveDepthFromVertex = 1u << 2, // 2D: the batch's depth projected from its first vertex
        kPrimitiveTextured = 1u << 3,        // colours not halved (the texture's own)
        kPrimitiveClipLines = 1u << 4,       // world lines: clipped to the view first
        kPrimitiveViewRelative = 1u << 24,   // world: the matrix composed with the view's
        kPrimitiveScreen = 1u << 25,         // the 2D packet (primitives.cpp)
        kPrimitiveSameMatrix = 1u << 26,     // the last batch's matrix again
    };

    // (primitives.cpp) drawPrimitives: a batch of strips to the 2D or the world
    // builder by its flags; primitiveRendererReady: always 1;
    // drawTexturedPrimitives: a 2D batch of line lists (drawScreenLineLists),
    // a world one nothing (0).
    int32_t drawPrimitives(uint32_t flags, ee::Address strip, int32_t strips, ee::Address matrix);
    uint32_t primitiveRendererReady();
    int32_t drawTexturedPrimitives(uint32_t flags, ee::Address lists, int32_t count, ee::Address matrix);
    // drawLines: a batch of lines (the lines' and the matrix's addresses as words).
    int32_t drawLines(uint32_t primitiveFlags, uint32_t vertices, uint32_t vertexCount, uint32_t vertexMatrix);

    // (ps2/primitive_packets.cpp)
    int32_t drawScreenPrimitives(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix);
    int32_t drawScreenFans(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix);
    int32_t drawWorldPrimitives(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix);
    int32_t drawWorldFans(uint32_t flags, ee::Ptr<PrimitiveStrip> strip, int32_t strips, ee::Ptr<Matrix> matrix);
    int32_t drawWorldLines(uint32_t flags, ee::Ptr<PrimitiveLine> line, int32_t lines, ee::Ptr<Matrix> matrix);
    int32_t drawScreenLines(uint32_t flags, ee::Ptr<PrimitiveLine> line, int32_t lines, ee::Ptr<Matrix> matrix);
    int32_t drawScreenLineLists(uint32_t flags, ee::Ptr<PrimitiveLineList> list, int32_t lists, ee::Ptr<Matrix> matrix);
    // The words after the renderer's flag (ps2/primitive_packets.cpp): the
    // materials the primitives bound, a list for each buffer set (each
    // material once a frame: it keeps the frame it was listed in), the end
    // of the frame's list, and each set's count. The VIF1 handler uploads
    // the textures of the list the last frame filled (primitiveTextures).
    // ZGE2's entry points (drawPrimitives, drawLines and the rest) draw nothing before initPrimitiveRenderer has run;
    // ZGE3 and ZGE4 dropped the test. The word ZGE2 returns then is what the test left in the result register: the ready
    // word's address high half.
    inline ee::Global<uint32_t, sym::g_primitiveRendererReady> g_primitiveRendererReadyWord;
    constexpr int32_t kPrimitiveNotReadyResult = static_cast<int32_t>(ee::high(sym::g_primitiveRendererReady));
    inline bool primitiveRendererOff()
    {
        if constexpr (game::readable::kIsDm2)
            return *g_primitiveRendererReadyWord == 0;
        else
            return false;
    }

    constexpr uint32_t kMaxBoundMaterials = game::readable::perGame(64u, 64u, 128u); // (ZGE4 lists twice as many: its words past them sit 0x200 further on)
    struct PrimitiveWords
    {
        uint32_t ready;
#if PS2X_GAME_DM2
        // DM2 +0x04: 1 while primitives are drawn (initPrimitiveRenderer sets it;
        // DM2's drawLines and drawPrimitiveQuads draw nothing at 0, and a setter
        // after them sets it, returning the value it had). ZGE3 has no such word.
        uint32_t primitivesOn;
#endif
        ee::Ptr<Material> defaultMaterials[7];          // +0x04 (DM2 +0x08) createPrimitiveMaterials
        ee::Ptr<Material> boundMaterials[2][kMaxBoundMaterials]; // +0x20 (DM2 +0x24)
        ee::Ptr<ee::Ptr<Material>> boundEnd;                     // +0x220 (DM2 +0x224)
        int32_t boundCount[2];                          // +0x224 (DM2 +0x228) one a set
        uint32_t frame;                                 // +0x22C (DM2 +0x230) counted by resetPrimitiveBuffers
    };

    // ZGE2's switch (DM2's PrimitiveWords::primitivesOn): the shared bodies reach it through this, so they compile in
    // every game and name it only where kIsDm2 (ZGE3's and ZGE4's words have none: never reached).
#if PS2X_GAME_DM2
    inline uint32_t &primitivesOnWord(PrimitiveWords &words) { return words.primitivesOn; }
#else
    inline uint32_t &primitivesOnWord(PrimitiveWords &) { static uint32_t none = 0; return none; }
#endif
    // (DM2's offsets as its createPrimitiveMaterials, primitiveTextures, closeScreenPrimitivePackets; BMX XXX's as its primitiveTextures,
    // closeScreenPrimitivePackets, initPrimitiveRenderer and the line builders
    // and resetPrimitiveBuffers use them)
    static_assert(offsetof(PrimitiveWords, defaultMaterials) == game::readable::perGame(0x04u, 0x08u, 0x04u) &&
                  offsetof(PrimitiveWords, boundMaterials) == game::readable::perGame(0x20u, 0x24u, 0x20u) &&
                  offsetof(PrimitiveWords, boundEnd) == game::readable::perGame(0x220u, 0x224u, 0x420u) &&
                  offsetof(PrimitiveWords, boundCount) == game::readable::perGame(0x224u, 0x228u, 0x424u) &&
                  offsetof(PrimitiveWords, frame) == game::readable::perGame(0x22Cu, 0x230u, 0x42Cu));

    int32_t drawPrimitiveStrips(uint32_t flags, ee::Address triangleRecords, int32_t count, ee::Address matrix);
    int32_t drawPrimitiveQuads(uint32_t flags, ee::Address quadRecords, int32_t count, ee::Address matrixAddress);
    void resetPrimitiveBuffers();
    // ZGE4's resetPrimitiveBuffers (its own body, BMX XXX's alone): the block for the primitives' textures opened in
    // the packet given (beginFramePackets' frame packet), none when it is 0.
    void resetPrimitiveBuffersIntoPacket(ee::Ptr<sce::Vif1Packet> packet);
    // ZGE4 only: the material listed for this frame's primitive texture uploads, once a frame.
    void listPrimitiveMaterial(ee::Ptr<Material> material);
    void primitiveTextures(ee::Ptr<int32_t> countOut, ee::Ptr<ee::Ptr<ee::Ptr<Material>>> listOut);
    void addViewportPacket();
    int32_t closeScreenPrimitivePackets();
    uint32_t initPrimitiveRenderer();
    uint32_t shutdownPrimitiveRenderer();
    int32_t flipBufferSet();
}
