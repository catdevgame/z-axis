#pragma once

// Shadow volumes: the skater's shadow, drawn by VU1 as a volume swept from
// the silhouette of a low-detail shadow part (buildShadowVolume, once per frame
// per shadow; applyShadowBuffer draws the volume's fill).
//
// Each frame the shadow driver buildShadowVolume (node, part):
//   1. poses a skinned shadow part when the node's pose changed
//      (posePart, with the face normals of the posed copy,
//      posedFaceNormals);
//   2. marks, per edge of the part, which of its two faces face the light
//      (markEdgesFacingLight), the light being a point above
//      the node in the part's own space;
//   3. writes the silhouette (the edges between a face that faces the
//      light and one that does not) into a VIF packet for VU1
//      (buildShadowVolumePacket).
//
// A skinned part keeps a posed copy of its positions after the bind pose
// (positions stream: vertexCount bind positions, then vertexCount posed)
// and of its face normals (faceCount, then faceCount posed); the edge marks
// and the volume use the posed copy when the part has skin weights.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/skeleton.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/shadow.h.h"

namespace sce
{
    struct Vif1Packet;
}

namespace zge
{
    struct Node;

    // An edge of a shadow part (ZgePart::shadowEdges, StreamKind::ShadowEdges),
    // built when the part is loaded (buildPartEdges) from the part's faces: the
    // edge shared by two faces.
    struct ShadowEdge
    {
        // Per face: 1 when the face faces the light, written every frame by
        // markEdgesFacingLight (both 0 when the faces have the same normal).
        uint8_t facesLight[2]; // +0x00
        // The edge's two corners, as indices into faces[0]'s vertex list.
        uint8_t corners[2];    // +0x02
        uint16_t faces[2];     // +0x04 the two faces (indices into the part's faces)
    };
    static_assert(sizeof(ShadowEdge) == 8 && offsetof(ShadowEdge, corners) == 2 && offsetof(ShadowEdge, faces) == 4);

    // The shadow edge stream's data (at +0x10 of the stream): a count, then
    // the edges (buildPartEdges allocates faceCount * 24 + 4 bytes: up to three
    // edges per face).
    struct ShadowEdgeList
    {
        uint32_t count;       // +0x00
        ShadowEdge edges[1];  // +0x04 [count]
    };
    static_assert(offsetof(ShadowEdgeList, edges) == 4);

    // A part's bone list (ZgePart +0x1C; the engine's pointer list,
    // newCursorArray): the bones, walked with a cursor kept in the list.
    struct BoneList
    {
        ee::Ptr<ee::Ptr<Bone>> items; // +0x00
        int32_t cursor;               // +0x04 the bone being visited, -1 when done
        uint16_t count;               // +0x08
    };
    static_assert(offsetof(BoneList, cursor) == 4 && offsetof(BoneList, count) == 8);

    // The shadows' switch and the posing's weight scale.
    namespace shadow
    {
        // 1: the frame's shadow casters get their volumes (set with the
        // buffers; no code clears it). ZGE2 has no such flag.
        inline ee::Global<uint32_t, sym::g_enabled> g_enabled;
        constexpr bool kHasEnabledFlag = !game::readable::kIsDm2;
        // 1/255, a vertex's weight byte to a weight.
        inline ee::Global<ee::Float, sym::g_weightScale> g_weightScale;
    }

    // forEachSilhouetteEdge: the silhouette from the light the edges were
    // marked for (markEdgesFacingLight): per edge between a face that faces
    // the light and one that does not, its two corners (posed, for a skinned
    // part), ordered so the face toward the light is on the same side:
    // edge(first, second). (The list is walked as the original walks it: its
    // count is never 0.)
    template <typename EdgeFn>
    void forEachSilhouetteEdge(const ZgePart &part, EdgeFn &&edge)
    {
        const ee::Ptr<Vec4> positions = streamRecords<Vec4>(part.positions) + (part.skinWeights ? part.vertexCount : 0u);
        const ee::Ptr<Face> faces = streamRecords<Face>(part.faces);
        const ShadowEdgeList &list = *streamRecords<ShadowEdgeList>(part.shadowEdges);
        const ShadowEdge *each = list.edges;
        uint32_t left = list.count;
        do
        {
            if (each->facesLight[0] != each->facesLight[1])
            {
                const Face &face = faces[each->faces[0]];
                const Vec4 &corner0 = positions[face.vertex[each->corners[0]]];
                const Vec4 &corner1 = positions[face.vertex[each->corners[1]]];
                const bool secondFaceLit = each->facesLight[1] != 0;
                edge(secondFaceLit ? corner1 : corner0, secondFaceLit ? corner0 : corner1);
            }
            ++each;
        } while (--left != 0u);
    }

    // The split screen's disc shadow (buildDiscShadowPacket): eight edges of
    // a circle of radius 0.33 in the node's ground plane, nine points, the
    // first and last the same; discShadowOutline makes them the first time
    // and returns the first.
    constexpr uint32_t kDiscEdges = 8;
    ee::Ptr<Vec4> discShadowOutline();

    // markEdgesFacingLight: per edge of the part, whether each of its two faces faces
    // the point `light` (the part's space): the face's normal against the
    // direction from the edge's first corner to the light (VU0 dot products;
    // > 0 faces it). An edge between two faces with the same normal is never
    // part of the silhouette: both marks are cleared.
    void markEdgesFacingLight(ee::Ptr<ZgePart> part, const Vec4 *light);

    // buildShadowVolumePacket (ps2/shadow_pass.cpp): appends the shadow volume of the part to the shadow
    // packet: the GS state for the volume (context 2), the light, the colours,
    // the strip GIF tag and the matrices; then the silhouette, two points per
    // edge, 64 edges per unpack; then VU1's parameters (the near clip + 0.1,
    // the point count, 1.25) and the microprogram call; then the GS state put
    // back. `world` is the node's matrix. Returns where the chain starts (a
    // physical address).
    ee::Ptr<uint32_t> buildShadowVolumePacket(ee::Ptr<ZgePart> part, ee::Ptr<Matrix> world, ee::Ptr<Vec4> light);

    // posedFaceNormals: the face normals of a skinned part's posed copy: per face
    // the normalised cross product of its edges (v1 - v0) x (v2 - v1), on VU0
    // (VSQRT and VDIV), into the posed normals.
    void posedFaceNormals(ee::Ptr<ZgePart> part);

    // posePart: poses a skinned part: per bone its pose's matrix with the
    // bone's offset taken out of the translation; per vertex its first bone's
    // matrix times the bind position, times the vertex's weight byte / 255,
    // into the posed positions. With flags bit 1 the posed face normals too
    // (posedFaceNormals). Sets the part's positions rebuild bit (flags bit 0).
    // `poses` is the node's bone poses (Node +0x1BC; bone pose indices start
    // at 1).
    void posePart(ee::Ptr<ZgePart> part, ee::Ptr<BonePose> poses, uint32_t flags);

    // beginShadowPass, applyShadowBuffer: the shadow pass's two chains (ps2/shadow_pass.cpp:
    // the buffer cleared and the casters' volumes drawn into it; the buffer
    // laid over the frame).
    ee::Ptr<uint32_t> beginShadowPass();
    ee::Ptr<uint32_t> applyShadowBuffer();
    // beginShadowPassIfEnabled, applyShadowBufferIfEnabled: the same when shadows are on, else 0.
    ee::Ptr<uint32_t> beginShadowPassIfEnabled();
    ee::Ptr<uint32_t> applyShadowBufferIfEnabled();

    // (ps2/shadow_pass.cpp) allocateShadowBuffers, freeShadowBuffers: the shadow packet's buffers (always 1).
    uint32_t allocateShadowBuffers();
    uint32_t freeShadowBuffers();
    // flipShadowPacket, closeShadowPacket: the shadow packet moved to its other
    // buffer at a frame's start; its DMA tag closed at the end.
    void flipShadowPacket();
    void closeShadowPacket();

    // buildDiscShadowPacket (ps2/shadow_pass.cpp): the volume of a fixed disc under the light (split screen).
    ee::Ptr<uint32_t> buildDiscShadowPacket(ee::Ptr<ZgePart> part, ee::Ptr<Matrix> world, ee::Ptr<Vec4> light);
    // buildShadowVolume: a caster's shadow packet: a disc in split screen, else
    // its volume; the light a little above it (and to the side in one view),
    // in the node's space. 0 with shadows off or no edges.
    ee::Ptr<uint32_t> buildShadowVolume(ee::Ptr<Node> node, ee::Ptr<ZgePart> part);
    // buildPartEdges: the part's shadow edges (StreamKind::ShadowEdges).
    void buildPartEdges(ee::Ptr<ZgePart> part);
    // drawShadowEdges (no caller): the silhouette as debug lines.
    void drawShadowEdges(ee::Ptr<Node> node, ee::Ptr<ZgePart> part, ee::Ptr<Vec4> light);
}
