#pragma once

// The trees' fade by distance (fx.c; tree_fade.cpp): in the two levels with
// alpha trees, a tree close to the camera is not drawn, and one a little
// further is drawn see-through, so that trees do not block the view of the
// rider.

#include "ee/ee.h"
#include "zge/scene/node.h"

#include <cstdint>

namespace game::dm2
{
    // What the trees' draw check is given: the distances the fade runs
    // between and the trees' node name.
    struct TreeFade
    {
        ee::Float hiddenWithin;   // +0x00 nearer than this a tree is not drawn
        ee::Float solidFrom;      // +0x04 from here on it is drawn as it is
        uint32_t nodeName;        // +0x08 the nodes it is for: their names begin with this
        uint32_t nodeNameLength;  // +0x0C
    };
    static_assert(sizeof(TreeFade) == 0x10);

    // In levels 0 and 6, every world mesh named as a tree gets the fade as
    // its draw check.
    void setupTreeFade();
    // The world meshes' visit: a node whose name begins with the fade's
    // gets fadeTreeByDistance as its draw check. Returns 1 (the walk goes on).
    uint32_t setTreeFadeCheck(ee::Ptr<zge::Node> node, ee::Ptr<TreeFade> fade);
    // The trees' draw check: 0 (not drawn) nearer than the fade's start;
    // else the node's materials get the alpha of its distance and 1.
    uint32_t fadeTreeByDistance(ee::Ptr<zge::Node> node, ee::Address levelOfDetail, ee::Ptr<TreeFade> fade);
}
