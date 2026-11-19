// The collision world: which cells a query's shape covers, which nodes in
// them its bounds touch, and the hand-off of each node to the face tests
// (zge/collision/collision.h).

#include "collision.h"

#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/assets/game_address.h"
#include "zge/math/vec.h"
#include "zge/math/box.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/self_call.h"
#include "zge/native/collision_check.h"
#include "zge/native/host.h"
#include "zge/platform/platform.h"
#include "zge/debug/debug.h"

#include <utility>
#include "sym/zge/collision/world.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kNodeMatrix = sym::nodeMatrix;           // (node, 0 local / 1 world / 2 inverse) -> Matrix*

        // queryOverlapsBox's scratch: the box doubled, the shape's
        // bounds, the room on each axis and how far apart the centres are.
        inline ee::Global<Vec4, sym::g_boxSum> g_boxSum;        // max + min
        inline ee::Global<Vec4, sym::g_boxSize> g_boxSize;       // max - min
        inline ee::Global<Vec4, sym::g_shapeSum> g_shapeSum;      // the shape's centre, doubled (w: a sphere's diameter)
        inline ee::Global<Vec4, sym::g_shapeSize> g_shapeSize;     // a box's size
        inline ee::Global<Vec4, sym::g_room> g_room;          // sizes added: the most the doubled centres may differ
        inline ee::Global<Vec4, sym::g_apart> g_apart;         // the doubled centres' difference

        // A node's box: boxMin and boxMax, as a query's box lies.
        const QueryBox *boxOf(const Node &node) { return reinterpret_cast<const QueryBox *>(&node.boxMin); }

        // Whether `query` lists `node` (SphereQuery::nodes); the scan goes on
        // to the end of the list.
        bool listsNode(const SphereQuery &query, ee::Ptr<Node> node)
        {
            bool found = false;
            const ee::Ptr<Node> *entry = query.nodes;
            for (; *entry; ++entry)
                if (*entry == node)
                    found = true;
            return found;
        }

        // Kept bug: with the HitNode it was filling dropped, the original makes
        // the pool's newest link (poolFirst) the HitNode being filled, not the
        // record that link stands for (poolRecord: 16 bytes on).
        ee::Ptr<HitNode> newestLinkAsHitNode(ee::Address pool) { return firstPoolLink(pool).cast<HitNode>(); }

        // A node the query reached: a HitNode for its faces, filled by the
        // face tests, dropped again if no face was hit. True if a face was.
        bool testFaces(ee::Ptr<Node> node, ee::Ptr<CollisionFrame> frame, ee::Ptr<ZgePart> part, ee::Ptr<SphereQuery> query)
        {
            // Host side (not under ps2xFnCheck): PS2X_NATIVE_COLLISION_CHECK
            // compares this frame's data with the GEO file's (ZGE3's layouts: ZGE2's leaf has no offset word).
            if (!game::readable::kIsDm2 && native::hostExtrasOn() && native::collisionCheckEnabled())
                native::checkCollisionFrameOnce(frame, part);
            QueryState &state = *g_queryState;
            if constexpr (game::readable::kIsDm2)
            {
                // ZGE2: the state's next HitNode; its faces are set where the first is hit (testNodeFaces).
                const ee::Ptr<HitNode> record(&state.hitNodes[state.hitCount]);
                state.hitNode = record;
                state.hitCount = state.hitCount + 1;
                record->node = node;
                state.hitNode->part = part;
                state.hitNode->faceCount = 0;
                state.hitNode->faces = ee::Ptr<FaceHit>();
            }
            else
            {
                const ee::Ptr<HitNode> record = poolAllocAs<HitNode>(state.hitNodePool);
                state.hitNode = record;
                record->node = node;
                state.hitNode->part = part;
                state.hitNode->faceCount = 0;
                state.hitNode->faces = state.facesBuffer + static_cast<ptrdiff_t>(state.faceCount);
            }
            int32_t hit = 0;
            if (part->header == 0) // a mesh
            {
                hit = testNodeFaces(node, frame, part, query);
            }
            if (!hit)
            {
                if constexpr (game::readable::kIsDm2)
                    state.hitCount = state.hitCount - 1;
                else
                {
                    poolFree(ee::Ptr<Pool>(state.hitNodePool), state.hitNode);
                    state.hitNode = newestLinkAsHitNode(state.hitNodePool);
                }
            }
            return hit != 0;
        }

        // ZGE2's colour for the node box a query tests (blue).
        inline ee::Global<uint32_t, sym::dm2::g_queryNodeBoxColour> g_queryNodeBoxColour;
        // ZGE2 only: the node's box drawn when the query asks (kQueryDrawTestedBoxes); ZGE3 dropped the step.
        void drawTestedNodeBox(ee::Address box, const SphereQuery &query)
        {
            if (query.flags & kQueryDrawTestedBoxes)
            {
                ee::Local<uint32_t> colour;
                *colour = *g_queryNodeBoxColour;
                addDebugText(box, ee::Ptr<Matrix>(), colour, 0);
            }
        }

        // The filter a query may carry: (context, node, 0, 0) -> keep it.
        bool filterKeeps(const SphereQuery &query, ee::Ptr<Node> node)
        {
            return !query.filter || ee::call<int32_t>(query.filter, query.filterContext, node, 0u, 0u) != 0;
        }
    }

    uint32_t collisionClock()
    {
        return gameClock();
    }
    READABLE_FUNCTION(collisionClock, Verified);

    int32_t queryOverlapsBox(uint32_t kind, ee::Ptr<SphereQuery> query, ee::Address boxAt, ee::Ptr<Matrix> matrix)
    {
        // The box at the start of a node or a sector.
        const ee::Ptr<const QueryBox> box(boxAt);
        // Everything doubled: centres as max + min, sizes as max - min, so
        // the boxes overlap on an axis when |sum - sum'| <= size + size'.
        *g_boxSum = box->max + box->min;
        *g_boxSize = box->max - box->min;
        const Vec4 &room = *g_room, &apart = *g_apart;
        // The centres' difference, made positive, within the room.
        const auto withinRoom = [&] { return ee::abs(apart.x) <= room.x && ee::abs(apart.y) <= room.y && ee::abs(apart.z) <= room.z; };
        switch (static_cast<CollisionKind>(kind))
        {
        case CollisionKind::Segment:
            // The query's own box around the segment (SphereQuery::boxSize, boxSum).
            *g_room = query->boxSize + *g_boxSize;
            *g_apart = query->boxSum - *g_boxSum;
            *g_apart = zge::abs(*g_apart);
            return apart.x <= room.x && apart.y <= room.y && apart.z <= room.z ? 1 : 0;
        case CollisionKind::Sphere:
        {
            const QuerySphere &sphere = queryShape<QuerySphere>(*query);
            // The doubled centre (its w: the box size's, as VU0 keeps it),
            // then the diameter over w.
            *g_shapeSum = sphere.centre * 2.0f;
            g_shapeSum->w = sphere.radius + sphere.radius;
            *g_apart = *g_shapeSum - *g_boxSum;
            const ee::Float diameter = g_shapeSum->w;
            const Vec4 &size = *g_boxSize;
            return ee::abs(apart.x) <= diameter + size.x && ee::abs(apart.y) <= diameter + size.y && ee::abs(apart.z) <= diameter + size.z ? 1 : 0;
        }
        case CollisionKind::Box:
        {
            const QueryBox &other = queryShape<QueryBox>(*query);
            *g_shapeSum = other.max + other.min;
            *g_shapeSize = other.max - other.min;
            g_shapeSize->x = ee::abs(g_shapeSize->x);
            g_shapeSize->y = ee::abs(g_shapeSize->y);
            g_shapeSize->z = ee::abs(g_shapeSize->z);
            *g_room = *g_shapeSize + *g_boxSize;
            *g_apart = *g_shapeSum - *g_boxSum;
            return withinRoom() ? 1 : 0;
        }
        case CollisionKind::SweptBox:
        {
            // The oriented box collisionQuery made: its centre is relative to
            // the matrix's position when there is one. These are not doubled.
            const QueryOrientedBox &oriented = queryShape<QueryOrientedBox>(*query);
            *g_room = oriented.extents + *g_boxSize;
            *g_apart = oriented.centre - *g_boxSum;
            if (matrix)
                *g_apart = oriented.centre - matrixRow(*matrix, 3);
            return withinRoom() ? 1 : 0;
        }
        }
        return 0;
    }

    // shapeBoundsOverlapBox: ZGE2's queryOverlapsBox. It is given the query's shape (the query's second word) and no
    // matrix, and has two kinds: a sphere (0) as ZGE3's sphere, and a segment (1) whose ends are taken as a box's
    // corners, as ZGE3's box (kind 2). The same doubled scratch words. (ZGE3 was given the query, kept the segment's
    // bounds in it and added the box kinds 2 and 3.)
    int32_t shapeBoundsOverlapBox(uint32_t kind, ee::Ptr<QuerySegment> shape, ee::Address boxAt)
    {
        const ee::Ptr<const QueryBox> box(boxAt);
        *g_boxSum = box->max + box->min;
        *g_boxSize = box->max - box->min;
        const Vec4 &room = *g_room, &apart = *g_apart;
        switch (static_cast<CollisionKind>(kind))
        {
        case CollisionKind::Sphere:
        {
            const QuerySphere &sphere = *shape.cast<QuerySphere>();
            *g_shapeSum = sphere.centre * 2.0f;
            g_shapeSum->w = sphere.radius + sphere.radius;
            *g_apart = *g_shapeSum - *g_boxSum;
            const ee::Float diameter = g_shapeSum->w;
            const Vec4 &size = *g_boxSize;
            return ee::abs(apart.x) <= diameter + size.x && ee::abs(apart.y) <= diameter + size.y && ee::abs(apart.z) <= diameter + size.z ? 1 : 0;
        }
        case CollisionKind::Segment:
        {
            *g_shapeSum = shape->end + shape->start;
            *g_shapeSize = shape->end - shape->start;
            g_shapeSize->x = ee::abs(g_shapeSize->x);
            g_shapeSize->y = ee::abs(g_shapeSize->y);
            g_shapeSize->z = ee::abs(g_shapeSize->z);
            *g_room = *g_shapeSize + *g_boxSize;
            *g_apart = *g_shapeSum - *g_boxSum;
            return ee::abs(apart.x) <= room.x && ee::abs(apart.y) <= room.y && ee::abs(apart.z) <= room.z ? 1 : 0;
        }
        default:
            return 0;
        }
    }
    READABLE_FUNCTION_VU0_OWN(queryOverlapsBox, Verified, dm2, shapeBoundsOverlapBox);

    int32_t queryTestShape(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query)
    {
        if (!(node->flags & kNodeCollides))
            return 1;
        const uint32_t flags = query->flags;
        if (flags & (kQueryExcludeListed | kQueryOnlyListed))
        {
            const bool found = listsNode(*query, node);
            if ((flags & kQueryExcludeListed) && found)
                return 1;
            if (flags & kQueryOnlyListed)
            {
                if (!found)
                    return 1;
            }
        }
        const ee::Ptr<Matrix> world = nodeMatrix(node, 1u);
        const QueryBox *nodeBox = boxOf(*node);
        if (!queryOverlapsBox(query->kind, query, nodeBox, world))
            return 1;

        const ee::Ptr<CollisionFrame> frame = nodeCollisionFrame(node);
        const ee::Ptr<ZgePart> part = frameCollisionPart(frame);
        // The frame's tree field (frameTree hands back its address; its size follows it).
        const ee::Ptr<ee::Ptr<uint8_t>> treeSlot = frameTree(frame);
        if (!part)
            return 1;
        if (!*treeSlot && part->header == 0)
        {
            // A mesh without a face tree: one leaf with every face.
            const uint32_t bytes = part->faceCount * 2u + 8u;
            frame->treeBytes = bytes;
            const uint32_t block = heapAllocate(bytes);
            *treeSlot = ee::Ptr<uint8_t>(block);
            const ee::Ptr<WholePartTree> whole(block);
            whole->leaf.axis = kFaceTreeLeaf;
            whole->leaf.faces = sizeof(FaceTreeLeaf) / 2u; // the indices right after it
            whole->leaf.count = part->faceCount;
            for (uint32_t i = 0; i < part->faceCount; ++i)
                whole->faces[i] = static_cast<uint16_t>(i);
        }
        if (!filterKeeps(*query, node))
            return 1;
        testFaces(node, frame, part, query);
        return 1;
    }

    namespace
    {
        // The word with a node's collides bit (kNodeCollides): ZGE2 keeps it in the flags' upper word, ZGE3 in the flags.
        uint32_t collidesWord(const Node &node) { return game::readable::kIsDm2 ? node.flagsHigh : node.flags; }
    }

    // queryTestShapeOnce: ZGE2's queryTestShape. The node's stamp is tested here (ZGE3 moved it into the visitor,
    // shapeQueryVisit), after the query's node lists and before the collides bit; the node's box goes against the
    // query's shape with no matrix (shapeBoundsOverlapBox) and is drawn when the query asks (kQueryDrawTestedBoxes);
    // a part without a tree gets ZGE2's leaf, its face indices right after the count; the HitNode is the query
    // state's next one, given back when no face is hit. (ZGE3 took the HitNodes from a pool.)
    int32_t queryTestShapeOnce(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query)
    {
        const uint32_t flags = query->flags;
        if (flags & (kQueryExcludeListed | kQueryOnlyListed))
        {
            const bool found = listsNode(*query, node);
            if ((flags & kQueryExcludeListed) && found)
                return 1;
            if ((flags & kQueryOnlyListed) && !found)
                return 1;
        }
        if (node->queryStamp == query->stamp)
            return 1;
        if (!(collidesWord(*node) & kNodeCollides))
            return 1;
        node->queryStamp = query->stamp;
        const ee::Address box(&node->boxMin);
        if (!shapeBoundsOverlapBox(query->kind, query->segment, box))
            return 1;

        const ee::Ptr<CollisionFrame> frame = nodeCollisionFrame(node);
        const ee::Ptr<ZgePart> part = frameCollisionPart(frame);
        const ee::Ptr<ee::Ptr<uint8_t>> treeSlot = frameTree(frame);
        if (!part)
            return 1;
        if (!*treeSlot && part->header == 0)
        {
            // A mesh without a face tree: one leaf with every face.
            const uint32_t bytes = part->faceCount * 2u + 4u;
            frame->treeBytes = bytes;
            const uint32_t block = heapAllocate(bytes);
            *treeSlot = ee::Ptr<uint8_t>(block);
            const ee::Ptr<InlineLeafTree> whole(block);
            whole->axis = kFaceTreeLeaf;
            whole->count = part->faceCount;
            for (uint32_t i = 0; i < part->faceCount; ++i)
                whole->faces[i] = static_cast<uint16_t>(i);
        }
        drawTestedNodeBox(box, *query);
        if (!filterKeeps(*query, node))
            return 1;
        testFaces(node, frame, part, query);
        return 1;
    }
    READABLE_FUNCTION_OWN(queryTestShape, Verified, dm2, queryTestShapeOnce);

    int32_t testHitNodeFaces(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query)
    {
        const ee::Ptr<CollisionFrame> frame = nodeCollisionFrame(node);
        const ee::Ptr<ZgePart> part = frameCollisionPart(frame);
        const ee::Ptr<ee::Ptr<uint8_t>> treeSlot = frameTree(frame);
        if (!part || !treeSlot)
            return 1;
        if constexpr (game::readable::kIsDm2)
            drawTestedNodeBox(ee::Address(&node->boxMin), *query);
        if (filterKeeps(*query, node))
            testFaces(node, frame, part, query);
        return 1;
    }
    READABLE_FUNCTION(testHitNodeFaces, Verified);

    // ---- the cells a shape covers ---------------------------------------------

    namespace
    {
        struct CellRange
        {
            int32_t low[3];
            int32_t high[3];
        };

        // The cell index of a coordinate on an axis (truncated, as cvt.w.s).
        int32_t cellIndex(const CollisionWorld &world, ee::Float coordinate, int axis)
        {
            const ee::Float *corner = &world.boundsMin.x;
            const ee::Float *size = &world.cellSize.x;
            return ee::toInt((coordinate - corner[axis]) / size[axis]);
        }

        // Into the grid: at most the last cell, at least the first.
        int32_t clampToGrid(int32_t index, int32_t count)
        {
            int32_t clamped = index < count - 1 ? index : count - 1;
            return clamped > -1 ? clamped : 0;
        }

        // visit(cell, context) for every cell in the range, x outermost,
        // until one returns 0 (then 0; else 1). The loops are the
        // original's: each checks its index against the range and the grid,
        // and x's is entered at its condition.
        int32_t visitCells(const CollisionWorld &world, const CellRange &range, ee::FunctionPtr<int32_t(ee::Ptr<Sector>, uint32_t)> visit, uint32_t context)
        {
            // (The original also skips negative indices and stops at the
            // grid's end on y and z; the clamps above leave none of those.)
            if (range.high[0] < range.low[0])
                return 1;
            for (int32_t x = range.low[0];;)
            {
                if (!(x < world.cellCount[0]))
                    return 1;
                int32_t y = range.low[1];
                if (!(range.high[1] < y) && y < world.cellCount[1])
                {
                    for (;;)
                    {
                        int32_t z = range.low[2];
                        if (!(range.high[2] < z) && z < world.cellCount[2])
                        {
                            for (bool first = true;; first = false)
                            {
                                const int32_t index = (x * world.cellCount[1] + y) * world.cellCount[2] + z;
                                if (!visit(world.cell + index, context))
                                    return 0;
                                ++z;
                                if (range.high[2] < z || !(z < world.cellCount[2]))
                                    break;
                            }
                        }
                        ++y;
                        if (range.high[1] < y || !(y < world.cellCount[1]))
                            break;
                    }
                }
                ++x;
                if (range.high[0] < x)
                    return 1;
            }
        }
    }

    int32_t forEachCellOnSegment(uint32_t visit, ee::Ptr<QuerySegment> segment, ee::Address context)
    {
        namespace here = points::forEachCellOnSegment;
        const CollisionWorld &world = **g_collisionWorld;
        CellRange range;
        for (int axis = 0; axis < 3; ++axis)
        {
            const ee::Float *start = &segment->start.x;
            range.low[axis] = cellIndex(world, start[axis], axis);
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const ee::Float *end = &segment->end.x;
            range.high[axis] = cellIndex(world, end[axis], axis);
        }
        // (Each swap's join is an entry block of the original.)
        constexpr uint32_t kSwapped[3] = {here::taken, here::taken2, here::taken3};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (range.high[axis] < range.low[axis])
                std::swap(range.low[axis], range.high[axis]);
        }
        for (int axis = 0; axis < 3; ++axis)
            range.low[axis] = clampToGrid(range.low[axis], world.cellCount[axis]);
        for (int axis = 0; axis < 3; ++axis)
            range.high[axis] = clampToGrid(range.high[axis], world.cellCount[axis]);
        return visitCells(world, range, visit, context);
    }
    READABLE_FUNCTION(forEachCellOnSegment, Verified);

    int32_t forEachCellInSphere(uint32_t visit, ee::Ptr<QuerySphere> sphere, ee::Address context)
    {
        const CollisionWorld &world = **g_collisionWorld;
        const ee::Float *centre = &sphere->centre.x;
        const ee::Float radius = sphere->radius;
        CellRange range;
        for (int axis = 0; axis < 3; ++axis)
            range.low[axis] = cellIndex(world, centre[axis] - radius, axis);
        for (int axis = 0; axis < 3; ++axis)
            range.high[axis] = cellIndex(world, centre[axis] + radius, axis);
        for (int axis = 0; axis < 3; ++axis)
            range.low[axis] = clampToGrid(range.low[axis], world.cellCount[axis]);
        for (int axis = 0; axis < 3; ++axis)
            range.high[axis] = clampToGrid(range.high[axis], world.cellCount[axis]);
        return visitCells(world, range, visit, context);
    }
    READABLE_FUNCTION(forEachCellInSphere, Verified);

    namespace
    {
        constexpr uint32_t kFrameStateHigh = ee::high(sym::g_queryState);
        inline ee::Global<ee::Float, sym::g_fixedFrameStep> g_fixedFrameStep; // g_fixedFrameStep ((anonymous)::g_fixedFrameStep)
        inline ee::Global<ee::Float, sym::g_clutRedScale> g_clutRedScale; // g_clutRedScale ((anonymous)::g_clutRedScale)
        inline ee::Global<ee::Float, sym::g_clutGreenScale> g_clutGreenScale; // g_clutGreenScale ((anonymous)::g_clutGreenScale)
        inline ee::Global<ee::Float, sym::g_clutBlueScale> g_clutBlueScale; // g_clutBlueScale ((anonymous)::g_clutBlueScale)
        inline ee::Global<ee::Float, sym::g_globalColourScaleAlpha> g_globalColourScaleAlpha;
    }

    uint32_t engineStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(engineStubZero, Draft);

    // g_fixedFrameStep = value; startEngine and openPadRecording set it.
    int32_t setGlobalTimeScale(ee::Float fixedFrameStep)
    {
        *g_fixedFrameStep = fixedFrameStep;
        return kFrameStateHigh;
    }
    READABLE_FUNCTION(setGlobalTimeScale, Verified);

    // (r, g, b): the colour scale g_clutRedScale..g_clutBlueScale (scaleClutColours reads it) and
    // g_globalColourScaleAlpha = 1.
    void setGlobalColourScale(ee::Float clutRedScale, ee::Float clutGreenScale, ee::Float clutBlueScale)
    {
        *g_globalColourScaleAlpha = ee::Float(1.0f);
        *g_clutRedScale = clutRedScale;
        *g_clutGreenScale = clutGreenScale;
        *g_clutBlueScale = clutBlueScale;
    }
    READABLE_FUNCTION(setGlobalColourScale, Verified);

    // Empty; newCollisionWorld.
    uint32_t collisionWorldHook()
    {
        return 1;
    }
    READABLE_FUNCTION(collisionWorldHook, Verified);

    // debugWriteAlias: debugWrite under another address (a tail call).
    int32_t debugWriteAlias(uint32_t text)
    {
        return game::tail(debugWrite, text);
    }
    READABLE_FUNCTION(debugWriteAlias, Draft);

    // A node of a tree of axis-aligned splits (four-byte units from the
    // table's start).
    struct SplitTreeNode
    {
        uint8_t axis;     // +0x00 1 x, 2 y, 3 z; 0 a leaf
        uint8_t _01;      // +0x01
        int16_t value;    // +0x02 a split: where, in hundredths; a leaf: 0 when empty
        uint16_t high;    // +0x04 the node above the split, in four-byte units
        uint16_t low;     // +0x06 the node below it
    };
    static_assert(sizeof(SplitTreeNode) == 0x08);

    void drawSplitTreeLeaves(uint32_t table, ee::Ptr<SplitTreeNode> node, ee::Ptr<Box> box, ee::Ptr<SplitTreeNode> only);

    namespace
    {
        constexpr uint32_t kSplitTreeUnit = 4;
        constexpr float kSplitScale = 0.01f;
        inline ee::Global<uint32_t, sym::g_splitTreeLeafColour> g_splitTreeLeafColour;
        // addDebugText (zdebug): while debug drawing is on, a box, placed by a
        // matrix (0: none), in a colour.
    }

    // drawSplitTreeLeaves: a debug drawing of a tree of splits: each leaf
    // that is not empty drawn as its box (g_splitTreeLeafColour), the box cut
    // at each split on the way down (the split's value / 100 on its axis: the
    // low side's high corner, the high side's low corner). With `only`, just
    // that leaf. (Nothing in the game calls it.)
    void drawSplitTreeLeaves(uint32_t table, ee::Ptr<SplitTreeNode> node, ee::Ptr<Box> box, ee::Ptr<SplitTreeNode> only)
    {
        if (node->axis == 0)
        {
            if (only && !same(only, node))
                return;
            if (node->value != 0)
                addDebugText(box, 0, g_splitTreeLeafColour.get(), 0);
            return;
        }
        const ee::Float split = ee::toFloat(node->value) * ee::Float(kSplitScale);
        ee::Local<Box> low;
        ee::Local<Box> high;
        *high = *box;
        *low = *box;
        switch (node->axis)
        {
        case 1:
            low->high.x = split;
            high->low.x = split;
            break;
        case 2:
            low->high.y = split;
            high->low.y = split;
            break;
        case 3:
            low->high.z = split;
            high->low.z = split;
            break;
        default:
            break;
        }
        // Each side in turn, the tree calling itself.
        drawSplitTreeLeaves(table, ee::Ptr<SplitTreeNode>(table + node->low * kSplitTreeUnit), low, only);
        drawSplitTreeLeaves(table, ee::Ptr<SplitTreeNode>(table + node->high * kSplitTreeUnit), high, only);
    }
    READABLE_FUNCTION_WAITS(drawSplitTreeLeaves, Draft);
}
