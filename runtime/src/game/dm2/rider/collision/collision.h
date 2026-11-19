#pragma once

// Dave Mirra BMX 2's game-side collision and surface layer (DM2 game.c's
// collision API, hitCount .. closestOnFace and the tag wrappers terrainTagWrapA
// .. materialSound): what the rider's physics asks of the world. It sits on DM2's
// engine (ZGE2: the scene query sceneQuery, the meshes' faces and
// vertices, the tag records); the same interface is implemented over Aggressive
// Inline's collision.
//
// The queries the physics makes, as this layer gives them:
//   - a ray (segment) from a point to a point: the first hit's face or
//     object, point (rayCast, rayCastAhead, their RayHit records);
//   - a sphere: the nodes and faces it touches (sphereQuery, the Query
//     records and their accessors);
//   - a face: its corners, normal, centre, shortest edge, in world space;
//   - an edge (a face's side): its ends, direction, plane, and tags;
//   - a face's or edge's tag record and the tag bytes and words read from
//     it (surface kind, grind, wall ride; DM2's level data, the TGFE tags).
//
// Handles. A face is (node, face index) packed in 64 bits, as DM2 passes it
// in one register (FaceRef); an edge adds the side (0 .. 2) in the top byte
// (EdgeRef). The node is a scene node (zgeNode); its collision mesh is
// node +0x15C -> +0x14.
//
// Every function here is DM2's own, registered in DM2's instance
// (READABLE_FUNCTION) and checked against DM2's recorded calls
// (ps2xFnCheck --cycles); they install only when DM2 runs.

#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/types.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "dm2/rider/vector.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/rider/collision/collision.h.h"

namespace zge
{
    struct GeoObject;      // zge/assets/geo.h
    struct Material;       // zge/render/material.h
    struct TagTableHeader; // zge/assets/tag_table.h
}

namespace game::dm2::rider::collision
{
    // A face: the scene node in the low 32 bits, the face's index in the
    // node's collision mesh in the high 32.
    using FaceRef = uint64_t;
    // An edge: a FaceRef with the face's side (0 .. 2: corner i to corner
    // i + 1) in bits 56 .. 63 and the face index in bits 32 .. 55.
    using EdgeRef = uint64_t;

    // A FaceRef's or EdgeRef's node (the low word, sign-extended as DM2's
    // dsll32 / dsra32 makes it).
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

    // One face a query touched (0x30 bytes): its index, and the point of
    // contact at +0x10.
    struct QueryFace
    {
        int32_t face;
        uint8_t unknown04[0x0C];
        ee::Float point[4];
        uint8_t unknown20[0x10];
    };
    static_assert(sizeof(QueryFace) == 0x30);

    // One node a query touched (16 bytes): the node, its faces.
    struct QueryNode
    {
        uint32_t node;
        uint32_t part;       // the node's mesh part (a zge::ZgePart: bailHandContact reads its faces' vertices)
        int32_t faceCount;
        ee::Ptr<QueryFace> faces;
    };
    static_assert(sizeof(QueryNode) == 0x10);

    // A query's hits: the nodes it touched.
    struct QueryHits
    {
        int32_t nodeCount;
        ee::Ptr<QueryNode> nodes;
    };

    // A query's record, as the callers keep it (the hits at +4).
    struct Query
    {
        uint32_t serial; // the query count when it was made
        ee::Ptr<QueryHits> hits;
    };

    // What a hit is (RayHit::flags).
    constexpr uint32_t kHitFace = 0x1u;   // a face of the world
    constexpr uint32_t kHitObject = 0x2u; // a face of an object's node

    // A ray's first hit (rayCast's record g_rayHit, rayCastAhead's
    // g_rayHitAhead): the query, the face, the point, the object if the face's
    // node is one, the ray, and which ray it was.
    struct RayHit
    {
        Query query;
        FaceRef face;
        ee::Float point[4];
        uint32_t flags;          // kHitFace or kHitObject
        uint32_t object;         // the owning object (kOwnerObject), or 0
        uint8_t unknown28[0x08];
        ee::Float from[4];
        ee::Float to[4];
        int32_t serial;          // g_raySerial when it was cast
        uint8_t unknown54[0x0C];
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

    // sphereQuery's record (g_sphereQuery): the query, where the walk over its
    // nodes and faces is, the sphere, the cursor.
    struct SphereHits
    {
        Query query;
        int32_t nodeCount;
        int32_t nodeIndex;
        int32_t faceCount; // the current node's
        int32_t faceIndex;
        int32_t serial;    // g_sphereSerial when it was made
        uint8_t unknown1C[0x04];
        ee::Float centre[4];
        ee::Float radius;
        uint8_t unknown34[0x0C];
        SphereFace current;
    };
    static_assert(offsetof(SphereHits, nodeCount) == 0x08 && offsetof(SphereHits, serial) == 0x18);
    static_assert(offsetof(SphereHits, centre) == 0x20 && offsetof(SphereHits, radius) == 0x30);
    static_assert(offsetof(SphereHits, current) == 0x40 && sizeof(SphereHits) == 0x50);

    // A collect callback's state (collectNodesA / B): the caller's two
    // words, the capacity, the count, the list (FaceRef-sized slots, ended
    // by a 0).
    struct NodeList
    {
        uint32_t arg0;
        uint32_t arg1;
        int32_t capacity;
        int32_t count;
        ee::Ptr<uint64_t> items;
    };

    // ---- DM2's engine (ZGE2) as this layer calls it, from DM2's rider map.
    // These are DM2's own code where AI has a function of the same name (or
    // none): each is called where it is, through its typed declaration.
    namespace engine
    {
        // (kind: sphere 0, segment 1; shape; 4; filter, filterState, 0; hits): the hits
        // into the last (t2, which DM2 sets in its prologue, not with the others).
        constexpr uint32_t kSceneQuery = sym::sceneQuery; // (no AI counterpart)
        constexpr uint32_t kNodeMatrix = sym::nodeMatrix; // (node, 1) -> world matrix
        constexpr uint32_t kOwnerObject = sym::nodeSceneObject; // (node, 0) -> the object, or 0

        // out = the point through the matrix (its rotation, then its
        // translation: ZGE2's transformPoint; ZGE3 and ZGE4 inline it).
        inline void transformPoint(ee::Float *out, ee::Ptr<zge::Matrix> matrix, const ee::Float *point)
        {
            ee::game(sym::dm2::transformPoint, out, matrix, point);
        }
        // Whether queries test the node (the engine's nodeCollides, on the 64-bit node word).
        inline uint32_t nodeCollides(uint64_t node)
        {
            return zge::nodeCollides(ee::Ptr<zge::Node>(static_cast<uint32_t>(node)));
        }
        // v.v over x, y and z.
        inline ee::Float lengthSquared(const ee::Float *v)
        {
            return zge::lengthSquared(&vec4(v)->xyz());
        }
        // The object a node belongs to, or 0, and with `boneOut` the bone it
        // is on (the engine's nodeSceneObject, on the 64-bit node word).
        inline uint32_t nodeObject(uint64_t node, ee::Address boneOut)
        {
            const ee::Ptr<zge::Node> sceneNode(static_cast<uint32_t>(node));
            return ee::Address(zge::nodeSceneObject(sceneNode, ee::Ptr<ee::Ptr<zge::SceneBone>>(boneOut)));
        }
    }
}

namespace game::dm2::rider
{
    struct Rider; // (rider.h)
}

namespace game::dm2::rider::collision
{
    // (faces.cpp, hits.cpp, queries.cpp, tags.cpp, touch.cpp)
    int32_t acceptAll();
    void closestOnFace(const SphereFace *face, ee::Float *point);
    ee::Float edgeDirection(EdgeRef edge, ee::Float *direction);
    int32_t edgeGrindSide(EdgeRef edge);
    uint32_t edgeGrindable(EdgeRef edge);
    void edgePlane(EdgeRef edge, ee::Float *plane);
    void edgePoint(EdgeRef edge, uint32_t end, ee::Float *point);
    int32_t edgeSideByNormal(uint64_t node, int32_t face, uint32_t side);
    uint32_t edgeTags(uint64_t node, int32_t face, uint32_t side);
    uint32_t edgeWallKind(EdgeRef edge);
    void faceCentre(FaceRef ref, ee::Float *centre);
    EdgeRef faceLinkedEdge(FaceRef ref);
    void faceNormal(FaceRef ref, ee::Float *normal);
    void faceNormalWorld(uint64_t node, int32_t face, ee::Float *normal);
    FaceRef faceRefAt(const FaceRef *ref);
    ee::Float faceShortestSide(FaceRef ref);
    uint32_t faceSound(FaceRef ref);
    uint32_t faceSurfaceKind(FaceRef ref);
    uint32_t faceTagByte9(FaceRef ref);
    uint32_t faceTagWord(FaceRef ref);
    uint32_t edgeTagWord(EdgeRef edge);
    void faceVertex(FaceRef ref, uint32_t corner, ee::Float *point);
    ee::Float hitClosing(const ee::Float *velocity, const RayHit *hit, ee::Float *objectApproach);
    FaceRef hitFace(const RayHit *hit);
    uint32_t hitFlags(const RayHit *hit);
    uint32_t hitObject(const RayHit *hit);
    void hitPlane(const RayHit *hit, ee::Float *plane);
    void hitPoint(const RayHit *hit, ee::Float *point);
    void hitRay(const RayHit *hit, ee::Float *ends);
    uint32_t materialSound(const uint8_t *record);
    SphereFace *nextSphereFace(SphereHits *s);
    uint32_t nodeCollisionMesh(uint64_t node);
    uint32_t nodeCollidable(uint64_t node);
    uint64_t nodeHandle(uint64_t node);
    // The node's object class (the engine's objectClass).
    ee::Ptr<zge::GeoObject> nodeObjectClass(uint64_t node);
    // The node shown or hidden, and whether queries test it (the node
    // alone, not its subtree; faces.cpp); the engine's answers.
    uint32_t showNode(uint64_t node, uint32_t shown);
    uint32_t setNodeCollidable(uint64_t node, uint32_t collides);
    uint64_t nodeOf(uint64_t node);
    uint64_t * railEdgesNear(const ee::Float *centre, ee::Float radius);
    RayHit *rayCast(const ee::Float *from, const ee::Float *to);
    RayHit *rayCastAhead(const ee::Float *from, const ee::Float *to);
    int32_t resultCount(const Query *query);
    int32_t resultFace(const Query *query, int32_t i, int32_t j);
    int32_t resultFaceCount(const Query *query, int32_t i);
    uint32_t resultNode(const Query *query, int32_t i);
    int32_t segmentQueryRaw(const ee::Float *from, const ee::Float *to, uint32_t filter, uint32_t filterArg, Query *query);
    SphereHits *sphereQuery(const ee::Float *centre, ee::Float radius);
    uint32_t tagGrindable(uint32_t record);
    uint32_t faceRecordTag(uint32_t record);
    uint32_t edgeRecordTag(uint32_t record);
    bool nodeHasEdgeTags(uint64_t node);
    bool nodeHasFaceTags(uint64_t node);
    uint16_t nodeFaceCount(uint64_t node);
    uint32_t tagWallKind(uint32_t record);
    int32_t touchObject(Rider *rider, const RayHit *hit);
}
