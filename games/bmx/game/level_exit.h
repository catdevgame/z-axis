#pragma once
// BMX XXX's way out of a level (game.c; level_exit.cpp).

#include <cstdint>

namespace game::bmx
{
    // The ground probe's query filter: every face, but of whole nodes only
    // the world's own (an object's node is passed over).
    int32_t worldNodesOnly(uint64_t node, int32_t face);
    // A player's frame of the level's way out: on the ground before the exit
    // the exit warning shows; on the exit itself the rider is put back at
    // the spawn.
    void checkLevelExit(uint32_t player);
}
