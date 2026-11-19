// BMX XXX's scene queries: a sphere's or a segment's hits from the engine's
// query (copied out of the engine's buffers), the faces a sphere touches one
// by one, the point of such a face nearest the sphere's centre, an edge's
// side by its face's normal, the tagged edges near a point, a ray hit's
// plane, a face's sound and an edge's wall kind. DM2's
// (game/dm2/rider/collision/queries.cpp) over ZGE4's collisionQuery: its
// flags (a sphere may take faces that do not collide too), its results (a
// pointer a node), and the sphere's faces capped at the buffer's size.
// collision.h has the records.

#include "collision.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include "zge/collision/closest.h"
#include "zge/collision/collision.h"
#include "zge/math/angle.h"
#include "zge/math/plane.h"
#include "zge/math/vector.h"
#include "zge/math/vec.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "sym/bmx/rider/collision/queries.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        // The engine query's kinds.
        constexpr uint32_t kQuerySphere = 0;
        constexpr uint32_t kQuerySegment = 1;
        // The game's adaptor for a query's face filter: calls filter(node,
        // face, filterArg).
        constexpr uint32_t kFilterAdaptor = sym::filterAdaptor;
        // edgeSideByNormal's angle: 0x2AAA of a 0x10000 turn (60 degrees).
        constexpr zge::Angle kSteepAngle = 0x2AAA;

        // The queries' results as the engine leaves them, and the game's
        // copies of their nodes and faces.
        inline ee::Global<zge::QueryResults, sym::g_sphereHits> g_sphereHits;
        inline ee::Global<zge::QueryResults, sym::g_segmentHits> g_segmentHits;
        inline ee::Global<zge::HitNode, sym::g_segmentNode> g_segmentNode;
        inline ee::Global<zge::FaceHit, sym::g_segmentFace> g_segmentFace;
        inline ee::Global<zge::HitNode, sym::g_sphereNodes> g_sphereNodes; // one a node of the results
        inline ee::Global<zge::FaceHit, sym::g_sphereFaces> g_sphereFaces;
        constexpr uint32_t kSphereFaceCapacity = 0x400;
        inline ee::Global<int32_t, sym::g_querySerial> g_querySerial;
        // sphereQuery's record and count.
        inline ee::Global<SphereHits, sym::g_sphereQuery> g_sphereQuery;
        inline ee::Global<int32_t, sym::g_sphereSerial> g_sphereSerial;
        // forEachEdgeNear's sphere filter, and railEdgesNear's callback
        // (keepGrindable) as the walk takes it.
        constexpr uint32_t kEdgeNearFilter = sym::edgeNearFilter;
        constexpr uint32_t kKeepGrindable = sym::keepGrindable;
        // sphereQueryRaw's word for faces that do not collide too.
        constexpr uint32_t kSphereAllFaces = 1;
        // The edges railEdgesNear keeps.
        inline ee::Global<uint64_t, sym::g_taggedEdges> g_nearEdges;
        inline ee::Global<int32_t, sym::g_taggedEdgeCount> g_nearEdgeCount;
        constexpr int32_t kNearEdgeCapacity = 0xFF;
        // A tag's 4-bit field as the value it stands for (each its own,
        // 0 .. 15): faceSound and edgeWallKind look theirs up here, where
        // DM2 keeps a value under 16 and makes any other 0.
        inline ee::Global<uint32_t[16], sym::g_tagFieldValues> g_tagFieldValues;

        // A query's face filter, as the engine calls it through the adaptor.
        struct FilterState
        {
            uint32_t filter;
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
        // What sphereQueryRaw keeps on its stack across the copies.
        struct SphereKept
        {
            ee::Ptr<Query> query;
            int32_t found;
        };
    }

    // ---- the engine's queries

    // The nodes and faces a sphere touches (with a face filter, or none; all
    // faces, or only those that collide), into `query`: the engine's results
    // with their nodes and faces copied to the game's buffers, at most 0x400
    // faces in all (a node past that keeps fewer, or none). The engine's
    // result (0 for none).
    int32_t sphereQueryRaw(Query *query, const ee::Float *centre, uint32_t allFaces, uint32_t filter,
                           ee::Address filterArg, ee::Float radius)
    {
        ee::Local<QuerySphere> sphere;
        ee::Local<FilterState> state;
        ee::Local<SphereKept> kept; // the original keeps the query and its result on its stack
        kept->query = query;
        query->hits = g_sphereHits.get();
        sphere->centre = *vec4(centre);
        const uint32_t flags = (allFaces & 1u) ? zge::kQueryWorldPoints | zge::kQueryAllFaces : zge::kQueryWorldPoints;
        sphere->radius = radius;
        uint32_t adaptor = 0;
        ee::Address filterState = 0;
        if (filter)
        {
            state->filter = filter;
            state->filterArg = filterArg;
            adaptor = kFilterAdaptor;
            filterState = state;
        }
        const int32_t found = zge::collisionQuery(kQuerySphere, sphere, flags, adaptor, filterState, 0u, query->hits,
                                                  ee::Ptr<zge::QueryCache>());
        kept->found = found;
        if (found)
        {
            zge::QueryResults *hits = query->hits;
            uint32_t faceTotal = 0;
            for (uint32_t i = 0; i < static_cast<uint32_t>(hits->count); ++i)
            {
                // The engine's records may be in the scratchpad: their EE addresses as they are.
                zge::HitNode *node = g_sphereNodes.get() + i;
                zge::memoryCopy(node, hits->nodes[i], sizeof(zge::HitNode));
                hits->nodes[i] = node;
                uint32_t faceCount = node->faceCount;
                if (faceTotal + faceCount > kSphereFaceCapacity)
                    faceCount = kSphereFaceCapacity - faceTotal;
                zge::FaceHit *faces = g_sphereFaces.get() + faceTotal;
                zge::memoryCopy(faces, node->faces, faceCount * static_cast<uint32_t>(sizeof(zge::FaceHit)));
                faceTotal += faceCount;
                hits->nodes[i]->faces = faces;
                hits->nodes[i]->faceCount = faceCount;
            }
        }
        *g_querySerial = *g_querySerial + 1;
        query->serial = static_cast<uint32_t>(*g_querySerial);
        return found;
    }
    READABLE_FUNCTION(sphereQueryRaw, Verified);

    // The first face a segment from `from` to `to` hits (with a face filter,
    // or none), into `query`: the engine's first node and its first face
    // copied to the game's. The engine's result (0 for none).
    int32_t segmentQueryRaw(const ee::Float *from, const ee::Float *to, uint32_t filter, uint32_t filterArg,
                            Query *query)
    {
        ee::Local<QuerySegment> segment;
        ee::Local<FilterState> state;
        query->hits = g_segmentHits.get();
        segment->from = *vec4(from);
        segment->to = *vec4(to);
        uint32_t adaptor = 0;
        ee::Address filterState = 0;
        if (filter)
        {
            state->filter = filter;
            state->filterArg = filterArg;
            adaptor = kFilterAdaptor;
            filterState = state;
        }
        const int32_t found = zge::collisionQuery(kQuerySegment, segment, zge::kQueryWorldPoints, adaptor, filterState, 0u,
                                                  query->hits, ee::Ptr<zge::QueryCache>());
        if (found)
        {
            zge::QueryResults *hits = query->hits;
            // The engine's records may be in the scratchpad: their EE addresses as they are.
            zge::memoryCopy(g_segmentNode.get(), hits->nodes[0], sizeof(zge::HitNode));
            hits->nodes[0] = g_segmentNode.get();
            zge::memoryCopy(g_segmentFace.get(), hits->nodes[0]->faces, sizeof(zge::FaceHit));
            hits->nodes[0]->faces = g_segmentFace.get();
        }
        *g_querySerial = *g_querySerial + 1;
        query->serial = static_cast<uint32_t>(*g_querySerial);
        return found;
    }
    READABLE_FUNCTION(segmentQueryRaw, Verified);

    // ---- a sphere's faces, one by one

    // The faces a sphere touches: its record, the cursor at the first node's
    // first face (nextSphereFace walks them), or 0 if none.
    SphereHits *sphereQuery(const ee::Float *centre, ee::Float radius)
    {
        SphereHits *s = g_sphereQuery.get();
        *g_sphereSerial = *g_sphereSerial + 1;
        if (!sphereQueryRaw(&s->query, centre, 0u, 0u, 0u, radius))
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
    // the last. Its face is 0 on a node with no collision faces.
    SphereFace *nextSphereFace(SphereHits *s)
    {
        if (s->nodeIndex == s->nodeCount)
            return nullptr;
        const uint32_t node = resultNode(&s->query, s->nodeIndex);
        const int32_t face = resultFace(&s->query, s->nodeIndex, s->faceIndex);
        s->current.face = nodeFaceCount(widen(node)) ? makeFace(node, face) : 0;
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
    READABLE_FUNCTION(nextSphereFace, Verified);

    // The point of a sphere query's face closest to the sphere's centre.
    void closestOnFace(const SphereFace *face, ee::Float *point)
    {
        ee::Local<zge::Vec4[3]> corners;
        const FaceRef ref = face->face;
        for (uint32_t i = 0; i < 3; ++i)
            faceVertex(ref, i, &corners[i].x);
        zge::closestPointOnTriangle(vec4(face->owner->centre), &corners[0], &corners[1], &corners[2], vec4(point),
                                    nullptr);
    }
    READABLE_FUNCTION(closestOnFace, Verified);

    // ---- edges near a point

    // Calls callback(tagRecord, edge, arg) for every tagged side of the
    // faces a sphere touches (faces that do not collide too), until one
    // returns other than 1 (returned); else 0. The edge is 0 for a node with
    // no collision faces.
    int32_t forEachEdgeNear(const ee::Float *centre, uint32_t callback, ee::Address arg, ee::Float radius)
    {
        struct Walk
        {
            uint32_t nodeTest; // which of its node tests edgeNearFilter makes (1 here; DM2 0)
            ee::FunctionPtr<int32_t(uint32_t, EdgeRef, uint32_t)> callback;
            uint32_t arg;
            int32_t nodeCount;
        };
        ee::Local<Query> query;
        ee::Local<Walk> walk;
        walk->nodeTest = 1;
        walk->callback = callback;
        walk->arg = arg;
        if (!sphereQueryRaw(query, centre, kSphereAllFaces, kEdgeNearFilter, walk, radius))
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
                        const EdgeRef edge = nodeFaceCount(widen(node)) ? makeEdge(node, static_cast<uint32_t>(face), side) : 0;
                        const int32_t result = walk->callback(record, edge, walk->arg);
                        if (result != 1)
                            return result;
                    }
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION(forEachEdgeNear, Verified);

    uint32_t tagGrindable(uint32_t record);

    // railEdgesNear's callback: a grindable edge goes on the list; 0 (stop)
    // when the list is full, else 1.
    int32_t keepGrindable(uint32_t record, EdgeRef edge, EdgeList *list)
    {
        if (!(tagGrindable(record) & 1u))
            return 1;
        if (list->count == list->capacity)
            return 0;
        list->items[list->count] = edge;
        list->count = list->count + 1;
        return 1;
    }
    READABLE_FUNCTION(keepGrindable, Verified);

    // The rails: the grindable edges near a point, as EdgeRefs in the tagged
    // edges' list (at most 0xFF), ended by a 0; 0 if none.
    uint64_t *railEdgesNear(const ee::Float *centre, ee::Float radius)
    {
        ee::Local<EdgeList> keep;
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

    namespace calls
    {
        // A tag record's kind (its byte 2 through the level's kinds table; 1 and 2 the corners the skitch holds on
        // to): the level's tag code (bmx.c), not written yet.
        // Scaffolding (address-call): docs/SCAFFOLDING.md
        inline uint32_t cornerTagKind(uint32_t record) { return ee::game<uint32_t>(sym::cornerTagKind, record); }
    }

    namespace
    {
        constexpr uint32_t kKeepTaggedCorner = sym::keepTaggedCorner;
        // The corners taggedCornersNear keeps (as EdgeRefs: the side is the corner), at most 0xFF.
        inline ee::Global<uint64_t, sym::g_nearCorners> g_nearCorners;
        inline ee::Global<int32_t, sym::g_nearCornerCount> g_nearCornerCount;
        constexpr int32_t kNearCornerCapacity = 0xFF;
        constexpr uint32_t kCornerKindsKept = 3; // kinds 1 and 2
    }

    // forEachCornerNear (centre, callback, arg, radius): forEachEdgeNear's walk over the corners of the faces a
    // sphere touches: callback(record, corner, arg) for each corner with a tag record (the corner an EdgeRef whose
    // side is the corner; 0 for a node with no collision faces), until one returns other than 1 (returned); else 0.
    int32_t forEachCornerNear(const ee::Float *centre, uint32_t callback, ee::Address arg, ee::Float radius)
    {
        struct Walk
        {
            uint32_t nodeTest; // which of its node tests edgeNearFilter makes (2 here)
            ee::FunctionPtr<int32_t(uint32_t, EdgeRef, uint32_t)> callback;
            uint32_t arg;
            int32_t nodeCount;
        };
        ee::Local<Query> query;
        ee::Local<Walk> walk;
        walk->nodeTest = 2;
        walk->callback = callback;
        walk->arg = arg;
        if (!sphereQueryRaw(query, centre, kSphereAllFaces, kEdgeNearFilter, walk, radius))
            return 0;
        walk->nodeCount = resultCount(query);
        for (int32_t i = 0; i < walk->nodeCount; ++i)
        {
            const uint32_t node = resultNode(query, i);
            const uint32_t faceCount = static_cast<uint32_t>(resultFaceCount(query, i));
            for (uint32_t j = 0; j < faceCount; ++j)
            {
                const int32_t face = resultFace(query, i, static_cast<int32_t>(j));
                for (uint8_t corner = 0; corner < 3; ++corner)
                {
                    if (const uint32_t record = cornerTags(widen(node), face, corner))
                    {
                        const EdgeRef ref = nodeFaceCount(widen(node)) ? makeEdge(node, static_cast<uint32_t>(face), corner) : 0;
                        const int32_t result = walk->callback(record, ref, walk->arg);
                        if (result != 1)
                            return result;
                    }
                }
            }
        }
        return 0;
    }
    READABLE_FUNCTION(forEachCornerNear, Verified);

    // taggedCornersNear's callback: a corner of kind 1 or 2 goes on the list; 0 (stop) when the list is full,
    // else 1.
    int32_t keepTaggedCorner(uint32_t record, EdgeRef corner, EdgeList *list)
    {
        const uint32_t kind = calls::cornerTagKind(record);
        if (kind == 0 || kind >= kCornerKindsKept)
            return 1;
        if (list->count == list->capacity)
            return 0;
        list->items[list->count] = corner;
        list->count = list->count + 1;
        return 1;
    }
    READABLE_FUNCTION(keepTaggedCorner, Draft);

    // The tagged corners (kinds 1 and 2: what the skitch holds on to) near a point, as EdgeRefs ended by a 0;
    // 0 if none. railEdgesNear's twin.
    uint64_t *taggedCornersNear(const ee::Float *centre, ee::Float radius)
    {
        ee::Local<EdgeList> keep;
        uint64_t *list = g_nearCorners.get();
        keep->capacity = kNearCornerCapacity;
        keep->items = list;
        keep->count = 0;
        forEachCornerNear(centre, kKeepTaggedCorner, keep, radius);
        const int32_t n = keep->count;
        *g_nearCornerCount = n;
        if (n == 0)
            return nullptr;
        list[n] = 0;
        return list;
    }
    READABLE_FUNCTION(taggedCornersNear, Verified);

    namespace
    {
        // cornerKind's: a corner tag kind as the skitch holds it (0 none, kCornerHold, kCornerHoldMirrored; a kind
        // past 2 none).
        inline ee::Global<uint32_t[4], sym::g_cornerHoldKinds> g_cornerHoldKinds;
    }

    // A corner's place in the world (the corner an EdgeRef whose side is the corner, as taggedCornersNear lists
    // them).
    void cornerPoint(EdgeRef corner, ee::Float *point)
    {
        game::tail(vertexWorld, widen(refNode(corner)), edgeFace(corner), edgeSide(corner), point);
    }
    READABLE_FUNCTION(cornerPoint, Draft);

    // How the skitch holds on at a corner (its tag record's kind): 0 for a corner with no tag record.
    uint32_t cornerKind(EdgeRef corner)
    {
        const uint32_t record = cornerTags(widen(refNode(corner)), edgeFace(corner), edgeSide(corner));
        if (record == 0)
            return 0;
        return (*g_cornerHoldKinds)[calls::cornerTagKind(record)];
    }
    READABLE_FUNCTION(cornerKind, Draft);

    // Which side an edge is ground from by its face: 1 if the face is less
    // steep than 60 degrees (its normal's dot with down above cos 60), else
    // -1. The side itself is not read.
    int32_t edgeSideByNormal(uint64_t node, int32_t face, uint32_t side)
    {
        (void)side;
        ee::Local<zge::Vec4> down; // its w is left as it was
        ee::Local<zge::Vec4> normal;
        const ee::Float limit = zge::angleCos(kSteepAngle);
        down->y = -1.0f;
        down->x = 0.0f;
        down->z = 0.0f;
        faceNormalWorld(node, face, &normal->x);
        return limit < zge::dot(*down, *normal) ? 1 : -1;
    }
    READABLE_FUNCTION(edgeSideByNormal, Verified);

    // ---- a ray hit's plane

    // The plane of a hit's face through the hit, its normal turned toward
    // the ray's start.
    void hitPlane(const RayHit *hit, ee::Float *plane)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> toStart;
        ee::Local<QuerySegment> ray;
        const FaceRef face = hitFace(hit);
        hitPoint(hit, &point->x);
        faceNormal(face, &normal->x);
        hitRay(hit, &ray->from.x);
        toStart = ray->from - *point;
        if (zge::dot(*normal, *toStart) <= 0.0f)
            zge::vec3Negate(&normal->xyz(), &normal->xyz());
        zge::planeThrough(vec4(plane), point, normal);
    }
    READABLE_FUNCTION(hitPlane, Verified);

    namespace
    {
        // An object's velocity (a second's) in a frame's.
        inline ee::Global<ee::Float, sym::g_objectVelocityFrameShare> g_objectVelocityFrameShare; // 1/60
        // A bounce off a moving object: the approach made a quarter more, the rider's velocity along the line to the
        // object taken back by 0.95 of it, the object's given 0.05.
        constexpr ee::Float kBounceMore(1.25f);
        inline ee::Global<ee::Float, sym::g_riderBounceShare> g_riderBounceShare;   // 0.95
        inline ee::Global<ee::Float, sym::g_objectBounceShare> g_objectBounceShare; // 0.05
    }

    // hitImpact (impact): how fast the rider closes on the hit's face (the velocity against its plane's normal). A
    // moving object hit that the rider closes on (along the line from the hit to the object's place) bounces both:
    // each velocity's part along that line is replaced, the rider's by 0.95 of the bounce back, the object's by 0.05
    // of it forward (kImpactBounced, the bounce returned instead).
    ee::Float hitImpact(HitImpact *impact)
    {
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> objectVelocity;
        ee::Local<zge::Vec4> relative;
        ee::Local<zge::Vec4> toObject;
        ee::Local<zge::Vec4> along;
        zge::Vec4 &velocity = *vec4(impact->velocity);
        zge::Vec4 &riderAfter = *vec4(impact->riderVelocity);
        zge::Vec4 &objectAfter = *vec4(impact->objectVelocity);
        hitPlane(impact->hit.get(), &plane->x);
        const ee::Float closing = -zge::dot(velocity, *plane);
        impact->flags = 0;
        riderAfter = zge::kZero;
        objectAfter = zge::kZero;
        const uint32_t object = hitObject(impact->hit.get());
        if (object == 0)
            return closing;
        zge::objectVelocity(ee::Ptr<zge::SceneObject>(object), objectVelocity);
        *objectVelocity = *objectVelocity * *g_objectVelocityFrameShare;
        *relative = velocity - *objectVelocity;
        hitPoint(impact->hit.get(), &point->x);
        *toObject = *zge::objectPosition(ee::Ptr<zge::SceneObject>(object)) - *point;
        *toObject = zge::unit(*toObject);
        const ee::Float approach = zge::dot(*relative, *toObject);
        if (!(ee::Float(0.0f) <= approach))
            return closing;
        riderAfter = velocity;
        objectAfter = *objectVelocity;
        const ee::Float bounce = approach * kBounceMore;
        *along = *toObject * zge::dot(riderAfter, *toObject);
        riderAfter = riderAfter - *along;
        *along = *toObject * -(bounce * *g_riderBounceShare);
        riderAfter = riderAfter + *along;
        *along = *toObject * zge::dot(objectAfter, *toObject);
        objectAfter = objectAfter - *along;
        *along = *toObject * (bounce * *g_objectBounceShare);
        objectAfter = objectAfter + *along;
        impact->flags |= kImpactBounced;
        return bounce;
    }
    READABLE_FUNCTION(hitImpact, Verified);

    // ---- a face's sound, an edge's wall kind

    uint32_t materialTags(uint64_t node, int32_t face);
    uint32_t tagWallKind(uint32_t record);

    // A face's material sound (0 .. 15), 0 untagged.
    uint32_t faceSound(FaceRef ref)
    {
        const uint32_t record = materialTags(widen(refNode(ref)), faceIndex(ref));
        if (!record)
            return 0;
        return (*g_tagFieldValues)[materialSound(ee::pointerAt<const uint8_t>(record))];
    }
    READABLE_FUNCTION(faceSound, Verified);

    // An edge's wall kind (0 .. 15), 0 untagged.
    uint32_t edgeWallKind(EdgeRef edge)
    {
        const uint32_t record = edgeTags(widen(refNode(edge)), edgeFace(edge), edgeSide(edge));
        if (!record)
            return 0;
        return (*g_tagFieldValues)[tagWallKind(record)];
    }
    READABLE_FUNCTION(edgeWallKind, Verified);
}
