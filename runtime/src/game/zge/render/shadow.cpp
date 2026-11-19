// Shadow volumes (zge/render/shadow.h): a part's shadow edges, which faces
// face the light, a skinned part posed, the silhouette and the disc's
// outline, the shadow driver and its switches. The pass's chains and the
// volume's packet are the PS2 form, ps2/shadow_pass.cpp.

#include "shadow.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/render/renderer.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/render/part.h"
#include "zge/render/primitives.h"

#include <array>
#include <cstring>
#include "sym/zge/render/shadow.cpp.h"

namespace zge
{
    namespace
    {
        using ee::Float;

        // How many bones a pose's frame has room for.
        constexpr uint32_t kPosedBoneSlots = 32;

        // The upper half of a vector read as one quadword: its z and w.
        uint64_t upperHalf(const Vec4 &v) { return v.z.bits() | static_cast<uint64_t>(v.w.bits()) << 32; }

        // The pass's colour (the second rectangle's, applyShadowBuffer): a node's colour at half strength, set per
        // caster.
        inline ee::Global<std::array<int32_t, 4>, sym::g_shadowColourRed> g_shadowColour;
    }

    // ---- which faces face the light -----------------------------------------

    void markEdgesFacingLight(ee::Ptr<ZgePart> partAddress, const Vec4 *light)
    {
        const ZgePart &part = *partAddress;
        // A skinned part's posed copies follow the bind pose's.
        const bool posed = part.skinWeights;
        const ee::Ptr<Vec3> normals = streamRecords<Vec3>(part.faceNormals) + (posed ? part.faceCount : 0u);
        const ee::Ptr<Vec4> positions = streamRecords<Vec4>(part.positions) + (posed ? part.vertexCount : 0u);
        const ee::Ptr<Face> faces = streamRecords<Face>(part.faces);
        ShadowEdgeList &list = *streamRecords<ShadowEdgeList>(part.shadowEdges);

        ShadowEdge *edge = list.edges;
        uint32_t left = list.count;
        do
        {
            const Vec3 &normal0 = normals[edge->faces[0]];
            const Vec3 &normal1 = normals[edge->faces[1]];
            // The same normal (x and y bit for bit, z as floats): a flat edge, never
            // part of the silhouette.
            if (normal0.x.bits() == normal1.x.bits() && normal0.y.bits() == normal1.y.bits() && normal0.z == normal1.z)
            {
                edge->facesLight[0] = 0;
                edge->facesLight[1] = 0;
            }
            else
            {
                // From the edge's first corner to the light, against each face's normal.
                const Face &face = faces[edge->faces[0]];
                const Vec4 toLight = *light - positions[face.vertex[edge->corners[0]]];
                edge->facesLight[0] = Float(0.0f) < dot(direction(normal0), toLight) ? 1 : 0;
                edge->facesLight[1] = Float(0.0f) < dot(direction(normal1), toLight) ? 1 : 0;
            }
            ++edge;
        } while (--left != 0u);
    }
    READABLE_FUNCTION_VU0(markEdgesFacingLight, Verified);

    // ---- posing a skinned part ----------------------------------------------

    void posedFaceNormals(ee::Ptr<ZgePart> partAddress)
    {
        const ZgePart &part = *partAddress;
        const uint32_t faceCount = part.faceCount;
        if (faceCount == 0u)
            return;
        const ee::Ptr<Vec4> posed = streamRecords<Vec4>(part.positions) + part.vertexCount;
        const ee::Ptr<Face> faces = streamRecords<Face>(part.faces);
        const ee::Ptr<Vec3> normals = streamRecords<Vec3>(part.faceNormals) + faceCount;
        for (uint32_t i = 0; i < faceCount; ++i)
        {
            const Face &face = faces[i];
            const Vec4 edge0 = posed[face.vertex[1]] - posed[face.vertex[0]];
            const Vec4 edge1 = posed[face.vertex[2]] - posed[face.vertex[1]];
            normals[i] = unit(cross(edge0, edge1)).xyz();
        }
    }
    READABLE_FUNCTION_VU0(posedFaceNormals, Verified);

    void posePart(ee::Ptr<ZgePart> partAddress, ee::Ptr<BonePose> poses, uint32_t flags)
    {
        ZgePart &part = *partAddress;
        const uint32_t vertexCount = part.vertexCount;
        const ee::Ptr<Vec4> bind = streamRecords<Vec4>(part.positions);
        const ee::Ptr<Vec4> posed = bind + vertexCount;
        BoneList &bones = *ee::Ptr<BoneList>(part.bones);

        // Per bone, its pose with the bone's offset taken out of the translation. In
        // the frame, as the original, room for 32 (a vertex's bone byte counts from
        // 1; no part has more): past the bones listed, the slots hold what the stack
        // held.
        ee::Local<Matrix[kPosedBoneSlots]> matrices;
        ee::Local<Vec4> turnedOffset;
        // The weight goes to VU0 through the integer register that last held the upper
        // half of a bone's first row; those bits stay in VU0's work register beside it.
        uint64_t weightUpperHalf = 0u;
        uint32_t boneCount = 0;
        ee::Ptr<Bone> bone;
        if (bones.count != 0u)
        {
            bones.cursor = 0;
            bone = bones.items[0];
        }
        else
            bones.cursor = -1;
        while (bone)
        {
            Matrix &matrix = matrices[boneCount & (kPosedBoneSlots - 1u)];
            matrix = poses[bone->poseIndex - 1u].matrix;
            weightUpperHalf = upperHalf(matrix.row(0));
            *turnedOffset = rotated(matrix, bone->offset);
            matrix.translation() -= *turnedOffset;
            ++boneCount;

            // The list's next bone.
            bone = ee::Ptr<Bone>();
            if (bones.cursor >= 0)
            {
                ++bones.cursor;
                if (bones.cursor < static_cast<int32_t>(bones.count))
                    bone = bones.items[bones.cursor];
            }
            if (!bone)
                bones.cursor = -1;
        }

        // Per vertex: through its first bone's matrix, then scaled by the weight byte
        // / 255 (byte 1 of the vertex's skin weights, which the skinned packet
        // builders do not read: SkinWeights::shadowWeight).
        const ee::Ptr<SkinWeights> weights = streamRecords<SkinWeights>(part.skinWeights);
        const Float weightScale = *shadow::g_weightScale;
        uint32_t i = 0;
        do
        {
            const SkinWeights &vertex = weights[i];
            const Float weight = ee::toFloat(vertex.shadowWeight) * weightScale;
            const Matrix &matrix = matrices[(vertex.bone0 - 1u) & (kPosedBoneSlots - 1u)];
            posed[i] = transformed(matrix, bind[i]);
            posed[i] = scaledCarrying(posed[i], weight, weightUpperHalf);
        } while (++i != vertexCount);

        if (flags & 2u)
            posedFaceNormals(partAddress);
        part.flags |= 1u; // the positions stream to rebuild
    }

    namespace
    {
        // ZGE2's skin weights (the stream ZGE3 reads as SkinWeights): up to four
        // bones a vertex, each with its weight in 255ths (addSkinnedPositionWords
        // keeps them summing to 255); a bone of 0 ends the list.
        struct BoneWeights
        {
            struct
            {
                uint8_t bone, weight;
            } bones[4];
        };
        static_assert(sizeof(BoneWeights) == sizeof(SkinWeights));
        // How many bones the frame of ZGE2's posePart has room for.
        constexpr uint32_t kZge2PosedBoneSlots = 24;
    }

    // posePartByBoneWeights: ZGE2's posePart: the bones walked with arrayFirst
    // and arrayNext, each pose's rotation and translation copied
    // (copyRotationAndTranslation) with the bone's offset, turned by the pose,
    // taken off the translation; per vertex each of its bones' matrices times
    // the bind position (transformPoint), times the bone's weight / 255,
    // summed. With flags bit 1 the posed face normals too; the positions
    // stream's rebuild bit set.
    void posePartByBoneWeights(ee::Ptr<ZgePart> partAddress, ee::Ptr<BonePose> poses, uint32_t flags)
    {
        ZgePart &part = *partAddress;
        const uint32_t vertexCount = part.vertexCount;
        const ee::Ptr<Vec4> bind = streamRecords<Vec4>(part.positions);
        const ee::Ptr<Vec4> posed = bind + vertexCount;
        const ee::Ptr<PointerArray> bones(part.bones);

        // Per bone, in the frame, its pose with the offset taken out (a vertex's
        // bone byte counts from 1).
        ee::Local<Matrix[kZge2PosedBoneSlots]> matrices;
        ee::Local<Vec4> turnedOffset;
        ee::Local<Vec4> weighted; // a later bone's share of a vertex
        const ee::Ptr<Matrix> boneMatrices = matrices;
        ee::Ptr<Matrix> matrix = boneMatrices;
        for (ee::Ptr<Bone> bone(arrayFirst(bones)); bone; bone = ee::Ptr<Bone>(arrayNext(bones)))
        {
            copyMatrixRotationAndTranslation(matrix, &poses[bone->poseIndex - 1u].matrix);
            *turnedOffset = rotated(*matrix, bone->offset);
            matrix->translation() -= *turnedOffset;
            matrix = matrix + 1;
        }

        // The weight reaches VU0 through an integer register whose upper half
        // nothing here has written.
        const uint64_t leftover = scaleLeftover(LeftBy::caller);
        const ee::Ptr<BoneWeights> weights = streamRecords<BoneWeights>(part.skinWeights);
        const Float weightScale = *shadow::g_weightScale;
        uint32_t i = 0;
        do
        {
            const BoneWeights &vertex = weights[i];
            const ee::Ptr<Vec4> point = posed + i;
            transformPoint(point, boneMatrices + (vertex.bones[0].bone - 1), bind + i);
            *point = scaledCarrying(*point, ee::toFloat(vertex.bones[0].weight) * weightScale, leftover);
            for (uint32_t k = 1; k < 4 && vertex.bones[k].bone != 0u; ++k)
            {
                const Float weight = ee::toFloat(vertex.bones[k].weight) * weightScale;
                transformPoint(weighted, boneMatrices + (vertex.bones[k].bone - 1), bind + i);
                *weighted = scaledCarrying(*weighted, weight, leftover);
                *point = *point + *weighted;
            }
        } while (++i != vertexCount);

        if (flags & 2u)
            posedFaceNormals(partAddress);
        part.flags |= 1u; // the positions stream to rebuild
    }
    READABLE_FUNCTION_VU0_OWN(posePart, Verified, dm2, posePartByBoneWeights);

    // beginShadowPassIfEnabled: beginShadowPass's chain when shadows are on, else 0 (ZGE2: always the chain).
    ee::Ptr<uint32_t> beginShadowPassIfEnabled()
    {
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no such switch)
            if (*shadow::g_enabled == 0)
                return {};
        const ee::Ptr<uint32_t> chain = beginShadowPass();
        return chain;
    }
    READABLE_FUNCTION(beginShadowPassIfEnabled, Verified);

    // applyShadowBufferIfEnabled: applyShadowBuffer's chain when shadows are on, else 0 (ZGE2: always the chain).
    ee::Ptr<uint32_t> applyShadowBufferIfEnabled()
    {
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no such switch)
            if (*shadow::g_enabled == 0)
                return {};
        const ee::Ptr<uint32_t> chain = applyShadowBuffer();
        return chain;
    }
    READABLE_FUNCTION(applyShadowBufferIfEnabled, Verified);

    // ---- the disc's outline ----------------------------------------------------------

    namespace
    {
        // The disc: nine points of a circle of radius 0.33 in the node's ground plane
        // (the first and last the same), made at the first call.
        inline ee::Global<std::array<Vec4, kDiscEdges + 1>, sym::discPoints> g_discPoints;
        constexpr int32_t kDiscStep = 0x20000000;     // an eighth of a turn, in the angle's high 16 bits
        inline ee::Global<uint8_t, sym::g_discToMake> g_discToMake; // 1 until the first call made the points
    }

    // discShadowOutline (buildDiscShadowPacket): the disc's points, made the
    // first time: (-sin a, 0, cos a) * 0.33 for a = 0, 1/8 .. 1 turn.
    ee::Ptr<Vec4> discShadowOutline()
    {
        if (*g_discToMake != 0)
        {
            const Float radius = 0.33f;
            int32_t angleWord = 0;
            for (uint32_t i = 0; i < kDiscEdges + 1; ++i)
            {
                const int32_t angle = angleWord >> 16;
                angleWord += kDiscStep;
                const Float sine = angleSin(angle) * radius;
                const Float cosine = angleCos(static_cast<Angle>(angle));
                Vec4 &point = (*g_discPoints)[i];
                point.y = Float::fromBits(0);
                point.z = cosine * radius;
                point.x = -sine;
            }
            *g_discToMake = 0;
        }
        return &(*g_discPoints)[0];
    }

    // ---- a part's edges -----------------------------------------------------------

    namespace
    {
        // Two corners are the same point when every axis is within 1e-6.
        constexpr float kSameCorner = 1e-06f;
        bool samePoint(const Vec4 &a, const Vec4 &b)
        {
            const auto near = [](Float x, Float y) { return ee::abs(x - y) < Float(kSameCorner); };
            return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
        }

        // A face's corner count (Face::flags bits 8-11).
        uint32_t cornerCount(const Face &face) { return (face.flags & kFaceVertexCount) >> 8; }
    }

    // buildPartEdges (queuePartDraw, a caster's first queueing): the part's
    // shadow edge stream (StreamKind::ShadowEdges, up to three edges a
    // face): per face and corner, the edge to the next corner joins the
    // first edge in the list that has one face only and the same two
    // corners (in either direction; corners the same within 1e-6), else it
    // is added with this face on both sides. Not for a part of another type
    // than 0.
    void buildPartEdges(ee::Ptr<ZgePart> part)
    {
        if (part->header != 0)
            return;
        ShadowEdgeList &list = *ee::Ptr<ShadowEdgeList>(newStream(part, StreamKind::ShadowEdges, (part->faceCount * 0x18u) | 4u, 0));
        list.count = 0;
        const ee::Ptr<Vec4> positions = streamRecords<Vec4>(part->positions);
        const ee::Ptr<Face> faces = streamRecords<Face>(part->faces);
        const auto corner = [&](uint32_t face, uint32_t index) -> const Vec4 & { return positions[faces[face].vertex[index]]; };

        for (uint32_t face = 0; face < part->faceCount; ++face)
        {
            const uint32_t corners = cornerCount(faces[face]);
            for (uint32_t index = 0; index < corners; ++index)
            {
                const uint32_t next = (index + 1u) % corners;
                const Vec4 &from = corner(face, index);
                const Vec4 &to = corner(face, next);

                // The listed edge this one is the other side of, either way round.
                bool joined = false;
                ShadowEdge *edge = list.edges;
                for (uint32_t i = 0; i < list.count && !joined; ++i, ++edge)
                {
                    if (edge->faces[0] != edge->faces[1])
                        continue; // two faces already
                    const Vec4 &listedFrom = corner(edge->faces[0], edge->corners[0]);
                    const Vec4 &listedTo = corner(edge->faces[0], edge->corners[1]);
                    joined = (samePoint(from, listedFrom) && samePoint(to, listedTo)) ||
                             (samePoint(to, listedFrom) && samePoint(from, listedTo));
                    if (joined)
                        edge->faces[1] = static_cast<uint16_t>(face);
                }
                if (!joined)
                {
                    // A new edge, this face on both sides.
                    ShadowEdge &added = list.edges[list.count];
                    added.corners[0] = static_cast<uint8_t>(index);
                    added.corners[1] = static_cast<uint8_t>(next);
                    added.faces[0] = static_cast<uint16_t>(face);
                    added.faces[1] = static_cast<uint16_t>(face);
                    ++list.count;
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS(buildPartEdges, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // A debug line as the primitive API takes it: the two ends' addresses, then
        // the ends (a position, and the colour 8 bytes past it).
        struct LineEnd
        {
            Vec4 position;
            uint8_t texel[8];
            uint8_t rgba[4];
            uint8_t padding[4];
        };
        static_assert(sizeof(LineEnd) == 0x20);
        struct alignas(16) DebugLine
        {
            uint32_t kind;
            ee::Ptr<LineEnd> ends[2];
            uint32_t padding;
            LineEnd end[2];
        };
        static_assert(offsetof(DebugLine, end) == 0x10 && sizeof(DebugLine) == 0x50);
    }

    // drawShadowEdges (no caller in the game: a debug view): the part's silhouette
    // from the light `light` (world space) drawn as white debug lines in the node's
    // world matrix, each edge's corners ordered as buildShadowVolumePacket orders them.
    void drawShadowEdges(ee::Ptr<Node> node, ee::Ptr<ZgePart> part, ee::Ptr<Vec4> light)
    {
        ee::Local<Vec4> lightInPart;
        ee::Local<Vec4> first;
        ee::Local<Vec4> second;
        ee::Local<DebugLine> line;
        ee::Local<Vec4> swap;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        const ee::Ptr<Vec4> positions = streamRecords<Vec4>(part->positions) + (part->skinWeights ? part->vertexCount : 0u);
        if (!part->shadowEdges)
            return;
        const ee::Ptr<ShadowEdgeList> list = streamRecords<ShadowEdgeList>(part->shadowEdges);
        lightInPart = transformed(*nodeMatrix(node, 2), *light);
        const ee::Ptr<Matrix> world = nodeMatrix(node, 1);
        markEdgesFacingLight(part, &*lightInPart);
        if (list->count == 0)
            return;
        const ShadowEdge *edge = list->edges;
        for (uint32_t i = 0; i < list->count; ++i, ++edge)
        {
            if (edge->facesLight[0] == edge->facesLight[1])
                continue;
            faceVertices(part, edge->faces[0], &*vertices);
            first = positions[(*vertices)[edge->corners[0]]];
            second = positions[(*vertices)[edge->corners[1]]];
            if (edge->facesLight[1] != 0)
            {
                swap = *first;
                first = *second;
                second = *swap;
            }
            for (int end = 0; end < 2; ++end)
            {
                line->end[end].position = end == 0 ? *first : *second;
                std::memset(line->end[end].rgba, 0xFF, sizeof(line->end[end].rgba));
                line->ends[end] = &line->end[end];
            }
            line->kind = 0;
            drawLines(0, ee::Address(line), 1, ee::Address(world));
        }
    }
    READABLE_FUNCTION_WAITS(drawShadowEdges, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Ptr<BonePose> bonePosesOf(const Node &node) { return node.bonePoses.cast<BonePose>(); }
    }

    // buildShadowVolume (the per-caster shadow, draw.cpp). The node's colour is the
    // pass's at half strength. The light: 5 above the node's place, 1 to the
    // side in one view (none in split screen, three or more scene records).
    ee::Ptr<uint32_t> buildShadowVolume(ee::Ptr<Node> node, ee::Ptr<ZgePart> part)
    {
        ee::Local<Vec4> above;
        ee::Local<Vec4> light;
        if (*shadow::g_enabled == 0 || !part->shadowEdges)
            return {};
        const bool disc = sceneRecordCount() >= 3;
        for (int channel = 0; channel < 4; ++channel)
            (*g_shadowColour)[channel] = node->colour[channel] >> 1;
        above = nodeMatrix(node, 1)->translation();
        above->y = above->y - Float(5.0f);
        if (!disc)
            above->x = above->x - Float(1.0f);
        light = transformed(*nodeMatrix(node, 2), above);
        const ee::Ptr<Matrix> world = nodeMatrix(node, 1);
        if (disc)
            return buildDiscShadowPacket(part, world, light);
        if (part->boneCount != 0 && (node->flags & kNodeBonesPosed) != 0)
        {
            const ee::Ptr<BonePose> poses = bonePosesOf(*node);
            node->flags &= ~kNodeBonesPosed;
            posePart(part, poses, 3);
        }
        markEdgesFacingLight(part, light);
        return buildShadowVolumePacket(part, world, light);
    }

    // shadowVolumeFromLight (ZGE2 only): the node's part's shadow volume
    // from a light at `light` (world space): none without the part's shadow
    // edges; the pass's colour the node's at half strength, the light taken
    // into the node's space, the edges facing it marked and the volume's
    // packet built. (ZGE3 writes it into buildShadowVolume.)
    inline ee::Ptr<uint32_t> shadowVolumeFromLight(ee::Ptr<Node> node, ee::Ptr<ZgePart> part, ee::Ptr<Vec4> light)
    {
        ee::Local<Vec4> lightInNode;
        if (!part->shadowEdges)
            return {};
        for (int channel = 0; channel < 4; ++channel)
            (*g_shadowColour)[channel] = node->colour[channel] >> 1;
        transformPoint(lightInNode, nodeMatrix(node, 2), light);
        const ee::Ptr<Matrix> world = nodeMatrix(node, 1);
        markEdgesFacingLight(part, lightInNode);
        return buildShadowVolumePacket(part, world, lightInNode);
    }
    READABLE_FUNCTION_ONLY(dm2, shadowVolumeFromLight);

    // buildShadowVolumeFromAbove: ZGE2's buildShadowVolume: the light 5
    // above the node's place and 1 to the side, always (ZGE2 has no disc
    // shadows, no posing here and no shadows-on switch).
    ee::Ptr<uint32_t> buildShadowVolumeFromAbove(ee::Ptr<Node> node, ee::Ptr<ZgePart> part)
    {
        ee::Local<Vec4> light;
        light = nodeMatrix(node, 1)->translation();
        light->y = light->y - Float(5.0f);
        light->x = light->x - Float(1.0f);
        return shadowVolumeFromLight(node, part, light);
    }
    READABLE_FUNCTION_OWN(buildShadowVolume, Verified, dm2, buildShadowVolumeFromAbove);
}
