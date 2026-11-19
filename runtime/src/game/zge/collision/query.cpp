// The collision query (collisionQuery): a shape against the world, with an
// optional cache of the last results (zge/collision/collision.h).
//
// A query builds a SphereQuery in its own stack frame, walks the world
// grid's cells that the shape can touch (each cell's nodes go through
// runQuery) and collects the faces hit in pool records: per
// node a HitNode (QueryState::hitNodePool) with its FaceHits. They end up
// listed in QueryResults at queryResultsAddress and are copied to the caller's
// results. A caller's QueryCache keeps a copy of the shape and the results,
// handed back as long as the shape stays close to the cached one.

#include "collision.h"
#include "query_trace.h"

#include "zge/text/cstring.h"
#include "zge/memory/pool.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/matrix.h"
#include "zge/math/vector.h"
#include "zge/native/host.h"
#include "zge/debug/debug.h"

#include <cstring>
#include "sym/zge/collision/query.cpp.h"

namespace zge
{
    void clearQueryCache(ee::Ptr<QueryCache> cache);
    void drawQueryHits(ee::Ptr<QueryResults> results, ee::Ptr<uint8_t> colour);
    void drawHitFaces(ee::Ptr<HitNode> hitNode, ee::Ptr<uint8_t> colour);

    namespace
    {
        // The game functions a query calls.
        constexpr uint32_t kLibcMemset = sym::memset;      // (dst, byte, bytes)
        constexpr uint32_t kPoolReset = sym::poolReset;       // (pool): frees every record
        constexpr uint32_t kRotationFromTo = sym::rotationFromTo;  // (matrix, from, to): turns `from` onto `to`
        constexpr uint32_t kBoxExtentAlong = sym::boxExtentAlong;  // (centre, half size, matrix, axis, *centre on axis, *extent on axis)
        constexpr uint32_t kRunQuery = sym::runQuery;        // the cell walkers' visitor (zge/scene/query.h runQuery)
        constexpr uint32_t kForEachCellInBox = sym::forEachCellInBox;        // (visit, box, query)
        constexpr uint32_t kForEachCellInOrientedBox = sym::forEachCellInOrientedBox; // (visit, local box, box to world, query)
        // Debug drawing of the faces hit.
        constexpr uint32_t kNodeMatrix = sym::nodeMatrix;        // (node, 1) -> its world matrix
        constexpr uint32_t kFaceVertexIndices = sym::faceVertices; // (part, face, uint16_t **indices)
        constexpr uint32_t kFaceNormal = sym::faceNormal;        // (part, face, Vec4 *normal)
        constexpr uint32_t kPartVertex = sym::vertexPosition;        // (part, vertex, Vec4 *position)

        // The cache's limits when the query brings none (kQueryCacheLimits):
        // how far the shape may move, how much a sphere's radius
        // or a swept box's width may change, and how old the results may be.
        inline ee::Global<ee::Float, sym::g_cacheMoveLimit> g_cacheMoveLimit;
        constexpr float kCacheSizeLimit = 0.25f;
        constexpr uint32_t kCacheMaxAge = 100u;
        // Hit faces are drawn this far out along their normal, so they show
        // over the surface.
        inline ee::Global<ee::Float, sym::g_hitFaceLift> g_hitFaceLift;

        // What collisionQuery hands out from its stack: the query its callees
        // get, and the swept box's oriented box and what makes it.
        struct QueryLocals
        {
            SphereQuery query;                         // +0x000
            // (ZGE2's query record is laid out otherwise and its collisionQuery has a frame of its own: this
            // frame is ZGE3's and ZGE4's, so its gap and the offsets below are theirs.)
            uint8_t afterQuery[game::readable::kIsDm2 ? 0x10 : 0x70 - sizeof(SphereQuery)];
            QuerySweptBox sweptBox;                    // +0x070 kind 3's shape, as the query's shape
            QueryBox localBox;                         // +0x0A0 the oriented box in its own space
            Matrix boxToWorld;                         // +0x0C0 its rotation and position
            ee::Float worldCentre[4];                  // +0x100 the box's centre on the world axes
            ee::Float worldExtent[4];                  // +0x110 its half size on the world axes
            uint8_t unused120[0x10];                   // +0x120
            Vec4 scratch;                              // +0x130 vec3Length's argument
            Vec4 sweep;                                // +0x140
            Vec4 halfSweep;                            // +0x150
            Vec4 up;                                   // +0x160
            Vec4 boxCentre;                            // +0x170
            Vec4 boxHalfSize;                          // +0x180
            Vec4 worldAxis;                            // +0x190
        };
        static_assert(game::readable::kIsDm2 ||
                      (offsetof(QueryLocals, sweptBox) == 0x70 && offsetof(QueryLocals, localBox) == 0xA0 && offsetof(QueryLocals, boxToWorld) == 0xC0 &&
                       offsetof(QueryLocals, scratch) == 0x130 && offsetof(QueryLocals, worldAxis) == 0x190));

        // An entry of the list of nodes a segment hit.
        struct HitListEntry
        {
            int32_t key;
            ee::Ptr<QueryHit> hit;
        };

        QueryState &state() { return *g_queryState; }
        QueryResults &found() { return *ee::at<QueryResults>(kQueryResultsAddress); }
        ee::Ptr<QueryResults> foundResults() { return ee::Ptr<QueryResults>(kQueryResultsAddress); }
        ee::Ptr<QueryResults> cachedResults(ee::Ptr<QueryCache> cache) { return &cache->results; }
        Vec4 *cachedShape(QueryCache &cache) { return reinterpret_cast<Vec4 *>(cache.shape); }

        namespace // the game functions below call: ee/plain.h
        {
            void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kLibcMemset, m, c, n); }
            void rotationFromTo(ee::Address matrix, ee::Address from, ee::Address to) { ee::game(kRotationFromTo, matrix, from, to); }
            void boxExtentAlong(ee::Address boxCentre, ee::Address boxHalfSize, ee::Address boxToWorld, ee::Address worldAxis, ee::Address centreOut, ee::Address extentOut) { ee::game(kBoxExtentAlong, boxCentre, boxHalfSize, boxToWorld, worldAxis, centreOut, extentOut); }
        }

        // How far a point of the shape is from the cached one.
        ee::Float distanceMoved(Vec4 &scratch, const Vec4 &cached, const Vec4 &now)
        {
            scratch = cached - now;
            return vec3Length(&scratch.xyz());
        }

        // Whether the cache's results still answer this query: the same kind
        // and flags, results young enough, and a shape that moved (and, for a
        // sphere or swept box, changed size) less than the limits.
        bool cacheFits(QueryLocals &locals, uint32_t kind, const Vec4 *shape, uint32_t flags, QueryCache &cache)
        {
            ee::Float moveLimit;
            ee::Float sizeLimit;
            uint32_t maxAge = kCacheMaxAge;
            if (flags & kQueryCacheLimits)
            {
                moveLimit = cache.moveLimit;
                sizeLimit = cache.sizeLimit;
                maxAge = cache.maxAge;
            }
            else
            {
                moveLimit = *g_cacheMoveLimit;
                sizeLimit = kCacheSizeLimit;
            }
            if (kind != cache.kind)
                return false;
            if (cache.hasResults && collisionClock() - cache.time > maxAge)
                return false;
            if ((flags & ~kQueryRefresh) != cache.flags)
                return false;

            const Vec4 *cached = cachedShape(cache);
            const auto pointFits = [&](int point) { return distanceMoved(locals.scratch, cached[point], shape[point]) <= moveLimit; };
            const auto sizeFits = [&](int point) { return !(sizeLimit < ee::abs(cached[point].x - shape[point].x)); };
            switch (static_cast<CollisionKind>(kind))
            {
            case CollisionKind::Sphere: // the centre, then the radius
                return pointFits(0) && sizeFits(1);
            case CollisionKind::Segment: // both ends
            case CollisionKind::Box:     // both corners
                return pointFits(0) && !(moveLimit < distanceMoved(locals.scratch, cached[1], shape[1]));
            case CollisionKind::SweptBox: // both ends, then the half width
                return pointFits(0) && !(moveLimit < distanceMoved(locals.scratch, cached[1], shape[1])) && sizeFits(2);
            default:
                return true;
            }
        }

        // The size of a kind's shape (what the cache keeps of it).
        uint32_t shapeBytes(uint32_t kind)
        {
            return static_cast<CollisionKind>(kind) == CollisionKind::SweptBox ? sizeof(QuerySweptBox) : 0x20u;
        }

        // The debug draw colour the flags pick, in a free slot of the stack:
        // red, green, blue from flags 0x400, 0x800, 0x1000, opaque.
        ee::Ptr<uint8_t> drawColour(Vec4 &slot, uint32_t flags)
        {
            uint8_t *colour = reinterpret_cast<uint8_t *>(&slot);
            colour[0] = (flags & 0x0400u) ? 0xFF : 0x00;
            colour[1] = (flags & 0x0800u) ? 0xFF : 0x00;
            colour[2] = (flags & 0x1000u) ? 0xFF : 0x00;
            colour[3] = 0xFF;
            return colour;
        }

        // Kind 3: the swept box as an oriented box: z along the sweep from
        // its start, x and y +-halfWidth, z 0 .. length; then its centre and
        // half size on the world axes (boxExtentAlong per axis), for the cell
        // walk.
        void buildOrientedBox(QueryLocals &locals, ee::Ptr<Vec4> shape)
        {
            constexpr ee::Float kOne = 1.0f;
            constexpr ee::Float kZero = 0.0f;
            constexpr ee::Float kHalf = 0.5f;

            memset(&locals.up, 0, sizeof(Vec4));
            locals.up.z = kOne;
            memoryCopy(&locals.sweptBox, shape, sizeof(QuerySweptBox));
            locals.query.segment = reinterpret_cast<QuerySegment *>(&locals.sweptBox);

            // The sweep, its length and direction.
            locals.sweep = shape[1] - shape[0];
            ee::Float length;
            locals.scratch = unit(locals.sweep, length);
            const Vec4 &direction = locals.scratch;

            // The box's rotation turns z (up here) onto the sweep. (It leaves
            // VU0 as it was: the half sweep's w stays the direction's 0.)
            Matrix &boxToWorld = locals.boxToWorld;
            matrixIdentity(&boxToWorld);
            rotationFromTo(&boxToWorld, &locals.up, &locals.scratch);

            const ee::Float halfLength = length * kHalf;
            boxToWorld.m[2][3] = kZero;
            boxToWorld.m[1][3] = kZero;
            boxToWorld.m[0][3] = kZero;
            boxToWorld.m[3][3] = kOne;
            locals.halfSweep = direction * halfLength;
            // The box starts half its length back from the sweep's middle
            // (the z of the middle only, as the game has it).
            Vec4 &middle = locals.sweep;
            middle = locals.halfSweep + shape[0];
            middle.z = middle.z - halfLength;
            Vec4 &position = matrixRow(boxToWorld, 3);
            position = middle; // w too: the vector is copied whole

            const ee::Float halfWidth = shape[2].x;
            QueryBox &local = locals.localBox;
            local.min.x = -halfWidth;
            local.min.y = -halfWidth;
            local.min.z = kZero;
            local.max.x = halfWidth;
            local.max.y = halfWidth;
            local.max.z = length;

            // The local box's centre and half size, both moved by the box's
            // position (FPU, in the compiled code's order).
            const ee::Float centreOffset = (-halfWidth + halfWidth) * kHalf;
            Vec4 &centre = locals.boxCentre;
            Vec4 &halfSize = locals.boxHalfSize;
            centre.x = centreOffset + position.x;
            centre.y = centreOffset + position.y;
            centre.z = halfLength + position.z;
            halfSize.x = (halfWidth - centre.x) + position.x;
            halfSize.y = (halfWidth - centre.y) + position.y;
            halfSize.z = (length - centre.z) + position.z;

            Vec4 &axis = locals.worldAxis;
            for (int i = 0; i < 3; ++i)
            {
                axis.x = i == 0 ? kOne : kZero;
                axis.y = i == 1 ? kOne : kZero;
                axis.z = i == 2 ? kOne : kZero;
                boxExtentAlong(&centre, &halfSize, &boxToWorld, &axis, &locals.worldCentre[i], &locals.worldExtent[i]);
            }
        }

        // Kind 1: the segment's bounds in the query (a sphere around it and
        // its box, doubled), the cells along it, then each node it hit,
        // nearest first, against the node's faces.
        void runSegmentQuery(ee::Ptr<Vec4> shape, SphereQuery &query)
        {
            constexpr ee::Float kHalf = 0.5f;
            query.boxSum = shape[1] + shape[0];
            query.boxSize = shape[1] - shape[0];
            query.radius = vec3Length(&query.boxSize.xyz()) * kHalf;
            query.boxSize = abs(query.boxSize);
            query.centre = query.boxSum * kHalf;

            forEachCellOnSegment(kRunQuery, query.segment, &query);
            for (ee::Ptr<HitListEntry> entry = priorityQueuePopAs<HitListEntry>(kQueryHitList); entry; entry = priorityQueuePopAs<HitListEntry>(kQueryHitList))
            {
                const ee::Ptr<QueryHit> hit = entry->hit;
                query.hit = hit;
                testHitNodeFaces(hit->node, &query);
                poolFree(ee::Ptr<Pool>(state().queryHitPool), hit);
            }
        }

        // The cache keeps copies of the HitNodes (in its own pool) and of
        // their FaceHits. Every copy's faces start at the start of the
        // cache's face buffer (the game's offset is the face index of a loop
        // that has not started), so with several nodes hit they overwrite
        // each other there.
        void keepInCache(QueryCache &cache)
        {
            cache.faces = poolAllocAs<FaceHit>(state().cacheFaceBufferPool);
            for (int32_t i = 0; i < found().count; ++i)
            {
                const ee::Ptr<HitNode> copy = poolAllocAs<HitNode>(state().cacheHitNodePool);
                const ee::Ptr<HitNode> hit = found().nodes[i];
                cache.results.nodes[i] = copy;
                copy->faceCount = hit->faceCount;
                copy->node = hit->node;
                copy->part = hit->part;
                copy->faces = cache.faces;
                for (int32_t face = 0; face < static_cast<int32_t>(copy->faceCount); ++face)
                    memoryCopy(copy->faces + face, hit->faces + face, sizeof(FaceHit));
            }
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void poolReset(uint32_t pool) { ee::game(kPoolReset, pool); }
        void forEachCellInBox(uint32_t visit, ee::Ptr<Vec4> box, ee::Address query) { ee::game(kForEachCellInBox, visit, box, query); }
        void forEachCellInOrientedBox(uint32_t runQuery, ee::Address localBox, ee::Address boxToWorld, ee::Address query) { ee::game(kForEachCellInOrientedBox, runQuery, localBox, boxToWorld, query); }
    }

    // The query itself (collisionQuery below adds the trace).
    static bool queryOnce(uint32_t kind, ee::Ptr<Vec4> shape, uint32_t flags, uint32_t filter, uint32_t filterContext,
                        ee::Ptr<ee::Ptr<Node>> nodes, ee::Ptr<QueryResults> results, ee::Ptr<QueryCache> cache)
    {
        ee::Local<QueryLocals> locals;
        SphereQuery &query = locals->query;

        // The cache: dropped when it no longer fits (or the caller asks for a
        // fresh query), started again for this shape, and its results handed
        // back when it has some.
        if (cache)
        {
            if (cache->valid && !cacheFits(*locals, kind, shape.get(), flags, *cache))
                flags |= kQueryRefresh;
            if (flags & kQueryRefresh)
                cache->valid = 0;
            if (!cache->valid)
            {
                clearQueryCache(cache);
                cache->kind = kind;
                cache->valid = 1;
                cache->flags = flags & ~kQueryRefresh;
                cache->hasResults = 0;
                if (kind <= static_cast<uint32_t>(CollisionKind::SweptBox))
                    memoryCopy(&cache->shape[0], shape, shapeBytes(kind));
            }
            if (cache->hasResults)
            {
                if (results)
                    *results = cache->results;
                if (flags & kQueryDrawHitsMask)
                    drawQueryHits(cachedResults(cache), drawColour(locals->scratch, flags));
                return cache->results.count != 0;
            }
        }

        // A fresh query: empty pools, no HitNode yet, a new FaceHit buffer.
        poolReset(state().hitNodePool);
        poolReset(state().faceBufferPool);
        state().hitNode = ee::Ptr<HitNode>();
        state().facesBuffer = poolAllocAs<FaceHit>(state().faceBufferPool);
        state().nearest = 10.0f;
        state().faceCount = 0;
        query.kind = kind;
        query.segment = shape.cast<QuerySegment>();
        query.flags = flags;
        query.filter = filter;
        query.filterContext = filterContext;
        query.results = results;
        query.nodes = nodes;
        query.stamp = *g_queryStamp;

        // Walk the cells the shape touches; each cell's nodes go through
        // runQuery, which tests a sphere or box against the node's faces at
        // once and only lists the nodes a segment hits.
        switch (static_cast<CollisionKind>(kind))
        {
        case CollisionKind::Segment:
            runSegmentQuery(shape, query);
            break;
        case CollisionKind::Sphere:
            forEachCellInSphere(kRunQuery, shape.cast<QuerySphere>(), &query);
            break;
        case CollisionKind::Box:
            forEachCellInBox(kRunQuery, shape, &query);
            break;
        case CollisionKind::SweptBox:
            buildOrientedBox(*locals, shape);
            forEachCellInOrientedBox(kRunQuery, &locals->localBox, &locals->boxToWorld, &query);
            break;
        default:
            break;
        }
        *g_queryStamp = static_cast<uint8_t>(*g_queryStamp + 1);

        if (!state().hitNode)
        {
            // Nothing hit: the cache keeps that too (the caller's results are
            // left as they were).
            if (cache && cache->valid)
            {
                cache->results.count = 0;
                cache->hasResults = 1;
                cache->time = collisionClock();
            }
            return false;
        }

        // The HitNodes in the pool, in its order, into QueryResults.
        found().count = poolCount(ee::Ptr<Pool>(state().hitNodePool));
        uint32_t listed = 0;
        for (ee::Ptr<ListNode> link = firstPoolLink(ee::Ptr<Pool>(state().hitNodePool)); link; link = nextPoolLink(link))
            found().nodes[listed++] = poolRecordAs<HitNode>(link);

        ee::Ptr<QueryResults> kept = foundResults();
        if (cache && cache->valid)
        {
            // The results kept in the cache, and handed back from there.
            kept = cachedResults(cache);
            cache->hasResults = 1;
            cache->results.count = found().count;
            cache->time = collisionClock();
            keepInCache(*cache);
        }
        if (results)
            *results = *kept;
        if (flags & kQueryDrawHitsMask)
            drawQueryHits(kept, drawColour(locals->boxCentre, flags));
        return true;
    }

    // collisionQuery: the query, through the host's trace and timer when
    // they are on (query_trace.h).
    bool collisionQuery(uint32_t kind, ee::Address shape, uint32_t flags, uint32_t filter, ee::Address filterContext,
                        ee::Address nodes, ee::Ptr<QueryResults> results, ee::Ptr<QueryCache> cache)
    {
        const auto query = [&] {
            return queryOnce(kind, ee::Ptr<Vec4>(shape), flags, filter, filterContext, ee::Ptr<ee::Ptr<Node>>(nodes), results, cache);
        };
        if (!native::hostExtrasOn() || !collisionQueryTraced())
            return query();
        return traceCollisionQuery(kind, flags, query);
    }

    // collisionQueryUncached: ZGE2's collisionQuery. Seven arguments (no cache) and
    // two kinds, a sphere (0) and a segment (1); the hits go to the query state's own
    // HitNode and FaceHit arrays, and the answer is ZGE2's 8-byte QueryResults
    // {count, &hitNodes[0]} (queryResultsAddress), copied to the caller's. Flags
    // 0x200, 0x400, 0x800 draw the shape in red, green, blue (opaque); 0x40, 0x80,
    // 0x100 draw the faces hit. (ZGE3 added the cache, kinds 2 and 3 and the
    // pooled records.)
    int32_t collisionQueryUncached(uint32_t kind, ee::Address shapeAddress, uint32_t flags, uint32_t filter, ee::Address filterContext,
                                   ee::Address nodes, ee::Ptr<HitNodeTable> results)
    {
        ee::Local<SphereQuery> query;
        ee::Local<Vec4> shapeColour;
        ee::Local<Vec4> hitsColour;
        QueryState &queries = state();
        const ee::Ptr<Vec4> shape(shapeAddress);
        HitNodeTable &found = *ee::at<HitNodeTable>(kQueryResultsAddress);

        const ee::Ptr<uint8_t> shapeRgba = drawColour(*shapeColour, flags << 1); // (0x200, 0x400, 0x800: a bit below ZGE3's draw bits)
        queries.nearest = 10.0f;
        queries.hitCount = 0;
        queries.faceCount = 0;
        query->kind = kind;
        query->segment = shape.cast<QuerySegment>();
        query->flags = flags;
        query->filter = filter;
        query->filterContext = filterContext;
        query->results = results.cast<QueryResults>();
        query->nodes = ee::Ptr<ee::Ptr<Node>>(nodes);
        query->stamp = *g_queryStamp;

        constexpr uint32_t kDrawShape = 0xE00;
        constexpr uint32_t kDrawHits = 0x1C0;
        if (kind == static_cast<uint32_t>(CollisionKind::Sphere))
        {
            if (flags & kDrawShape)
                addDebugPoint(shape, shapeRgba, 0, 0, shape.cast<QuerySphere>()->radius);
        }
        else if (kind == static_cast<uint32_t>(CollisionKind::Segment))
        {
            if (flags & kDrawShape)
                addDebugLine(shape, shape + 1, shapeRgba, 0);
        }

        // The cells the shape touches (runQuery tests each cell's nodes: a sphere
        // against their faces at once, a segment listing the nodes hit, which are
        // then tested nearest first).
        if (kind == static_cast<uint32_t>(CollisionKind::Segment))
        {
            forEachCellOnSegment(kRunQuery, shape.cast<QuerySegment>(), query);
            for (ee::Ptr<HitListEntry> entry = priorityQueuePopAs<HitListEntry>(kQueryHitList); entry; entry = priorityQueuePopAs<HitListEntry>(kQueryHitList))
            {
                const ee::Ptr<QueryHit> hit = entry->hit;
                query->hit = hit;
                testHitNodeFaces(hit->node, query);
                poolFree(ee::Ptr<Pool>(queries.queryHitPool), hit);
            }
        }
        else if (kind == static_cast<uint32_t>(CollisionKind::Sphere))
            forEachCellInSphere(kRunQuery, shape.cast<QuerySphere>(), query);
        *g_queryStamp = static_cast<uint8_t>(*g_queryStamp + 1);

        if (queries.hitCount == 0)
            return 0;
        found.count = queries.hitCount;
        found.nodes = &queries.hitNodes[0];
        if (results)
            *results = found;
        if (flags & kDrawHits)
            drawQueryHits(foundResults(), drawColour(*hitsColour, flags << 4)); // (0x40, 0x80, 0x100: four bits below ZGE3's)
        return 1;
    }
    READABLE_FUNCTION_OWN(collisionQuery, Verified, dm2, collisionQueryUncached); // kinds 0 and 1, no cache: what the game calls

    // resetQueryCache (no caller): a cache made ready for its first query.
    void resetQueryCache(ee::Ptr<QueryCache> cache)
    {
        cache->results.count = 0;
        cache->valid = 0;
        cache->hasResults = 0;
    }
    READABLE_FUNCTION(resetQueryCache, Draft);

    void clearQueryCache(ee::Ptr<QueryCache> cache)
    {
        int32_t i = 0;
        if (cache->results.count > 0)
        {
            for (;;)
            {
                poolFree(ee::Ptr<Pool>(state().cacheHitNodePool), cache->results.nodes[i]);
                cache->results.nodes[i] = ee::Ptr<HitNode>();
                ++i;
                if (!(i < cache->results.count))
                    break;
            }
        }
        poolReset(state().cacheFaceBufferPool);
        cache->results.count = 0;
        cache->valid = 0;
        cache->hasResults = 0;
    }
    READABLE_FUNCTION(clearQueryCache, Verified); // no caller in the game passes a cache

    void drawQueryHits(ee::Ptr<QueryResults> results, ee::Ptr<uint8_t> colour)
    {
        uint32_t i = 0;
        if (static_cast<uint32_t>(results->count) != 0)
        {
            for (;;)
            {
                drawHitFaces(results->nodes[i], colour);
                ++i;
                if (!(i < static_cast<uint32_t>(results->count)))
                    break;
            }
        }
    }
    READABLE_FUNCTION(drawQueryHits, Verified); // no caller in the game sets flags 0x1C00

    void drawHitFaces(ee::Ptr<HitNode> hitNode, ee::Ptr<uint8_t> colour)
    {
        ee::Local<Vec4[3]> world;
        ee::Local<Vec4> normal;
        ee::Local<Vec4[3]> corners;
        ee::Local<ee::Ptr<uint16_t>> indices;

        const ee::Ptr<Matrix> toWorld = nodeMatrix(hitNode->node, 1);
        for (uint32_t i = 0; i < hitNode->faceCount; ++i)
        {
            const ee::Ptr<FaceHit> faceHit = hitNode->faces + i;
            faceVertices(hitNode->part, faceHit->face, indices);
            faceNormal(hitNode->part, faceHit->face, normal);
            for (int k = 0; k < 3; ++k)
                vertexPosition(hitNode->part, static_cast<uint32_t>((*indices)[k]), &corners[k]);

            // Each corner moved out along the normal, then to world space.
            const ee::Float lift = *g_hitFaceLift;
            for (int k = 0; k < 3; ++k)
            {
                const Vec4 raised = normal * lift;
                const Vec4 lifted = raised + corners[k];
                world[k] = transformed(*toWorld, lifted);
            }
            drawTriangle(&world[0], &world[1], &world[2], colour, 0);
        }
    }
    READABLE_FUNCTION(drawHitFaces, Verified);

    namespace
    {
        constexpr uint32_t kCollisionQuery = sym::collisionQuery; // collisionQuery (zge collisionQuery (by address: a game override hooks it); readable: collisionQuery, Verified; hooked by a game override: called by address)
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(collisionQueryint32_t, ee::game<int32_t>, kCollisionQuery);
        }
    }

    uint32_t initQueries()
    {
        // ZGE4 doubled the leaf and hit lists and the query hits a block.
        constexpr uint32_t kLeafListCapacity = game::readable::perGame(0x100u, 0x100u, 0x200u);
        constexpr uint32_t kHitListCapacity = game::readable::perGame(0x80u, 0x80u, 0x100u);
        constexpr uint32_t kQueryHitsABlock = game::readable::perGame(0x80u, 0x80u, 0x100u);
        zge::memoryFill(kQueryResults, 0, kQueryStateBytes);
        QueryState &queries = state();
        queries.ready = 1;
        initPriorityQueue(&queries.leafList, 0, kLeafListCapacity);
        initPriorityQueue(&queries.hitList, 0, kHitListCapacity);
        queries.queryHitPool = createPool(0x50, kQueryHitsABlock, 0);
        if constexpr (!game::readable::kIsDm2) // (ZGE2's HitNodes and FaceHits are arrays of the state; no cache)
        {
            queries.hitNodePool = createPool(0x10, 0x80, 0);
            queries.faceBufferPool = createPool(0x6000, 0x00040001u, 0);
            queries.cacheHitNodePool = createPool(0x10, 0x80, 0);
            queries.cacheFaceBufferPool = createPool(0x6000, 0x00040001u, 0);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(initQueries, Verified);

    uint32_t shutdownCollisionQueries()
    {
        QueryState &queries = state();
        queries.ready = 0;
        freePriorityQueue(&queries.leafList);
        freePriorityQueue(&queries.hitList);
        destroyPool(ee::Ptr<Pool>(queries.queryHitPool));
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has the one pool)
        {
            destroyPool(ee::Ptr<Pool>(queries.hitNodePool));
            destroyPool(ee::Ptr<Pool>(queries.faceBufferPool));
            destroyPool(ee::Ptr<Pool>(queries.cacheHitNodePool));
            destroyPool(ee::Ptr<Pool>(queries.cacheFaceBufferPool));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownCollisionQueries, Verified);

}
