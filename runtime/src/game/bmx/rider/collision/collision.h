#pragma once

// BMX XXX's game-side collision and surface layer, DM2's (game/dm2/rider/
// collision) carried over to BMX XXX's engine (ZGE4): what the rider's
// physics asks of the world. The rays, the sphere queries and their
// cursors, faces and edges in world space, and the tag records the rider
// reads (surface kind, grind, wall ride) are DM2's; the differences are
// ZGE4's: the engine's query (collisionQuery) leaves its hits as the count
// and a pointer a node (zge::QueryResults, as ZGE3 does), a face or edge
// handle is made only for a node with a collision mesh that has faces
// (nodeFaceCount), and the transforms and tag record reads the engine's
// macros inline here.
//
// Handles. A face is (node, face index) packed in 64 bits, as the game
// passes it in one register (FaceRef); an edge adds the side (0 .. 2) in
// the top byte (EdgeRef). The node is a scene node (zge::Node); its
// collision mesh is its collision frame's part.
//
// Every function here is BMX XXX's own, registered in BMX XXX's instance
// (READABLE_FUNCTION) and checked against BMX XXX's recorded calls.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "zge/collision/collision.h"
#include "zge/render/part.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"
#include "bmx/rider/vector.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx::rider
{
    struct Rider; // ../rider.h
}

namespace game::bmx::rider::collision
{
    // A face: the scene node in the low 32 bits, the face's index in the
    // node's collision mesh in the high 32.
    using FaceRef = uint64_t;
    // An edge: a FaceRef with the face's side (0 .. 2: corner i to corner
    // i + 1) in bits 56 .. 63 and the face index in bits 32 .. 55.
    using EdgeRef = uint64_t;

    // A FaceRef's or EdgeRef's node (the low word, sign-extended as the
    // game's dsll32 / dsra32 makes it).
    inline int32_t refNode(uint64_t ref) { return static_cast<int32_t>(static_cast<uint32_t>(ref)); }
    // A 32-bit word as a register holds it (LW: sign-extended).
    inline uint64_t widen(uint32_t word) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word))); }
    inline int32_t faceIndex(FaceRef ref) { return static_cast<int32_t>(ref >> 32); }
    // An EdgeRef's face index: its low 24 bits.
    constexpr uint32_t kRefFaceMask = (1u << 24) - 1u;
    inline int32_t edgeFace(EdgeRef ref) { return static_cast<int32_t>((ref >> 32) & kRefFaceMask); }
    inline uint32_t edgeSide(EdgeRef ref) { return static_cast<uint32_t>(ref >> 56); }
    // A face from its node and index.
    inline FaceRef makeFace(uint32_t node, int32_t face)
    {
        return static_cast<uint64_t>(static_cast<uint32_t>(face)) << 32 | node;
    }
    // An edge from its node, face and side.
    inline EdgeRef makeEdge(uint32_t node, uint32_t face, uint8_t side)
    {
        return static_cast<uint64_t>(side) << 56 | static_cast<uint64_t>(face & kRefFaceMask) << 32 | node;
    }

    // A query's record, as the callers keep it: the query count when it was
    // made, and its hits (the engine's results, copied to the layer's own
    // buffers: segmentQueryRaw, sphereQueryRaw).
    struct Query
    {
        uint32_t serial;
        ee::Ptr<zge::QueryResults> hits;
    };
    static_assert(sizeof(Query) == 0x08);

    // A list of edges a walk fills (railEdgesNear's, keepGrindable adds to
    // it): its room, how many it has, the edges.
    struct EdgeList
    {
        int32_t capacity;
        int32_t count;
        ee::Ptr<EdgeRef> items;
    };
    static_assert(offsetof(EdgeList, count) == 0x04 && offsetof(EdgeList, items) == 0x08);

    // What a hit is (RayHit::flags).
    constexpr uint32_t kHitFace = 0x1u;   // a face of the world
    constexpr uint32_t kHitObject = 0x2u; // a face of an object's node

    // A ray's first hit (rayCast's record): the query, the face, the point,
    // the object if the face's node is one, the ray, and which ray it was.
    struct RayHit
    {
        Query query;
        FaceRef face;            // 0: the node has no collision faces
        ee::Float point[4];
        uint32_t flags;          // kHitFace or kHitObject
        uint32_t object;         // the owning object (nodeSceneObject), or 0
        uint8_t _28[0x08];
        ee::Float from[4];
        ee::Float to[4];
        int32_t serial;          // g_raySerial when it was cast
        uint8_t _54[0x0C];
    };
    static_assert(offsetof(RayHit, face) == 0x08 && offsetof(RayHit, point) == 0x10);
    static_assert(offsetof(RayHit, flags) == 0x20 && offsetof(RayHit, object) == 0x24);
    static_assert(offsetof(RayHit, from) == 0x30 && offsetof(RayHit, serial) == 0x50);
    static_assert(sizeof(RayHit) == 0x60);

    struct SphereHits;

    // A sphere query's cursor (nextSphereFace's result): the face, the
    // query's serial, the query.
    struct SphereFace
    {
        FaceRef face;
        int32_t serial;
        ee::Ptr<SphereHits> owner;
    };
    static_assert(sizeof(SphereFace) == 0x10);

    // sphereQuery's record: the query, where the walk over its nodes and
    // faces is, the sphere, the cursor.
    struct SphereHits
    {
        Query query;
        int32_t nodeCount;
        int32_t nodeIndex;
        int32_t faceCount; // the current node's
        int32_t faceIndex;
        int32_t serial;    // g_sphereSerial when it was made
        uint8_t _1c[0x04];
        ee::Float centre[4];
        ee::Float radius;
        uint8_t _34[0x0C];
        SphereFace current;
    };
    static_assert(offsetof(SphereHits, nodeCount) == 0x08 && offsetof(SphereHits, serial) == 0x18);
    static_assert(offsetof(SphereHits, centre) == 0x20 && offsetof(SphereHits, radius) == 0x30);
    static_assert(offsetof(SphereHits, current) == 0x40 && sizeof(SphereHits) == 0x50);

    // The touch message's data: whether it is taken (the receiver may
    // clear `accept`), how fast, how (the caller's word), where, which
    // face of which node.
    struct TouchData
    {
        int32_t accept;        // +0x00
        uint8_t _04[0x0C];
        ee::Float velocity[4]; // +0x10
        uint32_t how;          // +0x20
        uint8_t _24[0x0C];
        ee::Float point[4];    // +0x30
        uint8_t _40[0x14];
        int32_t face;          // +0x54
        uint32_t node;         // +0x58
        uint8_t _5c[4];
    };
    static_assert(sizeof(TouchData) == 0x60 && offsetof(TouchData, how) == 0x20 && offsetof(TouchData, face) == 0x54);
    constexpr uint32_t kMessageTouch = 0x104; // (DM2's is 0x102)

    // (faces.cpp, hits.cpp, queries.cpp, tags.cpp)
    int32_t acceptAll();
    void closestOnFace(const SphereFace *face, ee::Float *point);
    ee::Float edgeDirection(EdgeRef edge, ee::Float *direction);
    int32_t edgePlane(EdgeRef edge, ee::Float *plane);
    uint32_t edgeWallKind(EdgeRef edge);
    int32_t edgeGrindSide(EdgeRef edge);
    uint32_t edgeGrindable(EdgeRef edge);
    void edgePoint(EdgeRef edge, uint32_t end, ee::Float *point);
    int32_t edgeSideByNormal(uint64_t node, int32_t face, uint32_t side);
    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side);
    uint32_t edgeTagWord(EdgeRef edge);
    void faceCentre(FaceRef ref, ee::Float *centre);
    EdgeRef faceLinkedEdge(FaceRef ref);
    void faceNormal(FaceRef ref, ee::Float *normal);
    void faceNormalWorld(uint64_t node, int32_t face, ee::Float *normal);
    ee::Float faceShortestSide(FaceRef ref);
    uint32_t faceSound(FaceRef ref);
    FaceRef faceRefAt(const FaceRef *ref);
    int32_t faceSideCount(uint64_t node, int32_t face);
    uint32_t faceSurfaceKind(FaceRef ref);
    uint32_t faceTagByte9(FaceRef ref);
    uint32_t faceTags(uint64_t node, int32_t face);
    uint32_t faceTagWord(FaceRef ref);
    void faceVertex(FaceRef ref, uint32_t corner, ee::Float *point);
    void forEachNode(uint32_t callback, ee::Address arg);
    int32_t forEachEdgeNear(const ee::Float *centre, uint32_t callback, ee::Address arg, ee::Float radius);
    uint64_t *railEdgesNear(const ee::Float *centre, ee::Float radius);
    uint64_t *taggedCornersNear(const ee::Float *centre, ee::Float radius);
    int32_t forEachCornerNear(const ee::Float *centre, uint32_t callback, ee::Address arg, ee::Float radius);
    // A tagged corner's place, and how the skitch holds on there (cornerKind): on the hold point's side
    // (kCornerHold) or across it, the bike mirrored (kCornerHoldMirrored); 0 not at all.
    void cornerPoint(EdgeRef corner, ee::Float *point);
    uint32_t cornerKind(EdgeRef corner);
    constexpr uint32_t kCornerHold = 1;
    constexpr uint32_t kCornerHoldMirrored = 2;
    int32_t keepGrindable(uint32_t record, EdgeRef edge, EdgeList *list);
    FaceRef hitFace(const RayHit *hit);
    uint32_t hitFlags(const RayHit *hit);
    uint32_t hitObject(const RayHit *hit);
    int32_t touchObject(Rider *rider, const RayHit *hit, uint32_t how);
    RayHit *rayCastTouching(Rider *rider, const ee::Float *from, const ee::Float *to, uint32_t how);
    // A hit's impact (hitImpact): the hit and the rider's velocity in; out, with a moving object bounced
    // (kImpactBounced), the rider's velocity and the object's after it.
    struct HitImpact
    {
        ee::Ptr<RayHit> hit;          // +0x00
        uint8_t _04[0x0C];
        ee::Float velocity[4];        // +0x10
        uint32_t flags;               // +0x20 kImpactBounced
        uint8_t _24[0x0C];
        ee::Float riderVelocity[4];   // +0x30
        ee::Float objectVelocity[4];  // +0x40
    };
    static_assert(sizeof(HitImpact) == 0x50 && offsetof(HitImpact, riderVelocity) == 0x30);
    constexpr uint32_t kImpactBounced = 0x1u;
    ee::Float hitImpact(HitImpact *impact);
    void hitPlane(const RayHit *hit, ee::Float *plane);
    void hitPoint(const RayHit *hit, ee::Float *point);
    void hitRay(const RayHit *hit, ee::Float *ends);
    uint32_t materialSound(const uint8_t *record);
    SphereFace *nextSphereFace(SphereHits *s);
    uint32_t nodeCollisionMesh(uint64_t node);
    uint32_t nodeFaceCount(uint64_t node);
    // (nodes.cpp)
    ee::Ptr<zge::GeoObject> nodeObjectClass(uint64_t node);
    uint32_t showNode(uint64_t node, uint32_t shown);
    uint32_t setNodeCollidable(uint64_t node, uint32_t collides);
    bool nodeHasFaceTags(uint64_t node);
    bool nodeHasEdgeTags(uint64_t node);
    bool nodeHasVertexTags(uint64_t node);
    void forEachTaggedFace(uint32_t callback, uint32_t arg);
    void forEachTaggedEdge(uint32_t callback, uint32_t arg);
    uint64_t nodeHandle(uint64_t node);
    uint64_t nodeOf(uint64_t node);
    RayHit *rayCast(const ee::Float *from, const ee::Float *to);
    int32_t resultCount(const Query *query);
    int32_t resultFace(const Query *query, int32_t i, int32_t j);
    int32_t resultFaceCount(const Query *query, int32_t i);
    uint32_t resultNode(const Query *query, int32_t i);
    int32_t segmentQueryRaw(const ee::Float *from, const ee::Float *to, uint32_t filter, uint32_t filterArg, Query *query);
    int32_t sphereQueryRaw(Query *query, const ee::Float *centre, uint32_t allFaces, uint32_t filter,
                           ee::Address filterArg, ee::Float radius);
    SphereHits *sphereQuery(const ee::Float *centre, ee::Float radius);
    void vertexWorld(uint64_t node, int32_t face, uint32_t corner, ee::Float *point);
    // A node's collision mesh's box in the world (nodeCollisionBounds; the level's exploders ask for it).
    struct NodeBounds
    {
        ee::Float low[4];
        ee::Float high[4];
    };
    void nodeCollisionBounds(uint64_t node, NodeBounds *bounds);
    // A face of a node's collision mesh in the world, as forEachNodeFace hands it to its visit (one record, made
    // again for each face): its normal, its material, its corners (each with its texture coordinates and colour, in
    // one table).
    struct NodeFaceCorner
    {
        zge::Vec4 position;       // +0x00 in the world
        ee::Float u, v;           // +0x10 its texture coordinates
        zge::VertexColour colour; // +0x18
        uint8_t _1c[4];
    };
    static_assert(sizeof(NodeFaceCorner) == 0x20 && offsetof(NodeFaceCorner, colour) == 0x18);
    struct NodeFace
    {
        zge::Vec4 normal;                 // +0x00 in the world
        ee::Ptr<zge::Material> material;  // +0x10 the face's
        uint8_t cornerCount;              // +0x14
        uint8_t _15[3];
        ee::Ptr<NodeFaceCorner> corners;  // +0x18
    };
    static_assert(offsetof(NodeFace, material) == 0x10 && offsetof(NodeFace, cornerCount) == 0x14 && offsetof(NodeFace, corners) == 0x18);
    // A visit: 1 to go on to the next face, anything else ends the walk. The node is the walk's, as a word.
    using NodeFaceVisit = ee::FunctionPtr<int32_t(uint32_t node, ee::Ptr<NodeFace> face, ee::Address context)>;
    // Each face of a node's object's collision mesh (its first level of detail) handed to visit(node, face,
    // context) until it returns other than 1 (faces.cpp).
    void forEachNodeFace(uint64_t node, NodeFaceVisit visit, ee::Address context);
    // (tags.cpp) What a tag record says: its surface kind, its tag word,
    // whether it has a linked side.
    uint32_t tagSurfaceKind(uint32_t record);
    uint32_t faceRecordTag(uint32_t record);
    uint32_t edgeRecordTag(uint32_t record);
    uint32_t tagHasLink(uint32_t record);
    // A face corner's tag record (the mesh's vertex tags by the corner's index), and the same as the rider calls it.
    ee::Address cornerTagRecord(uint64_t node, int32_t face, uint32_t corner);
    uint32_t cornerTags(uint64_t node, int32_t face, uint32_t corner);
    // (nodes.cpp) forEachTaggedFace's and forEachTaggedEdge's node visitors:
    // the walk's callback called for each tagged face (record, node, face,
    // argument) or tagged edge (record, node, face, side, argument) of the
    // node; a callback that does not answer 1 ends the walk (0).
    struct TaggedWalk
    {
        uint32_t callback;
        uint32_t arg;
    };
    int32_t taggedFaceVisitor(uint64_t node, const TaggedWalk *walk);
    int32_t taggedEdgeVisitor(uint64_t node, const TaggedWalk *walk);
    // (tag_search.cpp) The level's faces, or edges, whose tag is one of
    // `count` tags: a shared list ended by a 0, or 0 when none is found
    // (the objectives' search). The search the walk's callbacks fill.
    struct TagSearch
    {
        ee::Ptr<uint32_t> tags;  // +0x00 the tags sought
        uint32_t count;          // +0x04 how many
        uint32_t capacity;       // +0x08 how many the list takes
        uint32_t found;          // +0x0C how many it has
        ee::Ptr<uint64_t> out;   // +0x10 the list (FaceRef or EdgeRef)
    };
    static_assert(offsetof(TagSearch, found) == 0x0C && sizeof(TagSearch) == 0x14);
    ee::Ptr<uint64_t> facesTagged(ee::Ptr<uint32_t> tags, uint32_t count);
    ee::Ptr<uint64_t> edgesTagged(ee::Ptr<uint32_t> tags, uint32_t count);
    int32_t collectTaggedFace(uint32_t record, uint64_t node, int32_t face, TagSearch *search);
    int32_t collectTaggedEdge(uint32_t record, uint64_t node, int32_t face, uint32_t side, TagSearch *search);
    // (links.cpp) The links between tagged faces and edges, made at a
    // level's load: every node's (always 1), and one node's (forEachNode's
    // visitor).
    int32_t scanNodesEdgeTags();
    int32_t linkNodeFaceEdges(uint64_t node);
    uint32_t tagLinkKind(uint32_t record);
    void setRecordTag(uint32_t record, uint32_t tag);
    void setTagLink(uint32_t record, uint32_t face, uint32_t side);
    void clearTagLink(uint32_t record);
    void linkFaceToEdge(uint32_t faceRecord, uint64_t node, int32_t face, uint32_t side);
    ee::Float linkReach();
}
