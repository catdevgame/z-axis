#pragma once

// The level's collision as DM2 keeps it (level_collision.cpp): the level's
// box sweep, the contact callback a level object's dynamics asks, and a face's
// centroid for the collision set-up. DM2's own code (its game block), word for
// word Aggressive Inline's (game/aggressive/level/level_collision.cpp).

#include "ee/ee.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::dm2
{
    // The level's box sweep (the one every object's collision box is in).
    ee::Ptr<zge::Sweep> levelSweep();
    // The level's box sweep made, with no dynamics objects yet and the
    // handler for two bodies touching (the level's start). Returns 1.
    uint32_t setupLevelContacts();
    // Each contact of the level's sweep given to its handler (every frame of play).
    void runLevelContacts();
    // The level's box sweep freed (the level's end).
    void freeLevelContacts();
    // A level object's dynamics: the game's dynamicsObjectContact asked about its faces.
    void setDynamicsContactCallback(ee::Ptr<zge::Dynamics> dynamics);
    // The centroid of COUNT points (the collision set-up's faces).
    void centroid(uint32_t count, ee::Ptr<zge::Vec4> points, ee::Ptr<zge::Vec4> out);
}
