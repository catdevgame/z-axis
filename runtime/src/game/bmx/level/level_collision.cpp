// The level's collision as BMX XXX keeps it (level_collision.h). BMX XXX's own code
// (its game block), word for word Aggressive Inline's
// (game/aggressive/level/level_collision.cpp): game code, so BMX XXX has its copy.

#include "level_collision.h"
#include "dynamics_objects.h"

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
#include "sym/bmx/level/level_collision.cpp.h"

namespace game::bmx
{
    using namespace zge;

    // levelSweep: the level's box sweep.
    ee::Ptr<Sweep> levelSweep()
    {
        return ee::Ptr<Sweep>(*g_levelSweep);
    }
    READABLE_FUNCTION(levelSweep, Verified);

    namespace
    {
        // Two bodies touching: bodiesTouch (dynamics_contacts.cpp).
        constexpr uint32_t kBodiesTouch = sym::bodiesTouch;
    }

    // setupLevelContacts (the level's load): the level's contact sweep made,
    // no dynamics objects yet, and two bodies' contact given its handler.
    // Aggressive Inline's sets a handler for every pair of its kinds (NPCs,
    // cars, skaters); BMX XXX's sweep has the bodies alone.
    uint32_t setupLevelContacts()
    {
        const ee::Ptr<Sweep> sweep = zge::newCollisionTable();
        *g_levelSweep = ee::Address(sweep);
        *g_dynamicsObjectCount = 0;
        zge::setCollisionPairHandler(sweep, kSweepRigidBody, kSweepRigidBody, kBodiesTouch);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setupLevelContacts, Verified);

    // runLevelContacts (the frame): the level sweep's contact handlers run.
    void runLevelContacts()
    {
        game::tail(zge::runContactHandlers, ee::Ptr<Sweep>(*g_levelSweep));
    }
    READABLE_FUNCTION(runLevelContacts, Verified);

    // freeLevelContacts (the level's end): the level's box sweep freed.
    void freeLevelContacts()
    {
        zge::freeCollisionTable(ee::Ptr<Sweep>(*g_levelSweep));
        *g_levelSweep = 0;
    }
    READABLE_FUNCTION_WAITS(freeLevelContacts, Verified);

    // setDynamicsContactCallback: a level object's dynamics asks the game's
    // dynamicsObjectContact (BMX XXX's own) about each face it touches.
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
    // (0, 0, 0, 1), then the xyz by 1 / 3 (the faces' three). The level's
    // collision set-up calls it.
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
