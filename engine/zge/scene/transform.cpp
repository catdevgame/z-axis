// The scene tree's transforms (module zglTerrainRender,
// zge/com/zglC/zglTerrainRender.c; BMX XXX has these functions word for
// word, DM2 most of them): a node's local matrix is set, the node's tree
// goes on the moved list, and once a frame each moved tree has its world
// matrices, bounds, collision-grid cells and region brought up to date.
//
// A node's matrices (zge/scene/node.h): local (relative to its parent),
// world and the world matrix's inverse. Its box (boxMin, boxMax) bounds its
// model's box transformed to the world, with a margin.

#include "node.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/collision/collision.h"
#include "zge/container/array.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/scene/visibility_regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/transform.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kUpdateWorldMatricesOf = sym::updateWorldMatrices; // updateWorldMatrices below
        constexpr uint32_t kUpdateBoxesOf = sym::updateBoxes;         // updateBoxes below

        // The trees moved since the last update.
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_movedNodes> g_movedNodes;
        // ZGE4's count of the view cameras (camera_records.cpp keeps it): nodeNotDrawn asks every view.
        inline ee::Global<int32_t, sym::bmx::g_viewCameraCount> g_viewCameraCount;
        // The view regions' pool (visibility_regions.cpp keeps it).
        inline ee::Global<ee::Ptr<Pool>, sym::g_regions> g_regions;

        // What a node's box is grown by on every side.
        inline ee::Global<ee::Float, sym::g_nodeBoxMargin> g_nodeBoxMargin;
        // Taken off a box's high corner before its cell is found, so a box
        // ending on a cell's edge stays in the cell below [inferred].
        inline ee::Global<ee::Float, sym::g_cellEdgeEpsilon> g_cellEdgeEpsilon;

        bool sameCells(const AxisCells &a, const AxisCells &b) { return a.low == b.low && a.high == b.high; }
    }

    // rootNode: the top of the node's tree.
    ee::Ptr<Node> rootNode(ee::Ptr<Node> node)
    {
        ee::Ptr<Node> root = node;
        for (ee::Ptr<Node> up = node->parent; up; up = up->parent)
            root = up;
        return root;
    }
    READABLE_FUNCTION(rootNode, Verified);

    // nodeMatrix: one of the node's matrices: 0 local, 1 world, 2 the
    // world's inverse; 0 for another value.
    ee::Ptr<Matrix> nodeMatrix(ee::Ptr<Node> node, uint32_t which)
    {
        if (which == 1)
            return &node->world;
        if (which == 0)
            return &node->local;
        if (which == 2)
            return &node->inverseWorld;
        return ee::Ptr<Matrix>();
    }
    READABLE_FUNCTION(nodeMatrix, Verified);

    // nodeNotDrawn: 0 if the node was culled in and kept in the last frame's
    // view 0 (its stamp the world's frame, its drawn bit set), else 1. A node
    // of a tree that is one unit (kNodeTreeIsOneUnit) goes by its root's view.
    // A game override wraps it (the Zoo's NPCs in split screen). ZGE4 asks every view camera's view.
    uint32_t nodeNotDrawn(ee::Ptr<Node> node)
    {
        auto drawn = [](const NodeView &view)
        {
            return viewFrameStamp(view) == (*g_collisionWorld)->frameStamp && (view.lodAndFlags & kViewDrawn) != 0;
        };
        if constexpr (game::readable::kIsBmx) // ZGE4: drawn in any of the view cameras' views
        {
            ee::Ptr<Node> source = node;
            if (node->parent)
                if (const ee::Ptr<Node> root = rootNode(node); (root->flags & kNodeTreeIsOneUnit) != 0)
                    source = root;
            for (int32_t view = 0; view < *g_viewCameraCount; ++view)
                if (drawn(source->views[view]))
                    return 0;
            return 1;
        }
        if (node->parent)
        {
            const ee::Ptr<Node> root = rootNode(node);
            if ((root->flags & kNodeTreeIsOneUnit) != 0)
            {
                const bool rootDrawn = drawn(root->views[0]);
                return rootDrawn ? 0 : 1;
            }
        }
        const bool nodeDrawn = drawn(node->views[0]);
        return nodeDrawn ? 0 : 1;
    }
    READABLE_FUNCTION(nodeNotDrawn, Verified);

    namespace
    {
        // The bounds in a node's model record.
        ee::Ptr<Box> modelBounds(ee::Ptr<Node> node) { return &node->modelBounds(); }
    }

    // nodeBox: the node's box (boxMin, then boxMax).
    ee::Ptr<Box> nodeBox(ee::Ptr<Node> node)
    {
        return &node->box();
    }
    READABLE_FUNCTION(nodeBox, Verified);

    // markNodeMoved: the tree (its top node) put on the moved list, unless
    // its loader asked not to (kNodeNotMoved) or it is on it already.
    void markNodeMoved(ee::Ptr<Node> node)
    {
        uint32_t flags = nodeStateFlags(*node);
        if (!(flags & kNodeNotMoved))
        {
            // Already flagged moved: only added if the list lacks it. ZGE2 adds
            // only such a node: one not flagged yet is flagged and left off the
            // list until it is marked again.
            bool add = !game::readable::kIsDm2;
            if (flags & kNodeMoved)
                add = pointerArrayFind(*g_movedNodes, node) < 0;
            if (add)
                pointerArrayAdd(*g_movedNodes, node);
            flags = nodeStateFlags(*node);
        }
        nodeStateFlags(*node) = (flags & ~kNodeNotMoved) | kNodeMoved;
    }
    READABLE_FUNCTION_WAITS(markNodeMoved, Verified);

    // setNodeLocalMatrix: the node's local matrix set (not on a mirrored
    // instance), its tree marked moved. The GameCube and Xbox builds of BMX
    // XXX print "Cant set matrix on mirrored instance" there.
    namespace
    {
        // ZGE2's set: the tree's root marked moved first, then the matrix taken, mirrored or not.
        void markRootThenSetLocalMatrix(ee::Ptr<Node> node, ee::Ptr<const Matrix> matrix)
        {
            markNodeMoved(rootNode(node));
            node->local = *matrix;
        }
    }

    void setNodeLocalMatrix(ee::Ptr<Node> node, ee::Ptr<const Matrix> matrix)
    {
        // ZGE3 leaves a mirrored instance alone and marks the root after the copy; ZGE2 marks first and copies always.
        if constexpr (game::readable::kIsDm2)
            return markRootThenSetLocalMatrix(node, matrix);
        if (node->flags & kNodeMirrored)
            return;
        node->local = *matrix; // four quadword moves
        const ee::Ptr<Node> root = rootNode(node);
        game::tail(zge::markNodeMoved, root);
    }
    READABLE_FUNCTION_WAITS(setNodeLocalMatrix, Verified);

    // forEachChild: visit(child, context) for each of the node's children
    // (not their children) until a visit returns 0: then 0, else 1.
    int32_t forEachChild(ee::Ptr<Node> node, uint32_t visitFunction, ee::Address context)
    {
        const ee::FunctionPtr<int32_t(ee::Ptr<Node> child, uint32_t context)> visit(visitFunction);
        for (ee::Ptr<Node> child = node->firstChild; child; child = child->nextSibling)
            if (visit(child, context) == 0)
                return 0;
        return 1;
    }
    READABLE_FUNCTION(forEachChild, Verified);

    // nodeGridCells: the collision-grid cells the node's box covers, an axis at
    // a time (x, y, z): (corner - grid corner) / cell size, truncated, the
    // high corner less g_cellEdgeEpsilon. What its last step leaves on VU0 is
    // the next vector operation's stale lane.
    void nodeGridCells(ee::Ptr<Node> node, ee::Ptr<AxisCells> cells)
    {
        ee::Local<Vec4> scaled;
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        *scaled = multiplied(node->boxMin - world->boundsMin, world->inverseCellSize);
        cells[0].low = ee::toInt(scaled->x);
        cells[1].low = ee::toInt(scaled->y);
        cells[2].low = ee::toInt(scaled->z);
        // A high corner on a cell's edge belongs to the cell below it.
        *scaled = multiplied((node->boxMax - world->boundsMin) - *g_cellEdgeEpsilon, world->inverseCellSize);
        cells[0].high = ee::toInt(scaled->x);
        cells[1].high = ee::toInt(scaled->y);
        cells[2].high = ee::toInt(scaled->z);
    }
    READABLE_FUNCTION_VU0(nodeGridCells, Verified);

    // updateNodeBox: the node's box from its model's, transformed by matrix,
    // grown by the margin.
    void updateNodeBox(ee::Ptr<Node> node, ee::Ptr<Matrix> matrix)
    {
        boxTransform(&node->box(), modelBounds(node), matrix); // nodeBox inline, as the original has it
        const ee::Float margin = *g_nodeBoxMargin;
        node->boxMin.x = node->boxMin.x - margin;
        node->boxMin.y = node->boxMin.y - margin;
        node->boxMin.z = node->boxMin.z - margin;
        node->boxMax.x = node->boxMax.x + margin;
        node->boxMax.y = node->boxMax.y + margin;
        node->boxMax.z = node->boxMax.z + margin;
    }
    READABLE_FUNCTION(updateNodeBox, Verified);

    // findRegion: the first region whose box holds box, or 0. ZGE2 is given a
    // point (pointGivenAsBox) and asks boxContainsPoint; ZGE3 a box.
    uint32_t findRegion(ee::Ptr<Box> box)
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Region> region = poolRecordAs<Region>(link);
            if constexpr (game::readable::kIsDm2)
            {
                if (boxContainsPoint(&region->box, pointGivenAsBox(box)))
                    return ee::Address(region);
            }
            else if (boxContainsBox(ee::Ptr<Box>(&region->box), box) != 0)
                return ee::Address(region);
        }
        return 0;
    }
    READABLE_FUNCTION(findRegion, Verified);

    // A node's local and world matrices lie one after the other: a child is handed its parent's local matrix,
    // and the parent's world is the matrix after it.
    static_assert(offsetof(Node, world) == offsetof(Node, local) + sizeof(Matrix));

    // updateWorldMatrices2: a node-tree walk's visitor: the node's world matrix (its local one, at the top; else
    // its local one taken into its parent's world, or the parent's world itself for a kNodeTakesParentWorld
    // node) and that matrix's inverse, then the same for its children, each handed this node's matrices
    // (parentMatrices: the parent's local matrix, its world after it; 0 at the top). Always 1 (go on). ZGE2
    // copies the matrices through copyRotationAndTranslation where ZGE3 moves them inline.
    int32_t updateWorldMatrices2(ee::Ptr<Node> node, ee::Ptr<Matrix> parentMatrices)
    {
        if (!parentMatrices)
        {
            if constexpr (game::readable::kIsDm2)
                copyMatrixRotationAndTranslation(&node->world, &node->local);
            else
                node->world = node->local; // four quadword moves
        }
        else if (nodeStateFlags(*node) & kNodeTakesParentWorld)
        {
            if constexpr (game::readable::kIsDm2)
                copyMatrixRotationAndTranslation(&node->world, parentMatrices + 1);
            else
                node->world = parentMatrices[1];
        }
        else
            matrixMultiply(&node->world, &parentMatrices[1], &node->local);
        matrixInvertRigid(&node->inverseWorld, &node->world);
        forEachChild(node, sym::updateWorldMatrices2, &node->local);
        return 1;
    }
    READABLE_FUNCTION(updateWorldMatrices2, Verified);

    // updateWorldMatrices: the subtree's world matrices from the local ones,
    // from the top (updateWorldMatrices2 with no parent).
    void updateWorldMatrices(ee::Ptr<Node> node)
    {
        game::tail(updateWorldMatrices2, node, ee::Ptr<Matrix>());
    }
    READABLE_FUNCTION(updateWorldMatrices, Verified);

    // updateBoxes2: a node-tree walk's visitor: the node's box from its model's, taken by its world matrix with
    // its translation moved by the node's origin (updateNodeBox), its sphere's centre the box's centre (the
    // radius kept), then the same for its children. Always 1 (go on).
    int32_t updateBoxes2(ee::Ptr<Node> node, uint32_t /*context: not read*/)
    {
        ee::Local<Vec4> centre;
        ee::Local<Matrix> placed;
        *placed = node->world; // four quadword moves
        placed->translation() = placed->translation() + node->origin;
        updateNodeBox(node, placed);
        boxCentre(&centre->xyz(), &node->box());
        node->sphereCentre = *centre;
        node->sphereCentre.w = node->radius; // (over the centre's fourth word, which boxCentre does not write)
        if (node->firstChild)
            forEachChild(node, sym::updateBoxes2, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateBoxes2, Verified);

    // updateBoxes: the subtree's boxes, from the top (updateBoxes2).
    void updateBoxes(ee::Ptr<Node> node)
    {
        game::tail(updateBoxes2, node, 0u);
    }
    READABLE_FUNCTION_WAITS(updateBoxes, Verified);

    // updateMovedNode: a moved tree brought up to date: world matrices and
    // boxes; the collision grid told if its cells changed; its region looked
    // up again if its position's key changed (unless it keeps its region).
    void updateMovedNode(ee::Ptr<Node> node)
    {
        if (!(nodeStateFlags(*node) & kNodeMoved))
            return;
        // Both cell sets: the grid's update gets their addresses.
        ee::Local<AxisCells[3]> before;
        ee::Local<AxisCells[3]> after;
        nodeGridCells(node, before);
        const uint32_t oldKey = node->positionKey;
        // Both by address: a readable callee called directly makes its tail
        // call as a call, which returns twice when it unwinds.
        updateWorldMatrices(node);
        updateBoxes(node);
        nodeGridCells(node, after);
        const bool xMoved = !sameCells(after[0], before[0]);
        if (xMoved || !sameCells(after[1], before[1]) || !sameCells(after[2], before[2]))
            moveNodeCells(node, before, after);
        uint32_t flags = node->flags;
        if (!(flags & kNodeKeepsRegion))
        {
            const ee::Float *position = node->world.m[3]; // the translation row
            const uint32_t key = (static_cast<uint32_t>(ee::toInt(position[0] * 0.5f)) & 0xFFFFu) |
                                 static_cast<uint32_t>(ee::toInt(position[2] * 0.5f)) << 16;
            if (oldKey != key)
            {
                node->positionKey = key;
                // ZGE4 hands findRegion the node's box itself; ZGE3 asks nodeBox for it.
                if constexpr (game::readable::kIsBmx)
                    node->region = findRegion(ee::Ptr<Box>(ee::Address(&node->boxMin)));
                else
                    node->region = findRegion(nodeBox(node));
                flags = node->flags;
            }
        }
        node->flags = flags & ~kNodeMoved;
    }

    // updateNodeBoxAndCentre (ZGE2 only): the node's box, its model's bounds
    // taken to the world, and its sphere's centre, the box's centre (the
    // quadword with it). No margin and no radius (ZGE3's updateNodeBounds
    // takes the model's sphere and pads the box).
    inline void updateNodeBoxAndCentre(ee::Ptr<Node> node)
    {
        ee::Local<Vec4> centre;
        boxTransform(&node->box(), modelBounds(node), &node->world);
        node->sphereCentre = quadwordOf(boxCentre(ee::Ptr<Vec3>(centre.address()), &node->box()));
    }
    READABLE_FUNCTION_ONLY(dm2, updateNodeBoxAndCentre);

    // moveNodeInGrid: ZGE2's updateMovedNode. A moved tree taken out of the
    // collision grid, its world matrices and its own bounds brought up to date,
    // put back in the grid, and its region looked up again, every time (ZGE2
    // compares no cells and keeps no position key).
    void moveNodeInGrid(ee::Ptr<Node> node)
    {
        if (!(nodeStateFlags(*node) & kNodeMoved))
            return;
        addNodeToGrid(node, 0);
        updateWorldMatrices(node);
        updateNodeBoxAndCentre(node);
        addNodeToGrid(node, 1);
        // ZGE2's findRegionAt takes the box (ZGE3's findRegion).
        node->region = ee::Address(findRegionAt(pointGivenAsBox(nodeBox(node))));
        nodeStateFlags(*node) &= ~kNodeMoved;
    }
    READABLE_FUNCTION_WAITS_OWN(updateMovedNode, Verified, dm2, moveNodeInGrid);

    // updateMovedNodes: every tree on the moved list brought up to date, taking
    // each off the list (the list's walk from its first item each time; ZGE2
    // starts it through the array's own function, arrayFirst).
    void updateMovedNodes()
    {
        if constexpr (game::readable::kIsDm2)
        {
            for (uint32_t node = arrayFirst(*g_movedNodes); node; node = arrayFirst(*g_movedNodes))
            {
                moveNodeInGrid(ee::Ptr<Node>(node));
                pointerArrayRemoveCurrent(*g_movedNodes);
            }
            return;
        }
        uint32_t node = 0;
        for (bool first = true;; first = false)
        {
            if (!first)
            {
                updateMovedNode(ee::Ptr<Node>(node));
                pointerArrayRemoveCurrent(*g_movedNodes);
            }
            const ee::Ptr<PointerArray> moved = *g_movedNodes;
            node = 0;
            if (moved->count != 0)
            {
                moved->cursor = 0;
                node = moved->items[0];
            }
            else
                moved->cursor = -1;
            if (!node)
                return;
        }
    }
    READABLE_FUNCTION_WAITS(updateMovedNodes, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_outOfViewMark> g_outOfViewMark; // g_outOfViewMark (aggressive::fx::(anonymous)::g_outOfViewMark, aggressive::fx::(anonymous)::g_outOfViewMark)
    }

}
