// BMX XXX's query results and ray hits: what a sphere or segment query
// touched (nodes, their faces, the points of contact) and a ray's first hit,
// read by the rider's physics. DM2's (game/dm2/rider/collision/hits.cpp)
// over ZGE4's results: the count, then a pointer a node hit. collision.h
// has the records.

#include "collision.h"

#include "ee/abi.h"
#include "ee/readable.h"

#include <cstring>
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/collision/hits.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        // The ray record and the count of rays cast.
        inline ee::Global<RayHit, sym::g_rayHit> g_rayHit;
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
    int32_t hitCount(const zge::QueryResults *hits)
    {
        return hits->count;
    }
    READABLE_FUNCTION(hitCount, Verified);

    // The i-th node a query touched.
    uint32_t hitNode(const zge::QueryResults *hits, int32_t i)
    {
        return static_cast<uint32_t>(nodeHandle(widen(ee::Address(hits->nodes[i]->node))));
    }
    READABLE_FUNCTION(hitNode, Verified);

    // How many of the i-th node's faces a query touched.
    int32_t hitFaceCount(const zge::QueryResults *hits, int32_t i)
    {
        return static_cast<int32_t>(hits->nodes[i]->faceCount);
    }
    READABLE_FUNCTION(hitFaceCount, Verified);

    // The index of the j-th face of the i-th node.
    int32_t hitFaceIndex(const zge::QueryResults *hits, int32_t i, int32_t j)
    {
        return static_cast<int32_t>(hits->nodes[i]->faces[j].face);
    }
    READABLE_FUNCTION(hitFaceIndex, Verified);

    // Where the j-th face of the i-th node was touched.
    void hitFacePoint(const zge::QueryResults *hits, int32_t i, int32_t j, ee::Float *point)
    {
        *vec4(point) = hits->nodes[i]->faces[j].point;
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

    // The first face a ray (segment) from `from` to `to` hits, or 0: its
    // record, valid until the next ray. Its face, point and owning object,
    // and the ray. BMX XXX keeps the face only when the node has collision
    // faces (DM2 keeps it always), and has no second record (DM2's
    // rayCastAhead).
    RayHit *rayCast(const ee::Float *from, const ee::Float *to)
    {
        RayHit *hit = g_rayHit.get();
        *g_raySerial = *g_raySerial + 1;
        if (!segmentQueryRaw(from, to, 0u, 0u, &hit->query))
            return nullptr;
        hit->serial = *g_raySerial;
        const uint32_t node = firstNode(&hit->query);
        const int32_t face = firstFace(&hit->query);
        hit->face = nodeFaceCount(widen(node)) ? makeFace(node, face) : 0;
        firstPoint(&hit->query, hit->point);
        const ee::Ptr<zge::Node> owner(static_cast<uint32_t>(nodeOf(widen(node))));
        hit->object = ee::Address(zge::nodeSceneObject(owner, ee::Ptr<ee::Ptr<zge::SceneBone>>()));
        hit->flags = hit->object ? kHitObject : kHitFace;
        copyVector(hit->from, from);
        copyVector(hit->to, to);
        return hit;
    }
    READABLE_FUNCTION(rayCast, Verified);

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
        game::tail(zge::memoryCopy, ends, hit->from, 0x20u);
    }
    READABLE_FUNCTION(hitRay, Verified);

    // A FaceRef kept in memory (a 64-bit load).
    FaceRef faceRefAt(const FaceRef *ref)
    {
        return *ref;
    }
    READABLE_FUNCTION(faceRefAt, Verified);
}
