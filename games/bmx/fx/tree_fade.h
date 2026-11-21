#pragma once
// BMX XXX's trees' draw check (fx.c; tree_fade.cpp): DM2 fades its alpha
// trees by their distance from the camera; BMX XXX keeps the set-up, and
// its check draws every tree as it is.

#include "ee/ee.h"
#include "zge/scene/node.h"

#include <cstdint>

namespace game::bmx
{
    // What the trees' draw check is given: DM2's fade distances and the
    // trees' node name.
    struct TreeFade
    {
        ee::Float hiddenWithin;  // +0x00 not read in BMX XXX
        ee::Float solidFrom;     // +0x04 not read in BMX XXX
        uint32_t nodeName;       // +0x08 the nodes it is for: their names begin with this
        uint32_t nodeNameLength; // +0x0C
    };
    static_assert(sizeof(TreeFade) == 0x10);

    // Every world mesh named as a tree gets the trees' draw check.
    void setupTreeFade();
    // The world meshes' visit: a node whose name begins with the fade's
    // gets treeDrawCheck as its draw check. Returns 1 (the walk goes on).
    uint32_t setTreeFadeCheck(ee::Ptr<zge::Node> node, ee::Ptr<TreeFade> fade);
    // The trees' draw check: 1 (drawn), always.
    uint32_t treeDrawCheck();
}
