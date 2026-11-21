// The level's collision as the game keeps it (level_collision.h): moved from
// zge/collision (sweep.cpp, dynamics_load.cpp, faces.cpp), where they sat as
// engine code; the blocks say they are AI's own (layer2). DM2 (PS2) has them
// word for word in its own code.

#include "aggressive/level/level_collision.h"
#include "level_collision.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/level_collision.cpp.h"

namespace aggressive
{
    using namespace zge;

    namespace
    {
        // The level's box sweep (the engine's sweep and prune of the
        // objects' collision boxes): the level's dynamics make it.
        inline ee::Global<uint32_t, sym::g_levelSweep> g_levelSweep;
    }

    // levelSweep: the level's box sweep. DM2 (PS2) has it word for word.
    ee::Ptr<Sweep> levelSweep()
    {
        return ee::Ptr<Sweep>(*g_levelSweep);
    }
    READABLE_FUNCTION(levelSweep, Verified);

    // freeLevelContacts (the level's end): the level's box sweep freed. DM2
    // (PS2) has it word for word.
    void freeLevelContacts()
    {
        zge::freeCollisionTable(ee::Ptr<Sweep>(*g_levelSweep));
        *g_levelSweep = 0;
    }
    READABLE_FUNCTION_WAITS(freeLevelContacts, Verified);

    // setDynamicsContactCallback: a level object's dynamics asks the game's
    // dynamicsObjectContact about each face it touches. DM2 (PS2) has it word
    // for word, with its own dynamicsObjectContact.
    void setDynamicsContactCallback(ee::Ptr<Dynamics> dynamics)
    {
        game::tail(zge::setDynamicsContactHandler, dynamics, ContactCallback(sym::dynamicsObjectContact));
    }
    READABLE_FUNCTION(setDynamicsContactCallback, Verified);

    namespace
    {
        inline ee::Global<ee::Float, sym::g_third> g_third; // 1 / 3
    }

    // centroid (count, points, out): the points' centroid: summed onto
    // (0, 0, 0, 1), then the xyz by 1 / 3 (the faces' three).
    // DM2 (PS2) has it word for word (the level's collision setup calls it).
    void centroid(uint32_t count, ee::Ptr<Vec4> points, ee::Ptr<Vec4> out)
    {
        out->x = out->y = out->z = ee::Float(0.0f);
        out->w = ee::Float(1.0f);
        const uint32_t n = count & 0xFFu;
        for (uint32_t point = 0; point < n; ++point)
            *out = *out + points[point];
        *out = *out * *g_third;
    }
    READABLE_FUNCTION(centroid, Verified);
}
