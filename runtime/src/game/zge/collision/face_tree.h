#pragma once

// The face tree as the face tests read it (zge/collision/faces.cpp; BMX XXX's segment test,
// game/bmx/collision/face_tree.cpp): the tree's nodes, the walk's stack and the faces seen so far in the
// scratchpad, the sorted list of leaves the shape reaches, and a part's faces, positions and normals from the
// game's RAM or from the host's copy (PS2X_NATIVE_COLLISION).

#include "collision.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/heap.h"
#include "zge/math/types.h"
#include "zge/native/collision_data.h"
#include "zge/native/host.h"
#include "zge/render/part.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include "sym/zge/collision/face_tree.h.h"

namespace zge
{
    int32_t pointInTriangle(const Vec4 *a, const Vec4 *b, const Vec4 *c, const Vec4 *normal, const Vec4 *point);
}

namespace zge::face_tree
{
    // ---- the sorted leaves ------------------------------------------------

    // The tree leaves a query reaches, sorted (QueryState::leafList).
    constexpr uint32_t kLeafList = kQueryResults + offsetof(QueryState, leafList);
    constexpr ee::Ptr<PriorityQueue> kLeafQueue{kLeafList};
    // An entry of a sorted list (kTakeFirst hands one back).
    struct SortedEntry
    {
        int32_t key;
        ee::Ptr<uint8_t> record;
    };

    // ---- the scratchpad --------------------------------------------------

    // One bit per face of the part: tested already in this call.
    constexpr uint32_t kVisitedFaces = 0x70000400u;
    // The tree walk's stack (grows up): a subtree still to visit, and for
    // segments the piece of the segment that runs through it.
    constexpr uint32_t kWalkStack = 0x70000800u;
    constexpr uint32_t kWalkStackEntries = 448;
    struct WalkEntry
    {
        Vec3 start;           // +0x00 segments: the piece
        ee::Float startAlong; // +0x0C segments: how far along the segment the piece starts where a
                              //       split plane cut it (else 0); its leaves are sorted by it
        Vec3 end;             // +0x10
        uint32_t node;        // +0x1C the subtree: its byte offset in the tree

        // start and startAlong as VU0 loads them.
        const Vec4 &startLanes() const { return *reinterpret_cast<const Vec4 *>(&start); }
    };
    static_assert(sizeof(WalkEntry) == 0x20);
    inline ee::Global<WalkEntry[kWalkStackEntries], kWalkStack> g_walkStack;

    // ---- tree nodes ------------------------------------------------------

    // A split's children by side: +0x04 (collision.h's `below`: below the
    // plane for y, which grows downward) holds the side where the
    // coordinate is greater than the plane, +0x06 (`above`) the lesser.
    inline uint16_t greaterSide(const FaceTreeSplit &split) { return split.greater; }
    inline uint16_t lesserSide(const FaceTreeSplit &split) { return split.lesser; }

    // The visited bitmap: tests and sets the face's bit.
    inline bool visitOnce(uint32_t face)
    {
        uint64_t &word = ee::at<uint64_t>(kVisitedFaces)[face >> 6];
        const uint64_t bit = uint64_t(1) << (face & 63u);
        if (word & bit)
            return false;
        word |= bit;
        return true;
    }

    inline void clearVisited(uint32_t faceCount)
    {
        // (faces + 63) / 64 words; the original's loop assumes one at least.
        const uint32_t words = (faceCount + 63u) >> 6;
        uint64_t *bitmap = ee::at<uint64_t>(kVisitedFaces);
        for (uint32_t i = 0;;)
        {
            bitmap[i] = 0;
            if (++i >= words)
                break;
        }
    }

    // ---- a part's data ---------------------------------------------------

    // Where a face test reads the part: its streams in RAM (face normals
    // TGFN: one float3 per face; TIFN: a table of them and a u8 or u16
    // index per face) and the face tree at its RAM address, or the host
    // part (PS2X_NATIVE_COLLISION, on by default; outside ps2xFnCheck, when the host
    // has the part and its tree).
    class MeshSource
    {
    public:
        MeshSource(ee::Ptr<ZgePart> part, ee::Ptr<uint8_t> tree) : m_part(part), m_tree(tree)
        {
            if (native::hostExtrasOn() && native::nativeCollisionEnabled())
            {
                m_hostPart = native::hostCollisionPart(part);
                if (m_hostPart && (!m_hostPart->hasTree || !fileTree(tree)))
                    m_hostPart.reset();
                native::countNativeCollisionTest(m_hostPart != nullptr);
                if (m_hostPart && native::nativeCollisionPoison())
                    native::poisonRamCopy(part, tree);
            }
            m_host = m_hostPart.get();
        }

        bool host() const { return m_host != nullptr; }
        uint32_t faceCount() const { return m_host ? m_host->faceCount : m_part->faceCount; }
        // Whether `tree` is the file's: a collision frame with no tree gets
        // one made in the game (testNodeFaces: one leaf with every face),
        // also when the part has a frame with a file tree elsewhere (the Park
        // Editor's pieces). (Under PS2X_NATIVE_COLLISION_POISON the RAM copy
        // is overwritten, so nothing can be told from it.)
        bool fileTree(ee::Ptr<uint8_t> tree) const
        {
            constexpr size_t kRoot = sizeof(FaceTreeLeaf);
            return native::nativeCollisionPoison() || m_hostPart->tree.size() < kRoot ||
                   std::memcmp(m_hostPart->tree.data(), &*tree, kRoot) == 0;
        }
        const Face &face(uint32_t index) const
        {
            if (!m_host)
                return ram().faces[index];
            return verified(m_host->faces[index], &ram().faces[index]);
        }
        // A vertex's position. A face of the host part may name a vertex
        // past the file's positions (collision_data.h): that one is
        // what the game reads, the RAM after its positions stream.
        const Vec4 &position(uint16_t vertex) const
        {
            const Vec4 *inRam = &*positionAddress(vertex);
            if (m_host && m_host->vertexInFile(vertex))
                return verified(m_host->positions[vertex], inRam);
            return *inRam;
        }
        // Where the vertex is in the game's RAM.
        ee::Ptr<Vec4> positionAddress(uint16_t vertex) const
        {
            return streamRecords<Vec4>(m_part->positions) + vertex;
        }
        const Vec3 &normal(uint32_t face) const
        {
            if (m_host)
                return native::nativeCollisionVerify() ? verified(m_host->faceNormals[face], &ramNormal(face))
                                                       : m_host->faceNormals[face];
            return ramNormal(face);
        }
        // How the part's face normals are indexed (the stream's index
        // table in RAM, which the original's face tests branch on).
        enum class NormalIndex
        {
            None, // one normal per face
            Byte,
            Wide,
        };
        NormalIndex normalIndex() const
        {
            const RamStreams &streams = ram();
            return streams.byteIndex ? NormalIndex::Byte : streams.wideIndex ? NormalIndex::Wide : NormalIndex::None;
        }
        const Vec3 &ramNormal(uint32_t face) const
        {
            const RamStreams &streams = ram();
            uint32_t index = face;
            if (streams.byteIndex)
                index = streams.byteIndex[face];
            else if (streams.wideIndex)
                index = streams.wideIndex[face];
            return *reinterpret_cast<const Vec3 *>(streams.normalTable + index * sizeof(Vec3));
        }
        // A tree node by its place in the game's tree.
        const FaceTreeLeaf &node(ee::Ptr<uint8_t> place) const
        {
            const FaceTreeLeaf *inRam = &*place.cast<FaceTreeLeaf>();
            if (m_host)
            {
                const ptrdiff_t offset = &*place - &*m_tree; // the node's place in the tree, the host's the same
                return verified(*reinterpret_cast<const FaceTreeLeaf *>(m_host->tree.data() + offset), inRam);
            }
            return *inRam;
        }
        const FaceTreeSplit &split(ee::Ptr<uint8_t> place) const
        {
            return reinterpret_cast<const FaceTreeSplit &>(node(place));
        }
        // A leaf's face indices (u16, at its halfword offset in the tree).
        const uint16_t *leafFaces(const FaceTreeLeaf &leaf) const
        {
            const uint16_t *inRam = &*(m_tree.cast<uint16_t>() + leaf.faces);
            if (!m_host)
                return inRam;
            const uint16_t *host = reinterpret_cast<const uint16_t *>(m_host->tree.data() + leaf.faces * 2u);
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(host, inRam, leaf.count * 2u);
            return host;
        }

    private:
        // PS2X_NATIVE_COLLISION=verify: a host value checked against RAM's.
        template <typename T>
        static const T &verified(const T &host, const T *inRam)
        {
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&host, inRam, sizeof(T));
            return host;
        }

        // The streams in RAM, read when a face is first needed (where the
        // original reads them).
        struct RamStreams
        {
            bool read = false;
            const Face *faces = nullptr;
            ee::Ptr<Vec4> positions;
            const uint8_t *normalTable = nullptr;
            const uint8_t *byteIndex = nullptr;
            const uint16_t *wideIndex = nullptr;
        };
        const RamStreams &ram() const
        {
            if (!m_ram.read)
            {
                const ZgePart &mesh = *m_part;
                m_ram.read = true;
                m_ram.faces = &*streamRecords<Face>(mesh.faces);
                m_ram.positions = streamRecords<Vec4>(mesh.positions);
                ZgeStream *normals = &*mesh.faceNormals.cast<ZgeStream>();
                m_ram.normalTable = normals->data();
                if (normals->indices)
                {
                    if (normals->wideIndices & 1u)
                        m_ram.wideIndex = &*normals->indices.cast<uint16_t>();
                    else
                        m_ram.byteIndex = normals->indices.get();
                }
            }
            return m_ram;
        }

        ee::Ptr<ZgePart> m_part;
        ee::Ptr<uint8_t> m_tree;
        std::shared_ptr<const native::HostCollisionPart> m_hostPart;
        const native::HostCollisionPart *m_host = nullptr;
        mutable RamStreams m_ram;
    };

    // pointInTriangle on a face's corners: as the original calls it (the
    // vertices' RAM addresses), or on the host part's corners when the
    // readable pointInTriangle runs here unhooked (the call then reaches
    // its body directly, with the corners where they are).
    inline int32_t faceContains(const MeshSource &mesh, const Face &face, const Vec4 &normal, const Vec4 &point)
    {
        static const game::readable::Function *const target = game::readable::find(sym::pointInTriangle);
        if (mesh.host() && target && target->enabled && !target->hooked && native::hostExtrasOn())
            return pointInTriangle(&mesh.position(face.vertex[0]), &mesh.position(face.vertex[1]), &mesh.position(face.vertex[2]),
                                   &normal, &point);
        return pointInTriangle(mesh.positionAddress(face.vertex[0]), mesh.positionAddress(face.vertex[1]), mesh.positionAddress(face.vertex[2]), &normal, &point);
    }

    inline ee::Float &component(Vec4 &v, int axis) { return (&v.x)[axis]; }
    inline ee::Float component(const Vec4 &v, int axis) { return (&v.x)[axis]; }
    inline ee::Float component(const Vec3 &v, int axis) { return (&v.x)[axis]; }

    // A split's axis as a coordinate index: 1 x, 2 y, 3 (or more) z.
    inline int splitCoordinate(uint8_t axis) { return axis == kFaceTreeSplitX ? 0 : axis == kFaceTreeSplitY ? 1 : 2; }

    // ---- the game functions the face tests call (ee/plain.h) ----------------

    inline void poolReset(uint32_t pool) { ee::game(sym::poolReset, pool); }
    inline void priorityQueueClear(uint32_t queue) { ee::game(sym::priorityQueueClear, queue); }
    // A query's filter: whether to keep the face.
    inline bool filterKeeps(const SphereQuery &query, ee::Ptr<Node> node, ee::Ptr<ZgePart> part, ee::Address face)
    {
        return ee::game<uint64_t>(query.filter, query.filterContext, node, part, face) != 0;
    }
}
