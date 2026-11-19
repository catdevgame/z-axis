// The level's rails at the load (gamehand13; were the sweep's drafts of
// forEachModelPart .. freeLevelRails and pairedRail): setupLevelRails, from the level load,
// the level's unload and the park editor's test run, hashes the mover
// classes' names (isMover, skater/surface.cpp), makes the double rails'
// pools and trees (level/double_rails.h), walks the world's meshes
// (collectDoubleRailEdges on each node, collision_setup.cpp) and models
// (setupCollisionPart on each part) and pairs every double rail edge of the
// first kind with the nearest parallel one of the other kind on the same
// object, within 4 units (pairDoubleRailEdge): their middle line is the
// rail. An edge left without a partner loses its tag. Also here: the walk
// over a model's collision parts and the closest points of two segments
// the grind search uses.
//

#include "aggressive/level/double_rails.h"
#include "double_rails.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "aggressive/level/matrix_points.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/tree.h"
#include "zge/math/vector.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/text/strings.h"
#include "zge/assets/geo.h"
#include "aggressive/skater/skater.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/rails_setup.cpp.h"

namespace aggressive::skater
{
}

namespace aggressive::level
{
    namespace
    {
        constexpr uint32_t kSearchTreeForEach = sym::searchTreeForEach; // (tree, visit(node, context), context)

        constexpr uint32_t kForEachModelPart = sym::forEachModelPart;
        constexpr uint32_t kCompareRailEdges = sym::compareRailEdges;
        constexpr zge::SearchTreeVisit kMatchDoubleRailEdge(sym::matchDoubleRailEdge);
        constexpr zge::SearchTreeVisit kPairDoubleRailEdge(sym::pairDoubleRailEdge);
        constexpr zge::SearchTreeVisit kKeepRailEdge(sym::keepRailEdge);
        constexpr uint32_t kCollectDoubleRailEdges = sym::collectDoubleRailEdges; // collision_setup.cpp
        constexpr uint32_t kSetupCollisionPart = sym::setupCollisionPart;     // collision_setup.cpp
        constexpr uint32_t kCollectMeshRailEdges = sym::collectMeshRailEdges;
        constexpr uint32_t kSetupModelCollision = sym::setupModelCollision;

        // The mover classes (isMover's): their type names (0x19 bytes each)
        // and the hashes made from them here.
        constexpr uint32_t kMoverClassNames = sym::moverClassNames; // "AnimPropDef", "AnimObj", "Part", "Car", "Trailer", "Collision"
        constexpr uint32_t kMoverClassNameBytes = 0x19;
        constexpr uint32_t kMoverClassHashes = sym::moverClassHashes;
        constexpr int32_t kMoverClassCount = 6;

        constexpr uint32_t kRailBlocks = 0x100, kEdgeNodeBlocks = 0x200;
        constexpr float kPairDistance = 4.0f;          // a partner nearer than this
        // .sdata
        inline ee::Global<ee::Float, sym::g_shortSegment> g_shortSegment;  // 1e-5: no length on an axis
        inline ee::Global<ee::Float, sym::g_shortSegment2> g_shortSegment2; // 1e-5
        inline ee::Global<ee::Float, sym::g_parallel> g_parallel;      // 1e-5: the segments' determinant at most this
        inline ee::Global<ee::Float, sym::g_railsParallel> g_railsParallel; // 0.99: the edges' directions' dot at least this

        // A node's world matrix copied to `to`.
        void copyWorldMatrix(ee::Ptr<zge::Node> node, ee::Ptr<zge::Matrix> to)
        {
            *to = *zge::nodeMatrix(node, 1);
        }

        ee::Ptr<zge::TagTableHeader> tagTable(ee::Ptr<uint8_t> table)
        {
            return table.cast<zge::TagTableHeader>();
        }

    }

    namespace
    {
        int32_t visitPart(uint32_t visit, ee::Address part, uint32_t context) { return ee::game<int32_t>(visit, part, context); }
    }

    // forEachModelPart (model, visit(part, context), context): visit called on
    // each collision part of the model, from the last, while it returns
    // non-zero.
    void forEachModelPart(uint32_t model, uint32_t visit, uint32_t context)
    {
        const ee::Ptr<zge::GeoObject> object(model);
        for (int32_t part = zge::modelPartCount(object) - 1; part != -1; --part)
        {
            const ee::Ptr<zge::CollisionFrame> frame = zge::classModel(object, part);
            if (visitPart(visit, zge::frameCollisionPart(frame), context) == 0)
                return;
        }
    }
    READABLE_FUNCTION(forEachModelPart, Verified);

    // closestBetweenSegments (a, b, p, q, onAb *, onPq *, s *, t *): the points of the
    // segments ab and pq nearest each other: s along ab, t along pq (each
    // past an end clamped to an end: beyond 1 the segment's first, below 0
    // its second, as the original has them). 0 when either segment has no
    // length (every axis under 1e-5) or they are parallel; 1 with the points.
    int32_t closestBetweenSegments(ee::Ptr<zge::Vec4> a, ee::Ptr<zge::Vec4> b, ee::Ptr<zge::Vec4> p, ee::Ptr<zge::Vec4> q, ee::Ptr<zge::Vec4> onAb,
                                   ee::Ptr<zge::Vec4> onPq, ee::Ptr<ee::Float> s, ee::Ptr<ee::Float> t)
    {
        const auto noLength = [](const zge::Vec4 &along, ee::Float shortest)
        {
            return ee::abs(along.x) < shortest && ee::abs(along.y) < shortest && ee::abs(along.z) < shortest;
        };
        const zge::Vec4 fromP = *a - *p;
        const zge::Vec4 alongPq = *q - *p;
        if (noLength(alongPq, *g_shortSegment))
            return 0;
        const zge::Vec4 alongAb = *b - *a;
        if (noLength(alongAb, *g_shortSegment2))
            return 0;

        // The five products run on VU0 as well: what they leave there is what
        // the next vector operation finds. The sums below are the ones used.
        zge::dot(fromP, alongPq);
        zge::dot(alongPq, alongAb);
        zge::dot(fromP, alongAb);
        zge::dot(alongPq, alongPq);
        zge::dot(alongAb, alongAb);
        const ee::Float pqAb = alongPq.x * alongAb.x + alongPq.y * alongAb.y + alongPq.z * alongAb.z;
        const ee::Float pqPq = alongPq.x * alongPq.x + alongPq.y * alongPq.y + alongPq.z * alongPq.z;
        const ee::Float apAb = fromP.x * alongAb.x + fromP.y * alongAb.y + fromP.z * alongAb.z;
        const ee::Float apPq = fromP.x * alongPq.x + fromP.y * alongPq.y + fromP.z * alongPq.z;
        const ee::Float determinant = (alongAb.x * alongAb.x + alongAb.y * alongAb.y + alongAb.z * alongAb.z) * pqPq - pqAb * pqAb;
        if (ee::abs(determinant) < *g_parallel)
            return 0;
        const ee::Float alongS = (apPq * pqAb - apAb * pqPq) / determinant;
        *s = alongS;
        const ee::Float alongT = (apPq + pqAb * alongS) / pqPq;
        *t = alongT;

        // The point on ab.
        if (ee::Float(1.0f) < alongS)
            *onAb = *a;
        else if (alongS < ee::Float(0.0f))
            *onAb = *b;
        else
        {
            onAb->x = a->x + alongS * alongAb.x;
            onAb->y = a->y + alongS * alongAb.y;
            onAb->z = a->z + alongS * alongAb.z;
        }

        // The point on pq.
        if (ee::Float(1.0f) < alongT)
            *onPq = *p;
        else if (alongT < ee::Float(0.0f))
            *onPq = *q;
        else
        {
            onPq->x = p->x + alongT * alongPq.x;
            onPq->y = p->y + alongT * alongPq.y;
            onPq->z = p->z + alongT * alongPq.z;
        }
        return 1;
    }
    READABLE_FUNCTION(closestBetweenSegments, Verified);

    // compareRailEdges: the rail edge trees' order (a sorts before b): by
    // object, then edge, the higher first.
    int32_t compareRailEdges(ee::Ptr<RailEdgeKey> a, ee::Ptr<RailEdgeKey> b)
    {
        if (a->object == b->object)
            return b->edge < a->edge ? 1 : 0;
        return b->object < a->object ? 1 : 0;
    }
    READABLE_FUNCTION(compareRailEdges, Verified);

    // matchDoubleRailEdge (an edge B node, the edge A node being paired), the
    // B tree's walk: an unpaired edge of the same object whose direction
    // is A's (dot over 0.99) and whose middle is nearer A's than the best
    // so far becomes A's partner. Always 1 (the walk goes on).
    uint32_t matchDoubleRailEdge(ee::Ptr<RailEdgeNode> candidate, ee::Ptr<RailEdgeNode> edgeA)
    {
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Vec4[2]> ends;
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4> middle;
        ee::Local<ee::Ptr<uint16_t>> vertices;
        if (candidate->rail || candidate->object != edgeA->object)
            return 1;
        copyWorldMatrix(candidate->node, matrix);
        const ee::Ptr<zge::ZgePart> mesh = skater::collisionMeshOf(candidate->node);
        zge::elementEdgeVertices(mesh, static_cast<int32_t>(candidate->edge), vertices);
        for (uint32_t end = 0; end < 2; ++end)
        {
            zge::vertexPosition(mesh, (*vertices)[end], &ends[end]);
            ends[end] = zge::transformed(*matrix, ends[end]);
        }
        *direction = zge::unit(ends[0] - ends[1]);
        const ee::Ptr<DoubleRail> rail = edgeA->rail;
        if (*g_railsParallel < zge::dot(*direction, rail->direction))
        {
            zge::vec4Midpoint(&*middle, &ends[0], &ends[1]);
            *direction = *middle - rail->start;
            const ee::Float distance = lengthOf(direction);
            if (distance < rail->nearest)
            {
                rail->partner = candidate->edge;
                rail->nearest = distance;
            }
        }
        return 1;
    }
    READABLE_FUNCTION(matchDoubleRailEdge, Verified);

    // pairDoubleRailEdge (an edge A node), the A tree's walk: its double rail made
    // (its ends in the world, its direction and middle), the B edges
    // searched for its partner (matchDoubleRailEdge). Without one, the
    // edge's tag (its face-edge record's kind) is cleared; with one, the
    // partner's node gets the rail too, and the rail becomes the line
    // between the two edges' ends (start the middle of their first ends, end
    // of their second ends), its direction along it and its up across the
    // two. Always 1.
    //
    // The original keeps the addresses of the indices and of the vectors it
    // works in on its stack, next to them, and works the up's two sides in
    // the place the ends are read to. They stay there after it returns, and a
    // later function reads them: the matrix product behind bonePoint at the
    // museum's load takes the first of A's ends' address as a row's w. So
    // they are kept as the original keeps them.
    uint32_t pairDoubleRailEdge(ee::Ptr<RailEdgeNode> edgeA)
    {
        ee::Local<RailEdgeKey> key;
        ee::Local<zge::Matrix> matrix;
        ee::Local<zge::Vec4[2]> read; // an edge's ends as read, last the up's two sides
        ee::Local<zge::Vec4[2]> endsA;
        ee::Local<zge::Vec4[2]> endsB;
        ee::Local<ee::Ptr<uint16_t>> indices;
        ee::Local<ee::Ptr<ee::Ptr<uint16_t>>> indicesAddress;
        ee::Local<ee::Ptr<zge::Vec4>> readAddress;
        ee::Local<ee::Ptr<zge::Vec4>> firstEndA;
        ee::Local<ee::Ptr<zge::Vec4>> secondEndA;
        ee::Local<ee::Ptr<zge::Vec4>> secondEndB;
        if (edgeA->rail)
            return 1;
        *indicesAddress = &*indices;
        *readAddress = &read[0];

        // An edge's two ends in the world, into two vectors in a row. Gives
        // the mesh the edge is on.
        const auto worldEnds = [&](ee::Ptr<zge::Node> object, uint32_t edge, zge::Vec4 *ends)
        {
            copyWorldMatrix(object, matrix);
            const ee::Ptr<zge::ZgePart> mesh = skater::collisionMeshOf(object);
            zge::elementEdgeVertices(mesh, static_cast<int32_t>(edge), indices);
            for (uint32_t end = 0; end < 2; ++end)
            {
                zge::vertexPosition(mesh, (*indices)[end], &read[end]);
                ends[end] = zge::transformed(*matrix, read[end]);
            }
            return mesh;
        };

        // The rail, edge A's ends in it.
        const ee::Ptr<DoubleRail> rail(aggressive::poolTake(*g_doubleRailPool));
        edgeA->rail = rail;
        rail->object = edgeA->object;
        rail->edge = edgeA->edge;
        rail->partner = kNoPartner;
        rail->nearest = kPairDistance;
        const ee::Ptr<zge::ZgePart> mesh = worldEnds(edgeA->node, edgeA->edge, &rail->start);
        rail->direction = zge::unit(rail->start - rail->end);
        zge::vec4Midpoint(&rail->start, &rail->start, &rail->end);
        zge::searchTreeForEach(kDoubleRailEdgesB, kMatchDoubleRailEdge, edgeA);

        if (rail->partner == kNoPartner)
        {
            // No partner: the edge is no double rail edge.
            const ee::Ptr<zge::TagTableHeader> faceEdges = tagTable(mesh->faceEdges);
            if (faceEdges)
                zge::setTagByte(zge::tagElementRecord(faceEdges, rail->edge), 2, offsetof(zge::EdgeRecord, kind), 0);
            return 1;
        }

        // The partner's node gets the rail.
        key->edge = rail->partner;
        key->object = edgeA->object;
        const ee::Ptr<RailEdgeNode> edgeB = zge::searchTreeEntry<RailEdgeNode>(zge::searchTreeFind(kDoubleRailEdgesB, key));
        edgeB->rail = edgeA->rail;

        *firstEndA = &endsA[0];
        *secondEndA = &endsA[1];
        *secondEndB = &endsB[1];
        worldEnds(edgeA->node, edgeA->edge, *firstEndA);
        worldEnds(edgeB->node, edgeB->edge, &endsB[0]);

        // The middle line, its direction and up.
        zge::vec4Midpoint(&rail->start, *firstEndA, &endsB[0]);
        zge::vec4Midpoint(&rail->end, *secondEndA, *secondEndB);
        rail->direction = zge::unit(rail->end - rail->start);
        zge::Vec4 &alongA = (*readAddress)[0];
        zge::Vec4 &across = (*readAddress)[1];
        alongA = **firstEndA - **secondEndA;
        across = **firstEndA - **secondEndB;
        rail->up = zge::unit(zge::cross(alongA, across));
        return 1;
    }
    READABLE_FUNCTION_WAITS(pairDoubleRailEdge, Verified);

    // keepRailEdge: the B tree's walk at the end of the set-up: nothing.
    uint32_t keepRailEdge()
    {
        return 1;
    }
    READABLE_FUNCTION(keepRailEdge, Verified);

    // collectMeshRailEdges (a world mesh's node, the world's mesh walk): the double
    // rail edges of every node under it collected (collectDoubleRailEdges).
    uint32_t collectMeshRailEdges(uint32_t mesh)
    {
        zge::forEachNodeInSubtree(ee::Ptr<zge::Node>(mesh), kCollectDoubleRailEdges, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(collectMeshRailEdges, Verified);

    // setupModelCollision (a world model, the world's model walk): each collision
    // part set up (setupCollisionPart).
    uint32_t setupModelCollision(uint32_t model)
    {
        forEachModelPart(model, kSetupCollisionPart, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupModelCollision, Verified);

    namespace
    {
        namespace tail
        {
        uint32_t searchTreeForEach(ee::Ptr<zge::SearchTree> tree, zge::SearchTreeVisit visit, uint32_t context) { return ee::tailCall<uint32_t>(kSearchTreeForEach, tree, visit, context); }
        }
    }

    // setupLevelRails: the level's rails set up (see the top).
    uint32_t setupLevelRails()
    {
        // The mover classes' name hashes, from the last.
        const ee::Ptr<uint32_t> hashes(kMoverClassHashes);
        for (int32_t i = 0; i < kMoverClassCount; ++i)
            hashes[i] = zge::nameHash(ee::Ptr<const char>(kMoverClassNames + static_cast<uint32_t>(i) * kMoverClassNameBytes));

        // The pools.
        if (*g_doubleRailPool != 0)
        {
            aggressive::freePool(*g_doubleRailPool);
            *g_doubleRailPool = 0;
        }
        *g_doubleRailPool = aggressive::newPool(static_cast<uint32_t>(sizeof(DoubleRail)), kRailBlocks, 0);
        if (*g_railEdgeNodePool != 0)
        {
            aggressive::freePool(*g_railEdgeNodePool);
            *g_railEdgeNodePool = 0;
        }
        *g_railEdgeNodePool = aggressive::newPool(static_cast<uint32_t>(sizeof(RailEdgeNode)), kEdgeNodeBlocks, 0);

        // The trees, the world's edges, the pairs.
        zge::initSearchTree(kDoubleRailEdgesA, kCompareRailEdges);
        zge::initSearchTree(kDoubleRailEdgesB, kCompareRailEdges);
        zge::forEachWorldMesh(kCollectMeshRailEdges, 0);
        zge::forEachWorldObject(kSetupModelCollision, 0);
        zge::searchTreeForEach(kDoubleRailEdgesA, kPairDoubleRailEdge, 0);
        return tail::searchTreeForEach(kDoubleRailEdgesB, kKeepRailEdge, 0);
    }
    READABLE_FUNCTION_WAITS(setupLevelRails, Verified);

    // freeLevelRails: the rails' pools freed (the level's unload).
    void freeLevelRails()
    {
        if (*g_doubleRailPool != 0)
        {
            aggressive::freePool(*g_doubleRailPool);
            *g_doubleRailPool = 0;
        }
        if (*g_railEdgeNodePool != 0)
        {
            aggressive::freePool(*g_railEdgeNodePool);
            *g_railEdgeNodePool = 0;
        }
    }
    READABLE_FUNCTION_WAITS(freeLevelRails, Verified);

    // pairedRail (edge, object, tag): the double rail an edge of that tag
    // is part of (0: none; the grind search's).
    ee::Ptr<DoubleRail> pairedRail(ee::Address edge, ee::Address object, uint32_t tag)
    {
        ee::Local<uint32_t> key;
        ee::Local<uint32_t> objectSlot;
        *key = edge;
        *objectSlot = object;
        const ee::Ptr<zge::SearchTree> edges(tag == kDoubleRailEdgeA ? kDoubleRailEdgesA : kDoubleRailEdgesB);
        const ee::Ptr<RailEdgeNode> node = zge::searchTreeEntry<RailEdgeNode>(zge::searchTreeFind(edges, key));
        return node ? node->rail : ee::Ptr<DoubleRail>();
    }
    READABLE_FUNCTION_WAITS(pairedRail, Verified);
}
