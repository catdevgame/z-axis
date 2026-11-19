#include "query.h"

#include "ee/dispatch.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/vu0.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/collision/collision.h"
#include "zge/debug/debug.h"
#include "zge/container/array.h"
#include "zge/memory/pool.h"
#include "sce/libgcc.h"
#include "ee/readable_waits.h"
#include "zge/container/heap.h"

#include <cstring>

#include <vector>
#include "sym/zge/scene/query.cpp.h"

namespace zge
{
    using NodeVisitor = ee::FunctionPtr<int32_t(ee::Ptr<Node> node, uint32_t context)>;

    int32_t forEachNodeInSubtree(ee::Ptr<Node> root, uint32_t visitFunction, ee::Address context)
    {
        const NodeVisitor visit(visitFunction);
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 recurses, over child and sibling alike (so it walks the root's next siblings too): each
            // subtree a call of this function, its visits in that call's frame.
            if (visit(root, context) == 0)
                return 0;
            if (root->firstChild && forEachNodeInSubtree(root->firstChild, visitFunction, context) == 0)
                return 0;
            if (root->nextSibling)
                return forEachNodeInSubtree(root->nextSibling, visitFunction, context) != 0 ? 1 : 0;
            return 1;
        }
        // The original keeps the pending nodes in its stack frame; that
        // memory is dead after the return, so a host vector does the same.
        std::vector<ee::Ptr<Node>> pending;
        for (ee::Ptr<Node> node = root; node;)
        {
            if (visit(node, context) == 0)
                return 0;
            // The root's own siblings are not part of its subtree.
            if (node != root)
            {
                if (node->nextSibling)
                    pending.push_back(node->nextSibling);
            }
            if (node->firstChild)
                pending.push_back(node->firstChild);
            // (The original pops its stack's bottom marker, 0, when it is empty.)
            if (pending.empty())
                break;
            node = pending.back();
            pending.pop_back();
        }
        return 1;
    }
    READABLE_FUNCTION(forEachNodeInSubtree, Verified);

    int32_t sphereQueryVisit(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query)
    {
        // A node reached twice in one query (several subtrees can lead to it)
        // is tested once.
        if (node->queryStamp == query->stamp)
            return 1;
        node->queryStamp = query->stamp;
        if (node->flags & kNodeTreeIsOneUnit)
            sphereQueryTestNode(node, query);
        else
            forEachNodeInSubtree(node, kTestNodeAgainstSphere, query);
        return 1;
    }
    READABLE_FUNCTION(sphereQueryVisit, Verified);

    // clipSegmentToBox (zge/math/plane.cpp): the segment clipped to the box, in place.
    uint32_t clipSegmentToBox(ee::Ptr<Vec4> start, ee::Ptr<Vec4> end, ee::Ptr<Box> box);

    namespace
    {
        ee::Float vec3Length(const Vec4 &v)
        {
            return zge::vec3Length(&v.xyz());
        }

        // The bounding spheres, then the boxes (both doubled in the query:
        // |sum - sum'| <= size + size' on every axis).
        bool overlaps(const Node &node, const SphereQuery &query)
        {
            const ee::Float reach = query.radius + node.radius;
            const Vec4 apart = node.sphereCentre - query.centre;
            const ee::Float distance2 = squaredLength(apart);
            if (reach * reach < distance2)
                return false;

            const Vec4 between = query.boxSum - (node.boxMax + node.boxMin);
            const Vec4 room = query.boxSize + (node.boxMax - node.boxMin);
            const Vec4 gap = abs(between);
            return gap.x <= room.x && gap.y <= room.y && gap.z <= room.z;
        }

        // Whether the query's null-ended node list holds the node. (The
        // original always walks the whole list.)
        bool listed(const SphereQuery &query, const Node *node)
        {
            bool found = false;
            for (ee::Ptr<ee::Ptr<Node>> entry = query.nodes; *entry; ++entry)
                if (entry->get() == node)
                    found = true;
            return found;
        }

        bool wanted(const Node &node, const SphereQuery &query)
        {
            if (!(query.flags & (kQueryExcludeListed | kQueryOnlyListed)))
                return true;
            const bool found = listed(query, &node);
            if ((query.flags & kQueryExcludeListed) && found)
                return false;
            return !(query.flags & kQueryOnlyListed) || found;
        }
    }

    // Tests one node against a query (see SphereQuery): a node hit gets a
    // QueryHit, sorted into the results by how far along the segment it starts.
    // Always 1 (go on).
    int32_t sphereQueryTestNode(Node *node, SphereQuery *query)
    {
        if (!(node->flags & kNodeCollides) || !overlaps(*node, *query) || !wanted(*node, *query))
            return 1;

        QueryHit *hit = ee::Ptr<QueryHit>(poolAlloc(ee::Ptr<Pool>(*g_queryHitPool))).get();
        const QuerySegment &segment = *query->segment;
        hit->segment = segment;
        hit->clipped = segment;
        clipSegmentToBox(&hit->clipped.start, &hit->clipped.end, &node->box());

        ee::Local<Vec4> span;
        span = hit->segment.end - hit->segment.start;
        const ee::Float length = vec3Length(span);
        span = hit->clipped.start - hit->segment.start;
        const ee::Float along = vec3Length(span);

        hit->node = node;
        hit->fraction = along / length;
        // Sorted by hundredths of the segment.
        priorityQueueInsert(ee::toInt(hit->fraction * 100.0f), ee::Address(hit), ee::Ptr<PriorityQueue>(kQueryHitList));
        return 1;
    }

    namespace
    {
        // ZGE2's room around a node's box in segmentQueryTestNode.
        constexpr float kSegmentBoxPadding = 0.01f;
    }

    // segmentQueryTestNode: ZGE2's sphereQueryTestNode, a segment against the node's box: the query's node lists
    // first, then the node's stamp (each node once a query) and its collides bit (ZGE2's in the flags' upper word);
    // the box widened by kSegmentBoxPadding on every axis goes against the query's shape (shapeBoundsOverlapBox),
    // with no sphere test. A hit is made as ZGE3's (clipped to the widened box), its key converted unsigned.
    int32_t segmentQueryTestNode(Node *node, SphereQuery *query)
    {
        const uint32_t flags = query->flags;
        if (flags & (kQueryExcludeListed | kQueryOnlyListed))
        {
            const bool found = listed(*query, node);
            if ((flags & kQueryExcludeListed) && found)
                return 1;
            if ((flags & kQueryOnlyListed) && !found)
                return 1;
        }
        if (node->queryStamp == query->stamp)
            return 1;
        if (!(node->flagsHigh & kNodeCollides))
            return 1;
        ee::Local<Vec4[2]> box;
        const ee::Float padding = ee::Float(kSegmentBoxPadding);
        box[0] = node->boxMin;
        box[1] = node->boxMax;
        box[0].x = box[0].x - padding;
        box[0].y = box[0].y - padding;
        box[0].z = box[0].z - padding;
        box[1].x = box[1].x + padding;
        box[1].y = box[1].y + padding;
        box[1].z = box[1].z + padding;
        node->queryStamp = query->stamp;
        if (!shapeBoundsOverlapBox(query->kind, query->segment, ee::Address(&box[0])))
            return 1;

        QueryHit *hit = ee::Ptr<QueryHit>(poolAlloc(ee::Ptr<Pool>(*g_queryHitPool))).get();
        const QuerySegment &segment = *query->segment;
        hit->segment = segment;
        hit->clipped = segment;
        clipSegmentToBox(&hit->clipped.start, &hit->clipped.end, ee::Ptr<Vec4>(box).cast<Box>());

        ee::Local<Vec4> span;
        span = hit->segment.end - hit->segment.start;
        const ee::Float length = vec3Length(span);
        span = hit->clipped.start - hit->segment.start;
        const ee::Float along = vec3Length(span);

        hit->node = node;
        hit->fraction = along / length;
        priorityQueueInsert(sce::libgcc::fptoui(hit->fraction * 100.0f), ee::Address(hit), ee::Ptr<PriorityQueue>(kQueryHitList));
        return 1;
    }
    READABLE_FUNCTION_OWN(sphereQueryTestNode, Verified, dm2, segmentQueryTestNode);
}

namespace zge
{
    using NodeVisitor = ee::FunctionPtr<int32_t(ee::Ptr<Node> node, uint32_t context)>;

    namespace
    {
        // ZGE2's walk of a sector's nodes: through the array's own functions (zge/container/array.h: walkFirst and
        // walkNext out of line, over ZGE2's array record), the list read from the sector each step.
        int32_t forEachSectorNodeByArrayWalk(ee::Ptr<Sector> sector, const NodeVisitor &visit, ee::Address context)
        {
            const auto list = [&]() { return sector->nodes.cast<PointerArray>(); };
            for (ee::Ptr<Node> node(arrayFirst(list())); node; node = ee::Ptr<Node>(arrayNext(list())))
                if (!visit(node, context))
                    return 0;
            return 1;
        }
    }

    int32_t forEachSectorNode(ee::Ptr<Sector> sector, uint32_t visitFunction, ee::Address context)
    {
        const NodeVisitor visit(visitFunction);
        // ZGE2 walks the list through its array's own functions (arrayFirst, arrayNext); ZGE3 inlines the walk over
        // its record.
        if constexpr (game::readable::kIsDm2)
            return forEachSectorNodeByArrayWalk(sector, visit, context);
        // The list keeps the walk's cursor, so a visit can see where the walk
        // is (and end it by setting -1).
        NodeList *list = sector->nodes.get();
        ee::Ptr<Node> node;
        if (list->count)
        {
            list->cursor = 0;
            node = list->items[0];
        }
        else
            list->cursor = -1;
        while (node)
        {
            if (!visit(node, context))
                return 0;
            list = sector->nodes.get();
            const int32_t next = list->cursor + 1;
            if (list->cursor >= 0 && next < static_cast<int32_t>(list->count))
            {
                list->cursor = next;
                node = list->items[static_cast<uint32_t>(next)];
            }
            else
            {
                list->cursor = -1;
                node = ee::Ptr<Node>();
            }
        }
        return 1;
    }
    READABLE_FUNCTION(forEachSectorNode, Verified);

    namespace
    {
        constexpr uint32_t kShapeQueryVisit = sym::shapeQueryVisit;
        constexpr uint32_t kSphereQueryVisit = sym::sphereQueryVisit;
    }

    int32_t shapeQueryVisit(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query)
    {
        if (node->queryStamp != query->stamp)
        {
            node->queryStamp = query->stamp;
            if (node->flags & kNodeTreeIsOneUnit)
                queryTestShape(node, query);
            else
                forEachNodeInSubtree(node, kQueryTestShape, query);
        }
        return 1;
    }
    READABLE_FUNCTION(shapeQueryVisit, Verified); // kind 0 (spheres); kinds 2 and 3 have no caller in the game

    int32_t runQuery(ee::Ptr<Sector> sector, ee::Ptr<SphereQuery> query)
    {
        const uint32_t kind = query->kind;
        // Kinds 2 and 3 walk every node; the others only a sector whose box
        // the query touches (no matrix: the sector's box is in world space).
        if (kind == static_cast<uint32_t>(QueryKind::ShapeAll2) || kind == static_cast<uint32_t>(QueryKind::ShapeAll3))
        {
            forEachSectorNode(sector, kShapeQueryVisit, query);
            return 1;
        }
        if (!queryOverlapsBox(kind, query, sector, ee::Ptr<Matrix>()))
            return 1;
        switch (static_cast<QueryKind>(query->kind))
        {
        case QueryKind::Shape:
            forEachSectorNode(sector, kShapeQueryVisit, query);
            return 1;
        case QueryKind::Sphere:
            forEachSectorNode(sector, kSphereQueryVisit, query);
            return 1;
        default:
            return 1;
        }
    }

    namespace
    {
        constexpr uint32_t kQueryTestShapeVisit = sym::queryTestShape;
        constexpr uint32_t kSphereQueryTestNodeVisit = sym::sphereQueryTestNode;
        // ZGE2's colour for the sector box a query tests (green).
        inline ee::Global<uint32_t, sym::dm2::g_querySectorBoxColour> g_querySectorBoxColour;
    }

    // ZGE2's runQuery: the sector's box against the query's shape (shapeBoundsOverlapBox, ZGE2's queryOverlapsBox: the
    // shape and no matrix), drawn when the query asks (kQueryDrawTestedBoxes),
    // then the kind's node test itself walked over the sector's nodes (ZGE2 has no visitor step: each test does the
    // stamp test, so no node is tested twice). ZGE3 added kinds 2 and 3, which walk every sector, and the visitors.
    int32_t runQueryOverSectorBox(ee::Ptr<Sector> sector, ee::Ptr<SphereQuery> query)
    {
        const ee::Address box(&sector->box);
        if (!shapeBoundsOverlapBox(query->kind, query->segment, box))
            return 1;
        if (query->flags & kQueryDrawTestedBoxes)
        {
            ee::Local<uint32_t> colour;
            *colour = *g_querySectorBoxColour;
            addDebugText(box, ee::Ptr<Matrix>(), colour, 0);
        }
        switch (static_cast<QueryKind>(query->kind))
        {
        case QueryKind::Shape:
            forEachSectorNode(sector, kQueryTestShapeVisit, query);
            return 1;
        case QueryKind::Sphere:
            forEachSectorNode(sector, kSphereQueryTestNodeVisit, query);
            return 1;
        default:
            return 1;
        }
    }
    READABLE_FUNCTION_OWN(runQuery, Verified, dm2, runQueryOverSectorBox);
}
