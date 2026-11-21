// The level's faces and edges with given tags (collision.h): BMX XXX's own
// code (bmx.c's collision layer), the objectives' search for the faces and
// edges their scripts name.

#include "collision.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/rider/collision/tag_search.cpp.h"

namespace game::bmx::rider::collision
{
    namespace
    {
        // The faces found, and the edges (a list ended by a 0; each has room
        // for kTagSearchCapacity), and how many the last search found. The
        // edges' list is the near-edge query's too.
        constexpr uint32_t kTagSearchCapacity = 0xFF;
        inline ee::Global<uint64_t[kTagSearchCapacity + 1], sym::g_taggedFaces> g_taggedFaces;
        inline ee::Global<uint32_t, sym::g_taggedFaceCount> g_taggedFaceCount;
        inline ee::Global<uint64_t[kTagSearchCapacity + 1], sym::g_taggedEdges> g_taggedEdges;
        inline ee::Global<uint32_t, sym::g_taggedEdgeCount> g_taggedEdgeCount;

        constexpr uint32_t kCollectTaggedFace = sym::collectTaggedFace;
        constexpr uint32_t kCollectTaggedEdge = sym::collectTaggedEdge;

        // A face of a node that has faces, else none.
        FaceRef foundFace(uint64_t node, int32_t face)
        {
            return nodeFaceCount(node) != 0 ? makeFace(static_cast<uint32_t>(node), face) : 0;
        }
    }

    // facesTagged: every face of the level whose tag is one of `count` tags,
    // up to kTagSearchCapacity, in the shared list (ended by a 0); 0 when
    // there is none.
    ee::Ptr<uint64_t> facesTagged(ee::Ptr<uint32_t> tags, uint32_t count)
    {
        ee::Local<TagSearch> search;
        search->tags = tags;
        search->count = count;
        search->capacity = kTagSearchCapacity;
        search->found = 0;
        search->out = ee::Ptr<uint64_t>(sym::g_taggedFaces);
        forEachTaggedFace(kCollectTaggedFace, search.address());
        *g_taggedFaceCount = search->found;
        if (search->found == 0)
            return ee::Ptr<uint64_t>();
        (*g_taggedFaces)[search->found] = 0;
        return ee::Ptr<uint64_t>(sym::g_taggedFaces);
    }
    READABLE_FUNCTION_WAITS(facesTagged, Verified);

    // edgesTagged: the same for the level's edges.
    ee::Ptr<uint64_t> edgesTagged(ee::Ptr<uint32_t> tags, uint32_t count)
    {
        ee::Local<TagSearch> search;
        search->tags = tags;
        search->count = count;
        search->capacity = kTagSearchCapacity;
        search->found = 0;
        search->out = ee::Ptr<uint64_t>(sym::g_taggedEdges);
        forEachTaggedEdge(kCollectTaggedEdge, search.address());
        *g_taggedEdgeCount = search->found;
        if (search->found == 0)
            return ee::Ptr<uint64_t>();
        (*g_taggedEdges)[search->found] = 0;
        return ee::Ptr<uint64_t>(sym::g_taggedEdges);
    }
    READABLE_FUNCTION_WAITS(edgesTagged, Verified);

    // The searches' callbacks (forEachTaggedFace's and forEachTaggedEdge's):
    // a full list ends the walk (0); a face or edge whose tag is one sought
    // goes on the list. 1: the walk goes on.
    int32_t collectTaggedFace(uint32_t record, uint64_t node, int32_t face, TagSearch *search)
    {
        if (search->capacity == search->found)
            return 0;
        const uint32_t tag = faceRecordTag(record);
        for (uint32_t i = 0; i < search->count; ++i)
        {
            if (search->tags[i] != tag)
                continue;
            search->out[search->found] = foundFace(node, face);
            search->found = search->found + 1;
            break;
        }
        return 1;
    }
    READABLE_FUNCTION(collectTaggedFace, Verified);

    int32_t collectTaggedEdge(uint32_t record, uint64_t node, int32_t face, uint32_t side, TagSearch *search)
    {
        side &= 0xFF;
        if (search->capacity == search->found)
            return 0;
        const uint32_t tag = edgeRecordTag(record);
        for (uint32_t i = 0; i < search->count; ++i)
        {
            if (search->tags[i] != tag)
                continue;
            const EdgeRef edge = nodeFaceCount(node) != 0
                                     ? makeEdge(static_cast<uint32_t>(node), static_cast<uint32_t>(face), static_cast<uint8_t>(side))
                                     : 0;
            search->out[search->found] = edge;
            search->found = search->found + 1;
            break;
        }
        return 1;
    }
    READABLE_FUNCTION(collectTaggedEdge, Verified);
}
