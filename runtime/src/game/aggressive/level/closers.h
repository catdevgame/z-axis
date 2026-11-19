#pragma once

// "AreYouCloser" (closers.cpp): the objects that listen for the skater's action
// button, kept in a search tree, and the nearest in reach found.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace aggressive::level
{
    // A listener in the tree: the tree's links, then the listening object,
    // the value the tree sorts by.
    struct CloserRecord
    {
        zge::SearchTreeNode links; // +0x00
        ee::Ptr<zge::SceneObject> object; // +0x08
    };
    static_assert(sizeof(CloserRecord) == 12 && offsetof(CloserRecord, object) == zge::kSearchTreeValue);
}

namespace aggressive::level
{
    // (closers.cpp)
    ee::Ptr<CloserRecord> findCloserListener(ee::Address object);
    uint32_t findCloserObject(ee::Ptr<zge::Vec4> position, ee::Ptr<ee::Float> distance, ee::Float radius);
    int32_t freeCloserQueries();
    int32_t initCloserQueries();
    void registerCloserListener(ee::Ptr<zge::SceneObject> object);
}
