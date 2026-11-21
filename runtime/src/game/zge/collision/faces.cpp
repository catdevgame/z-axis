// The face tests: a node's collision mesh against a query's shape, through
// the face tree (zge/collision/collision.h).
//
// testNodeFaces brings the query's shape into the node's space and hands it
// to the face test of its kind. The face tests walk the face tree with a
// stack in the scratchpad, gather the leaves the shape reaches in a sorted
// list, then test each leaf's faces once (a bitmap in the scratchpad marks
// the faces seen, as leaves share faces):
//   segments  the plane each face lies in, crossed going forward (or from
//             behind, for two-sided faces), nearer than the nearest hit so
//             far, inside the triangle; only the nearest face is kept
//   spheres   the faces whose plane is within the radius and whose triangle
//             has a point within it (a corner, the centre's foot on the
//             plane, or a point of an edge); every such face is kept
//
// The arithmetic is the original's, in the same order: VU0 where it used VU0
// (zge/math/vec.h), the FPU elsewhere. What a test leaves in VU0 stays there
// for the next vector: the -1 a query leaves is what keeps the skater's
// position w at 1 in moveByVelocity's sum.
//
// The data: the part's faces, positions and face normals and the face tree
// are read from the game's RAM, or, with ZX_NATIVE_COLLISION (on by default), from the
// host's copy made from the GEO file (zge/native/collision_data.h; the same
// bytes, ZX_NATIVE_COLLISION_CHECK shows). What the original hands on
// keeps its EE address either way: tree leaves go to the sorted list by
// their address in the game's tree, and the triangle test gets the
// vertices' addresses when it runs as a game call.

#include "collision.h"
#include "closest.h"
#include "face_tree.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "zge/container/heap.h"
#include "zge/memory/pool.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/native/collision_data.h"
#include "zge/native/host.h"

#include <cstring>
#include "sym/zge/collision/faces.cpp.h"

namespace zge
{
    ee::Ptr<Vec4> transformPoint(ee::Ptr<Vec4> out, ee::Ptr<const Matrix> matrix, ee::Ptr<const Vec4> point); // ZGE2's (zge/math/matrix.h)
    ee::Float lengthSquared(const Vec3 *v); // ZGE2's (zge/math/vector.h)

    namespace
    {
        using namespace face_tree; // the tree, the scratchpad and a part's data: face_tree.h

        // ---- what the face tests call --------------------------------------

        constexpr uint32_t kNodeToWorld = 1;              // nodeMatrix: Node::world
        constexpr uint32_t kWorldToNode = 2;              //   its inverse

        // ---- constants (.sdata) ------------------------------------------------

        // A segment's piece is on a split's lesser side where it starts below
        // +0.01 (and crosses the plane when its end is above -0.01), on the
        // greater side where it starts above -0.01 (and crosses when its end
        // is below +0.01). The two overlap, so the third case, a piece
        // starting in the plane itself, never comes up.
        inline ee::Global<ee::Float, sym::g_lesserBelow> g_lesserBelow;   // 0.01
        inline ee::Global<ee::Float, sym::g_crossesAbove> g_crossesAbove;  // -0.01
        inline ee::Global<ee::Float, sym::g_greaterAbove> g_greaterAbove;  // -0.01
        // Leaf keys are hundredths: key * this is back in units.
        inline ee::Global<ee::Float, sym::g_keyToDistance> g_keyToDistance; // 0.01
        // The sphere test's split planes are plane * this.
        inline ee::Global<ee::Float, sym::g_spherePlaneScale> g_spherePlaneScale; // 0.01
        // pointInTriangle: a normal component within these of 0 is edge-on.
        inline ee::Global<ee::Float, sym::g_edgeOnAbove> g_edgeOnAbove;   // 1e-5
        inline ee::Global<ee::Float, sym::g_edgeOnBelow> g_edgeOnBelow;   // -1e-5
        // ZGE4's pointInTriangle (BMX XXX) lets a point this far on the outer side of an edge count as inside: one
        // per axis looked down, each -0.003 (AI's tests against 0).
        inline ee::Global<ee::Float[3], sym::bmx::g_edgeInsideTolerances> g_edgeInsideTolerances;

        // ---- VU0 arithmetic ----------------------------------------------------

        using ee::vu0::add;
    }

    // ---- pointInTriangleByNormals -----------------------------------------------

    // pointInTriangleByNormals (nothing calls it in AI): whether the point is
    // inside the triangle a, b, c of this normal, tested edge by edge: the
    // unit normal of the triangle the edge makes with the point (each edge's
    // direction crossed with the way on from its end to the point) must be
    // the face's normal within the tolerance on every axis. 1 inside, else 0.
    int32_t pointInTriangleByNormals(ee::Ptr<const Vec4> a, ee::Ptr<const Vec4> b, ee::Ptr<const Vec4> c,
                                     ee::Ptr<const Vec4> normal, ee::Ptr<const Vec4> point, ee::Float tolerance)
    {
        ee::Local<Vec4> side; // the edge's normal (the original's one frame slot)
        const auto inside = [&](const Vec4 &from, const Vec4 &to)
        {
            const Vec4 along = to - from;
            const Vec4 onToPoint = *point - to;
            *side = cross(along, onToPoint);
            *side = unit(*side);
            return ee::abs(side->x - normal->x) < tolerance && ee::abs(side->y - normal->y) < tolerance &&
                   ee::abs(side->z - normal->z) < tolerance;
        };
        if (!inside(*a, *b) || !inside(*b, *c) || !inside(*c, *a))
            return 0;
        return 1;
    }
    READABLE_FUNCTION(pointInTriangleByNormals, Draft);

    // ---- pointInTriangle --------------------------------------------------------

    int32_t pointInTriangle(const Vec4 *a, const Vec4 *b, const Vec4 *c, const Vec4 *normal, const Vec4 *point)
    {
        // Seen down the normal's largest axis, the triangle is a 2D one in
        // the other two (u, v): the point is inside when it is on the inner
        // side of every edge.
        // (The sizes stay in the frame, the original's one slot: a later function's unset local reads them there.)
        ee::Local<Vec4> size;
        *size = abs(*normal);
        const ee::Float nx = size->x, ny = size->y, nz = size->z;
        int axis;
        if (ny <= nx)
            axis = nx < nz ? 2 : 0;
        else
            axis = nz <= ny ? 1 : 2;
        // Which way the triangle winds seen from that side: the inner side of
        // an edge has the sign of the normal's component, flipped. An edge-on
        // normal makes every point inside.
        const ee::Float along = component(*normal, axis);
        int32_t winding = -1;
        if (along <= *g_edgeOnAbove)
            winding = along < *g_edgeOnBelow ? 1 : 0;
        const ee::Float sign = ee::toFloat(winding);

        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const ee::Float pu = component(*point, u), pv = component(*point, v);
        // The point's side of the edge from -> to (the 2D cross product).
        auto inner = [&](const Vec4 &from, const Vec4 &to)
        {
            const ee::Float side = (component(to, v) - component(from, v)) * (pu - component(from, u)) -
                                   (component(to, u) - component(from, u)) * (pv - component(from, v));
            if constexpr (game::readable::kIsBmx) // ZGE4: within its tolerance outside the edge counts as inside
                return !(side * sign < (*g_edgeInsideTolerances)[axis]);
            return !(side * sign < 0.0f);
        };
        // The original has a copy of the tests per axis (the y and z axes'
        // last two shared).
        int32_t inside = 0;
        if (inner(*c, *a))
            inside = inner(*b, *c) && inner(*a, *b) ? 1 : 0;
        return inside;
    }

    // pointInTriangleBySides: ZGE2's pointInTriangle, on the FPU: the same view down the normal's largest axis and
    // the same winding, but the edges are taken in turn (a to c, c to b, b to a), one the point lies on (its side
    // within 1e-5 of 0) does not count, and each other edge's side, as -1, 0 or 1, must be the winding itself.
    // (ZGE3 counts a point on an edge's line as inside by the sign alone, with no tolerance.)
    inline int32_t pointInTriangleBySides(const Vec4 *a, const Vec4 *b, const Vec4 *c, const Vec4 *normal, const Vec4 *point)
    {
        constexpr float kOnEdge = 1e-05f; // (each test its own literal in the original, all 1e-5)
        const ee::Float nx = ee::abs(normal->x), ny = ee::abs(normal->y), nz = ee::abs(normal->z);
        int axis;
        if (ny <= nx)
            axis = nz <= nx ? 0 : 2;
        else
            axis = nz <= ny ? 1 : 2;
        const ee::Float along = component(*normal, axis);
        int32_t winding = -1;
        if (along <= ee::Float(kOnEdge))
            winding = along < ee::Float(-kOnEdge) ? 1 : 0;
        const ee::Float sign = ee::toFloat(winding);

        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const ee::Float pu = component(*point, u), pv = component(*point, v);
        const Vec4 *corner[3] = {a, b, c};
        const Vec4 *from = a;
        for (int i = 2; i >= 0; --i)
        {
            const Vec4 &to = *corner[i];
            const ee::Float side = (component(*from, v) - component(to, v)) * (pu - component(to, u)) -
                                   (component(*from, u) - component(to, u)) * (pv - component(to, v));
            from = &to;
            if (ee::abs(side) < ee::Float(kOnEdge))
                continue;
            ee::Float sideSign = 1.0f;
            if (side <= ee::Float(kOnEdge))
                sideSign = side < ee::Float(-kOnEdge) ? -1.0f : 0.0f;
            if (!(sign == sideSign))
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_VU0_OWN(pointInTriangle, Verified, dm2, pointInTriangleBySides);

    // ---- ZGE2's faces -------------------------------------------------------------------

    namespace
    {
        // A face as ZGE2's faceCorners copies it out (0x48 bytes): its corners, its normal (w 0), a word of 0, and
        // bit 0 of the next word whether it is two-sided (its other bits are left as they were).
        struct FaceCorners
        {
            Vec4 corners[3];     // +0x00
            Vec4 normal;         // +0x30
            uint32_t zero;       // +0x40
            uint32_t twoSided;   // +0x44 bit 0
        };
        static_assert(offsetof(FaceCorners, normal) == 0x30 && offsetof(FaceCorners, twoSided) == 0x44);

        // ZGE2's walk entries (0x40 bytes, in the query state): the subtree's offset; for a segment the piece of
        // it that runs through the subtree, and how far along the segment the piece starts where a split plane
        // cut it (else 0).
        struct TreeWalkEntry
        {
            uint32_t node;          // +0x00
            uint8_t _04[0x0C];
            Vec4 start;             // +0x10
            Vec4 end;               // +0x20
            ee::Float startAlong;   // +0x30
            uint8_t _34[0x0C];
        };
        static_assert(sizeof(TreeWalkEntry) == 0x40 && offsetof(TreeWalkEntry, start) == 0x10 &&
                      offsetof(TreeWalkEntry, startAlong) == 0x30);

        // ZGE2's visited faces: a bit each, eight to a byte, in RAM.
        inline ee::Global<uint8_t, sym::dm2::g_visitedFaceBits> g_visitedFaceBits;

        // A face's record and its normal (ZGE2's normals have no index table).
        const Face &faceRecord(ee::Ptr<ZgePart> part, uint32_t index) { return streamRecords<Face>(part->faces)[index]; }
        const Vec3 &faceNormalOf(ee::Ptr<ZgePart> part, uint32_t index) { return streamRecords<Vec3>(part->faceNormals)[index]; }
    }

    // faceCorners (ZGE2 only): the face's corners (through vertexPosition), its normal and its two-sided bit.
    inline void faceCorners(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<FaceCorners> out)
    {
        ee::Local<Vec4> corner;
        const Face &face = faceRecord(part, index);
        const Vec3 &normal = faceNormalOf(part, index);
        out->normal.x = normal.x;
        out->normal.y = normal.y;
        out->normal.z = normal.z;
        out->normal.w = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            vertexPosition(part, face.vertex[i], corner);
            out->corners[i] = *corner;
        }
        out->zero = 0;
        out->twoSided = (out->twoSided & ~1u) | ((face.flags & kFaceTwoSided) ? 1u : 0u);
    }
    READABLE_FUNCTION_ONLY(dm2, faceCorners);

    // ---- segmentFaceTree ----------------------------------------------------------

    // (ZGE3 and ZGE4 are handed the frame's tree field, ZGE2 the tree itself; each has none of the other.)
    int32_t segmentFaceTree(const QuerySegment *segment, const QuerySegment *clipped
                            READABLE_PARAM_EXCEPT(dm2, ee::Ptr<ee::Ptr<uint8_t>> treeSlot = {})
                            READABLE_PARAM_ONLY(dm2, ee::Ptr<uint8_t> tree = {}),
                            ee::Ptr<ZgePart> part, ee::Ptr<SphereQuery> query, const Matrix *toWorld)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<Vec4> directionSlot;
        ee::Local<WalkEntry> pieceSlot;
        ee::Local<Vec4> crossingSlot;
        ee::Local<Vec4> facePoint;
        ee::Local<Vec4> startSlot;
        ee::Local<Vec4> faceNormal;
        ee::Local<uint32_t> filterFace;
        // A piece cut at a split plane, its two parts as the original builds
        // them before it pushes them: the far one (from the crossing) and the
        // near one (to it). They stay on the stack after the walk, where the
        // next node's sphere and box walks find them.
        ee::Local<WalkEntry> farPiece;
        ee::Local<WalkEntry> nearPiece;
        Vec4 &direction = *directionSlot;
        Vec4 &start = *startSlot;

        const ee::Ptr<uint8_t> faceTree = game::readable::kIsDm2 ? tree : *treeSlot;
        const MeshSource mesh(part, faceTree);
        int32_t hit = 0;
        clearVisited(mesh.faceCount());

        // The segment's direction (unit length) and length, as VU0 works them out.
        direction = segment->end - segment->start;
        ee::Float length;
        direction = unit(direction, length);

        // Walk the tree front to back with the clipped segment, cutting it
        // at each split plane it crosses; leaves go to the sorted list keyed
        // by how far along the segment their piece starts.
        WalkEntry *stack = *g_walkStack;
        std::memcpy(&stack[0].start, &clipped->start, sizeof(Vec4));
        std::memcpy(&stack[0].end, &clipped->end, sizeof(Vec4));
        stack[0].startAlong = 0.0f;
        stack[0].node = 0;
        uint32_t top = 1;
        uint32_t leaves = 0;
        do
        {
            const WalkEntry &piece = *pieceSlot = stack[--top];
            const ee::Ptr<uint8_t> place = faceTree + piece.node;
            const FaceTreeLeaf &leaf = mesh.node(place);
            if (leaf.axis == kFaceTreeLeaf)
            {
                if (leaf.count)
                {
                    priorityQueueInsert(ee::toInt(piece.startAlong * 100.0f), ee::Address(place), kLeafQueue);
                    leaves = (leaves + 1u) & 0xFFFFu;
                }
                continue;
            }
            const FaceTreeSplit &split = mesh.split(place);
            const ee::Float plane = ee::toFloat(split.plane) * *g_faceTreePlaneScale;
            const int k = splitCoordinate(split.axis);
            const ee::Float startOffset = component(piece.start, k) - plane;
            const ee::Float endOffset = component(piece.end, k) - plane;
            const ee::Float segmentOffset = component(segment->start, k) - plane;
            const ee::Float towards = component(direction, k);

            auto push = [&](const Vec3 &start, ee::Float startAlong, const Vec3 &end, uint16_t child)
            {
                WalkEntry &entry = stack[top++];
                entry.start = start;
                entry.startAlong = startAlong;
                entry.end = end;
                entry.node = child * 4u;
            };
            // Where the piece crosses the plane: the far part (pushed first,
            // visited last) starts there, the near part ends there.
            auto cut = [&](uint16_t farChild, uint16_t nearChild)
            {
                crossingSlot = direction * (-startOffset / towards);
                const Vec4 &crossing = *crossingSlot = piece.startLanes() + crossingSlot;
                // Whole quadwords: each half takes the lane after it along
                // (the crossing's w, the piece's startAlong and node).
                std::memcpy(&farPiece->start, &crossing, sizeof(Vec4));
                std::memcpy(&farPiece->end, &piece.end, sizeof(Vec4));
                std::memcpy(&nearPiece->start, &piece.start, sizeof(Vec4));
                std::memcpy(&nearPiece->end, &crossing, sizeof(Vec4));
                push(farPiece->start, -segmentOffset / towards, farPiece->end, farChild);
                push(nearPiece->start, 0.0f, nearPiece->end, nearChild);
            };
            const auto whole = [&](uint16_t child) { push(piece.start, 0.0f, piece.end, child); };
            if (startOffset < *g_lesserBelow)
            {
                if (*g_crossesAbove < endOffset)
                    cut(greaterSide(split), lesserSide(split));
                else
                    whole(lesserSide(split));
            }
            else if (*g_greaterAbove < startOffset)
            {
                if (endOffset < *g_lesserBelow)
                    cut(lesserSide(split), greaterSide(split));
                else
                    whole(greaterSide(split));
            }
            else if (endOffset < *g_lesserBelow)
                whole(lesserSide(split));
            else if (*g_greaterAbove < endOffset)
                whole(greaterSide(split));
            else
            {
                whole(greaterSide(split));
                whole(lesserSide(split));
            }
        } while (top);
        if (!leaves)
            return hit;

        start = segment->start;
        faceNormal->w = 0.0f;

        // One face of a leaf.
        QueryState &state = *g_queryState;
        auto testFace = [&](uint16_t index)
        {
            if (!visitOnce(index))
                return;
            const Face &face = mesh.face(index);
            if (query->filter)
            {
                *filterFace = index;
                if (!filterKeeps(*query, state.node, part, filterFace))
                    return;
            }
            if (!(face.flags & kFaceCollides) && !(query->flags & kQueryAllFaces))
                return;
            const Vec3 &normal = mesh.normal(index);
            faceNormal->x = normal.x;
            faceNormal->y = normal.y;
            faceNormal->z = normal.z;

            // The start's height over the face's plane, and how fast the
            // segment closes on it: faces are hit from the front (from
            // behind too when two-sided), between start and end, and
            // only nearer than the nearest hit yet.
            facePoint = start - mesh.position(face.vertex[0]);
            const ee::Float height = zge::dot(facePoint, faceNormal);
            if (height <= 0.0f && !(face.flags & kFaceTwoSided))
                return;
            const ee::Float closing = zge::dot(direction, faceNormal);
            const ee::Float fraction = height / (-closing * length);
            if (fraction < 0.0f || 1.0f < fraction || state.nearest < fraction)
                return;
            const ee::Float distance = fraction * length;
            facePoint = direction * distance;
            facePoint = facePoint + start;
            if (!faceContains(mesh, face, faceNormal, facePoint))
                return;

            // The nearest face yet: the node's hit record is made again
            // (its pool emptied) with this one FaceHit. (The hit node is
            // not checked: [0] writes where the pointer points, 0 too.)
            hit = 1;
            if (state.hitNode)
            {
                const HitNode kept = *state.hitNode;
                poolReset(state.hitNodePool);
                const ee::Ptr<HitNode> fresh = poolAllocAs<HitNode>(state.hitNodePool);
                *fresh = kept;
                state.hitNode = fresh;
            }
            state.hitNode[0].faceCount = 1;
            state.faceCount = 1;
            state.hitNode[0].faces = state.facesBuffer;
            FaceHit &faceHit = state.hitNode[0].faces[0];
            faceHit.distance = distance;
            faceHit.face = index;
            state.nearest = fraction;
            if (toWorld && (query->flags & kQueryWorldPoints))
                faceHit.point = transformed(*toWorld, facePoint);
            else
                faceHit.point = facePoint;
        };

        for (uint32_t taken = 0; taken < leaves; ++taken)
        {
            // Leaves come nearest first: once one starts beyond the nearest
            // hit so far, nothing further on can be nearer.
            const ee::Ptr<SortedEntry> first = priorityQueuePopAs<SortedEntry>(kLeafList);
            if (state.nearest * length < ee::toFloat(first->key) * *g_keyToDistance)
            {
                priorityQueueClear(kLeafList);
                return hit;
            }
            const FaceTreeLeaf &leaf = mesh.node(first->record);
            const uint16_t *leafFace = mesh.leafFaces(leaf);
            for (uint32_t n = 0; n < leaf.count; ++n)
                testFace(leafFace[n]);
        }
        return hit;
    }

    namespace
    {
        // A piece of a segment, as ZGE2's walk copies it into its frame.
        struct SegmentPiece
        {
            Vec4 start;
            Vec4 end;
        };
    }

    // segmentFaceTreeByCorners: ZGE2's segmentFaceTree: the same walk and face test (with split
    // planes 0.001 thick, not 0.01), with its own state: the walk stack, the HitNodes and the
    // FaceHits in the query state, the visited bits in RAM, a leaf's faces right after its count,
    // each face through faceCorners and no face flags test but its two sides; a leaf's key by
    // libgcc's unsigned conversion. The nearest face yet leaves the query one HitNode, this one,
    // with this one FaceHit, the first of the state's.
    int32_t segmentFaceTreeByCorners(const QuerySegment *segment, const QuerySegment *clipped, ee::Ptr<uint8_t> tree,
                                     ee::Ptr<ZgePart> part, ee::Ptr<SphereQuery> query, const Matrix *toWorld)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<Vec4> facePoint;   // the segment's start from the face's first corner
        ee::Local<Vec4> directionSlot;
        ee::Local<SegmentPiece> piece;
        // A piece cut at a split plane, its two parts as the original builds them before it pushes
        // them: the far one (from the crossing) and the near one (to it).
        ee::Local<SegmentPiece> farPiece;
        ee::Local<SegmentPiece> nearPiece;
        ee::Local<Vec4> crossing;
        ee::Local<FaceCorners> face;
        ee::Local<uint32_t> filterFace;
        ee::Local<Vec4> hitPoint;
        Vec4 &direction = *directionSlot;

        int32_t hit = 0;
        uint32_t leaves = 0;
        memoryFill(&*g_visitedFaceBits, 0, (part->faceCount + 7u) / 8u);

        // The segment's direction (unit length) and length, as VU0 works them out.
        direction = segment->end - segment->start;
        ee::Float length;
        direction = unit(direction, length);

        // Walk the tree front to back with the clipped segment, cutting it at each split plane it
        // crosses; leaves go to the sorted list keyed by how far along the segment their piece starts.
        QueryState &state = *g_queryState;
        const ee::Ptr<TreeWalkEntry> stack(ee::Ptr<uint8_t>(state.walkStack).cast<TreeWalkEntry>());
        stack[0].startAlong = 0.0f;
        stack[0].start = clipped->start;
        stack[0].end = clipped->end;
        stack[0].node = 0;
        uint32_t top = 1;
        do
        {
            const TreeWalkEntry &entry = stack[--top];
            piece->start = entry.start;
            piece->end = entry.end;
            const ee::Float startAlong = entry.startAlong;
            const ee::Ptr<uint8_t> place = tree + entry.node;
            if (place.cast<FaceTreeLeaf>()->axis == kFaceTreeLeaf)
            {
                if (place.cast<InlineLeafTree>()->count != 0)
                {
                    priorityQueueInsert(sce::libgcc::fptoui(startAlong * 100.0f), ee::Address(place), kLeafQueue);
                    leaves = (leaves + 1u) & 0xFFFFu;
                }
                continue;
            }
            const FaceTreeSplit &split = *place.cast<FaceTreeSplit>();
            const ee::Float plane = ee::toFloat(split.plane) * *g_faceTreePlaneScale;
            const int k = splitCoordinate(split.axis);
            const ee::Float startOffset = component(piece->start, k) - plane;
            const ee::Float segmentOffset = component(segment->start, k) - plane;
            const ee::Float endOffset = component(piece->end, k) - plane;
            const ee::Float towards = component(direction, k);

            auto push = [&](const Vec4 &start, ee::Float startAlong, const Vec4 &end, uint16_t child)
            {
                TreeWalkEntry &next = stack[top++];
                next.start = start;
                next.end = end;
                next.startAlong = startAlong;
                next.node = child * 4u;
            };
            // Where the piece crosses the plane: the far part (pushed first, visited last) starts
            // there, the near part ends there.
            auto cut = [&](uint16_t farChild, uint16_t nearChild)
            {
                crossing = direction * (-startOffset / towards);
                crossing = piece->start + crossing;
                farPiece->start = *crossing;
                farPiece->end = piece->end;
                nearPiece->start = piece->start;
                nearPiece->end = *crossing;
                push(farPiece->start, -segmentOffset / towards, farPiece->end, farChild);
                push(nearPiece->start, 0.0f, nearPiece->end, nearChild);
            };
            const auto whole = [&](uint16_t child) { push(piece->start, 0.0f, piece->end, child); };
            if (startOffset < *g_lesserBelow)
            {
                if (*g_crossesAbove < endOffset)
                    cut(greaterSide(split), lesserSide(split));
                else
                    whole(lesserSide(split));
            }
            else if (*g_greaterAbove < startOffset)
            {
                if (endOffset < *g_lesserBelow)
                    cut(lesserSide(split), greaterSide(split));
                else
                    whole(greaterSide(split));
            }
            else if (endOffset < *g_lesserBelow)
                whole(lesserSide(split));
            else if (*g_greaterAbove < endOffset)
                whole(greaterSide(split));
            else
            {
                whole(greaterSide(split));
                whole(lesserSide(split));
            }
        } while (top);
        if (!leaves)
            return hit;

        for (uint32_t taken = 0; taken < leaves; ++taken)
        {
            // Leaves come nearest first: once one starts beyond the nearest hit so far, nothing
            // further on can be nearer.
            const ee::Ptr<SortedEntry> first = priorityQueuePopAs<SortedEntry>(kLeafList);
            if (state.nearest * length < ee::toFloat(first->key) * *g_keyToDistance)
            {
                priorityQueueClear(kLeafList);
                return hit;
            }
            const ee::Ptr<InlineLeafTree> leaf = first->record.cast<InlineLeafTree>();
            for (uint32_t n = 0; n < leaf->count; ++n)
            {
                const uint16_t index = leaf->faces[n];
                uint8_t &visited = (&*g_visitedFaceBits)[index >> 3];
                const uint8_t bit = static_cast<uint8_t>(1u << (index & 7u));
                if (visited & bit)
                    continue;
                visited = static_cast<uint8_t>(visited | bit);
                *filterFace = index;
                if (query->filter && !filterKeeps(*query, state.node, part, filterFace))
                    continue;
                faceCorners(part, index, face);

                // The start's height over the face's plane, and how fast the segment closes on it:
                // faces are hit from the front (from behind too when two-sided), between start and
                // end, and only nearer than the nearest hit yet.
                facePoint = segment->start - face->corners[0];
                const ee::Float height = zge::dot(facePoint, face->normal);
                const ee::Float closing = zge::dot(direction, face->normal);
                if (height <= 0.0f && !(face->twoSided & 1u))
                    continue;
                const ee::Float fraction = height / (-closing * length);
                if (fraction < 0.0f || 1.0f < fraction || state.nearest < fraction)
                    continue;
                const ee::Float distance = fraction * length;
                hitPoint = direction * distance;
                hitPoint = hitPoint + segment->start;
                if (!pointInTriangle(&face->corners[0], &face->corners[1], &face->corners[2], &face->normal, hitPoint))
                    continue;

                // The nearest face yet: the query keeps one HitNode, this one (moved to the first
                // slot when others were recorded since), with this one FaceHit.
                hit = 1;
                if (static_cast<uint32_t>(state.hitCount) >= 2u)
                {
                    state.hitNodes[0] = *state.hitNode;
                    state.hitNode = &state.hitNodes[0];
                }
                state.hitCount = 1;
                state.hitNode->faceCount = 1;
                state.hitNode->faces = &state.faces[0];
                FaceHit &faceHit = state.faces[0];
                faceHit.distance = distance;
                state.nearest = fraction;
                faceHit.face = leaf->faces[n];
                if (toWorld && (query->flags & kQueryWorldPoints))
                    transformPoint(&faceHit.point, toWorld, hitPoint);
                else
                    faceHit.point = *hitPoint;
            }
        }
        return hit;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_VU0_OWN(segmentFaceTree, Verified, dm2, segmentFaceTreeByCorners)); // (BMX XXX's: game/bmx/collision/face_tree.cpp)

    // ---- sphereFaceTree -------------------------------------------------------------

    int32_t sphereFaceTree(const QuerySphere *sphere, ee::Ptr<uint8_t> tree, ee::Ptr<ZgePart> part,
                           ee::Ptr<SphereQuery> query, const Matrix *toWorld)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<uint32_t> filterFace;
        ee::Local<Vec4> foot;      // the centre's foot on the face's plane
        ee::Local<Vec4> scratch;
        ee::Local<Vec4> toFootSlot;
        ee::Local<Vec4> edgePoint; // an edge's nearest point
        ee::Local<Vec4[3]> corner;
        ee::Local<Vec4> faceNormal;

        WalkEntry *stack = *g_walkStack;
        stack[0].node = 0;
        QueryState &state = *g_queryState;
        state.hitNode[0].faceCount = 0;
        int32_t hit = 0;
        const MeshSource mesh(part, tree);
        clearVisited(mesh.faceCount());

        // Walk the tree into every side the sphere reaches; leaves go to the
        // list (all with the same key).
        uint32_t top = 1;
        uint32_t leaves = 0;
        do
        {
            const ee::Ptr<uint8_t> place = tree + stack[--top].node;
            const FaceTreeLeaf &leaf = mesh.node(place);
            if (leaf.axis == kFaceTreeLeaf)
            {
                if (leaf.count)
                {
                    ++leaves;
                    priorityQueueInsert(1, ee::Address(place), kLeafQueue);
                }
                continue;
            }
            const FaceTreeSplit &split = mesh.split(place);
            const ee::Float plane = ee::toFloat(split.plane) * *g_spherePlaneScale;
            ee::Float centre = 0.0f;
            if (split.axis >= kFaceTreeSplitX && split.axis <= kFaceTreeSplitZ)
                centre = component(sphere->centre, split.axis - 1);
            if (ee::abs(centre - plane) < sphere->radius)
            {
                stack[top++].node = greaterSide(split) * 4u;
                stack[top++].node = lesserSide(split) * 4u;
            }
            else
                stack[top++].node = (centre <= plane ? lesserSide(split) : greaterSide(split)) * 4u;
        } while (top);
        if (!leaves)
            return hit;

        // One face of a leaf.
        auto testFace = [&](uint16_t index)
        {
            if (!visitOnce(index))
                return;
            *filterFace = index;
            if (query->filter && !filterKeeps(*query, state.node, part, filterFace))
                return;

            const Face &face = mesh.face(index);
            for (int i = 0; i < 3; ++i)
                corner[i] = mesh.position(face.vertex[i]);
            const Vec3 &normal = mesh.normal(index);
            faceNormal->x = normal.x;
            faceNormal->y = normal.y;
            faceNormal->z = normal.z;
            faceNormal->w = 0.0f;
            if (!(face.flags & kFaceCollides) && !(query->flags & kQueryAllFaces))
                return;

            // The centre's height over the face's plane (either side):
            // within the radius, the sphere cuts the plane in a circle of
            // squared radius reach2 around the centre's foot.
            scratch = sphere->centre - corner[0];
            const ee::Float height = zge::dot(faceNormal, scratch);
            if (height < 0.0f ? height < -sphere->radius : sphere->radius < height)
                return;
            scratch = faceNormal * height;
            foot = sphere->centre - scratch;
            const ee::Float reach2 = sphere->radius * sphere->radius - height * height;
            auto within = [&](const Vec4 &p)
            {
                toFootSlot = p - foot;
                return squaredLength(toFootSlot) <= reach2;
            };

            // The point hit: a corner in the circle, else the foot if it is
            // inside the triangle, else an edge's point nearest the foot.
            Vec4 hitPoint;
            bool found = false;
            for (int i = 0; i < 3 && !found; ++i)
            {
                if (within(corner[i]))
                {
                    hitPoint = corner[i];
                    found = true;
                }
            }
            if (!found)
            {
                if (pointInTriangle(&corner[0], &corner[1], &corner[2], faceNormal, foot))
                {
                    hitPoint = foot;
                    found = true;
                }
                else
                {
                    for (int i = 0; i < 3 && !found; ++i)
                    {
                        closestPointOnSegment(&corner[i], &corner[(i + 1) % 3], nullptr, foot, edgePoint);
                        if (within(edgePoint))
                        {
                            hitPoint = edgePoint;
                            found = true;
                        }
                    }
                    if (!found)
                        return;
                }
            }

            // Every face hit is kept.
            hit = 1;
            HitNode &record = state.hitNode[0];
            FaceHit &faceHit = record.faces[record.faceCount];
            faceHit.distance = ee::abs(height);
            faceHit.face = index;
            if (toWorld && (query->flags & kQueryWorldPoints))
                faceHit.point = transformed(*toWorld, hitPoint);
            else
                faceHit.point = hitPoint;
            state.hitNode[0].faceCount += 1;
            state.faceCount += 1;
        };

        for (uint32_t taken = 0; taken < leaves; ++taken)
        {
            const ee::Ptr<SortedEntry> first = priorityQueuePopAs<SortedEntry>(kLeafList);
            const FaceTreeLeaf &leaf = mesh.node(first->record);
            const uint16_t *leafFace = mesh.leafFaces(leaf);
            for (uint32_t n = 0; n < leaf.count; ++n)
                testFace(leafFace[n]);
        }
        return hit;
    }

    // ---- ZGE2's sphere test ----------------------------------------------------------

    // sphereFaceTreeByCorners: ZGE2's sphereFaceTree: the same walk and face test, with its own state: the walk
    // stack and the face hits in the query state, the visited bits in RAM, a leaf's faces right after its count,
    // each face through faceCorners and no face flags test; every face hit is added to the query's face array.
    int32_t sphereFaceTreeByCorners(const QuerySphere *sphere, ee::Ptr<uint8_t> tree, ee::Ptr<ZgePart> part,
                                    ee::Ptr<SphereQuery> query, const Matrix *toWorld)
    {
        ee::Local<uint32_t> filterFace;
        ee::Local<Vec4> foot;      // the centre's foot on the face's plane
        ee::Local<Vec4> scratch;   // also the point hit
        ee::Local<Vec4> toFootSlot;
        ee::Local<Vec4> edgePoint; // an edge's nearest point
        ee::Local<FaceCorners> face;

        QueryState &state = *g_queryState;
        const ee::Ptr<TreeWalkEntry> stack(ee::Ptr<uint8_t>(state.walkStack).cast<TreeWalkEntry>());
        stack[0].node = 0;
        state.hitNode->faceCount = 0;
        state.hitNode->faces = &state.faces[state.faceCount];
        uint32_t leaves = 0;
        int32_t hit = 0;
        memoryFill(&*g_visitedFaceBits, 0, (part->faceCount + 7u) / 8u);

        uint32_t top = 1;
        while (top != 0)
        {
            const ee::Ptr<uint8_t> place = tree + stack[--top].node;
            if (place.cast<FaceTreeLeaf>()->axis == kFaceTreeLeaf)
            {
                if (place.cast<InlineLeafTree>()->count != 0)
                {
                    ++leaves;
                    priorityQueueInsert(1, ee::Address(place), kLeafQueue);
                }
                continue;
            }
            const FaceTreeSplit &split = *place.cast<FaceTreeSplit>();
            const ee::Float plane = ee::toFloat(split.plane) * *g_spherePlaneScale;
            ee::Float centre = 0.0f;
            if (split.axis >= kFaceTreeSplitX && split.axis <= kFaceTreeSplitZ)
                centre = component(sphere->centre, split.axis - 1);
            if (ee::abs(centre - plane) < sphere->radius)
            {
                stack[top++].node = greaterSide(split) * 4u;
                stack[top++].node = lesserSide(split) * 4u;
            }
            else
                stack[top++].node = (centre <= plane ? lesserSide(split) : greaterSide(split)) * 4u;
        }
        if (!leaves)
            return hit;

        for (uint32_t taken = 0; taken < leaves; ++taken)
        {
            const ee::Ptr<InlineLeafTree> leaf = priorityQueuePopAs<SortedEntry>(kLeafList)->record.cast<InlineLeafTree>();
            for (uint32_t n = 0; n < leaf->count; ++n)
            {
                const uint16_t index = leaf->faces[n];
                uint8_t &visited = (&*g_visitedFaceBits)[index >> 3];
                const uint8_t bit = static_cast<uint8_t>(1u << (index & 7u));
                if (visited & bit)
                    continue;
                visited = static_cast<uint8_t>(visited | bit);
                *filterFace = index;
                if (query->filter && !filterKeeps(*query, state.node, part, filterFace))
                    continue;
                faceCorners(part, index, face);

                // The centre's height over the face's plane (either side), and the circle the sphere cuts in it.
                scratch = sphere->centre - face->corners[0];
                const ee::Float height = zge::dot(face->normal, scratch);
                if (height < 0.0f ? height < -sphere->radius : sphere->radius < height)
                    continue;
                scratch = face->normal * height;
                foot = sphere->centre - scratch;
                const ee::Float reach2 = sphere->radius * sphere->radius - height * height;
                auto within = [&](const Vec4 &p)
                {
                    toFootSlot = p - foot;
                    return lengthSquared(&toFootSlot->xyz()) <= reach2;
                };

                // A corner in the circle, else the foot inside the triangle, else an edge's point nearest it.
                bool found = false;
                for (int i = 0; i < 3 && !found; ++i)
                {
                    if (within(face->corners[i]))
                    {
                        *scratch = face->corners[i];
                        found = true;
                    }
                }
                if (!found)
                {
                    if (pointInTriangle(&face->corners[0], &face->corners[1], &face->corners[2], &face->normal, foot))
                    {
                        *scratch = *foot;
                        found = true;
                    }
                    else
                    {
                        for (int i = 0; i < 3 && !found; ++i)
                        {
                            closestPointOnSegment(&face->corners[i], &face->corners[(i + 1) % 3], nullptr, foot, edgePoint);
                            if (within(edgePoint))
                            {
                                *scratch = *edgePoint;
                                found = true;
                            }
                        }
                        if (!found)
                            continue;
                    }
                }

                // Every face hit is kept, at the end of the query's face array.
                hit = 1;
                state.hitNode->faceCount += 1;
                FaceHit &faceHit = state.faces[state.faceCount];
                faceHit.distance = ee::abs(height);
                faceHit.face = leaf->faces[n];
                if (toWorld && (query->flags & kQueryWorldPoints))
                    transformPoint(&faceHit.point, toWorld, scratch);
                else
                    faceHit.point = *scratch;
                state.faceCount += 1;
            }
        }
        return hit;
    }
    READABLE_FUNCTION_OWN(sphereFaceTree, Verified, dm2, sphereFaceTreeByCorners);

    // ---- the box test's helper --------------------------------------------------------

    // triangleExtentAlong: a triangle's reach along an axis: the middle of its corners'
    // dots with the axis and how far they go from it.
    void triangleExtentAlong(const Vec4 *corners, const Vec4 *axis, ee::Float *centre, ee::Float *extent)
    {
        const ee::Float first = zge::dot(*axis, corners[0]);
        ee::Float lowest = first;
        ee::Float highest = first;
        const ee::Float second = zge::dot(*axis, corners[1]);
        if (second < highest)
            lowest = second;
        if (highest < second)
            highest = second;
        const ee::Float third = zge::dot(*axis, corners[2]);
        if (third < lowest)
            lowest = third;
        if (highest < third)
            highest = third;
        const ee::Float middle = (highest + lowest) * ee::Float(0.5f);
        *centre = middle;
        *extent = highest - middle;
    }
    READABLE_FUNCTION(triangleExtentAlong, Verified);

    // ---- testNodeFaces ----------------------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        int32_t boxFaceTree(ee::Address box, ee::Ptr<uint8_t> tree, ee::Ptr<ZgePart> part, ee::Ptr<SphereQuery> query, ee::Ptr<Matrix> toWorld, ee::Address boxToNode) { return ee::game<int32_t>(sym::boxFaceTree, box, tree, part, query, toWorld, boxToNode); }
        // The shape in the node's space, each kind in its place.
        struct NodeSpaceShape
        {
            QuerySegment segment;    // +0x000
            QuerySegment clipped;    // +0x020
            QuerySphere sphere;      // +0x040
            QueryBox box;            // +0x060
            Matrix boxToNode;        // +0x080
            QueryOrientedBox swept;  // +0x0C0
            Matrix sweptToNode;      // +0x170 the box's matrix by the node's, turned only, the node's translation after
            Matrix turnOnly;         // +0x1B0 the box's
            Matrix nodeTurnOnly;     // +0x1F0 the node's

            // ZGE2 has the segment and the sphere only: DM2's frame holds the shape up to the box.
            static constexpr uint32_t frameBytes()
            {
                return game::readable::perGame<uint32_t>(sizeof(NodeSpaceShape), offsetof(NodeSpaceShape, box));
            }
        };
        static_assert(offsetof(NodeSpaceShape, sphere) == 0x40 && offsetof(NodeSpaceShape, box) == 0x60 && offsetof(NodeSpaceShape, boxToNode) == 0x80 && offsetof(NodeSpaceShape, swept) == 0xC0 &&
                      offsetof(NodeSpaceShape, sweptToNode) == 0x170 && offsetof(NodeSpaceShape, turnOnly) == 0x1B0 && offsetof(NodeSpaceShape, nodeTurnOnly) == 0x1F0);
    }

    // testNodeFaces: a node against a query's shape: the shape brought into the
    // node's space and given to the face test of its kind. Returns whether
    // any face was hit.
    int32_t testNodeFaces(ee::Ptr<Node> node, ee::Ptr<CollisionFrame> collision, ee::Ptr<ZgePart> part,
                          ee::Ptr<SphereQuery> query)
    {
        ee::Local<NodeSpaceShape> shape;

        // The frame's tree field (frameTree hands back its address).
        const ee::Ptr<ee::Ptr<uint8_t>> treeSlot = frameTree(collision);
        if (!treeSlot)
            return 0;
        CollisionFrame &owner = *collision;
        // A mesh part without a tree gets one: a single leaf with every face.
        if (!owner.tree && part->header == 0)
        {
            if constexpr (game::readable::kIsDm2)
            {
                // ZGE2's leaf: the face indices right after the count.
                const uint32_t bytes = part->faceCount * 2u + offsetof(InlineLeafTree, faces);
                owner.treeBytes = bytes;
                const uint32_t block = heapAllocate(bytes);
                owner.tree = ee::Ptr<uint8_t>(block);
                const ee::Ptr<InlineLeafTree> whole(block);
                whole->axis = kFaceTreeLeaf;
                whole->count = part->faceCount;
                for (uint32_t i = 0; i < part->faceCount; ++i)
                    whole->faces[i] = static_cast<uint16_t>(i);
            }
            else
            {
                const uint32_t bytes = part->faceCount * 2u + sizeof(FaceTreeLeaf);
                owner.treeBytes = bytes;
                const uint32_t block = heapAllocate(bytes);
                owner.tree = ee::Ptr<uint8_t>(block);
                const ee::Ptr<WholePartTree> whole(block);
                whole->leaf.axis = kFaceTreeLeaf;
                whole->leaf.faces = sizeof(FaceTreeLeaf) / 2u; // the indices follow it
                whole->leaf.count = part->faceCount;
                for (uint32_t i = 0; i < part->faceCount; ++i)
                    whole->faces[i] = static_cast<uint16_t>(i);
            }
        }
        if (!owner.tree)
            return 0;

        QueryState &state = *g_queryState;
        switch (static_cast<CollisionKind>(query->kind))
        {
        case CollisionKind::Segment:
        {
            // A node further along the segment than the nearest face yet
            // cannot have a nearer one.
            if (state.nearest < query->hit->fraction)
                return 0;
            const Matrix &toNode = *nodeMatrix(node, kWorldToNode);
            const ee::Ptr<Matrix> toWorld = nodeMatrix(node, kNodeToWorld);
            if constexpr (game::readable::kIsDm2) // (ZGE2 through its transformPoint, ZGE3 inline)
            {
                transformPoint(&shape->segment.start, &toNode, &query->hit->segment.start);
                transformPoint(&shape->segment.end, &toNode, &query->hit->segment.end);
                transformPoint(&shape->clipped.start, &toNode, &query->hit->clipped.start);
                transformPoint(&shape->clipped.end, &toNode, &query->hit->clipped.end);
            }
            else
            {
                shape->segment.start = transformed(toNode, query->hit->segment.start);
                shape->segment.end = transformed(toNode, query->hit->segment.end);
                shape->clipped.start = transformed(toNode, query->hit->clipped.start);
                shape->clipped.end = transformed(toNode, query->hit->clipped.end);
            }
            state.node = node;
            return segmentFaceTree(&shape->segment, &shape->clipped READABLE_PARAM_EXCEPT(dm2, treeSlot) READABLE_PARAM_ONLY(dm2, owner.tree),
                                   part, query, toWorld);
        }
        case CollisionKind::Sphere:
        {
            shape->sphere = queryShape<QuerySphere>(*query);
            const Matrix &toNode = *nodeMatrix(node, kWorldToNode);
            const ee::Ptr<Matrix> toWorld = nodeMatrix(node, kNodeToWorld);
            state.node = node;
            if constexpr (game::readable::kIsDm2)
                transformPoint(&shape->sphere.centre, &toNode, &queryShape<QuerySphere>(*query).centre);
            else
                shape->sphere.centre = transformed(toNode, queryShape<QuerySphere>(*query).centre);
            return sphereFaceTree(&shape->sphere, owner.tree, part, query, toWorld);
        }
        case CollisionKind::Box:
        {
            if constexpr (game::readable::kIsDm2) // (ZGE2 has the sphere and the segment only)
                return 0;
            shape->box = queryShape<QueryBox>(*query);
            const ee::Ptr<Matrix> toWorld = nodeMatrix(node, kNodeToWorld);
            matrixInvertRigid(&shape->boxToNode, toWorld);
            state.node = node;
            return boxFaceTree(&shape->box, owner.tree, part, query, toWorld, &shape->boxToNode);
        }
        case CollisionKind::SweptBox:
        {
            if constexpr (game::readable::kIsDm2)
                return 0;
            QueryOrientedBox &swept = shape->swept;
            swept = queryShape<QueryOrientedBox>(*query);
            const Matrix &toNode = *nodeMatrix(node, kWorldToNode);
            const ee::Ptr<Matrix> toWorld = nodeMatrix(node, kNodeToWorld);
            state.node = node;
            // The box's matrix and the node's, without translations, multiplied,
            // the node's translation after it; the box's ends moved by its
            // translation.
            shape->turnOnly = swept.matrix;
            shape->nodeTurnOnly = toNode;
            matrixClearTranslation(&shape->turnOnly);
            matrixClearTranslation(&shape->nodeTurnOnly);
            const Vec4 &offset = matrixRow(swept.matrix, 3);
            auto moveBy = [](Vec4 &v, const Vec4 &by) { v = {add(v.x, by.x), add(v.y, by.y), add(v.z, by.z), add(v.w, by.w)}; };
            moveBy(swept.start, offset);
            moveBy(swept.end, offset);
            matrixMultiply(&shape->sweptToNode, &shape->turnOnly, &shape->nodeTurnOnly);
            matrixRow(shape->sweptToNode, 3) = matrixRow(toNode, 3);
            return boxFaceTree(&swept.start, owner.tree, part, query, toWorld, &shape->sweptToNode);
        }
        }
        return 0;
    }
    READABLE_FUNCTION(testNodeFaces, Verified);

    namespace
    {
        // closestOnSegment (a drafted engine function): the point of segment
        // a-b nearest `point`, into `out`.
        void closestOnSegment(Vec4 &out, const Vec4 &a, const Vec4 &b, const Vec4 &point)
        {
            ee::game(sym::closestOnSegment, out, a, b, point);
        }
    }

    // trianglePointInSphere: a point of a triangle (its corners and its
    // plane's unit normal) inside a sphere, into `out`, or 0 when it has
    // none. Measured on the plane: the sphere cuts it in a circle of squared
    // radius reach2 around the centre's foot. The point is the foot when it
    // is inside the triangle, else the first corner in the circle (the third
    // corner first), else the first edge's point nearest the foot that is in
    // it (edges 1-3, 3-2, 2-1). It does not test the plane's height itself:
    // with the foot inside the triangle the foot is given back however far
    // the plane is. (Nothing calls it in AI or DM2.)
    ee::Ptr<Vec4> trianglePointInSphere(ee::Ptr<Vec4> out, ee::Ptr<const Vec4> centre, ee::Ptr<const Vec4> corner0,
                                         ee::Ptr<const Vec4> corner1, ee::Ptr<const Vec4> corner2, ee::Ptr<const Vec4> normal,
                                         ee::Float radius)
    {
        ee::Local<Vec4> foot;      // the centre's foot on the plane
        ee::Local<Vec4> edgePoint; // an edge's point nearest the foot, then its offset from the foot
        const ee::Ptr<const Vec4> corners[3] = {corner0, corner1, corner2};

        // The foot's w stays the centre's less the first corner's.
        *foot = *centre - *corner0;
        const ee::Float height = dot(*foot, *normal);
        const ee::Float reach2 = radius * radius - height * height;
        foot->x = centre->x - normal->x * height;
        foot->y = centre->y - normal->y * height;
        foot->z = centre->z - normal->z * height;
        if (pointInTriangle(corner0, corner1, corner2, normal, foot))
        {
            *out = *foot;
            return out;
        }

        for (int i = 2; i >= 0; --i)
        {
            if (squaredLength(*corners[i] - *foot) < reach2)
            {
                *out = *corners[i];
                return out;
            }
        }

        ee::Ptr<const Vec4> from = corners[0];
        for (int i = 2; i >= 0; --i)
        {
            const ee::Ptr<const Vec4> to = corners[i];
            closestOnSegment(*edgePoint, *from, *to, *foot);
            *edgePoint = *edgePoint - *foot;
            if (squaredLength(*edgePoint) < reach2)
            {
                // Given back as the offset added to the foot, rounded as the
                // original rounds it.
                *out = *edgePoint + *foot;
                return out;
            }
            from = to;
        }
        return ee::Ptr<Vec4>();
    }
    READABLE_FUNCTION(trianglePointInSphere, Draft);
}
