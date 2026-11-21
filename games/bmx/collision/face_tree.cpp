// BMX XXX's segmentFaceTree: a segment against a node's collision mesh through its face tree (ZGE4's; ZGE3's and
// ZGE2's are zge/collision/faces.cpp). The same walk and face test as ZGE3's, with three differences:
//
//   the direction  made unit length only when the segment is longer than a millionth (vec3Length, then a scale);
//                  a shorter one is used as it is
//   the walk       never cuts the segment at a split plane: a piece that crosses one goes down both sides whole
//                  (the greater side pushed first, so the lesser is visited first), so every piece starts where
//                  the segment does and every leaf's key is 0
//   the face test  a start up to 0.01 behind a one-sided face's plane still counts as in front; and a segment
//                  that closes on the plane by less than 0.01 over its length runs along it: it hits the face
//                  only when its end is within 0.01 of the plane too, at the fraction 0.01
//
// The tree, the walk's stack, the faces seen so far and a part's data are the engine's (zge/collision/
// face_tree.h); the arithmetic is the original's, in the same order: VU0 where it used VU0 (zge/math/vec.h),
// the FPU elsewhere.

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/collision/face_tree.h"
#include "zge/container/heap.h"
#include "zge/memory/pool.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/collision/face_tree.cpp.h"

namespace game::bmx
{
    namespace
    {
        using namespace zge::face_tree;

        // ---- constants (.sdata) ----------------------------------------------------

        // A segment no longer than this keeps its direction as it is (1e-6).
        inline ee::Global<ee::Float, sym::g_shortestDirection> g_shortestDirection;
        // A piece is on a split's lesser side where it starts below +0.01 (and crosses the plane when its end is
        // above -0.01), on the greater side where it starts above -0.01 (and crosses when its end is below
        // +0.01): ZGE3's, at BMX XXX's addresses.
        inline ee::Global<ee::Float, sym::g_lesserBelow> g_lesserBelow;   // 0.01
        inline ee::Global<ee::Float, sym::g_crossesAbove> g_crossesAbove; // -0.01
        inline ee::Global<ee::Float, sym::g_greaterAbove> g_greaterAbove; // -0.01
        // A start at or below this height over a one-sided face's plane is behind it (-0.01).
        inline ee::Global<ee::Float, sym::g_behindFaceHeight> g_behindFaceHeight;
        // A segment closing on a face's plane by less than this over its whole length runs along the face; it
        // hits it only when its end is within this of the plane, and at this fraction (0.01).
        inline ee::Global<ee::Float, sym::g_alongFaceTolerance> g_alongFaceTolerance;
        // Leaf keys are hundredths: key * this is back in units (0.01).
        inline ee::Global<ee::Float, sym::g_keyToDistance> g_keyToDistance;
    }

    // segmentFaceTreeUncut: BMX XXX's segmentFaceTree. The nearest face of the part the segment hits, kept as
    // the query's one FaceHit (the node's hit record made again with it alone); 1 when one was found.
    int32_t segmentFaceTreeUncut(const zge::QuerySegment *segment, const zge::QuerySegment *clipped,
                                 ee::Ptr<ee::Ptr<uint8_t>> treeSlot, ee::Ptr<zge::ZgePart> part,
                                 ee::Ptr<zge::SphereQuery> query, const zge::Matrix *toWorld)
    {
        // (On the game's stack, where the original keeps them.)
        ee::Local<zge::Vec4> directionSlot;
        ee::Local<WalkEntry> pieceSlot;
        ee::Local<zge::Vec4> facePoint;
        ee::Local<zge::Vec4> startSlot;
        ee::Local<zge::Vec4> faceNormal;
        ee::Local<uint32_t> filterFace;
        zge::Vec4 &direction = *directionSlot;
        zge::Vec4 &start = *startSlot;

        const ee::Ptr<uint8_t> faceTree = *treeSlot;
        const MeshSource mesh(part, faceTree);
        int32_t hit = 0;
        clearVisited(mesh.faceCount());

        // The segment's direction and length: unit length unless the segment is next to none. (The scale's
        // factor reaches VU0 beside what this function's caller left in its register.)
        direction = segment->end - segment->start;
        const ee::Float length = zge::vec3Length(&direction.xyz());
        if (*g_shortestDirection < length)
            direction = zge::scaledCarrying(direction, ee::Float(1.0f) / length, zge::scaleLeftover(zge::LeftBy::caller));

        // Walk the tree with the clipped segment; a piece that crosses a split plane goes down both sides, so
        // every leaf goes to the sorted list with the key 0.
        WalkEntry *stack = *g_walkStack;
        std::memcpy(&stack[0].start, &clipped->start, sizeof(zge::Vec4));
        std::memcpy(&stack[0].end, &clipped->end, sizeof(zge::Vec4));
        stack[0].startAlong = 0.0f;
        stack[0].node = 0;
        uint32_t top = 1;
        uint32_t leaves = 0;
        do
        {
            const WalkEntry &piece = *pieceSlot = stack[--top];
            const ee::Ptr<uint8_t> place = faceTree + piece.node;
            const zge::FaceTreeLeaf &leaf = mesh.node(place);
            if (leaf.axis == zge::kFaceTreeLeaf)
            {
                if (leaf.count)
                {
                    zge::priorityQueueInsert(ee::toInt(piece.startAlong * 100.0f), ee::Address(place), kLeafQueue);
                    leaves = (leaves + 1u) & 0xFFFFu;
                }
                continue;
            }
            const zge::FaceTreeSplit &split = mesh.split(place);
            const ee::Float plane = ee::toFloat(split.plane) * *zge::g_faceTreePlaneScale;
            const int k = splitCoordinate(split.axis);
            const ee::Float startOffset = component(piece.start, k) - plane;
            const ee::Float endOffset = component(piece.end, k) - plane;

            const auto whole = [&](uint16_t child)
            {
                WalkEntry &entry = stack[top++];
                entry.start = piece.start;
                entry.startAlong = 0.0f;
                entry.end = piece.end;
                entry.node = child * 4u;
            };
            const auto bothSides = [&]
            {
                whole(greaterSide(split));
                whole(lesserSide(split));
            };
            if (startOffset < *g_lesserBelow)
            {
                if (*g_crossesAbove < endOffset)
                    bothSides();
                else
                    whole(lesserSide(split));
            }
            else if (*g_greaterAbove < startOffset)
            {
                if (endOffset < *g_lesserBelow)
                    bothSides();
                else
                    whole(greaterSide(split));
            }
            else if (endOffset < *g_lesserBelow)
                whole(lesserSide(split));
            else if (*g_greaterAbove < endOffset)
                whole(greaterSide(split));
            else
                bothSides();
        } while (top);
        if (!leaves)
            return hit;

        start = segment->start;
        faceNormal->w = 0.0f;

        // One face of a leaf.
        zge::QueryState &state = *zge::g_queryState;
        auto testFace = [&](uint16_t index)
        {
            if (!visitOnce(index))
                return;
            const zge::Face &face = mesh.face(index);
            if (query->filter)
            {
                *filterFace = index;
                if (!filterKeeps(*query, state.node, part, filterFace))
                    return;
            }
            if (!(face.flags & zge::kFaceCollides) && !(query->flags & zge::kQueryAllFaces))
                return;
            const zge::Vec3 &normal = mesh.normal(index);
            faceNormal->x = normal.x;
            faceNormal->y = normal.y;
            faceNormal->z = normal.z;

            // The start's height over the face's plane, and how fast the segment closes on it: faces are hit
            // from the front (from up to 0.01 behind; from behind too when two-sided), between start and end,
            // and only nearer than the nearest hit yet.
            facePoint = start - mesh.position(face.vertex[0]);
            const ee::Float height = zge::dot(facePoint, faceNormal);
            if (height <= *g_behindFaceHeight && !(face.flags & zge::kFaceTwoSided))
                return;
            const ee::Float closing = zge::dot(direction, faceNormal);
            const ee::Float approach = -closing * length;
            ee::Float fraction;
            if (ee::abs(approach) < *g_alongFaceTolerance)
            {
                // Along the face: a hit only where the segment ends in its plane.
                facePoint = segment->end - mesh.position(face.vertex[0]);
                const ee::Float endHeight = zge::dot(facePoint, faceNormal);
                if (!(ee::abs(endHeight) <= *g_alongFaceTolerance))
                    return;
                fraction = *g_alongFaceTolerance;
            }
            else
                fraction = height / approach;
            if (fraction < 0.0f || 1.0f < fraction || state.nearest < fraction)
                return;
            const ee::Float distance = fraction * length;
            facePoint = direction * distance;
            facePoint = facePoint + start;
            if (!faceContains(mesh, face, faceNormal, facePoint))
                return;

            // The nearest face yet: the node's hit record is made again (its pool emptied) with this one
            // FaceHit. (The hit node is not checked: [0] writes where the pointer points, 0 too.)
            hit = 1;
            if (state.hitNode)
            {
                const zge::HitNode kept = *state.hitNode;
                poolReset(state.hitNodePool);
                const ee::Ptr<zge::HitNode> fresh = zge::poolAllocAs<zge::HitNode>(state.hitNodePool);
                *fresh = kept;
                state.hitNode = fresh;
            }
            state.hitNode[0].faceCount = 1;
            state.faceCount = 1;
            state.hitNode[0].faces = state.facesBuffer;
            zge::FaceHit &faceHit = state.hitNode[0].faces[0];
            faceHit.distance = distance;
            faceHit.face = index;
            state.nearest = fraction;
            if (toWorld && (query->flags & zge::kQueryWorldPoints))
                faceHit.point = zge::transformed(*toWorld, facePoint);
            else
                faceHit.point = facePoint;
        };

        for (uint32_t taken = 0; taken < leaves; ++taken)
        {
            // Leaves come nearest first: once one starts beyond the nearest hit so far, nothing further on can
            // be nearer.
            const ee::Ptr<SortedEntry> first = zge::priorityQueuePopAs<SortedEntry>(kLeafList);
            if (state.nearest * length < ee::toFloat(first->key) * *g_keyToDistance)
            {
                priorityQueueClear(kLeafList);
                return hit;
            }
            const zge::FaceTreeLeaf &leaf = mesh.node(first->record);
            const uint16_t *leafFace = mesh.leafFaces(leaf);
            for (uint32_t n = 0; n < leaf.count; ++n)
                testFace(leafFace[n]);
        }
        return hit;
    }
    READABLE_FUNCTION_VU0(segmentFaceTreeUncut, Draft);
}
