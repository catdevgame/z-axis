// DM2's query results and ray hits: what a sphere or segment query touched
// (nodes, their faces, the points of contact) and a ray's first hit, read by
// the rider's physics. collision.h has the records.

#include "collision.h"

#include "ee/abi.h"
#include "ee/readable.h"

#include <cstring>
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "sym/dm2/rider/collision/hits.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        // rayCastAhead's filter: which faces its query keeps.
        constexpr uint32_t kAheadFilter = sym::notWaterSurface;
        // The water's level object type ("WaterSurface"), and its hash
        // (notWaterSurface makes it on every call).
        constexpr ee::Ptr<const char> kWaterSurfaceName(sym::g_textWaterSurface);
        inline ee::Global<uint32_t, sym::g_waterSurfaceHash> g_waterSurfaceHash;

        // The two ray records and the count of rays cast.
        inline ee::Global<RayHit, sym::dm2RayHit> g_rayHit;
        inline ee::Global<RayHit, sym::dm2RayHitAhead> g_rayHitAhead;
        inline ee::Global<int32_t, sym::g_raySerial> g_raySerial;

        // A vector copied whole (the game's vectors are quadword aligned:
        // the original copies them with one quadword load and store).
        void copyVector(ee::Float *to, const ee::Float *from)
        {
            std::memcpy(to, from, 4 * sizeof(ee::Float));
        }
    }

    // ---- query results

    // How many nodes a query touched.
    int32_t hitCount(const QueryHits *hits)
    {
        return hits->nodeCount;
    }
    READABLE_FUNCTION(hitCount, Verified);

    uint64_t nodeHandle(uint64_t node);

    // The i-th node a query touched.
    uint32_t hitNode(const QueryHits *hits, int32_t i)
    {
        return static_cast<uint32_t>(nodeHandle(widen(hits->nodes[i].node)));
    }
    READABLE_FUNCTION(hitNode, Verified);

    // How many of the i-th node's faces a query touched.
    int32_t hitFaceCount(const QueryHits *hits, int32_t i)
    {
        return hits->nodes[i].faceCount;
    }
    READABLE_FUNCTION(hitFaceCount, Verified);

    // The index of the j-th face of the i-th node.
    int32_t hitFaceIndex(const QueryHits *hits, int32_t i, int32_t j)
    {
        return hits->nodes[i].faces[j].face;
    }
    READABLE_FUNCTION(hitFaceIndex, Verified);

    // Where the j-th face of the i-th node was touched.
    void hitFacePoint(const QueryHits *hits, int32_t i, int32_t j, ee::Float *point)
    {
        copyVector(point, hits->nodes[i].faces[j].point);
    }
    READABLE_FUNCTION(hitFacePoint, Verified);

    // The same on a query's record.
    int32_t resultCount(const Query *query)
    {
        return hitCount(query->hits.get());
    }
    READABLE_FUNCTION(resultCount, Verified);

    uint32_t resultNode(const Query *query, int32_t i)
    {
        return hitNode(query->hits.get(), i);
    }
    READABLE_FUNCTION(resultNode, Verified);

    int32_t resultFaceCount(const Query *query, int32_t i)
    {
        return hitFaceCount(query->hits.get(), i);
    }
    READABLE_FUNCTION(resultFaceCount, Verified);

    int32_t resultFace(const Query *query, int32_t i, int32_t j)
    {
        return hitFaceIndex(query->hits.get(), i, j);
    }
    READABLE_FUNCTION(resultFace, Verified);

    // The first node, face and point of contact.
    uint32_t firstNode(const Query *query)
    {
        return hitNode(query->hits.get(), 0);
    }
    READABLE_FUNCTION(firstNode, Verified);

    int32_t firstFace(const Query *query)
    {
        return hitFaceIndex(query->hits.get(), 0, 0);
    }
    READABLE_FUNCTION(firstFace, Verified);

    void firstPoint(const Query *query, ee::Float *point)
    {
        game::tail(hitFacePoint, query->hits.get(), 0, 0, point);
    }
    READABLE_FUNCTION(firstPoint, Verified);

    // ---- ray hits

    uint64_t nodeOf(uint64_t node);

    namespace
    {
        // A segment query from `from` to `to` (with a face filter, or none)
        // into `hit`: its first face, point and owning object, and the ray.
        RayHit *castInto(RayHit *hit, const ee::Float *from, const ee::Float *to, uint32_t filter)
        {
            *g_raySerial = *g_raySerial + 1;
            if (!segmentQueryRaw(from, to, filter, 0u, &hit->query))
                return nullptr;
            hit->serial = *g_raySerial;
            const uint32_t node = firstNode(&hit->query);
            const int32_t face = firstFace(&hit->query);
            hit->face = static_cast<uint64_t>(static_cast<uint32_t>(face)) << 32 | node;
            firstPoint(&hit->query, hit->point);
            hit->object = engine::nodeObject(nodeOf(widen(node)), 0u);
            hit->flags = hit->object ? kHitObject : kHitFace;
            copyVector(hit->from, from);
            copyVector(hit->to, to);
            return hit;
        }
    }

    // The first face a ray (segment) from `from` to `to` hits, or 0: its
    // record, valid until the next ray.
    RayHit *rayCast(const ee::Float *from, const ee::Float *to)
    {
        return castInto(g_rayHit.get(), from, to, 0u);
    }
    READABLE_FUNCTION(rayCast, Verified);

    // The same with rayCastAhead's face filter, into its own record, so it
    // does not overwrite rayCast's.
    RayHit *rayCastAhead(const ee::Float *from, const ee::Float *to)
    {
        return castInto(g_rayHitAhead.get(), from, to, kAheadFilter);
    }
    READABLE_FUNCTION(rayCastAhead, Verified);

    // rayCastAhead's query filter: everything but the water's surface (a
    // node of a WaterSurface level object), so the ray sees what is under it.
    int32_t notWaterSurface(uint64_t node)
    {
        *g_waterSurfaceHash = zge::nameHash(kWaterSurfaceName);
        if (node == 0)
            return 1;
        const uint64_t sceneNode = nodeOf(node);
        if (sceneNode == 0)
            return 1;
        const uint32_t object = engine::nodeObject(sceneNode, 0u);
        if (object == 0)
            return 1;
        if (*g_waterSurfaceHash == zge::levelObjectTypeHash(ee::Ptr<zge::SceneObject>(object)))
            return 0;
        return 1;
    }
    READABLE_FUNCTION(notWaterSurface, Verified);

    uint32_t hitFlags(const RayHit *hit)
    {
        return hit->flags;
    }
    READABLE_FUNCTION(hitFlags, Verified);

    FaceRef hitFace(const RayHit *hit)
    {
        return hit->face;
    }
    READABLE_FUNCTION(hitFace, Verified);

    void hitPoint(const RayHit *hit, ee::Float *point)
    {
        copyVector(point, hit->point);
    }
    READABLE_FUNCTION(hitPoint, Verified);

    uint32_t hitObject(const RayHit *hit)
    {
        return hit->object;
    }
    READABLE_FUNCTION(hitObject, Verified);

    // The ray a hit came from: its two ends (32 bytes) into `ends`.
    void hitRay(const RayHit *hit, ee::Float *ends)
    {
        zge::memoryCopy(ends, hit->from, 0x20u);
    }
    READABLE_FUNCTION(sym::hitCopy, hitRay, Verified);

    // A FaceRef kept in memory (a 64-bit load).
    FaceRef faceRefAt(const FaceRef *ref)
    {
        return *ref;
    }
    READABLE_FUNCTION(sym::stub, faceRefAt, Verified);
}
