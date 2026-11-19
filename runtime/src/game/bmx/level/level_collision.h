#pragma once

// The level's collision as BMX XXX keeps it (level_collision.cpp): the level's
// box sweep, the contact callback a level object's dynamics asks, and a face's
// centroid for the collision set-up. BMX XXX's own code (its game block), word for
// word Aggressive Inline's (game/aggressive/level/level_collision.cpp).

#include "ee/ee.h"
#include "zge/collision/dynamics.h"
#include "zge/collision/sweep.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::bmx
{
    // The level's box sweep (the one every object's collision box is in).
    ee::Ptr<zge::Sweep> levelSweep();
    // The level's box sweep freed (the level's end).
    void freeLevelContacts();
    // The sweep made at the level's load (returns 1), and its contact handlers
    // run each frame.
    uint32_t setupLevelContacts();
    void runLevelContacts();
    // A level object's dynamics: the game's dynamicsObjectContact asked about its faces.
    void setDynamicsContactCallback(ee::Ptr<zge::Dynamics> dynamics);
    // (dynamics_contacts.cpp) The level objects' contact callback: 0 for a face a body of `dynamics` does not
    // collide with (the rider's own callbacks ask it first).
    uint32_t dynamicsObjectContact(ee::Ptr<zge::Dynamics> dynamics, ee::Ptr<zge::RigidBody> body, ee::Ptr<zge::FaceContact> contact);
    // The centroid of COUNT points (the collision set-up's faces).
    void centroid(uint32_t count, ee::Ptr<zge::Vec4> points, ee::Ptr<zge::Vec4> out);
}
