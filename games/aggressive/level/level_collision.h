#pragma once

// The level's collision as the game keeps it (level_collision.cpp): the level's
// box sweep, the contact callback a level object's dynamics asks, and a face's
// centroid for the collision set-up. AI's own code (its game block; DM2 (PS2)
// has the same words in its own).

#include "ee/ee.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/types.h"

#include <cstdint>

namespace aggressive
{
    // The level's box sweep (the one every object's collision box is in).
    ee::Ptr<zge::Sweep> levelSweep();
    // The level's box sweep freed (the level's end).
    void freeLevelContacts();
    // A level object's dynamics: the game's dynamicsObjectContact asked about its faces.
    void setDynamicsContactCallback(ee::Ptr<zge::Dynamics> dynamics);
    // The centroid of COUNT points (the collision set-up's faces).
    void centroid(uint32_t count, ee::Ptr<zge::Vec4> points, ee::Ptr<zge::Vec4> out);
}
