#pragma once

// The child states: what the level scripts changed about an object's
// geometry (setObjectVisible, setChildVisible, setChildProperties), a list
// the saves keep, so a load shows the level as the scripts left it.

#include "ee/ee.h"

#include <cstdint>
#include "sym/aggressive/script/child_states.h.h"

namespace aggressive::script
{
    // A state's flags, compiled as one-bit fields of a word. An object's own
    // state (childHash 0, queueObjectVisibility) is always kWholeTree and
    // kChildFlagKeep says its children go with it; a child's (recordChildState)
    // always kChildFlagKeep.
    constexpr uint32_t kChildShown = 0x1u;
    constexpr uint32_t kChildCollides = 0x2u;
    constexpr uint32_t kChildWholeTree = 0x4u;
    constexpr uint32_t kChildFlagKeep = 0x8u;
    constexpr uint32_t kChildStateFlags = 0xFu;

    struct ChildState
    {
        uint32_t objectHash;           // +0x00 the object's name hash, as findObjects takes it
        uint32_t childHash;            // +0x04 the child node's name hash; 0: the object itself
        uint32_t flags;                // +0x08 kChild*, the rest of the word left as the block had it
        ee::Ptr<ChildState> next;      // +0x0C
        ee::Ptr<ChildState> previous;  // +0x10
    };
    static_assert(sizeof(ChildState) == 0x14);

    // A state in a saved game.
    struct SavedChildState
    {
        uint32_t objectHash; // +0x00
        uint32_t childHash;  // +0x04
        uint32_t flags;      // +0x08
    };
    static_assert(sizeof(SavedChildState) == 0xC);

    // What setChildShownByHash sets on the child it finds.
    struct ChildProperties
    {
        uint32_t childHash; // +0x00
        uint32_t shown;     // +0x04
        uint32_t collides;  // +0x08
        uint32_t wholeTree; // +0x0C
    };
    static_assert(sizeof(ChildProperties) == 0x10);

    constexpr uint32_t kSetChildShownByHash = sym::setChildShownByHash; // the visitor over a node's subtree
}

namespace aggressive::script
{
    // (child_states.cpp)
    void queueObjectVisibility(uint32_t objectHash, uint32_t visible, uint32_t keepChildren);
    void recordChildState(uint32_t objectHash, ee::Ptr<ChildProperties> properties);
    void restoreChildStates(ee::Ptr<SavedChildState> records, uint32_t bytes);
    uint32_t saveChildStates(ee::Ptr<uint32_t> bytes);
    int32_t unloadLevelScript();
}
