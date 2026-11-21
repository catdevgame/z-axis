// DM2's scene queries: a sphere's or a segment's hits from the engine's
// query (copied out of the engine's buffers), the faces a sphere touches
// one by one, the edges near a point (DM2's rails: grindable edges), a
// hit's plane, how fast the rider closes on it, and an edge's side by its
// face's normal. collision.h has the records.

#include "collision.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstring>
#include "zge/collision/closest.h"
#include "zge/math/plane.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/scene/scene.h"
#include "sym/dm2/rider/collision/queries.cpp.h"

namespace game::dm2::rider::collision
{
    namespace
    {
        // The engine query's kinds and the shape's size in quadwords.
        constexpr int32_t kQuerySphere = 0;
        constexpr int32_t kQuerySegment = 1;
        constexpr int32_t kQueryShapeWords = 4;
        // DM2's adaptor for a query's face filter: calls filter(node, face,
        // filterArg).
        constexpr uint32_t kFilterAdaptor = sym::filterAdaptor;
        // forEachEdgeNear's sphere filter.
        constexpr uint32_t kEdgeNearFilter = sym::edgeNearFilter;
        // railEdgesNear's callback: keeps grindable edges.
        constexpr uint32_t kKeepGrindable = sym::keepGrindable;

        // The engine's query (DM2's own; AI has none): the nodes and faces a
        // shape of `words` quadwords touches, with a face filter or none,
        // into `hits` (the sixth word is 0 at every call). A sphere's radius
        // goes as a float too.
        int32_t sceneQuery(int32_t kind, ee::Address shape, int32_t words, uint32_t filter, ee::Address filterState,
                           uint32_t alwaysZero, QueryHits *hits)
        {
            return ee::game<int32_t>(sym::sceneQuery, kind, shape, words, filter, filterState, alwaysZero, hits);
        }
        int32_t sceneQuery(int32_t kind, ee::Address shape, int32_t words, uint32_t filter, ee::Address filterState,
                           uint32_t alwaysZero, QueryHits *hits, ee::Float radius)
        {
            return ee::game<int32_t>(sym::sceneQuery, kind, shape, words, filter, filterState, alwaysZero, hits,
                                     radius);
        }
        // The cosine of an angle (0x10000 a turn).
        ee::Float cosTurnOf(uint32_t angle) { return ee::game<ee::Float>(sym::cosTurnOf, angle); }

        // The queries' hit lists as the engine leaves them, and DM2's
        // copies of their node and face arrays.
        inline ee::Global<QueryHits, sym::g_sphereHits> g_sphereHits;
        inline ee::Global<QueryHits, sym::g_segmentHits> g_segmentHits;
        inline ee::Global<QueryNode, sym::g_segmentNode> g_segmentNode;
        inline ee::Global<QueryNode, sym::g_sphereNodes> g_sphereNodes;
        inline ee::Global<QueryFace, sym::g_sphereFaces> g_sphereFaces;
        inline ee::Global<QueryFace, sym::g_segmentFace> g_segmentFace;
        inline ee::Global<int32_t, sym::g_querySerial> g_querySerial;
        // sphereQuery's record and count.
        inline ee::Global<SphereHits, sym::dm2Sphere> g_sphereQuery;
        inline ee::Global<int32_t, sym::g_sphereSerial> g_sphereSerial;
        // The edges railEdgesNear keeps (collectTaggedEdges' list).
        inline ee::Global<uint64_t, sym::g_taggedEdges> g_nearEdges;
        inline ee::Global<int32_t, sym::g_taggedEdgeCount> g_nearEdgeCount;
        constexpr int32_t kNearEdgeCapacity = 0xFF;
        // hitClosing's constants: below this an object is still (1e-5);
        // a frame's time (1 / 60).
        inline ee::Global<ee::Float, sym::g_stillSpeed> g_stillSpeed;
        inline ee::Global<ee::Float, sym::g_frameTime> g_frameTime;
        // edgeSideByNormal's angle: 0x2AAA of a 0x10000 turn (60 degrees).
        constexpr uint32_t kSteepAngle = 0x2AAAu;

        // A query's face filter, as the engine calls it through the adaptor.
        struct FilterState
        {
            ee::FunctionPtr<int32_t(uint64_t, int32_t, uint32_t)> filter; // (node, face or -1, filterArg)
            uint32_t filterArg;
        };
        // The shapes the engine queries.
        struct QuerySphere
        {
            zge::Vec4 centre;
            ee::Float radius;
        };
        struct QuerySegment
        {
            zge::Vec4 from;
            zge::Vec4 to;
        };

        // forEachEdgeNear's walk: which nodes its query keeps (edgeNearFilter),
        // the callback and its argument, and the nodes found.
        struct Walk
        {
            uint32_t nodeTest; // an EdgeNodeTest
            ee::FunctionPtr<int32_t(uint32_t, EdgeRef, uint32_t)> callback;
            uint32_t arg;
            int32_t nodeCount;
        };
        // edgeNearFilter's node tests: nodes with edge tags, or with face tags.
        enum EdgeNodeTest : uint32_t
        {
            kNodesWithEdgeTags = 0,
            kNodesWithFaceTags = 1,
        };
        // railEdgesNear's list (keepGrindable's): its capacity, the edges
        // kept and their count.
        struct Keep
        {
            int32_t capacity;
            int32_t count;
            ee::Ptr<uint64_t> items;
        };
        // A query's whole-node test (the filter's face, -1).
        constexpr int32_t kWholeNode = -1;

        // FABS.S: the sign bit cleared.
        ee::Float absolute(ee::Float v) { return ee::Float::fromBits(v.bits() & 0x7FFFFFFFu); }
    }

    // ---- the engine's queries

    // The engine's query filter as DM2's queries give it (sceneQuery's
    // callback): the query's filter asked of a node's handle and a face (-1
    // when the engine asks of the node as a whole, with no face), with the
    // filter's argument. The engine's third word is not read.
    int32_t filterAdaptor(ee::Address query, uint64_t node, uint32_t, const int32_t *face)
    {
        const ee::Ptr<FilterState> state(query);
        const uint64_t handle = nodeHandle(node);
        const int32_t faceIndex = face ? *face : kWholeNode;
        return state->filter(handle, faceIndex, state->filterArg);
    }
    READABLE_FUNCTION(filterAdaptor, Verified);

    // The nodes and faces a sphere touches (with a face filter, or none),
    // into `query`: the engine's hit list with its node and face arrays
    // copied to DM2's buffers. The engine's result (0 for none).
    int32_t sphereQueryRaw(Query *query, const ee::Float *centre, uint32_t filter, ee::Address filterArg,
                           ee::Float radius)
    {
        ee::Local<QuerySphere> sphere;
        ee::Local<FilterState> state;
        ee::Local<ee::Ptr<Query>> kept; // the original keeps the query on its stack
        kept = query;
        query->hits = g_sphereHits.get();
        sphere->centre = *vec4(centre);
        sphere->radius = radius;
        int32_t found;
        if (filter)
        {
            state->filter = filter;
            state->filterArg = filterArg;
            found = sceneQuery(kQuerySphere, sphere, kQueryShapeWords, kFilterAdaptor, state, 0, query->hits, radius);
        }
        else
            found = sceneQuery(kQuerySphere, sphere, kQueryShapeWords, 0, 0, 0, query->hits, radius);
        if (found)
        {
            QueryHits *hits = query->hits;
            QueryNode *nodes = g_sphereNodes.get();
            // The engine's arrays may be in the scratchpad: their EE addresses as they are.
            zge::memoryCopy(nodes, hits->nodes, static_cast<uint32_t>(hits->nodeCount) * sizeof(QueryNode));
            hits->nodes = nodes;
            uint32_t faceTotal = 0;
            for (uint32_t i = 0; i < static_cast<uint32_t>(hits->nodeCount); ++i)
            {
                const int32_t faceCount = hits->nodes[i].faceCount;
                QueryFace *faces = g_sphereFaces.get() + faceTotal;
                zge::memoryCopy(faces, hits->nodes[i].faces,
                                static_cast<uint32_t>(faceCount) * static_cast<uint32_t>(sizeof(QueryFace)));
                hits->nodes[i].faces = faces;
                faceTotal += static_cast<uint32_t>(faceCount);
            }
        }
        *g_querySerial = *g_querySerial + 1;
        query->serial = static_cast<uint32_t>(*g_querySerial);
        return found;
    }
    READABLE_FUNCTION(sphereQueryRaw, Verified);

    // The first face a segment from `from` to `to` hits (with a face filter,
    // or none), into `query`: the engine's first node and its first face
    // copied to DM2's. The engine's result (0 for none).
    int32_t segmentQueryRaw(const ee::Float *from, const ee::Float *to, uint32_t filter, uint32_t filterArg,
                            Query *query)
    {
        ee::Local<QuerySegment> segment;
        ee::Local<FilterState> state;
        query->hits = g_segmentHits.get();
        segment->from = *vec4(from);
        segment->to = *vec4(to);
        int32_t found;
        if (filter)
        {
            state->filterArg = filterArg;
            state->filter = filter;
            found = sceneQuery(kQuerySegment, segment, kQueryShapeWords, kFilterAdaptor, state, 0, query->hits);
        }
        else
            found = sceneQuery(kQuerySegment, segment, kQueryShapeWords, 0, 0, 0, query->hits);
        if (found)
        {
            QueryHits *hits = query->hits;
            // The engine's arrays may be in the scratchpad: their EE addresses as they are.
            zge::memoryCopy(g_segmentNode.get(), hits->nodes, sizeof(QueryNode));
            hits->nodes = g_segmentNode.get();
            zge::memoryCopy(g_segmentFace.get(), g_segmentNode->faces, sizeof(QueryFace));
            hits->nodes->faces = g_segmentFace.get();
        }
        *g_querySerial = *g_querySerial + 1;
        query->serial = static_cast<uint32_t>(*g_querySerial);
        return found;
    }
    READABLE_FUNCTION(segmentQueryRaw, Verified);

    // ---- a sphere's faces, one by one

    int32_t resultCount(const Query *query);
    uint32_t resultNode(const Query *query, int32_t i);
    int32_t resultFaceCount(const Query *query, int32_t i);
    int32_t resultFace(const Query *query, int32_t i, int32_t j);

    // The faces a sphere touches: its record, the cursor at the first node's
    // first face (nextSphereFace walks them), or 0 if none.
    SphereHits *sphereQuery(const ee::Float *centre, ee::Float radius)
    {
        SphereHits *s = g_sphereQuery.get();
        *g_sphereSerial = *g_sphereSerial + 1;
        if (!sphereQueryRaw(&s->query, centre, 0u, 0u, radius))
            return nullptr;
        s->current.serial = *g_sphereSerial;
        s->serial = *g_sphereSerial;
        s->nodeCount = resultCount(&s->query);
        s->nodeIndex = 0;
        s->faceCount = resultFaceCount(&s->query, 0);
        s->faceIndex = 0;
        *vec4(s->centre) = *vec4(centre);
        s->radius = radius;
        return s;
    }
    READABLE_FUNCTION(sphereQuery, Verified);

    // The next face a sphere query touched (the record's cursor), or 0 after
    // the last.
    SphereFace *nextSphereFace(SphereHits *s)
    {
        if (s->nodeIndex == s->nodeCount)
            return nullptr;
        const uint32_t node = resultNode(&s->query, s->nodeIndex);
        const int32_t face = resultFace(&s->query, s->nodeIndex, s->faceIndex);
        s->current.face = makeFace(node, face);
        s->current.owner = s;
        s->faceIndex = s->faceIndex + 1;
        if (s->faceIndex == s->faceCount)
        {
            s->nodeIndex = s->nodeIndex + 1;
            if (static_cast<uint32_t>(s->nodeIndex) < static_cast<uint32_t>(s->nodeCount))
            {
                s->faceIndex = 0;
                s->faceCount = resultFaceCount(&s->query, s->nodeIndex);
            }
        }
        return &s->current;
    }
    READABLE_FUNCTION(sym::sphereQueryFaces, nextSphereFace, Verified);

    void faceVertex(FaceRef ref, uint32_t corner, ee::Float *point);

    // The point of a sphere query's face closest to the sphere's centre.
    void closestOnFace(const SphereFace *face, ee::Float *point)
    {
        ee::Local<zge::Vec4[3]> corners;
        const FaceRef ref = face->face;
        for (uint32_t i = 0; i < 3; ++i)
            faceVertex(ref, i, &corners[i].x);
        zge::closestPointOnTriangle(vec4(face->owner->centre), &corners[0], &corners[1], &corners[2], vec4(point), nullptr);
    }
    READABLE_FUNCTION(closestOnFace, Verified);

    // ---- edges near a point

    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side);

    // Calls callback(tagRecord, edge, arg) for every tagged side of the
    // faces a sphere touches, until one returns other than 1 (returned);
    // else 0.
    int32_t forEachEdgeNear(const ee::Float *centre, uint32_t callback, ee::Address arg, ee::Float radius)
    {
        ee::Local<Query> query;
        ee::Local<Walk> walk;
        walk->arg = arg;
        walk->callback = callback;
        walk->nodeTest = kNodesWithEdgeTags;
        if (!sphereQueryRaw(query, centre, kEdgeNearFilter, walk, radius))
            return 0;
        walk->nodeCount = resultCount(query);
        for (int32_t i = 0; i < walk->nodeCount; ++i)
        {
            const uint32_t node = resultNode(query, i);
            const uint32_t faceCount = static_cast<uint32_t>(resultFaceCount(query, i));
            for (uint32_t j = 0; j < faceCount; ++j)
            {
                const int32_t face = resultFace(query, i, static_cast<int32_t>(j));
                for (uint8_t side = 0; side < 3; ++side)
                {
                    if (const uint32_t record = edgeTags(widen(node), face, side))
                    {
                        const int32_t result = walk->callback(record, makeEdge(node, face, side), walk->arg);
                        if (result != 1)
                            return result;
                    }
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION(forEachEdgeNear, Verified);

    // forEachEdgeNear's query filter: a node (face -1) is kept if it has the
    // tags the walk asks for, a face always.
    int32_t edgeNearFilter(uint64_t node, int64_t face, ee::Address walk)
    {
        if (face != kWholeNode)
            return 1;
        const uint32_t test = ee::Ptr<Walk>(walk)->nodeTest;
        if (test == kNodesWithEdgeTags)
            return nodeHasEdgeTags(node) ? 1 : 0;
        if (test == kNodesWithFaceTags)
            return nodeHasFaceTags(node) ? 1 : 0;
        return 0;
    }
    READABLE_FUNCTION(edgeNearFilter, Verified);

    // railEdgesNear's callback: a grindable edge kept; 0, which stops the
    // walk, once the list is full.
    int32_t keepGrindable(uint32_t record, EdgeRef edge, ee::Address list)
    {
        if (!tagGrindable(record))
            return 1;
        const ee::Ptr<Keep> keep(list);
        if (keep->count == keep->capacity)
            return 0;
        keep->items[static_cast<size_t>(keep->count)] = edge;
        ++keep->count;
        return 1;
    }
    READABLE_FUNCTION(keepGrindable, Verified);

    // DM2's rails: the grindable edges near a point, as EdgeRefs in the
    // tagged edges' list (at most 0xFF), ended by a 0; 0 if none.
    uint64_t *railEdgesNear(const ee::Float *centre, ee::Float radius)
    {
        ee::Local<Keep> keep;
        uint64_t *list = g_nearEdges.get();
        keep->capacity = kNearEdgeCapacity;
        keep->items = list;
        keep->count = 0;
        forEachEdgeNear(centre, kKeepGrindable, keep, radius);
        const int32_t n = keep->count;
        *g_nearEdgeCount = n;
        if (n == 0)
            return nullptr;
        list[n] = 0;
        return list;
    }
    READABLE_FUNCTION(railEdgesNear, Verified);

    void faceNormalWorld(uint64_t node, int32_t face, ee::Float *normal);

    // Which side an edge is ground from by its face: 1 if the face is less
    // steep than 60 degrees (its normal's dot with down above cos 60), else
    // -1. The side itself is not read.
    int32_t edgeSideByNormal(uint64_t node, int32_t face, uint32_t side)
    {
        (void)side;
        ee::Local<zge::Vec4> down; // its w is left as it was
        ee::Local<zge::Vec4> normal;
        const ee::Float limit = cosTurnOf(kSteepAngle);
        down->y = -1.0f;
        down->x = 0.0f;
        down->z = 0.0f;
        faceNormalWorld(node, face, &normal->x);
        return limit < zge::dot(*down, *normal) ? 1 : -1;
    }
    READABLE_FUNCTION(sym::edgeDirection, edgeSideByNormal, Verified);

    // ---- a ray hit's plane

    FaceRef hitFace(const RayHit *hit);
    void hitPoint(const RayHit *hit, ee::Float *point);
    void faceNormal(FaceRef ref, ee::Float *normal);
    void hitRay(const RayHit *hit, ee::Float *ends);
    uint32_t hitObject(const RayHit *hit);

    // The plane of a hit's face through the hit, its normal turned toward
    // the ray's start.
    void hitPlane(const RayHit *hit, ee::Float *plane)
    {
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> toStart;
        ee::Local<QuerySegment> ray;
        const FaceRef face = hitFace(hit);
        hitPoint(hit, &point->x);
        collision::faceNormal(face, &normal->x);
        hitRay(hit, &ray->from.x);
        toStart = ray->from - *point;
        if (zge::dot(*normal, *toStart) <= 0.0f)
            zge::vec3Negate(&normal->xyz(), &normal->xyz());
        zge::planeThrough(vec4(plane), point, normal);
    }
    READABLE_FUNCTION(hitPlane, Verified);

    // How far the rider may close on a hit's plane this frame: a moving
    // object's speed toward the rider (per frame, plus a frame's margin;
    // into *objectApproach when asked) less the velocity's part along the
    // plane's normal.
    ee::Float hitClosing(const ee::Float *velocity, const RayHit *hit, ee::Float *objectApproach)
    {
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> normal; // its w is left as it was
        ee::Local<zge::Vec4> objectVelocity;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> objectDirection;
        hitPlane(hit, &plane->x);
        ee::Float approach = 0.0f;
        normal->x = plane->x;
        normal->y = plane->y;
        normal->z = plane->z;
        if (const uint32_t object = hitObject(hit))
        {
            zge::objectVelocity(ee::Ptr<zge::SceneObject>(object), objectVelocity);
            approach = zge::vec3Length(&objectVelocity->xyz());
            if (!(absolute(approach) < *g_stillSpeed))
            {
                direction = zge::unit(*vec4(velocity));
                objectDirection = zge::unit(*objectVelocity);
                approach = approach * -zge::dot(*direction, *objectDirection);
                if (approach < 0.0f)
                    approach = 0.0f;
                else
                    approach = approach * *g_frameTime + *g_frameTime;
            }
        }
        if (objectApproach)
            *objectApproach = approach;
        return approach - zge::dot(*vec4(velocity), *normal);
    }
    READABLE_FUNCTION(sym::hitDistance, hitClosing, Verified);

    // ---- by face and edge

    uint32_t materialTags(uint64_t node, int32_t face);
    uint32_t materialSound(const uint8_t *record);
    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side);
    uint32_t tagWallKind(uint32_t record);

    // A face's material sound (0 .. 15), 0 untagged.
    uint32_t faceSound(FaceRef ref)
    {
        const uint32_t record = collision::materialTags(widen(refNode(ref)), faceIndex(ref));
        if (!record)
            return 0;
        const uint32_t sound = materialSound(ee::pointerAt<const uint8_t>(record));
        return sound < 16u ? sound : 0u;
    }
    READABLE_FUNCTION(faceSound, Verified);

    // An edge's wall kind (0 .. 15), 0 untagged.
    uint32_t edgeWallKind(EdgeRef edge)
    {
        const uint32_t record = edgeTags(widen(refNode(edge)), edgeFace(edge), edgeSide(edge));
        if (!record)
            return 0;
        const uint32_t kind = tagWallKind(record);
        return kind < 16u ? kind : 0u;
    }
    READABLE_FUNCTION(sym::faceWallRidable, edgeWallKind, Verified);
}
