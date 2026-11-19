#pragma once

// PS2X_NATIVE_COLLISION_CHECK=1: the collision data the game queries, in
// RAM, against the same data made on the host from the GEO files
// (collision_data.h: the parts' positions, faces, face normals, face tags
// and edges with every word the level's setup writes at load, nearest
// edges included, the objects' TGCI face trees). The readable collision code
// (zge/collision/world.cpp) hands each collision frame to the check before
// its faces are first tested; a summary line is logged as frames are
// checked, and the first differences are described. =2: a line per frame.
//
// The part is found through the readable TGPT loader's notes
// (collision_data.h hostCollisionPart). Nothing the game does changes: the check only
// reads.

#include "ee/plain.h"

#include <cstdint>

namespace zge::native
{
    bool collisionCheckEnabled();

    // The collision frame at frameAddress and its part, once per part (again
    // after the part is noted anew).
    void checkCollisionFrameOnce(uint32_t frameAddress, uint32_t partAddress);
    // The same, given the addresses as the game's code holds them.
    void checkCollisionFrameOnce(ee::Address frame, ee::Address part);
}
