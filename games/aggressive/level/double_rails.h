#pragma once

// The level's double rails (gamehand13; level/rails_setup.cpp makes them at
// the load): a pair of grind edges tagged 5 and 6 in an object's edge
// table, near and parallel, is one rail the skater grinds with both feet
// (skater/double_grind.cpp). Every such edge is a node of one of two search
// trees (by its object and edge; collectDoubleRailEdges, collision_setup.cpp puts
// them in); each edge of the first kind gets a DoubleRail record with the
// nearest parallel edge of the other kind, and that edge's node the same
// record (pairedRail finds it for the grind search, skater/grind_search.cpp).

#include "ee/ee.h"
#include "zge/container/tree.h"
#include "zge/math/types.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/double_rails.h.h"

namespace zge
{
    struct Node;
}

namespace aggressive::level
{
    // The edges' tags (EdgeRecord::kind): the double rails' two sides.
    constexpr uint8_t kDoubleRailEdgeA = 5, kDoubleRailEdgeB = 6;

    // A tree node (20 bytes from g_railEdgeNodePool): the tree's links, the
    // edge and its object (the value the trees sort by), its double rail.
    struct DoubleRail;
    struct RailEdgeNode
    {
        zge::SearchTreeNode links; // +0x00 its place in a rail edge tree
        uint32_t edge;          // +0x08 the face-edge element (3 x face + edge)
        union
        {
            uint32_t object;    // +0x0C the object (its collision mesh's node)
            ee::Ptr<zge::Node> node;
        };
        ee::Ptr<DoubleRail> rail; // +0x10 0 until paired
    };
    static_assert(sizeof(RailEdgeNode) == 20 && offsetof(RailEdgeNode, rail) == 0x10);

    // The trees' key (a node's +0x08, or a search's own): edge, object.
    struct RailEdgeKey
    {
        uint32_t edge;
        uint32_t object;
    };
    static_assert(offsetof(RailEdgeNode, object) - offsetof(RailEdgeNode, edge) == offsetof(RailEdgeKey, object));

    // A double rail (0x50 bytes from g_doubleRailPool).
    struct DoubleRail
    {
        uint32_t edge;       // +0x00 the first edge (tag 5)
        uint32_t partner;    // +0x04 the paired edge (tag 6), kNoPartner while none
        ee::Float nearest;   // +0x08 the partner's distance (4 at most)
        // +0x0C the object the rails are part of (Player::lastRail when the grind ends)
        union
        {
            uint32_t object;
            ee::Ptr<zge::Node> node;
        };
        zge::Vec4 start;     // +0x10 the line between the two edges' first ends
        zge::Vec4 end;       // +0x20 and their second ends
        zge::Vec4 direction; // +0x30 unit, start to end
        zge::Vec4 up;        // +0x40 the rails' up (across the two edges): a jump leaves along it
    };
    static_assert(sizeof(DoubleRail) == 0x50 && offsetof(DoubleRail, start) == 0x10 && offsetof(DoubleRail, up) == 0x40);
    constexpr uint32_t kNoPartner = (1u << 24) - 1; // 24 bits all set, as edges are numbered

    // The two trees (zge::SearchTree) and the pools.
    constexpr ee::Ptr<zge::SearchTree> kDoubleRailEdgesA(sym::doubleRailEdgesA);
    constexpr ee::Ptr<zge::SearchTree> kDoubleRailEdgesB(sym::doubleRailEdgesB);
    inline ee::Global<uint32_t, sym::g_doubleRailPool> g_doubleRailPool;   // 256 DoubleRail blocks
    inline ee::Global<uint32_t, sym::g_railEdgeNodePool> g_railEdgeNodePool; // 512 RailEdgeNode blocks
}

namespace aggressive::level
{
    // (rails_setup.cpp)
    int32_t closestBetweenSegments(ee::Ptr<zge::Vec4> a, ee::Ptr<zge::Vec4> b, ee::Ptr<zge::Vec4> p, ee::Ptr<zge::Vec4> q, ee::Ptr<zge::Vec4> onAb, ee::Ptr<zge::Vec4> onPq, ee::Ptr<ee::Float> s, ee::Ptr<ee::Float> t);
    void freeLevelRails();
    ee::Ptr<DoubleRail> pairedRail(ee::Address edge, ee::Address object, uint32_t tag);
    uint32_t setupLevelRails();
}
