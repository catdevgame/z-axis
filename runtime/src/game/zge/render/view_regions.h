#pragma once

// A level's view regions, read from its chunk file (readPvsoChunk in
// zge/assets/chunk_reader.cpp).
//
// A view region (a PVSO chunk) is a box of the level with the list of the
// regions that cannot be seen from inside it. For each drawn scene
// (drawScene) markRegionsOutOfView finds the region holding the view's
// position, takes a new g_outOfViewMark and stamps it on every region of
// that list; the scene
// walk then skips sectors and nodes whose region carries the current mark
// (render/node_tree.cpp, regionInView).

#include "ee/ee.h"
#include "zge/math/box.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Region
    {
        uint32_t mesh;         // +0x00 the hash of the mesh this region is (findRegionForMesh)
        uint32_t outOfViewMark; // +0x04 g_outOfViewMark when the view's region last hid it
#if PS2X_GAME_DM2
        // ZGE2's region is 0x40 bytes with the box at +0x10 as ZGE3's and the hidden list after it: hiddenCount a word
        // at +0x30, hidden at +0x34 (newRegion, freeRegion, finishRegions; +0x08 and +0x38 are in no DM2 function read).
        uint8_t _08[0x02];
        // [unplaced: ZGE2's reader knows only the chunk without a flag and writes none; AI's field stands in the unread
        // words so the shared reader compiles]
        uint16_t chunkFlag;
        uint8_t _0c[0x04];
        Box box;               // DM2 +0x10
        uint32_t hiddenCount;  // DM2 +0x30
        ee::Ptr<uint32_t> hidden; // DM2 +0x34
        uint8_t _38[0x08];
#else
        uint16_t hiddenCount;  // +0x08 the regions hidden from inside this one
        // +0x0A the PVSO chunk's flag word (0 or 1 in AI's levels), or 1 for
        // the older chunk without one (AI's multiplayer Cannery; DM2's reader
        // knows only that one). No build reads it.
        uint16_t chunkFlag;
        // +0x0C hiddenCount words: the hidden regions' mesh hashes as read,
        // which finishRegions turns into their regions' addresses (0 for a
        // mesh with no region).
        ee::Ptr<uint32_t> hidden;
        Box box; // +0x10 (w 1 on both corners)
#endif
    };
    static_assert(offsetof(Region, hiddenCount) == game::readable::perGame(0x08u, 0x30u) && offsetof(Region, box) == 0x10);
    static_assert(offsetof(Region, hidden) == game::readable::perGame(0x0Cu, 0x34u));
    static_assert(sizeof(Region) == game::readable::perGame(0x30u, 0x40u));
    static_assert(offsetof(Region, chunkFlag) == 0x0A);
}
