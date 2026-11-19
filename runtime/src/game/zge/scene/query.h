#pragma once

// Sphere queries over the scene (collision and proximity): a sphere is
// tested against every node whose bounding sphere it touches. The visitor
// sphereQueryVisit runs on each node of a subtree, once per query, and hands
// the node to the sphere test sphereQueryTestNode.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/node.h"

#include <cstddef>
#include "sym/zge/scene/query.h.h"

namespace zge
{
    // The segment a query sweeps along, from start to end.
    struct QuerySegment
    {
        Vec4 start;
        Vec4 end;
    };

    struct QueryHit;
    struct QueryResults; // zge/collision/collision.h

    enum SphereQueryFlags : uint32_t
    {
        kQueryExcludeListed = 0x08, // skip the nodes in SphereQuery::nodes
        kQueryOnlyListed = 0x10,    // test only the nodes in SphereQuery::nodes
        // ZGE2 only: the boxes the query tests are drawn (runQuery draws the sector's, queryTestShape the node's: a
        // debug box each, addDebugText); ZGE3 dropped the step.
        kQueryDrawTestedBoxes = 0x20,
    };

    // A collision query (collisionQuery builds it in its frame; the full
    // layout is in zge/collision/collision.h): its kind, its shape, the
    // nodes to skip or keep. For a segment (kind 1) the bounds of the segment
    // (a sphere and a box around it) are tested against the scene's nodes;
    // every node hit gets a QueryHit, sorted by how far along the segment it
    // is.
#if PS2X_GAME_DM2
    // ZGE2's query (0x40 bytes on collisionQuery's stack; read in DM2's collisionQuery, queryTestShape and
    // sphereQueryTestNode): the same first seven words, the hit and the stamp right after, and no centre, radius
    // or doubled box (ZGE2's queryOverlapsBox works the bounds out from the shape in scratch words of its own).
    // AI's fields ZGE2 has not got stand past the end, for the functions with no DM2 counterpart.
    struct SphereQuery
    {
        uint32_t kind;                     // DM2 +0x00 QueryKind
        ee::Ptr<QuerySegment> segment;     // DM2 +0x04 the shape (a QuerySegment for kind 1)
        uint32_t flags;                    // DM2 +0x08 SphereQueryFlags (zge/collision/collision.h QueryFlags)
        uint32_t filter;                   // DM2 +0x0C a game function (context, node, part, face*) -> keep; 0: none
        uint32_t filterContext;            // DM2 +0x10 its first argument
        ee::Ptr<QueryResults> results;     // DM2 +0x14 the caller's (0: none)
        ee::Ptr<ee::Ptr<Node>> nodes;      // DM2 +0x18 null-terminated, for the flags above
        uint8_t _1c[0x18];
        ee::Ptr<QueryHit> hit;             // DM2 +0x34 the node hit being tested (kind 1, testHitNodeFaces)
        uint8_t stamp;                     // DM2 +0x38 the query's number (Node::queryStamp)
        uint8_t _39[0x07];
        Vec4 centre;                       // (ZGE3 and ZGE4 only, from here on)
        ee::Float radius;
        uint8_t _54[0x0C];
        Vec4 boxSize;
        Vec4 boxSum;
    };
    static_assert(offsetof(SphereQuery, hit) == 0x34 && offsetof(SphereQuery, stamp) == 0x38 &&
                  offsetof(SphereQuery, centre) == 0x40);
#else
    struct SphereQuery
    {
        uint32_t kind;                     // +0x00 QueryKind
        ee::Ptr<QuerySegment> segment;     // +0x04 the shape (a QuerySegment for kind 1)
        uint32_t flags;                    // +0x08 SphereQueryFlags (zge/collision/collision.h QueryFlags)
        uint32_t filter;                   // +0x0C a game function (context, node, part, face*) -> keep; 0: none
        uint32_t filterContext;            // +0x10 its first argument
        ee::Ptr<QueryResults> results;     // +0x14 the caller's (0: none)
        ee::Ptr<ee::Ptr<Node>> nodes;      // +0x18 null-terminated, for the flags above
        uint8_t _1c[0x04];
        Vec4 centre;      // +0x20
        ee::Float radius; // +0x30 (added to the node's radius for the overlap test)
        uint8_t _34[0x0C];
        // +0x40 the bounding box, doubled: max - min, and max + min (+0x50),
        // so an overlap is |sum - sum'| <= size + size' per axis.
        Vec4 boxSize;
        Vec4 boxSum;      // +0x50
        ee::Ptr<QueryHit> hit; // +0x60 the node hit being tested (kind 1, testHitNodeFaces)
        // +0x64 the query's number: a node visited by this query already has
        // it in Node::queryStamp.
        uint8_t stamp;
    };
    static_assert(offsetof(SphereQuery, centre) == 0x20 && offsetof(SphereQuery, radius) == 0x30);
    static_assert(offsetof(SphereQuery, boxSize) == 0x40 && offsetof(SphereQuery, boxSum) == 0x50);
    static_assert(offsetof(SphereQuery, stamp) == 0x64);
#endif
    static_assert(offsetof(SphereQuery, segment) == 0x04 && offsetof(SphereQuery, flags) == 0x08);
    static_assert(offsetof(SphereQuery, nodes) == 0x18);
    static_assert(offsetof(SphereQuery, filter) == 0x0C && offsetof(SphereQuery, results) == 0x14);
    static_assert(offsetof(SphereQuery, hit) == game::readable::perGame(0x60u, 0x34u, 0x60u) &&
                  offsetof(SphereQuery, stamp) == game::readable::perGame(0x64u, 0x38u, 0x64u));

    // A node a query's segment hits.
    struct QueryHit
    {
        QuerySegment segment;  // +0x00 the query's
        QuerySegment clipped;  // +0x20 the part inside the node's box (clipSegmentToBox)
        ee::Float fraction;    // +0x40 where the clipped part starts, 0 at start .. 1 at end
        ee::Ptr<Node> node;    // +0x44
    };
    static_assert(offsetof(QueryHit, clipped) == 0x20 && offsetof(QueryHit, fraction) == 0x40 &&
                  offsetof(QueryHit, node) == 0x44);

    // Where the hits go (g_queryState + 0x38: the sorted list, + 0x48: the
    // record pool; ZGE2 keeps both at the end of its 0xD850-byte state,
    // +0xD834 and +0xD844: zge/collision/collision.h QueryState).
    constexpr uint32_t kQueryResults = sym::g_queryState;
    inline ee::Global<uint32_t, kQueryResults + game::readable::perGame(0x48u, 0xD844u, 0x48u)> g_queryHitPool;
    constexpr uint32_t kQueryHitList = kQueryResults + game::readable::perGame(0x38u, 0xD834u, 0x38u);

    // sphereQueryTestNode: tests one node against a query (see SphereQuery); a
    // node hit gets a QueryHit. Always 1 (go on).
    int32_t sphereQueryTestNode(Node *node, SphereQuery *query);
    constexpr uint32_t kTestNodeAgainstSphere = sym::sphereQueryTestNode;

    // forEachNodeInSubtree: calls visit(node, context) on the node and every node
    // below it, depth first, until a visit returns 0. Returns 0 if one did,
    // else 1. `visit` is a game function's address.
    int32_t forEachNodeInSubtree(ee::Ptr<Node> root, uint32_t visitFunction, ee::Address context);

    // sphereQueryVisit: a sphere query's visitor (see above). Always 1.
    int32_t sphereQueryVisit(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query);

    // ---- running a query over a sector ------------------------------------

    // SphereQuery::_00, the query's kind: what runQuery walks it with.
    enum class QueryKind : uint32_t
    {
        Shape = 0,       // nodes' collision shapes, after the sector's box passed
        Sphere = 1,      // the sphere test (sphereQueryTestNode), after the sector's box passed
        ShapeAll2 = 2,   // collision shapes, every sector [inferred: the box test does not apply]
        ShapeAll3 = 3,
    };

    // The nodes a sector holds at its top level, walked with a cursor kept in
    // the list (-1 when no walk is under way): ZGE3's array record (zge/container/array.h PointerArray). ZGE2's list is
    // its own array record (items +0x00, count +0x04, capacity +0x08, cursor +0x0C), which only its array functions
    // touch here (arrayFirst, arrayNext).
    struct NodeList
    {
        ee::Ptr<ee::Ptr<Node>> items; // +0x00
        int32_t cursor;               // +0x04
        uint16_t count;               // +0x08
    };
    static_assert(offsetof(NodeList, cursor) == 0x04 && offsetof(NodeList, count) == 0x08);

    // What a query runs over: a box (its first bytes; handed to the box test
    // as is) and the nodes in it; one cell of the world. [inferred: a level sector]
    // ZGE2's sector (DM2) is 0x60 bytes: four words in front of the box, which
    // buildWorldGrid clears (its stride in forEachWorldSector, freeWorldGrid and
    // reportWorldCells).
    struct Sector
    {
#if PS2X_GAME_DM2
        uint32_t cleared[4];     // DM2 +0x00 the four words buildWorldGrid zeroes [what they hold: no DM2 function read so far]
        uint8_t box[0x20];       // DM2 +0x10 (runQuery and reportWorldCells hand it on)
        Vec4 sphere;             // DM2 +0x30 (sectorInView)
        uint8_t _40[0x10];       // DM2 +0x40
        ee::Ptr<NodeList> nodes; // DM2 +0x50 (newCursorArray's in buildWorldGrid; forEachSectorNode walks it)
        // [unplaced: no DM2 function read so far assigns or reads a sector's region (DM2 has no
        // assignCellRegions pair); it stands where nothing else is]
        uint32_t region;         // DM2 +0x54
        uint8_t _58[0x08];
#else
        uint8_t box[0x20];       // +0x00 (queryOverlapsBox's box)
        Vec4 sphere;             // +0x20 what the scene draw tests against the view
        uint8_t _30[0x10];
        ee::Ptr<NodeList> nodes; // +0x40
        uint32_t region;         // +0x44 as Node::region: 0 when none
        uint8_t _48[0x08];
#endif
    };
    static_assert(offsetof(Sector, sphere) == game::readable::perGame(0x20u, 0x30u) &&
                  offsetof(Sector, nodes) == game::readable::perGame(0x40u, 0x50u) &&
                  sizeof(Sector) == game::readable::perGame(0x50u, 0x60u));

    constexpr uint32_t kQueryTestShape = sym::queryTestShape;  // (node, query): a node's collision shape against a query
    constexpr uint32_t kQueryOverlapsBox = sym::queryOverlapsBox; // (kind, query, box, matrix or 0) -> non-zero on overlap

    // forEachSectorNode: visit(node, context) for each node of the sector's list
    // until one returns 0 (then 0; else 1).
    int32_t forEachSectorNode(ee::Ptr<Sector> sector, uint32_t visitFunction, ee::Address context);

    // shapeQueryVisit: the visitor of the other query kinds: as
    // sphereQueryVisit, with the shape test. Always 1.
    int32_t shapeQueryVisit(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query);

    // runQuery: runs a query over a sector's nodes by its kind. Always 1.
    int32_t runQuery(ee::Ptr<Sector> sector, ee::Ptr<SphereQuery> query);
}
