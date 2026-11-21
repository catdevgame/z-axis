// The level's collision set-up (collision_setup.cpp, setupCollisionPart)
// made on the host: the same load-time words, written into a host collision
// part's tag tables (zge/native/collision_data.h), so the tag mirrors and
// the face tests on host data read what the game's own pass writes into
// RAM. AI's install registers it with zge/native (setLevelSetupStep); the
// other games' setups are their own and are not modelled.
//   - every face tag of kind 1 or 2: +0x04 = the face-edge element of a
//     matching kind (1 or 9 for kind 1, 2 for kind 2) whose edge comes
//     nearest the face's centroid (the edge's point nearest it,
//     closestOnSegment, then the distance; edges shorter than 1e-5 on
//     every axis skipped; the first nearest kept; a record several
//     faces share keeps the last face's);
//   - every face tag of kind 0xB: +0x08 = the face;
//   - every edge record: +0x04 = the last element that picks it.
// The arithmetic is the game's (VU0 for the centroid, the closest point
// and the difference; the FPU for the length and the compares).

#include "collision_setup_host.h"

#include "zge/native/collision_data.h"

#include "ee/vu0.h"
#include "zge/collision/closest.h"

#include <cstring>

namespace aggressive
{
    using zge::native::HostCollisionPart;
    using zge::native::HostTagTable;
    using zge::native::LevelSetupWords;
    using zge::Vec3;
    using zge::Vec4;
    using zge::Face;
    using zge::FaceTagRecord;
    using zge::EdgeRecord;
    using zge::closestOnSegment;

    namespace
    {
        template <typename T>
        void put(uint8_t *record, uint32_t offset, T value)
        {
            std::memcpy(record + offset, &value, sizeof(value));
        }

        constexpr uint32_t kFaceTagKind = offsetof(FaceTagRecord, kind);
        constexpr uint32_t kFaceTagNearestEdge = offsetof(FaceTagRecord, nearestEdge);
        constexpr uint32_t kFaceTagFace = offsetof(FaceTagRecord, face);
        constexpr uint32_t kEdgeKind = offsetof(EdgeRecord, kind);
        constexpr uint32_t kEdgeElement = offsetof(EdgeRecord, element);

        // .sdata constants setupCollisionPart and centroid read.
        const ee::Float kThird = ee::Float::fromBits(0x3EAAAAABu);          // gp-0x7CE8 (g_third): 1/3
        const ee::Float kShortestEdge = ee::Float::fromBits(0x3727C5ACu);   // gp-0x7CE4 (g_shortestEdge): 1e-5
        const ee::Float kFarthest = ee::Float::fromBits(0x7F7FFFFFu);       // g_farthest: the largest float

        // cornersCentroid, the game's centroid (3, corners, out): the corners summed onto VU0's vf0
        // (0, 0, 0, 1), then xyz times 1/3; w stays the sum's.
        Vec4 cornersCentroid(const Vec4 corner[3])
        {
            using ee::vu0::add;
            Vec4 sum = {0.0f, 0.0f, 0.0f, 1.0f};
            for (int i = 0; i < 3; ++i)
                sum = {add(sum.x, corner[i].x), add(sum.y, corner[i].y), add(sum.z, corner[i].z),
                       add(sum.w, corner[i].w)};
            return {ee::vu0::mul(sum.x, kThird), ee::vu0::mul(sum.y, kThird), ee::vu0::mul(sum.z, kThird), sum.w};
        }

        uint32_t vertexCountOf(const Face &face) { return (face.flags >> 8) & 0xFu; }
    }

    void applyLevelSetupOnHost(zge::native::HostCollisionPart &part)
    {
        HostTagTable &tags = part.faceTags, &edges = part.faceEdges;
        LevelSetupWords &words = part.setup;
        words = {};
        const uint32_t edgeElements = part.faceCount * 3u;
        if (tags.present)
        {
            for (uint32_t face = 0; face < part.faceCount; ++face)
            {
                uint8_t *tag = tags.record(face);
                if (!tag)
                    continue;
                const uint8_t kind = tag[kFaceTagKind];
                if (kind == 1 || kind == 2)
                {
                    ++words.searchedFaces;
                    const Face &f = part.faces[face];
                    if (!part.vertexInFile(f.vertex[0]) || !part.vertexInFile(f.vertex[1]) ||
                        !part.vertexInFile(f.vertex[2]))
                    {
                        ++words.undetermined;
                        continue;
                    }
                    const Vec4 corner[3] = {part.positions[f.vertex[0]], part.positions[f.vertex[1]],
                                            part.positions[f.vertex[2]]};
                    const Vec4 middle = cornersCentroid(corner);
                    ee::Float nearest = kFarthest;
                    bool found = false;
                    for (uint32_t element = 0; edges.present && element < edgeElements; ++element)
                    {
                        const uint8_t *edge = edges.record(element);
                        if (!edge)
                            continue;
                        const uint8_t edgeKind = edge[kEdgeKind];
                        if (!(kind == 1 ? edgeKind == 1 || edgeKind == 9 : edgeKind == 2))
                            continue;
                        // The edge's two vertices (elementEdgeVertices: edge k of the
                        // face runs from vertex k to the next, by its count).
                        const Face &owner = part.faces[element / 3u];
                        const uint32_t n = vertexCountOf(owner);
                        if (n == 0)
                            continue; // the game's division traps here
                        const uint32_t k = element % 3u;
                        if (!part.vertexInFile(owner.vertex[k % n]) || !part.vertexInFile(owner.vertex[(k + 1u) % n]))
                        {
                            ++words.undetermined;
                            continue;
                        }
                        const Vec4 &a = part.positions[owner.vertex[k % n]];
                        const Vec4 &b = part.positions[owner.vertex[(k + 1u) % n]];
                        if (!(kShortestEdge <= ee::abs(a.x - b.x) || kShortestEdge <= ee::abs(a.y - b.y) ||
                              kShortestEdge <= ee::abs(a.z - b.z)))
                            continue;
                        const Vec4 onEdge = closestOnSegment(a, b, middle);
                        const Vec3 apart = {ee::vu0::sub(onEdge.x, middle.x), ee::vu0::sub(onEdge.y, middle.y),
                                            ee::vu0::sub(onEdge.z, middle.z)};
                        // vec3Length (zge/math/vector.cpp): the FPU's sum, SQRT.S.
                        const ee::Float distance = ee::sqrt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
                        if (distance < nearest)
                        {
                            put<uint32_t>(tag, kFaceTagNearestEdge, element);
                            nearest = distance;
                            found = true;
                        }
                    }
                    words.nearestEdges += found;
                }
                if (kind == 0x0B)
                {
                    put<uint32_t>(tag, kFaceTagFace, face);
                    ++words.taggedFaces;
                }
            }
        }
        if (edges.present)
            for (uint32_t element = 0; element < edgeElements; ++element)
                if (uint8_t *edge = edges.record(element))
                {
                    put<uint32_t>(edge, kEdgeElement, element);
                    ++words.edgeElements;
                }
    }
}
