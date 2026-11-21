#pragma once

// Two bodies of the level's sweep touching (bodies.cpp): the sweep's
// handler for a pair of rigid bodies, DM2's own (its dynamics objects and
// its riders' bail bodies share the sweep's one kind).

#include "ee/ee.h"
#include "zge/collision/dynamics.h"

#include <cstdint>

namespace game::dm2
{
    // Bodies `a` and `b` touching. Nothing for two bodies of one owner, for
    // two riders' bodies, or when the engine says the contact no longer
    // holds. A dynamics object asleep is woken when what it touches is
    // awake or a rider's body; a rider's body touching an object tells the
    // game's touch handler. Then the engine keeps the contact and the
    // objects are told of the touch with the bodies' relative motion over a
    // step; a rider's own speed over the ground is cut to its body's share
    // of the two masses.
    void bodiesTouch(ee::Ptr<zge::RigidBody> a, ee::Ptr<zge::RigidBody> b);
}
